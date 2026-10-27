#include "modules/coverage_kernel.hpp"
#include "modules/property_tree.hpp"
#include "modules/coverage_sql.hpp"
#include "modules/quantity.hpp"
#include "duckdb_compat.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "core/named_parameters.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

namespace {

//------------------------------------------------------------------------------
// In-memory trees
//------------------------------------------------------------------------------

struct KAttr {
	string id;
	string name;
	string dimension;
	string unit;
	bool has_lo = false;
	bool has_hi = false;
	double lo = 0;
	double hi = 0;
	string bound;
	string default_bound;
	string category;
	vector<double> name_vec;
};

struct KItem {
	string id;
	string cls;
	double count = 1; // effective count (product along the path)
	bool reviewed = false;
	std::map<string, double> probs;
	vector<KAttr> attrs;
	vector<std::pair<string, string>> choices; // (OR parent, chosen child) along the path
	string path;                               // node ids from the root, joined by '/'
};

struct KTree {
	vector<KItem> items; // sorted by node id
	bool reviewed = true;
};

const Value *Field(const Value &v, const char *name) {
	if (v.IsNull() || v.type().id() != LogicalTypeId::STRUCT) {
		return nullptr;
	}
	auto &types = StructType::GetChildTypes(v.type());
	auto &vals = StructValue::GetChildren(v);
	for (idx_t i = 0; i < types.size(); i++) {
		if (StringUtil::CIEquals(static_cast<const string &>(types[i].first), name)) {
			return &vals[i];
		}
	}
	return nullptr;
}

string FieldText(const Value &v, const char *name) {
	auto f = Field(v, name);
	return f && !f->IsNull() ? f->ToString() : string();
}

bool FieldNumber(const Value &v, const char *name, double &out) {
	auto f = Field(v, name);
	if (!f || f->IsNull()) {
		return false;
	}
	double v_out = f->DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	if (!std::isfinite(v_out)) {
		return false;
	}
	out = v_out;
	return true;
}

//------------------------------------------------------------------------------
// Input types: checked per chunk, so malformed input is a clean error
//------------------------------------------------------------------------------

bool IsNullType(const LogicalType &t) {
	return t.id() == LogicalTypeId::SQLNULL;
}

[[noreturn]] void TypeError(const string &fn, const string &what, const LogicalType &t) {
	throw InvalidInputException("%s: %s, not %s", fn, what, t.ToString());
}

//! LIST(STRUCT) or NULL; returns the struct type, or nullptr when there is none to check.
const LogicalType *ListOfStruct(const string &fn, const string &name, const LogicalType &t) {
	if (IsNullType(t)) {
		return nullptr;
	}
	if (t.id() != LogicalTypeId::LIST) {
		TypeError(fn, name + " must be a LIST of STRUCT", t);
	}
	auto &child = ListType::GetChildType(t);
	if (IsNullType(child)) {
		return nullptr;
	}
	if (child.id() != LogicalTypeId::STRUCT) {
		TypeError(fn, name + " must be a LIST of STRUCT", t);
	}
	return &child;
}

void CheckScalarField(const string &fn, const string &name, const LogicalType &t) {
	if (IsNullType(t)) {
		return;
	}
	if (name == "count" || name == "lo" || name == "hi" || name == "score" || name == "label_weight" ||
	    name == "mu" || name == "partition_gate" || name == "count_gate" || name == "name_floor") {
		if (!t.IsNumeric()) {
			TypeError(fn, name + " must be numeric", t);
		}
	} else if (name == "reviewed" || name == "trace") {
		if (t.id() != LogicalTypeId::BOOLEAN) {
			TypeError(fn, name + " must be BOOLEAN", t);
		}
	} else if (t.id() == LogicalTypeId::LIST || t.id() == LogicalTypeId::STRUCT || t.id() == LogicalTypeId::MAP) {
		TypeError(fn, name + " must be a scalar", t);
	}
}

void CheckNodesType(const string &fn, const string &name, const LogicalType &t) {
	auto node = ListOfStruct(fn, name, t);
	if (!node) {
		return;
	}
	for (auto &field : StructType::GetChildTypes(*node)) {
		auto fname = StringUtil::Lower(static_cast<const string &>(field.first));
		auto &ft = field.second;
		if (fname == "probs") {
			if (!IsNullType(ft) && (ft.id() != LogicalTypeId::MAP || !MapType::ValueType(ft).IsNumeric())) {
				TypeError(fn, "probs must be a MAP(VARCHAR, DOUBLE)", ft);
			}
		} else if (fname == "attrs") {
			auto attr = ListOfStruct(fn, "attrs", ft);
			if (!attr) {
				continue;
			}
			for (auto &af : StructType::GetChildTypes(*attr)) {
				auto aname = StringUtil::Lower(static_cast<const string &>(af.first));
				if (aname == "name_vec") {
					auto &vt = af.second;
					if (!IsNullType(vt) && (vt.id() != LogicalTypeId::LIST ||
					                        !(ListType::GetChildType(vt).IsNumeric() ||
					                          IsNullType(ListType::GetChildType(vt))))) {
						TypeError(fn, "name_vec must be a LIST of numbers", vt);
					}
				} else {
					CheckScalarField(fn, aname, af.second);
				}
			}
		} else {
			CheckScalarField(fn, fname, ft);
		}
	}
}

void CheckOptionsType(const string &fn, const LogicalType &t) {
	if (IsNullType(t)) {
		return;
	}
	if (t.id() != LogicalTypeId::STRUCT) {
		TypeError(fn, "options must be a STRUCT", t);
	}
	for (auto &field : StructType::GetChildTypes(t)) {
		auto fname = StringUtil::Lower(static_cast<const string &>(field.first));
		if (fname == "rules") {
			auto rule = ListOfStruct(fn, "rules", field.second);
			if (rule) {
				for (auto &rf : StructType::GetChildTypes(*rule)) {
					CheckScalarField(fn, StringUtil::Lower(static_cast<const string &>(rf.first)), rf.second);
				}
			}
		} else {
			CheckScalarField(fn, fname, field.second);
		}
	}
}

//! A packed node as read, before the walk from the roots.
struct RawNode {
	string id, parent, kind, edge, cls;
	double count = 1;
	bool reviewed = false;
	std::map<string, double> probs;
	vector<KAttr> attrs;
};

//! A soft-label MAP(class, p): positive finite probabilities, normalised to sum 1.
std::map<string, double> ReadDistribution(const Value *probs) {
	std::map<string, double> out;
	if (!probs || probs->IsNull()) {
		return out;
	}
	for (auto &kv : ListValue::GetChildren(*probs)) {
		auto &pair = StructValue::GetChildren(kv);
		if (pair.size() == 2 && !pair[0].IsNull() && !pair[1].IsNull()) {
			double p = pair[1].DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			if (std::isfinite(p) && p > 0) {
				out[pair[0].ToString()] += p;
			}
		}
	}
	double total = 0;
	for (auto &kv : out) {
		total += kv.second;
	}
	for (auto &kv : out) {
		kv.second /= total;
	}
	return out;
}

vector<KAttr> ReadAttributes(const Value *attrs) {
	vector<KAttr> out;
	if (!attrs || attrs->IsNull()) {
		return out;
	}
	for (auto &a : ListValue::GetChildren(*attrs)) {
		if (a.IsNull()) {
			continue;
		}
		KAttr k;
		k.id = FieldText(a, "attr_id");
		k.name = FieldText(a, "name");
		k.dimension = FieldText(a, "dimension");
		k.unit = FieldText(a, "unit");
		k.has_lo = FieldNumber(a, "lo", k.lo);
		k.has_hi = FieldNumber(a, "hi", k.hi);
		k.bound = FieldText(a, "bound");
		k.default_bound = FieldText(a, "default_bound");
		k.category = FieldText(a, "category");
		auto vec = Field(a, "name_vec");
		if (vec && !vec->IsNull()) {
			for (auto &x : ListValue::GetChildren(*vec)) {
				k.name_vec.push_back(x.IsNull() ? 0.0 : x.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
			}
		}
		out.push_back(std::move(k));
	}
	return out;
}

//! The packed nodes in input order; the first occurrence of a node id wins, as in the resolver.
vector<RawNode> ReadRawNodes(const Value &list) {
	vector<RawNode> raws;
	std::unordered_set<string> seen;
	for (auto &entry : ListValue::GetChildren(list)) {
		if (entry.IsNull()) {
			continue;
		}
		RawNode r;
		r.id = FieldText(entry, "node_id");
		if (r.id.empty() || !seen.insert(r.id).second) {
			continue;
		}
		r.parent = FieldText(entry, "parent_id");
		r.kind = FieldText(entry, "node_kind");
		r.edge = StringUtil::Upper(FieldText(entry, "edge_type"));
		r.cls = FieldText(entry, "class_id");
		if (auto cnt = Field(entry, "count"); cnt && !cnt->IsNull()) {
			r.count = cnt->DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
			if (!std::isfinite(r.count) || r.count <= 0) {
				throw InvalidInputException("coverage_kernel: node '%s': count must be positive and finite (validate_trees "
				                            "reports such nodes as bad_count)",
				                            r.id);
			}
		}
		auto rev = Field(entry, "reviewed");
		r.reviewed = rev && !rev->IsNull() && rev->GetValue<bool>();
		r.probs = ReadDistribution(Field(entry, "probs"));
		r.attrs = ReadAttributes(Field(entry, "attrs"));
		raws.push_back(std::move(r));
	}
	return raws;
}

//! Walks from the roots (reachable nodes only) and collects the items with their effective
//! counts (products along the path), OR choices and paths, sorted by node id.
KTree WalkTree(const vector<RawNode> &raws) {
	KTree tree;
	std::unordered_map<string, vector<idx_t>> children;
	vector<idx_t> roots;
	for (idx_t i = 0; i < raws.size(); i++) {
		if (raws[i].parent.empty()) {
			roots.push_back(i);
		} else {
			children[raws[i].parent].push_back(i);
		}
	}
	struct Frame {
		idx_t node;
		idx_t depth;
		double count;
		vector<std::pair<string, string>> choices;
		string path;
	};
	vector<Frame> stack;
	for (auto r : roots) {
		stack.push_back({r, 0, raws[r].count, {}, raws[r].id});
	}
	while (!stack.empty()) {
		auto f = std::move(stack.back());
		stack.pop_back();
		auto &n = raws[f.node];
		if (f.depth > MAX_TREE_DEPTH) {
			throw InvalidInputException("coverage_kernel: a tree is deeper than %llu levels",
			                            (unsigned long long)MAX_TREE_DEPTH);
		}
		if (n.kind == "item") {
			KItem item;
			item.id = n.id;
			item.cls = n.cls;
			item.count = f.count;
			item.reviewed = n.reviewed;
			item.probs = n.probs;
			if (item.probs.empty() && !item.cls.empty()) {
				item.probs[item.cls] = 1.0; // a known class is a one-hot distribution
			}
			item.attrs = n.attrs;
			item.choices = f.choices;
			item.path = f.path;
			tree.reviewed = tree.reviewed && n.reviewed;
			tree.items.push_back(std::move(item));
		}
		for (auto c : children[n.id]) {
			auto choices = f.choices;
			if (raws[c].edge == "OR") {
				choices.emplace_back(n.id, raws[c].id);
			}
			stack.push_back({c, f.depth + 1, f.count * raws[c].count, std::move(choices), f.path + "/" + raws[c].id});
		}
	}
	std::sort(tree.items.begin(), tree.items.end(), [](const KItem &a, const KItem &b) { return a.id < b.id; });
	return tree;
}

KTree LoadTree(const Value &list) {
	return WalkTree(ReadRawNodes(list));
}

//------------------------------------------------------------------------------
// Scoring
//------------------------------------------------------------------------------

struct Options {
	bool soft = true;
	string assignment = "auto";
	double label_weight = 0.0; // lambda in HLD §4.6
	double mu = 0.2;
	double partition_gate = 0.3;
	double count_gate = 0.5;
	double name_floor = 0.85;
	bool trace = false;
	std::map<std::tuple<string, string, string>, double> rules;
};

//! The first invalid kernel setting as a message, or "" (shared by the coverage_kernel scalar and
//! the table functions, which wrap it in their own exception type).
string KernelSettingsError(const string &labels, const string &assignment, double label_weight, double mu,
                           double partition_gate, double count_gate, double name_floor) {
	if (!labels.empty() && labels != "soft" && labels != "hard") {
		return "labels must be 'soft' or 'hard'";
	}
	if (assignment != "auto" && assignment != "best_match" && assignment != "partition") {
		return "assignment must be one of 'auto', 'best_match', 'partition'";
	}
	for (auto &pr : {std::make_pair("label_weight", label_weight), std::make_pair("mu", mu),
	                 std::make_pair("partition_gate", partition_gate), std::make_pair("count_gate", count_gate)}) {
		if (pr.second < 0 || pr.second > 1) {
			return string(pr.first) + " must be between 0 and 1";
		}
	}
	if (name_floor < 0 || name_floor >= 1) {
		return "name_floor must be in [0, 1)";
	}
	return string();
}

Options LoadOptions(const Value &v) {
	Options o;
	if (v.IsNull()) {
		return o;
	}
	auto labels = FieldText(v, "labels");
	o.soft = labels.empty() || StringUtil::Lower(labels) != "hard";
	auto assignment = FieldText(v, "assignment");
	if (!assignment.empty()) {
		o.assignment = StringUtil::Lower(assignment);
	}
	FieldNumber(v, "label_weight", o.label_weight);
	FieldNumber(v, "mu", o.mu);
	FieldNumber(v, "partition_gate", o.partition_gate);
	FieldNumber(v, "count_gate", o.count_gate);
	FieldNumber(v, "name_floor", o.name_floor);
	auto trace = Field(v, "trace");
	o.trace = trace && !trace->IsNull() && trace->GetValue<bool>();
	auto error = KernelSettingsError(StringUtil::Lower(labels), o.assignment, o.label_weight, o.mu, o.partition_gate,
	                                 o.count_gate, o.name_floor);
	if (!error.empty()) {
		throw InvalidInputException("coverage_kernel: %s", error);
	}
	auto rules = Field(v, "rules");
	if (rules && !rules->IsNull()) {
		for (auto &r : ListValue::GetChildren(*rules)) {
			double score = 0;
			if (!r.IsNull() && FieldNumber(r, "score", score)) {
				o.rules[std::make_tuple(FieldText(r, "dimension"), FieldText(r, "required"), FieldText(r, "provided"))] = score;
			}
		}
	}
	return o;
}

double LabelSim(const KItem &q, const KItem &c, const Options &o) {
	if (!o.soft) {
		return !q.cls.empty() && q.cls == c.cls ? 1.0 : 0.0;
	}
	auto &pq = q.probs;
	auto &pc = c.probs;
	double s = 0;
	for (auto &kv : pq) {
		auto it = pc.find(kv.first);
		if (it != pc.end() && kv.second > 0 && it->second > 0) {
			s += std::sqrt(kv.second * it->second);
		}
	}
	return MinValue(1.0, s);
}

vector<string> Tokens(const string &name) {
	vector<string> out;
	string cur;
	for (char ch : name) {
		bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
		             (unsigned char)ch >= 0x80;
		if (alnum) {
			cur += (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch;
		} else if (!cur.empty()) {
			out.push_back(cur);
			cur.clear();
		}
	}
	if (!cur.empty()) {
		out.push_back(cur);
	}
	return out;
}

double NameSim(const KAttr &x, const KAttr &y, const Options &o) {
	if (!x.name_vec.empty() && x.name_vec.size() == y.name_vec.size()) {
		double dot = 0, nx = 0, ny = 0;
		for (idx_t i = 0; i < x.name_vec.size(); i++) {
			dot += x.name_vec[i] * y.name_vec[i];
			nx += x.name_vec[i] * x.name_vec[i];
			ny += y.name_vec[i] * y.name_vec[i];
		}
		if (nx > 0 && ny > 0) {
			double cos = dot / std::sqrt(nx * ny);
			double floor = o.name_floor; // validated to [0, 1)
			return MaxValue(0.0, MinValue(1.0, (cos - floor) / (1 - floor)));
		}
	}
	if (StringUtil::CIEquals(x.name, y.name)) {
		return x.name.empty() ? 0.0 : 1.0;
	}
	auto a = Tokens(x.name), b = Tokens(y.name);
	if (a.empty() || b.empty()) {
		return 0.0;
	}
	std::unordered_set<string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
	idx_t inter = 0;
	for (auto &t : sa) {
		inter += sb.count(t);
	}
	return double(inter) / double(sa.size() + sb.size() - inter);
}

string FormatNumber(double v) {
	return StringUtil::Format("%g", v);
}

string ValueText(const KAttr &a, bool requirement, const string &bound) {
	string s;
	if (requirement) {
		s = bound.empty() ? "point" : bound;
		s += " ";
	}
	if (!a.category.empty()) {
		return s + a.category;
	}
	if (!a.has_lo && !a.has_hi) {
		return s + "?";
	}
	double lo = a.has_lo ? a.lo : a.hi, hi = a.has_hi ? a.hi : a.lo;
	s += lo == hi ? FormatNumber(lo) : FormatNumber(lo) + ".." + FormatNumber(hi);
	if (!a.unit.empty() && a.unit != "1") {
		s += " " + a.unit;
	}
	return s;
}

struct AttrMatch {
	const KAttr *req = nullptr;
	const KAttr *prov = nullptr;
	bool gate = true;
	double name_sim = 0;
	double value_score = 0;
	bool has_distance = false;
	double distance = 0;
	string rule;
	double score = 0;
	string bound;
};

//! The best provided attribute for one required attribute.
AttrMatch MatchAttr(const KAttr &x, const KItem &c, const Options &o) {
	AttrMatch best;
	best.req = &x;
	best.bound = !x.bound.empty() ? x.bound : x.default_bound;
	best.rule = "gate";
	for (auto &y : c.attrs) {
		AttrMatch m;
		m.req = &x;
		m.prov = &y;
		m.bound = best.bound;
		if (!x.category.empty()) {
			if (y.category.empty() || (!x.dimension.empty() && x.dimension != y.dimension)) {
				continue;
			}
			auto dim = !y.dimension.empty() ? y.dimension : x.dimension;
			auto it = o.rules.find(std::make_tuple(dim, x.category, y.category));
			m.value_score = it != o.rules.end() ? it->second : (x.category == y.category ? 1.0 : 0.0);
			m.rule = "compat";
		} else {
			if (x.dimension.empty() || y.dimension.empty() || x.dimension != y.dimension) {
				continue;
			}
			ValueSide req, prov;
			req.has_lo = x.has_lo;
			req.has_hi = x.has_hi;
			req.lo = x.lo;
			req.hi = x.hi;
			req.dimension = x.dimension;
			req.unit = x.dimension == "currency" ? x.unit : string();
			req.bound = m.bound;
			prov.has_lo = y.has_lo;
			prov.has_hi = y.has_hi;
			prov.lo = y.lo;
			prov.hi = y.hi;
			prov.dimension = y.dimension;
			prov.unit = y.dimension == "currency" ? y.unit : string();
			ValueOptions vo;
			auto r = ValueSatisfies(req, prov, vo);
			m.value_score = r.score;
			m.has_distance = r.has_distance;
			m.distance = r.distance;
			m.rule = r.rule;
		}
		m.gate = false;
		m.name_sim = NameSim(x, y, o);
		m.score = (o.mu * m.name_sim + (1 - o.mu)) * m.value_score;
		if (best.gate || m.score > best.score) {
			best = m;
		}
	}
	return best;
}

struct ItemScore {
	double label = 0;
	double a = 1;
	double score = 0;
	vector<AttrMatch> attrs;
};

ItemScore Score(const KItem &q, const KItem &c, const Options &o, bool with_attrs) {
	ItemScore p;
	p.label = LabelSim(q, c, o);
	if (p.label <= 0) {
		p.a = 0;
		return p; // no agreement: the score is 0 whatever the attributes
	}
	if (!q.attrs.empty()) {
		double sum = 0;
		for (auto &x : q.attrs) {
			auto m = MatchAttr(x, c, o);
			sum += m.gate ? 0.0 : m.score;
			if (with_attrs) {
				p.attrs.push_back(m);
			}
		}
		p.a = sum / double(q.attrs.size());
	}
	p.score = o.label_weight * p.label + (1 - o.label_weight) * p.label * p.a;
	return p;
}

bool Compatible(const KItem &a, const KItem &b) {
	for (auto &ca : a.choices) {
		for (auto &cb : b.choices) {
			if (ca.first == cb.first && ca.second != cb.second) {
				return false; // different alternatives of the same OR group
			}
		}
	}
	return true;
}

struct ReqResult {
	const KItem *q = nullptr;
	const KItem *matched = nullptr;
	ItemScore pair;
	double count_available = 0;
	double count_factor = 0;
	double score = 0;
	bool gap = true;
};

//! The requirements of a request subtree that count for one candidate.
struct AlternativeTotals {
	double sum = 0;        // sum of requirement scores
	idx_t n = 0;           // number of requirements
	vector<idx_t> chosen;  // indices into the results of the counted requirements
};

constexpr double SCORE_EPSILON = 1e-12;

//! Every node gives (sum of requirement scores, number of requirements); an OR group takes the
//! alternative with the best mean (ties: the lowest alternative id; an alternative without
//! items counts as fully met). Without OR this is the mean over all items. Each alternative is
//! evaluated once, so the cost is linear in the tree size.
AlternativeTotals AggregateAlternatives(const vector<ReqResult> &results, const vector<idx_t> &members, idx_t level) {
	AlternativeTotals out;
	std::map<string, std::map<string, vector<idx_t>>> groups; // OR parent -> alternative -> items
	for (auto i : members) {
		auto &choices = results[i].q->choices;
		if (choices.size() <= level) {
			out.sum += results[i].score;
			out.n++;
			out.chosen.push_back(i);
		} else {
			groups[choices[level].first][choices[level].second].push_back(i);
		}
	}
	for (auto &group : groups) {
		AlternativeTotals best;
		double best_mean = -1;
		for (auto &alternative : group.second) {
			auto t = AggregateAlternatives(results, alternative.second, level + 1);
			double mean = t.n ? t.sum / double(t.n) : 1.0;
			if (mean > best_mean + SCORE_EPSILON) {
				best_mean = mean;
				best = std::move(t);
			}
		}
		out.sum += best.sum;
		out.n += best.n;
		out.chosen.insert(out.chosen.end(), best.chosen.begin(), best.chosen.end());
	}
	return out;
}

LogicalType TraceType() {
	child_list_t<LogicalType> c {
	    {"req_node_id", LogicalType::VARCHAR},  {"matched_node_id", LogicalType::VARCHAR},
	    {"matched_path", LogicalType::VARCHAR}, {"label_sim", LogicalType::DOUBLE},
	    {"attr_id", LogicalType::VARCHAR},      {"attr_name", LogicalType::VARCHAR},
	    {"req_value", LogicalType::VARCHAR},    {"prov_value", LogicalType::VARCHAR},
	    {"gate", LogicalType::BOOLEAN},         {"name_sim", LogicalType::DOUBLE},
	    {"value_score", LogicalType::DOUBLE},   {"distance", LogicalType::DOUBLE},
	    {"rule", LogicalType::VARCHAR},         {"attr_score", LogicalType::DOUBLE},
	    {"count_needed", LogicalType::DOUBLE},  {"count_available", LogicalType::DOUBLE},
	    {"req_score", LogicalType::DOUBLE},     {"is_gap", LogicalType::BOOLEAN}};
	return LogicalType::STRUCT(c);
}

LogicalType KernelResultType() {
	child_list_t<LogicalType> c {{"coverage", LogicalType::DOUBLE},
	                             {"n_requirements", LogicalType::BIGINT},
	                             {"n_gaps", LogicalType::BIGINT},
	                             {"reviewed", LogicalType::BOOLEAN},
	                             {"assignment_used", LogicalType::VARCHAR},
	                             {"trace", LogicalType::LIST(TraceType())}};
	return LogicalType::STRUCT(c);
}

Value TextOrNull(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}
Value DoubleOrNull(bool has, double v) {
	return has ? Value::DOUBLE(v) : Value(LogicalType::DOUBLE);
}

//! A requirement's best counterpart in the pool, then its count over the compatible items
//! that agree on the label; a gap when there is no counterpart, too few instances or a value
//! below 1.
void ScoreRequirement(ReqResult &r, const vector<const KItem *> &pool, const Options &o) {
	const KItem *best = nullptr;
	ItemScore best_pair;
	for (auto c : pool) {
		auto p = Score(*r.q, *c, o, false);
		if (p.label <= 0) {
			continue;
		}
		if (!best || p.score > best_pair.score) {
			best = c;
			best_pair = p;
		}
	}
	if (!best) {
		r.score = 0;
		r.gap = true;
		return;
	}
	r.matched = best;
	r.pair = Score(*r.q, *best, o, true);
	double available = 0;
	for (auto c : pool) {
		if (LabelSim(*r.q, *c, o) >= o.count_gate && Compatible(*c, *best)) {
			available += c->count;
		}
	}
	r.count_available = available;
	r.count_factor = r.q->count > 0 ? MinValue(1.0, available / r.q->count) : 1.0;
	r.score = r.pair.score * r.count_factor;
	bool values_ok = true;
	for (auto &m : r.pair.attrs) {
		if (m.gate || m.value_score < 1.0) {
			values_ok = false;
		}
	}
	r.gap = !values_ok || r.count_factor < 1.0;
}

//! Partition (HLD §4.6, §11.4): each candidate item joins the block of the requirement it agrees
//! with most (ties: the largest remaining count deficit, then the lowest node id). OR
//! alternatives are options and join every tied requirement (§11.9); an item tied between
//! exclusive request alternatives may serve each of them (§11.11).
vector<vector<const KItem *>> PartitionBlocks(const KTree &Q, const KTree &C, const Options &o) {
	vector<vector<const KItem *>> blocks(Q.items.size());
	vector<double> need(Q.items.size());
	for (idx_t i = 0; i < Q.items.size(); i++) {
		need[i] = Q.items[i].count;
	}
	for (auto &c : C.items) {
		double best = 0;
		vector<idx_t> tied;
		for (idx_t i = 0; i < Q.items.size(); i++) {
			double l = LabelSim(Q.items[i], c, o);
			if (l > best + SCORE_EPSILON) {
				best = l;
				tied.assign(1, i);
			} else if (std::fabs(l - best) <= SCORE_EPSILON && l > 0) {
				tied.push_back(i);
			}
		}
		if (tied.empty() || best < o.partition_gate) {
			continue;
		}
		if (!c.choices.empty()) {
			for (auto i : tied) {
				blocks[i].push_back(&c);
			}
			continue;
		}
		idx_t pick = tied[0];
		for (auto i : tied) {
			if (need[i] > need[pick] + SCORE_EPSILON ||
			    (std::fabs(need[i] - need[pick]) <= SCORE_EPSILON && Q.items[i].id < Q.items[pick].id)) {
				pick = i;
			}
		}
		need[pick] -= c.count;
		blocks[pick].push_back(&c);
		for (auto i : tied) {
			if (i != pick && !Compatible(Q.items[i], Q.items[pick])) {
				blocks[i].push_back(&c);
			}
		}
	}
	return blocks;
}

//! One trace row: a requirement and one of its attributes (or none).
Value TraceRow(const ReqResult &r, const AttrMatch *m) {
	child_list_t<Value> v;
	v.emplace_back("req_node_id", Value(r.q->id));
	v.emplace_back("matched_node_id", r.matched ? Value(r.matched->id) : Value(LogicalType::VARCHAR));
	v.emplace_back("matched_path", r.matched ? Value(r.matched->path) : Value(LogicalType::VARCHAR));
	v.emplace_back("label_sim", r.matched ? Value::DOUBLE(r.pair.label) : Value::DOUBLE(0.0));
	v.emplace_back("attr_id", m ? TextOrNull(m->req->id) : Value(LogicalType::VARCHAR));
	v.emplace_back("attr_name", m ? TextOrNull(m->req->name) : Value(LogicalType::VARCHAR));
	v.emplace_back("req_value", m ? Value(ValueText(*m->req, true, m->bound)) : Value(LogicalType::VARCHAR));
	v.emplace_back("prov_value", m && m->prov ? Value(ValueText(*m->prov, false, "")) : Value(LogicalType::VARCHAR));
	v.emplace_back("gate", m ? Value::BOOLEAN(m->gate) : Value(LogicalType::BOOLEAN));
	v.emplace_back("name_sim", m && !m->gate ? Value::DOUBLE(m->name_sim) : Value(LogicalType::DOUBLE));
	v.emplace_back("value_score", m ? Value::DOUBLE(m->gate ? 0.0 : m->value_score) : Value(LogicalType::DOUBLE));
	v.emplace_back("distance", m ? DoubleOrNull(!m->gate && m->has_distance, m->distance) : Value(LogicalType::DOUBLE));
	v.emplace_back("rule", m ? TextOrNull(m->rule) : Value(LogicalType::VARCHAR));
	v.emplace_back("attr_score", m ? Value::DOUBLE(m->gate ? 0.0 : m->score) : Value(LogicalType::DOUBLE));
	v.emplace_back("count_needed", Value::DOUBLE(r.q->count));
	v.emplace_back("count_available", Value::DOUBLE(r.count_available));
	v.emplace_back("req_score", Value::DOUBLE(r.score));
	v.emplace_back("is_gap", Value::BOOLEAN(r.gap));
	return Value::STRUCT(std::move(v));
}

//! The trace rows of one requirement: one per required attribute (missing counterparts give
//! gated rows), or one row without attributes (HLD §11.9).
void AppendTrace(const ReqResult &r, vector<Value> &trace) {
	if (r.matched && !r.pair.attrs.empty()) {
		for (auto &m : r.pair.attrs) {
			trace.push_back(TraceRow(r, &m));
		}
	} else if (!r.matched && !r.q->attrs.empty()) {
		for (auto &x : r.q->attrs) {
			AttrMatch m;
			m.req = &x;
			m.bound = !x.bound.empty() ? x.bound : x.default_bound;
			m.rule = "gate";
			trace.push_back(TraceRow(r, &m));
		}
	} else {
		trace.push_back(TraceRow(r, nullptr));
	}
}

Value RunKernel(const Value &query, const Value &candidate, const Options &o) {
	auto Q = LoadTree(query);
	auto C = LoadTree(candidate);
	string assignment = o.assignment;
	if (assignment == "auto") {
		assignment = Q.reviewed && C.reviewed ? "partition" : "best_match"; // HLD §11.12: both trees reviewed
	}
	vector<ReqResult> results(Q.items.size());
	for (idx_t i = 0; i < Q.items.size(); i++) {
		results[i].q = &Q.items[i];
	}
	if (assignment == "partition") {
		auto blocks = PartitionBlocks(Q, C, o);
		for (idx_t i = 0; i < Q.items.size(); i++) {
			ScoreRequirement(results[i], blocks[i], o);
		}
	} else {
		vector<const KItem *> all;
		for (auto &c : C.items) {
			all.push_back(&c);
		}
		for (auto &r : results) {
			ScoreRequirement(r, all, o);
		}
	}
	// request-side OR groups: only the chosen alternatives are requirements (HLD §11.11)
	vector<idx_t> all_items(results.size());
	for (idx_t i = 0; i < results.size(); i++) {
		all_items[i] = i;
	}
	auto totals = AggregateAlternatives(results, all_items, 0);
	int64_t gaps = 0;
	vector<Value> trace;
	for (auto i : totals.chosen) {
		gaps += results[i].gap ? 1 : 0;
	}
	if (o.trace) {
		vector<bool> selected(results.size(), false);
		for (auto i : totals.chosen) {
			selected[i] = true;
		}
		for (idx_t i = 0; i < results.size(); i++) {
			if (selected[i]) {
				AppendTrace(results[i], trace);
			}
		}
	}
	child_list_t<Value> out;
	out.emplace_back("coverage", totals.n == 0 ? Value(LogicalType::DOUBLE) : Value::DOUBLE(totals.sum / totals.n));
	out.emplace_back("n_requirements", Value::BIGINT(int64_t(totals.n)));
	out.emplace_back("n_gaps", Value::BIGINT(gaps));
	out.emplace_back("reviewed", Value::BOOLEAN(C.reviewed));
	out.emplace_back("assignment_used", Value(assignment));
	out.emplace_back("trace", Value::LIST(TraceType(), std::move(trace)));
	return Value::STRUCT(std::move(out));
}

void CoverageKernelFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	CheckNodesType("coverage_kernel", "query", args.data[0].GetType());
	CheckNodesType("coverage_kernel", "candidate", args.data[1].GetType());
	CheckOptionsType("coverage_kernel", args.data[2].GetType());
	Options cached;
	bool options_constant = args.data[2].GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (options_constant) {
		cached = LoadOptions(args.data[2].GetValue(0));
	}
	for (idx_t i = 0; i < count; i++) {
		auto q = args.data[0].GetValue(i);
		auto c = args.data[1].GetValue(i);
		if (q.IsNull() || c.IsNull()) {
			result.SetValue(i, Value(KernelResultType()));
			continue;
		}
		auto o = options_constant ? cached : LoadOptions(args.data[2].GetValue(i));
		result.SetValue(i, RunKernel(q, c, o));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> CoverageKernelBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("coverage_kernel");
	return nullptr;
}

//------------------------------------------------------------------------------
// Table functions
//------------------------------------------------------------------------------




struct TreeCoverageParams {
	vector<string> query_ids;
	bool has_candidates = false;
	vector<string> candidates;
	int64_t k = 20;
	TreeSource q, c;
	string labels = "soft";
	string assignment = "auto";
	double label_weight = 0.0, mu = 0.2, partition_gate = 0.3, count_gate = 0.5, stage1_weight = 0.05, tie_break = 0.01;
	string recall_mode = "auto";
	string default_bounds;
	string templates_table = "catalogue_attributes";
	string rules_table;
	string name_vectors;
	double name_floor = 0.85;
	bool same_nodes_table = false; // query and candidate trees come from one table
	string fn;                     // the table function, for error messages
	string multiset_table; // precomputed candidate class multisets (tree_class_multiset output)
	bool multiset_has_reviewed = false;
};



//! The query and candidate trees: ids, tables, class sources (validated).
void ReadTreeSides(const CoverageParamReader &r, TableFunctionBindInput &input, TreeCoverageParams &p, bool stage2,
                   bool fit) {
	if (!fit) {
		if (input.inputs.empty() || input.inputs[0].IsNull()) {
			throw BinderException("%s: query_tree_id must not be NULL", r.fn);
		}
		p.query_ids = StringListParameter(input.inputs[0], "query_tree_id");
	}
	if (auto v = r.Get("candidates"); v && !v->IsNull()) {
		p.has_candidates = true;
		p.candidates = StringListParameter(*v, "candidates");
	}
	p.k = r.Int("k", p.k);
	if (p.k <= 0) {
		throw BinderException("%s: k must be positive", r.fn);
	}
	p.q.nodes_table = r.Str("nodes_table", "tree_nodes");
	if (p.q.nodes_table.empty()) {
		p.q.nodes_table = "tree_nodes";
	}
	p.q.attrs_table = r.Str("attrs_table", "tree_attributes");
	// the candidate side defaults to the query side's tables
	p.c.nodes_table = r.Str("candidate_nodes_table", p.q.nodes_table);
	if (p.c.nodes_table.empty()) {
		p.c.nodes_table = p.q.nodes_table;
	}
	p.c.attrs_table = r.Str("candidate_attrs_table", p.q.attrs_table);
	p.q.overrides_table = p.c.overrides_table = r.Str("overrides_table");
	p.q.label_table = p.c.label_table = r.Str("label_table");
	p.q.tree_ids = p.query_ids;
	p.q.has_tree_ids = !fit; // an empty list selects nothing; the fit takes its pairs from a table
	if (p.has_candidates) {
		p.c.tree_ids = p.candidates;
		p.c.has_tree_ids = true;
	}
	if (!stage2) {
		p.q.attrs_table.clear();
		p.c.attrs_table.clear();
	}
	ValidateTreeSource(r.context, p.q, r.fn);
	ValidateTreeSource(r.context, p.c, r.fn, "candidate_");
	if (stage2) {
		// an explicit nodes table without its attributes would silently read another table's values
		if (r.Get("nodes_table") && !r.Get("attrs_table") && p.q.nodes_table != "tree_nodes") {
			throw BinderException("%s: nodes_table '%s' is given but attrs_table is not; name the attributes table "
			                      "(attrs_table := NULL for none)",
			                      r.fn, p.q.nodes_table);
		}
		if (r.Get("candidate_nodes_table") && !r.Get("candidate_attrs_table") &&
		    p.c.nodes_table != p.q.nodes_table) {
			throw BinderException("%s: candidate_nodes_table '%s' is given but candidate_attrs_table is not; name the "
			                      "candidates' attributes table (candidate_attrs_table := NULL for none)",
			                      r.fn, p.c.nodes_table);
		}
	}
	p.same_nodes_table =
	    ResolvedRelationName(r.context, p.q.nodes_table) == ResolvedRelationName(r.context, p.c.nodes_table);
}

//! Kernel and stage-1 settings (validated as for the coverage_kernel scalar).
void ReadKernelSettings(const CoverageParamReader &r, TreeCoverageParams &p) {
	p.labels = StringUtil::Lower(r.Str("labels", p.labels));
	p.assignment = StringUtil::Lower(r.Str("assignment", p.assignment));
	if (p.labels.empty()) {
		p.labels = "soft";
	}
	if (p.assignment.empty()) {
		p.assignment = "auto";
	}
	p.label_weight = r.Dbl("label_weight", p.label_weight);
	p.mu = r.Dbl("mu", p.mu);
	p.partition_gate = r.Dbl("partition_gate", p.partition_gate);
	p.count_gate = r.Dbl("count_gate", p.count_gate);
	p.stage1_weight = r.Dbl("stage1_weight", p.stage1_weight);
	p.tie_break = r.Dbl("tie_break", p.tie_break);
	p.name_floor = r.Dbl("name_floor", p.name_floor);
	auto error = KernelSettingsError(p.labels, p.assignment, p.label_weight, p.mu, p.partition_gate, p.count_gate,
	                                 p.name_floor);
	if (!error.empty()) {
		throw BinderException("%s: %s", r.fn, error);
	}
	if (p.stage1_weight < 0 || p.tie_break < 0) {
		throw BinderException("%s: stage1_weight and tie_break must be non-negative", r.fn);
	}
	auto mode = r.Str("mode", r.Str("recall_mode", p.recall_mode));
	p.recall_mode = StringUtil::Lower(mode.empty() ? string("auto") : mode);
	if (p.recall_mode != "auto" && p.recall_mode != "containment" && p.recall_mode != "jaccard") {
		throw BinderException("%s: mode must be one of 'auto', 'containment', 'jaccard'", r.fn);
	}
}

//! The optional side tables: catalogue templates, compatibility rules, name vectors, the index.
void ReadSideTables(const CoverageParamReader &r, TreeCoverageParams &p) {
	p.default_bounds = StringUtil::Lower(r.Str("default_bounds"));
	if (!p.default_bounds.empty() && p.default_bounds != "catalogue") {
		throw BinderException("%s: default_bounds must be NULL or 'catalogue'", r.fn);
	}
	p.templates_table = r.Str("templates_table", p.templates_table);
	if (!p.default_bounds.empty()) {
		p.templates_table = ValidateSQLIdentifierPath(p.templates_table, "templates_table");
		if (!TableExists(r.context, p.templates_table)) {
			throw BinderException("%s: templates_table '%s' does not exist (default_bounds := 'catalogue' needs the "
			                      "catalogue attribute templates: class_id, dimension, bound)",
			                      r.fn, p.templates_table);
		}
		ValidateTableColumns(r.context, p.templates_table, {"class_id", "dimension", "bound"}, "templates_table");
	}
	p.rules_table = r.Table("rules_table", "", {"dimension", "required", "provided", "score"});
	p.name_vectors = r.Table("name_vectors", "", {"name", "vec"});
	p.multiset_table = r.Table("candidate_multiset_table", "", {"entity_id", "feature_id", "weight"});
	p.multiset_has_reviewed = !p.multiset_table.empty() && r.Has(p.multiset_table, "reviewed");
}

TreeCoverageParams ReadCoverageParams(ClientContext &context, TableFunctionBindInput &input, const string &fn,
                                  bool stage2, bool fit = false) {
	CoverageParamReader reader {context, input.named_parameters, fn};
	TreeCoverageParams p;
	p.fn = fn;
	ReadTreeSides(reader, input, p, stage2, fit);
	ReadKernelSettings(reader, p);
	ReadSideTables(reader, p);
	return p;
}

//! No silent empty results: an unknown query id, or a query whose items have no class, is an
//! error that says how to give classes.
string QueryGuardSQL(const TreeCoverageParams &p) {
	string unknown = "false";
	string first_unknown = "NULL";
	if (p.q.has_tree_ids && !p.query_ids.empty()) {
		auto ids = "(SELECT unnest(" + QuotedList(p.query_ids) + ") AS id)";
		unknown = "EXISTS (SELECT 1 FROM " + ids + " WHERE id NOT IN (SELECT tree_id FROM qn))";
		first_unknown = "(SELECT min(id) FROM " + ids + " WHERE id NOT IN (SELECT tree_id FROM qn))";
	}
	string unclassified = "SELECT tree_id FROM qn GROUP BY tree_id HAVING count(*) FILTER (WHERE node_kind = 'item') > 0 "
	                      "AND count(class_id) FILTER (WHERE node_kind = 'item') = 0";
	return StringUtil::Format(
	    "SELECT CASE WHEN %s THEN error(%s || ': query tree ''' || %s || ''' is not in nodes_table ' || %s) "
	    "WHEN EXISTS (%s) THEN error(%s || ': query tree ''' || (SELECT min(tree_id) FROM (%s)) || ''' has no "
	    "classified items (give class_id, overrides_table or label_table)') END AS g",
	    unknown, QuoteSQLStringLiteral(p.fn), first_unknown, QuoteSQLStringLiteral("'" + p.q.nodes_table + "'"),
	    unclassified, QuoteSQLStringLiteral(p.fn), unclassified);
}

//! Stage 1: query and candidate class multisets, the recall mode and the top candidates.
string Stage1SQL(const TreeCoverageParams &p) {
	// a query is not its own candidate when both come from the same table
	string self = !p.has_candidates && p.same_nodes_table ? " AND c.tree_id <> q.tree_id" : "";
	// explicit candidates are all scored; otherwise only candidates sharing a class can score > 0
	string pairs = p.has_candidates ? "SELECT q.tree_id AS query_id, c.tree_id AS candidate_id FROM q_ms q, c_ms c"
	                                : "SELECT query_id, candidate_id FROM ov";
	string top = p.has_candidates
	                 ? "SELECT * FROM s1s"
	                 : StringUtil::Format("SELECT * FROM s1s WHERE score > 0 QUALIFY row_number() OVER "
	                                      "(PARTITION BY query_id ORDER BY score DESC, candidate_id) <= %lld",
	                                      (long long)p.k);
	string mode = p.recall_mode == "auto"
	                  ? "(SELECT CASE WHEN bool_and(reviewed) THEN 'containment' ELSE 'jaccard' END FROM c_ms)"
	                  : QuoteSQLStringLiteral(p.recall_mode);
	// the candidate side: class multisets built from the resolved trees, or read from a
	// precomputed index (then candidate nodes are resolved for the top k only, in KernelSQL)
	string candidates;
	if (p.multiset_table.empty()) {
		candidates = StringUtil::Format(R"(
		cn AS MATERIALIZED (%s),
		c_tree AS MATERIALIZED (
			SELECT tree_id, COALESCE(bool_and(reviewed) FILTER (WHERE node_kind = 'item'), true) AS reviewed,
			       tree_multiset(list(struct_pack(tree_id, node_id, parent_id, node_kind, edge_type, class_id, count))) AS m
			FROM cn GROUP BY tree_id
		),
		c_ms AS (SELECT tree_id, reviewed FROM c_tree),
		c_f AS MATERIALIZED (SELECT tree_id, unnest(map_keys(m)) AS f, unnest(map_values(m)) AS w FROM c_tree),)",
		                                ResolvedNodesSQL(p.c));
	} else {
		string filter;
		if (p.c.has_tree_ids) {
			filter = p.c.tree_ids.empty() ? " AND false"
			                              : " AND CAST(entity_id AS VARCHAR) IN (SELECT unnest(" +
			                                    QuotedList(p.c.tree_ids) + "))";
		}
		candidates = StringUtil::Format(R"(
		c_f AS MATERIALIZED (
			SELECT CAST(entity_id AS VARCHAR) AS tree_id, CAST(feature_id AS VARCHAR) AS f, CAST(weight AS DOUBLE) AS w,
			       %s AS reviewed
			FROM %s WHERE feature_id IS NOT NULL AND weight > 0%s
		),
		c_ms AS MATERIALIZED (SELECT tree_id, bool_and(reviewed) AS reviewed FROM c_f GROUP BY tree_id),)",
		                                p.multiset_has_reviewed ? "COALESCE(CAST(reviewed AS BOOLEAN), false)" : "false",
		                                QuotedIdentifier(p.multiset_table), filter);
	}
	return StringUtil::Format(R"(
		qn AS MATERIALIZED (%s),%s
		q_ms AS (
			SELECT tree_id, tree_multiset(list(struct_pack(tree_id, node_id, parent_id, node_kind, edge_type, class_id, count))) AS m
			FROM qn GROUP BY tree_id
		),
		mode AS (SELECT %s AS mode_used),
		q_f AS (SELECT tree_id, unnest(map_keys(m)) AS f, unnest(map_values(m)) AS w FROM q_ms),
		q_tot AS (SELECT tree_id, sum(w) AS tot FROM q_f GROUP BY tree_id),
		c_tot AS (SELECT tree_id, sum(w) AS tot FROM c_f GROUP BY tree_id),
		ov AS (
			SELECT q.tree_id AS query_id, c.tree_id AS candidate_id, sum(least(q.w, c.w)) AS inter
			FROM q_f q JOIN c_f c ON c.f = q.f%s
			GROUP BY ALL
		),
		pairs AS (%s),
		s1 AS (
			SELECT pr.query_id, pr.candidate_id, c.reviewed,
			       COALESCE(%s, 0) AS containment,
			       COALESCE(%s, 0) AS jaccard
			FROM pairs pr JOIN c_ms c ON c.tree_id = pr.candidate_id
			LEFT JOIN ov ON ov.query_id = pr.query_id AND ov.candidate_id = pr.candidate_id
			LEFT JOIN q_tot qt ON qt.tree_id = pr.query_id LEFT JOIN c_tot ct ON ct.tree_id = pr.candidate_id
		),
		s1s AS (
			SELECT s1.*, mode.mode_used,
			       CASE WHEN mode.mode_used = 'containment' THEN %s ELSE jaccard END AS score
			FROM s1, mode
		),
		q_guard AS (%s),
		top AS MATERIALIZED (SELECT t.* FROM (%s) t, q_guard WHERE q_guard.g IS NULL)
	)",
	                          ResolvedNodesSQL(p.q), candidates, mode, self, pairs, ContainmentSQL("ov.inter", "qt.tot"),
	                          JaccardSQL("ov.inter", "qt.tot", "ct.tot"),
	                          TieBrokenScoreSQL(true, "containment", "jaccard", p.tie_break), QueryGuardSQL(p), top);
}

//! Attributes of one side, packed per node, with default bounds and name vectors.
string AttrsSQL(ClientContext &context, const TreeCoverageParams &p, const TreeSource &src, const string &nodes_cte,
                bool query_side) {
	if (src.attrs_table.empty()) {
		return "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL AS attrs WHERE false";
	}
	auto has = [&](const char *col) { return TableHasColumn(context, src.attrs_table, col); };
	string base = StringUtil::Format(
	    "SELECT CAST(a.tree_id AS VARCHAR) AS tree_id, CAST(a.node_id AS VARCHAR) AS node_id, "
	    "CAST(a.attr_id AS VARCHAR) AS attr_id, %s AS name, %s AS dimension, %s AS unit, %s AS lo, %s AS hi, "
	    "%s AS bound, %s AS category FROM %s a WHERE CAST(a.tree_id AS VARCHAR) IN (SELECT tree_id FROM %s)",
	    CastOrNullSQL(has("name"), "name", "VARCHAR", "a"),
	    CastOrNullSQL(has("dimension"), "dimension", "VARCHAR", "a"),
	    CastOrNullSQL(has("unit"), "unit", "VARCHAR", "a"), CastOrNullSQL(has("lo"), "lo", "DOUBLE", "a"),
	    CastOrNullSQL(has("hi"), "hi", "DOUBLE", "a"), CastOrNullSQL(has("bound"), "bound", "VARCHAR", "a"),
	    CastOrNullSQL(has("category"), "category", "VARCHAR", "a"), QuotedIdentifier(src.attrs_table),
	    query_side ? "q_ids" : "c_ids");
	string default_bound = "NULL::VARCHAR";
	string templates_join;
	if (query_side && !p.default_bounds.empty()) {
		templates_join = StringUtil::Format(
		    " LEFT JOIN (SELECT CAST(class_id AS VARCHAR) AS class_id, CAST(dimension AS VARCHAR) AS dimension, "
		    "CASE WHEN count(DISTINCT CAST(bound AS VARCHAR)) = 1 THEN any_value(CAST(bound AS VARCHAR)) END AS bound "
		    "FROM %s GROUP BY ALL) t ON t.class_id = n.class_id AND t.dimension = x.dimension",
		    QuotedIdentifier(p.templates_table));
		default_bound = "t.bound";
	}
	string vec = "NULL::DOUBLE[]";
	string vec_join;
	if (!p.name_vectors.empty()) {
		vec_join = " LEFT JOIN (" + NameVectorsSQL(p.name_vectors) + ") v ON v.name = x.name";
		vec = "v.vec";
	}
	return StringUtil::Format(
	    "SELECT x.tree_id, x.node_id, list(struct_pack(attr_id := x.attr_id, name := x.name, dimension := x.dimension, "
	    "unit := x.unit, lo := x.lo, hi := x.hi, bound := x.bound, category := x.category, default_bound := %s, "
	    "name_vec := %s) ORDER BY x.attr_id) AS attrs "
	    "FROM (%s) x JOIN %s n ON n.tree_id = x.tree_id AND n.node_id = x.node_id%s%s GROUP BY x.tree_id, x.node_id",
	    default_bound, vec, base, nodes_cte, templates_join, vec_join);
}

string LabelsSQL(const TreeSource &src, const string &ids_cte) {
	if (src.label_table.empty()) {
		return "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::MAP(VARCHAR, DOUBLE) AS probs WHERE false";
	}
	// duplicate rows add up; NULL classes and non-positive or non-finite probabilities drop out
	return StringUtil::Format(
	    "SELECT tree_id, node_id, map_from_entries(list((class_id, p))) AS probs FROM ("
	    "SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id, "
	    "CAST(class_id AS VARCHAR) AS class_id, sum(CAST(p AS DOUBLE)) AS p FROM %s "
	    "WHERE CAST(tree_id AS VARCHAR) IN (SELECT tree_id FROM %s) AND class_id IS NOT NULL "
	    "AND isfinite(CAST(p AS DOUBLE)) AND CAST(p AS DOUBLE) > 0 GROUP BY ALL) GROUP BY tree_id, node_id",
	    QuotedIdentifier(src.label_table), ids_cte);
}

string PackedSQL(const string &nodes_cte, const string &attrs_cte, const string &labels_cte, const string &ids_cte) {
	return StringUtil::Format(
	    "SELECT n.tree_id, list(struct_pack(node_id := n.node_id, parent_id := n.parent_id, node_kind := n.node_kind, "
	    "edge_type := n.edge_type, class_id := n.class_id, reviewed := n.reviewed, count := n.count, "
	    "probs := CASE WHEN n.class_origin IN ('class_id', 'vocabulary') THEN MAP {n.class_id: 1.0::DOUBLE} "
	    "              WHEN n.class_origin = 'label' THEN l.probs END, "
	    "attrs := a.attrs)) AS nodes "
	    "FROM %s n LEFT JOIN %s a USING (tree_id, node_id) LEFT JOIN %s l USING (tree_id, node_id) "
	    "WHERE n.tree_id IN (SELECT tree_id FROM %s) GROUP BY n.tree_id",
	    nodes_cte, attrs_cte, labels_cte, ids_cte);
}

//! The packed trees of both sides, as CTEs over qn/cn and q_ids/c_ids: attributes (qa, ca),
//! soft labels (ql, cl) and the node lists the kernel reads (qp, cp).
string PackedSidesSQL(ClientContext &context, const TreeCoverageParams &p) {
	return StringUtil::Format("qa AS (%s),\n\t\tca AS (%s),\n\t\tql AS (%s),\n\t\tcl AS (%s),\n\t\tqp AS (%s),\n\t\tcp AS (%s),",
	                          AttrsSQL(context, p, p.q, "qn", true), AttrsSQL(context, p, p.c, "cn", false),
	                          LabelsSQL(p.q, "q_ids"), LabelsSQL(p.c, "c_ids"), PackedSQL("qn", "qa", "ql", "q_ids"),
	                          PackedSQL("cn", "ca", "cl", "c_ids"));
}

//! The kernel's options STRUCT; label_weight and mu as SQL expressions (literals or grid columns).
string OptionsSQL(const TreeCoverageParams &p, bool trace, const string &label_weight, const string &mu) {
	string rules = p.rules_table.empty()
	                   ? "NULL"
	                   : StringUtil::Format("(SELECT list(struct_pack(dimension := CAST(dimension AS VARCHAR), required "
	                                        ":= CAST(required AS VARCHAR), provided := CAST(provided AS VARCHAR), score "
	                                        ":= CAST(score AS DOUBLE))) FROM %s)",
	                                        QuotedIdentifier(p.rules_table));
	return StringUtil::Format(
	    "struct_pack(labels := %s, assignment := %s, label_weight := %s, mu := %s, partition_gate := %s, count_gate := %s, "
	    "name_floor := %s, trace := %s, rules := %s)",
	    QuoteSQLStringLiteral(p.labels), QuoteSQLStringLiteral(p.assignment), label_weight, mu, DoubleLiteral(p.partition_gate),
	    DoubleLiteral(p.count_gate), DoubleLiteral(p.name_floor), trace ? "true" : "false", rules);
}

string KernelSQL(ClientContext &context, const TreeCoverageParams &p, bool trace) {
	// with an index, stage 1 did not resolve the candidate trees: resolve the top k only
	string candidate_nodes =
	    p.multiset_table.empty()
	        ? ""
	        : StringUtil::Format("\n\t\tcn AS MATERIALIZED (SELECT * FROM (%s) WHERE tree_id IN (SELECT tree_id FROM c_ids)),",
	                             ResolvedNodesSQL(p.c));
	string options = OptionsSQL(p, trace, DoubleLiteral(p.label_weight), DoubleLiteral(p.mu));
	return StringUtil::Format(R"(
		WITH %s,
		q_ids AS (SELECT DISTINCT query_id AS tree_id FROM top),
		c_ids AS (SELECT DISTINCT candidate_id AS tree_id FROM top),%s
		%s
		k AS (
			SELECT t.query_id, t.candidate_id, t.score AS stage1_score,
			       coverage_kernel(qp.nodes, cp.nodes, %s) AS r
			FROM top t JOIN qp ON qp.tree_id = t.query_id JOIN cp ON cp.tree_id = t.candidate_id
		),
		ranked AS (
			SELECT query_id, candidate_id, r.coverage AS coverage, stage1_score,
			       COALESCE(r.coverage, 0) + %s * stage1_score AS rank_score,
			       r.n_requirements AS n_requirements, r.n_gaps AS n_gaps, r.reviewed AS reviewed,
			       r.assignment_used AS assignment_used, r.trace AS trace
			FROM k
		)
	)",
	                          Stage1SQL(p), candidate_nodes, PackedSidesSQL(context, p), options,
	                          DoubleLiteral(p.stage1_weight));
}

unique_ptr<TableRef> TreeCoverageBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("tree_coverage");
	auto p = ReadCoverageParams(context, input, "tree_coverage", true);
	auto sql = KernelSQL(context, p, false) + R"(
		SELECT query_id, candidate_id, coverage, stage1_score, rank_score,
		       row_number() OVER (PARTITION BY query_id ORDER BY rank_score DESC, candidate_id) AS rank,
		       n_requirements, n_gaps, reviewed, assignment_used
		FROM ranked
		ORDER BY query_id, rank
	)";
	return CoverageSubquery(sql, context, "tree_coverage");
}

unique_ptr<TableRef> TreeCoverageTraceBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("tree_coverage_trace");
	auto p = ReadCoverageParams(context, input, "tree_coverage_trace", true);
	auto sql = KernelSQL(context, p, true) + R"(
		SELECT query_id, candidate_id, t.req_node_id, t.matched_node_id, t.matched_path, t.label_sim, t.attr_id,
		       t.attr_name, t.req_value, t.prov_value, t.gate, t.name_sim, t.value_score, t.distance, t.rule,
		       t.attr_score, t.count_needed, t.count_available, t.req_score, t.is_gap
		FROM (SELECT query_id, candidate_id, unnest(trace) AS t FROM ranked)
		ORDER BY query_id, candidate_id, t.req_node_id, t.attr_id
	)";
	return CoverageSubquery(sql, context, "tree_coverage_trace");
}

constexpr idx_t MAX_GRID_VALUES = 20;

//! fit_coverage_weights: the kernel over reviewed pairs for every grid point (HLD §4.8, BR-52).
unique_ptr<TableRef> FitWeightsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("fit_coverage_weights");
	const string fn = "fit_coverage_weights";
	CoverageParamReader reader {context, input.named_parameters, fn};
	auto reviewed = reader.Positional(input, 0, "reviewed_matches", {"query_id", "candidate_id", "true_coverage"});
	auto p = ReadCoverageParams(context, input, fn, true, true);
	auto grid = [&](const char *name, vector<double> fallback) {
		vector<double> out = std::move(fallback);
		if (auto v = reader.Get(name); v && !v->IsNull()) {
			out.clear();
			for (auto &x : ListValue::GetChildren(*v)) {
				double d = x.IsNull() ? -1 : x.GetValue<double>();
				if (!(d >= 0 && d <= 1)) {
					throw BinderException("%s: %s must be between 0 and 1", fn, name);
				}
				out.push_back(d);
			}
		}
		if (out.empty()) {
			throw BinderException("%s: %s must not be empty", fn, name);
		}
		if (out.size() > MAX_GRID_VALUES) {
			throw BinderException("%s: at most 20 values per grid axis", fn);
		}
		string list;
		for (auto d : out) {
			list += (list.empty() ? "" : ", ") + DoubleLiteral(d);
		}
		return "[" + list + "]";
	};
	auto label_weights = grid("label_weights", {0.0, 0.1, 0.2, 0.3, 0.5});
	auto mus = grid("mus", {0.0, 0.1, 0.2, 0.3});
	auto sql = StringUtil::Format(R"(
		WITH top AS (
			SELECT DISTINCT CAST(query_id AS VARCHAR) AS query_id, CAST(candidate_id AS VARCHAR) AS candidate_id,
			       CAST(true_coverage AS DOUBLE) AS true_coverage
			FROM %s WHERE true_coverage IS NOT NULL
		),
		q_ids AS (SELECT DISTINCT query_id AS tree_id FROM top),
		c_ids AS (SELECT DISTINCT candidate_id AS tree_id FROM top),
		qn AS MATERIALIZED (SELECT * FROM (%s) WHERE tree_id IN (SELECT tree_id FROM q_ids)),
		cn AS MATERIALIZED (SELECT * FROM (%s) WHERE tree_id IN (SELECT tree_id FROM c_ids)),
		%s
		grid AS (SELECT lw, mu FROM (SELECT unnest(%s) AS lw) CROSS JOIN (SELECT unnest(%s) AS mu)),
		k AS (
			SELECT g.lw, g.mu, t.true_coverage,
			       coverage_kernel(qp.nodes, cp.nodes, %s).coverage AS coverage
			FROM top t JOIN qp ON qp.tree_id = t.query_id JOIN cp ON cp.tree_id = t.candidate_id CROSS JOIN grid g
		)
		SELECT lw AS label_weight, mu, avg(abs(COALESCE(coverage, 0) - true_coverage)) AS mae, count(*) AS n_pairs,
		       (SELECT count(*) FROM top) - count(*) AS n_unresolved,
		       row_number() OVER (ORDER BY avg(abs(COALESCE(coverage, 0) - true_coverage)), lw, mu) AS rank
		FROM k GROUP BY lw, mu
		ORDER BY rank
	)",
	                              QuotedIdentifier(reviewed), ResolvedNodesSQL(p.q), ResolvedNodesSQL(p.c),
	                              PackedSidesSQL(context, p), label_weights, mus,
	                              OptionsSQL(p, false, "g.lw", "g.mu"));
	return CoverageSubquery(sql, context, fn);
}

unique_ptr<TableRef> CoverageRecallBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("coverage_recall");
	auto p = ReadCoverageParams(context, input, "coverage_recall", false);
	auto sql = "WITH " + Stage1SQL(p) + R"(
		SELECT query_id, candidate_id, score, containment, jaccard, reviewed, mode_used,
		       row_number() OVER (PARTITION BY query_id ORDER BY score DESC, candidate_id) AS rank
		FROM top
		ORDER BY query_id, rank
	)";
	return CoverageSubquery(sql, context, "coverage_recall");
}

using NamedParameterList = vector<std::pair<string, LogicalType>>;

//! The parameters of stage 1 (all three coverage functions); order as documented.
NamedParameterList Stage1Parameters(bool stage2) {
	auto vc = LogicalType(LogicalTypeId::VARCHAR);
	return {{"candidates", LogicalType::LIST(LogicalType::VARCHAR)},
	        {"k", LogicalType::BIGINT},
	        {"nodes_table", vc},
	        {"candidate_nodes_table", vc},
	        {"overrides_table", vc},
	        {"label_table", vc},
	        {stage2 ? "recall_mode" : "mode", vc},
	        {"tie_break", LogicalType::DOUBLE},
	        {"candidate_multiset_table", vc}};
}

//! The parameters of stage 2 (tree_coverage, tree_coverage_trace).
NamedParameterList KernelParameters() {
	auto vc = LogicalType(LogicalTypeId::VARCHAR);
	auto dbl = LogicalType(LogicalTypeId::DOUBLE);
	return {{"attrs_table", vc},     {"candidate_attrs_table", vc}, {"labels", vc},         {"assignment", vc},
	        {"label_weight", dbl},   {"mu", dbl},                   {"partition_gate", dbl}, {"count_gate", dbl},
	        {"stage1_weight", dbl},  {"default_bounds", vc},        {"templates_table", vc}, {"rules_table", vc},
	        {"name_vectors", vc},    {"name_floor", dbl}};
}

void RegisterCoverageTable(ExtensionLoader &loader, const string &name, table_function_bind_replace_t fn,
                           const string &description, vector<string> examples, bool stage2) {
	auto named = Stage1Parameters(stage2);
	if (stage2) {
		for (auto &spec : KernelParameters()) {
			named.push_back(spec);
		}
	}
	RegisterCoverageTableFunction(loader, name, {LogicalType::ANY}, {"query_tree_id"}, fn, std::move(named), description,
	                              std::move(examples), {"coverage", "search"});
}

} // namespace

void RegisterCoverageKernelFunctions(ExtensionLoader &loader) {
	{
		ScalarFunction f(duckdb_compat::FunctionName("coverage_kernel"), {LogicalType::ANY, LogicalType::ANY, LogicalType::ANY},
		                 KernelResultType(), DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, CoverageKernelFun),
		                 compat_anofox::ScalarBind<&CoverageKernelBind>());
		duckdb_compat::SetFallible(f);
		f.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING); // NULL options mean the defaults
		CreateScalarFunctionInfo info(f);
		FunctionDescription d;
		d.description =
		    "The stage-2 coverage kernel over two packed trees (LIST of STRUCT(node_id, parent_id, node_kind, edge_type, "
		    "class_id, reviewed, count, probs MAP, attrs LIST(STRUCT(attr_id, name, dimension, unit, lo, hi, bound, "
		    "category, default_bound, name_vec)))) with an options STRUCT (labels, assignment, label_weight, mu, "
		    "partition_gate, count_gate, name_floor, trace, rules). Returns STRUCT(coverage, n_requirements, n_gaps, "
		    "reviewed, assignment_used, trace). tree_coverage and tree_coverage_trace build its inputs; use it "
		    "directly for custom pipelines.";
		d.examples = {"SELECT coverage_kernel(q.nodes, c.nodes, struct_pack(labels := 'hard')).coverage FROM q, c;"};
		d.categories = {"coverage"};
		d.parameter_names = {"query", "candidate", "options"};
		d.parameter_types = {LogicalType::ANY, LogicalType::ANY, LogicalType::ANY};
		info.descriptions.push_back(std::move(d));
		duckdb_compat::NameArgumentsFromDescriptions(info);
		loader.RegisterFunction(std::move(info));
	}
	const string common =
	    " query_tree_id is a tree id or a LIST of ids. Trees come from nodes_table/attrs_table (default "
	    "'tree_nodes'/'tree_attributes'); candidates from candidate_nodes_table/candidate_attrs_table (default: the "
	    "same tables). Naming a nodes table without its attributes table is an error (pass NULL for none). Classes resolve class_id -> overrides_table (reviewed vocabulary) -> label_table (classifier). "
	    "Stage 1 ranks candidates by containment (+ tie_break x jaccard) when every candidate tree is reviewed, else "
	    "by weighted jaccard (mode 'auto'). candidate_multiset_table: a precomputed index of the candidate class "
	    "multisets (tree_class_multiset output: entity_id, feature_id, weight[, reviewed]) makes stage 1 skip "
	    "resolving every candidate tree.";
	RegisterCoverageTable(
	    loader, "tree_coverage", TreeCoverageBindReplace,
	    "Stage-2 coverage: ranks candidate trees by the share of the request they cover. Per request "
	    "item q and candidate item c: score = label_weight*L + (1-label_weight)*L*a, with L the label agreement (Bhattacharyya of "
	    "soft labels, or 0/1) and a the mean over q's attributes of the best (mu*name_sim + 1-mu)*value_satisfies; "
	    "counts are summed over compatible items (never across OR alternatives). assignment 'partition' gives each "
	    "candidate item one requirement block (no substitution), 'best_match' lets requirements share, 'auto' uses "
	    "partition when both the query and the candidate trees are reviewed. Returns query_id, candidate_id, coverage, stage1_score, rank_score "
	    "(coverage + stage1_weight x stage1_score), rank, n_requirements, n_gaps, reviewed, assignment_used. "
	    "default_bounds := 'catalogue' gives unqualified values the bound of their template (templates_table)." +
	        common,
	    {"SELECT * FROM tree_coverage('R01', k := 20, candidate_nodes_table := 'programme_nodes', "
	     "candidate_attrs_table := 'programme_attributes');",
	     "SELECT * FROM tree_coverage(['R01', 'R02'], candidates := ['P001', 'P002']);"},
	    true);
	RegisterCoverageTable(
	    loader, "tree_coverage_trace", TreeCoverageTraceBindReplace,
	    "The explanation of tree_coverage: one row per request item and required attribute with the matched "
	    "candidate item and path, label_sim, req_value, prov_value, gate, name_sim, value_score, distance, rule, "
	    "attr_score, count_needed, count_available, req_score and is_gap (no counterpart, a count shortfall or a "
	    "value below 1). Same parameters as tree_coverage." +
	        common,
	    {"SELECT * FROM tree_coverage_trace('R01', candidates := ['P001']) WHERE is_gap;"}, true);
	RegisterCoverageTable(
	    loader, "coverage_recall", CoverageRecallBindReplace,
	    "Stage-1 recall over class multisets: the top-k candidate trees per query with score, "
	    "containment, jaccard, reviewed, mode_used and rank. mode 'auto' uses containment when every candidate tree "
	    "is reviewed and weighted jaccard otherwise." +
	        common,
	    {"SELECT * FROM coverage_recall('R01', k := 20, candidate_nodes_table := 'programme_nodes');"}, false);
	{
		auto vc = LogicalType(LogicalTypeId::VARCHAR);
		auto dbl = LogicalType::DOUBLE;
		RegisterCoverageTableFunction(
		    loader, "fit_coverage_weights", {vc}, {"reviewed_matches"}, FitWeightsBindReplace,
		    {{"label_weights", LogicalType::LIST(dbl)},
		     {"mus", LogicalType::LIST(dbl)},
		     {"nodes_table", vc},
		     {"attrs_table", vc},
		     {"candidate_nodes_table", vc},
		     {"candidate_attrs_table", vc},
		     {"overrides_table", vc},
		     {"label_table", vc},
		     {"labels", vc},
		     {"assignment", vc},
		     {"partition_gate", dbl},
		     {"count_gate", dbl},
		     {"default_bounds", vc},
		     {"templates_table", vc},
		     {"rules_table", vc},
		     {"name_vectors", vc},
		     {"name_floor", dbl}},
		    "Fits the kernel weights on reviewed matches: for every grid point of label_weights "
		    "(lambda, default 0, 0.1, 0.2, 0.3, 0.5) x mus (default 0, 0.1, 0.2, 0.3), the mean absolute error of "
		    "the kernel's coverage against reviewed_matches (query_id, candidate_id, true_coverage), with n_pairs, "
		    "n_unresolved (reviewed pairs whose trees are missing) and rank (best first). At most 20 values per axis. The trees and options are given as for tree_coverage. The classifier's beta "
		    "and tau are tuned with classify_to_catalogue and eval_ranking instead.",
		    {"SELECT * FROM fit_coverage_weights('reviewed_matches', candidate_nodes_table := 'programme_nodes', "
		     "candidate_attrs_table := 'programme_attributes') LIMIT 3;"},
		    {"coverage", "evaluation"});
	}
}

} // namespace anofox
} // namespace duckdb
