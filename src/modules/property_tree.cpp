#include "modules/property_tree.hpp"
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

#include <cmath>
#include <functional>
#include <map>
#include <unordered_set>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// A small JSON reader: enough for extracted trees, no dependency on the json extension
//------------------------------------------------------------------------------

namespace {

struct Json {
	enum class Kind { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT } kind = Kind::NUL;
	bool boolean = false;
	double number = 0;
	string text;
	vector<Json> items;
	vector<std::pair<string, Json>> fields;

	const Json *Get(const string &key) const {
		for (auto &f : fields) {
			if (f.first == key) {
				return &f.second;
			}
		}
		return nullptr;
	}
	bool IsNull() const {
		return kind == Kind::NUL;
	}
};

class JsonReader {
public:
	explicit JsonReader(const string &input) : s(input) {
	}
	Json Parse() {
		auto v = Value();
		Space();
		if (pos != s.size()) {
			Fail("unexpected text after the document");
		}
		return v;
	}

private:
	const string &s;
	idx_t pos = 0;

	[[noreturn]] void Fail(const string &what) {
		throw InvalidInputException("tree_from_json: invalid JSON at position %llu: %s", (unsigned long long)pos, what);
	}
	void Space() {
		while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\n' || s[pos] == '\r' || s[pos] == '\t')) {
			pos++;
		}
	}
	bool Consume(const char *literal) {
		idx_t n = strlen(literal);
		if (s.compare(pos, n, literal) == 0) {
			pos += n;
			return true;
		}
		return false;
	}
	Json Value() {
		Space();
		if (pos >= s.size()) {
			Fail("unexpected end of input");
		}
		Json v;
		char c = s[pos];
		if (c == '{') {
			pos++;
			v.kind = Json::Kind::OBJECT;
			Space();
			if (pos < s.size() && s[pos] == '}') {
				pos++;
				return v;
			}
			while (true) {
				Space();
				if (pos >= s.size() || s[pos] != '"') {
					Fail("expected a field name");
				}
				auto key = String();
				Space();
				if (pos >= s.size() || s[pos] != ':') {
					Fail("expected ':'");
				}
				pos++;
				v.fields.emplace_back(key, Value());
				Space();
				if (pos < s.size() && s[pos] == ',') {
					pos++;
					continue;
				}
				if (pos < s.size() && s[pos] == '}') {
					pos++;
					return v;
				}
				Fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			pos++;
			v.kind = Json::Kind::ARRAY;
			Space();
			if (pos < s.size() && s[pos] == ']') {
				pos++;
				return v;
			}
			while (true) {
				v.items.push_back(Value());
				Space();
				if (pos < s.size() && s[pos] == ',') {
					pos++;
					continue;
				}
				if (pos < s.size() && s[pos] == ']') {
					pos++;
					return v;
				}
				Fail("expected ',' or ']'");
			}
		}
		if (c == '"') {
			v.kind = Json::Kind::STRING;
			v.text = String();
			return v;
		}
		if (Consume("true")) {
			v.kind = Json::Kind::BOOL;
			v.boolean = true;
			return v;
		}
		if (Consume("false")) {
			v.kind = Json::Kind::BOOL;
			return v;
		}
		if (Consume("null")) {
			return v;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			idx_t start = pos;
			pos++;
			while (pos < s.size() && (isdigit((unsigned char)s[pos]) || s[pos] == '.' || s[pos] == 'e' ||
			                          s[pos] == 'E' || s[pos] == '+' || s[pos] == '-')) {
				pos++;
			}
			v.kind = Json::Kind::NUMBER;
			v.number = std::strtod(s.substr(start, pos - start).c_str(), nullptr);
			return v;
		}
		Fail(string("unexpected character '") + c + "'");
	}
	static void AppendUtf8(string &out, uint32_t cp) {
		if (cp < 0x80) {
			out += char(cp);
		} else if (cp < 0x800) {
			out += char(0xC0 | (cp >> 6));
			out += char(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += char(0xE0 | (cp >> 12));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		} else {
			out += char(0xF0 | (cp >> 18));
			out += char(0x80 | ((cp >> 12) & 0x3F));
			out += char(0x80 | ((cp >> 6) & 0x3F));
			out += char(0x80 | (cp & 0x3F));
		}
	}
	uint32_t Hex4() {
		if (pos + 4 > s.size()) {
			Fail("truncated \\u escape");
		}
		uint32_t cp = 0;
		for (int i = 0; i < 4; i++) {
			char h = s[pos++];
			cp <<= 4;
			if (h >= '0' && h <= '9') {
				cp |= uint32_t(h - '0');
			} else if (h >= 'a' && h <= 'f') {
				cp |= uint32_t(h - 'a' + 10);
			} else if (h >= 'A' && h <= 'F') {
				cp |= uint32_t(h - 'A' + 10);
			} else {
				Fail("bad \\u escape");
			}
		}
		return cp;
	}
	string String() {
		pos++; // opening quote
		string out;
		while (pos < s.size()) {
			char c = s[pos++];
			if (c == '"') {
				return out;
			}
			if (c != '\\') {
				out += c;
				continue;
			}
			if (pos >= s.size()) {
				break;
			}
			char e = s[pos++];
			switch (e) {
			case '"':
			case '\\':
			case '/':
				out += e;
				break;
			case 'b':
				out += '\b';
				break;
			case 'f':
				out += '\f';
				break;
			case 'n':
				out += '\n';
				break;
			case 'r':
				out += '\r';
				break;
			case 't':
				out += '\t';
				break;
			case 'u': {
				uint32_t cp = Hex4();
				if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
					pos += 2;
					uint32_t low = Hex4();
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				}
				AppendUtf8(out, cp);
				break;
			}
			default:
				Fail("bad escape");
			}
		}
		Fail("unterminated string");
	}
};

string JsonString(const Json *v) {
	if (!v || v->IsNull()) {
		return string();
	}
	if (v->kind == Json::Kind::STRING) {
		return v->text;
	}
	if (v->kind == Json::Kind::NUMBER) {
		auto n = v->number;
		return n == std::floor(n) && std::fabs(n) < 1e15 ? std::to_string((long long)n) : StringUtil::Format("%g", n);
	}
	if (v->kind == Json::Kind::BOOL) {
		return v->boolean ? "true" : "false";
	}
	return string();
}

bool JsonNumber(const Json *v, double &out) {
	if (!v || v->IsNull()) {
		return false;
	}
	if (v->kind == Json::Kind::NUMBER) {
		out = v->number;
		return true;
	}
	if (v->kind == Json::Kind::STRING) {
		char *end = nullptr;
		out = std::strtod(v->text.c_str(), &end);
		return end && *end == '\0' && !v->text.empty();
	}
	return false;
}

Value OptVarchar(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

LogicalType AttributeType() {
	child_list_t<LogicalType> c {{"tree_id", LogicalType::VARCHAR},  {"node_id", LogicalType::VARCHAR},
	                             {"attr_id", LogicalType::VARCHAR},  {"name", LogicalType::VARCHAR},
	                             {"value_text", LogicalType::VARCHAR}, {"dimension", LogicalType::VARCHAR},
	                             {"unit", LogicalType::VARCHAR},     {"lo", LogicalType::DOUBLE},
	                             {"hi", LogicalType::DOUBLE},        {"role", LogicalType::VARCHAR},
	                             {"bound", LogicalType::VARCHAR},    {"qualifier", LogicalType::VARCHAR},
	                             {"category", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(c);
}

LogicalType NodeType() {
	child_list_t<LogicalType> c {{"tree_id", LogicalType::VARCHAR},   {"node_id", LogicalType::VARCHAR},
	                             {"parent_id", LogicalType::VARCHAR}, {"node_kind", LogicalType::VARCHAR},
	                             {"edge_type", LogicalType::VARCHAR}, {"label", LogicalType::VARCHAR},
	                             {"context", LogicalType::VARCHAR},   {"class_id", LogicalType::VARCHAR},
	                             {"count", LogicalType::DOUBLE},      {"source", LogicalType::VARCHAR},
	                             {"attributes", LogicalType::LIST(AttributeType())}};
	return LogicalType::STRUCT(c);
}

struct JsonTreeContext {
	string tree_id;
	string source;
	string role;
	vector<string> langs;
	const UnitTable *units = nullptr;
	vector<Value> rows;
	std::unordered_set<string> seen;
};

Value AttributeFromJson(const Json &a, idx_t index, const string &node_id, const JsonTreeContext &ctx) {
	auto &units = *ctx.units;
	string attr_id = JsonString(a.Get("attr_id"));
	if (attr_id.empty()) {
		attr_id = "a" + std::to_string(index + 1);
	}
	string text = JsonString(a.Get("text"));
	if (text.empty()) {
		text = JsonString(a.Get("value_text"));
	}
	string unit_written = JsonString(a.Get("unit"));
	string category = JsonString(a.Get("category"));
	string role = JsonString(a.Get("role"));
	if (role.empty()) {
		role = ctx.role;
	}
	auto bound_field = a.Get("bound");
	string bound = JsonString(bound_field);
	double lo = 0, hi = 0;
	bool has_lo = JsonNumber(a.Get("lo"), lo);
	bool has_hi = JsonNumber(a.Get("hi"), hi);
	string dimension, unit;
	bool has_value = false;
	if (has_lo || has_hi) {
		if (!has_lo) {
			lo = hi;
		}
		if (!has_hi) {
			hi = lo;
		}
		has_value = true;
		auto def = unit_written.empty() ? nullptr : units.Find(unit_written);
		if (def) {
			dimension = def->dimension;
			if (std::isnan(def->factor)) {
				unit = def->symbol;
			} else {
				lo = lo * def->factor + def->offset;
				hi = hi * def->factor + def->offset;
				unit = units.BaseUnit(def->dimension);
			}
		} else if (unit_written.empty()) {
			dimension = "count";
			unit = "1";
		} else {
			unit = unit_written; // unknown: never guessed
		}
	} else if (category.empty() && !text.empty()) {
		auto q = ParseQuantity(text, ctx.langs, units);
		if (q.has_value) {
			has_value = true;
			lo = q.lo;
			hi = q.hi;
			dimension = q.dimension;
			unit = q.unit;
		}
		category = q.category;
		if (!bound_field) {
			bound = q.bound; // the text states the bound when the extraction gave none
		}
	}
	child_list_t<Value> v;
	v.emplace_back("tree_id", Value(ctx.tree_id));
	v.emplace_back("node_id", Value(node_id));
	v.emplace_back("attr_id", Value(attr_id));
	v.emplace_back("name", OptVarchar(JsonString(a.Get("name"))));
	v.emplace_back("value_text", OptVarchar(text));
	v.emplace_back("dimension", OptVarchar(dimension));
	v.emplace_back("unit", OptVarchar(unit));
	v.emplace_back("lo", has_value ? Value::DOUBLE(lo) : Value(LogicalType::DOUBLE));
	v.emplace_back("hi", has_value ? Value::DOUBLE(hi) : Value(LogicalType::DOUBLE));
	v.emplace_back("role", OptVarchar(role));
	v.emplace_back("bound", OptVarchar(bound));
	v.emplace_back("qualifier", OptVarchar(JsonString(a.Get("qualifier"))));
	v.emplace_back("category", OptVarchar(category));
	return Value::STRUCT(std::move(v));
}

void NodeFromJson(const Json &n, const string &parent_id, JsonTreeContext &ctx, idx_t depth) {
	string where = parent_id.empty() ? "under the root" : "under node '" + parent_id + "'";
	if (depth > 256) {
		throw InvalidInputException("tree_from_json: the tree is nested more than 256 levels deep (%s)", where);
	}
	if (n.kind != Json::Kind::OBJECT) {
		throw InvalidInputException("tree_from_json: every node must be a JSON object (%s)", where);
	}
	string node_id = JsonString(n.Get("node_id"));
	if (node_id.empty()) {
		throw InvalidInputException("tree_from_json: a node has no node_id (%s)", where);
	}
	if (!ctx.seen.insert(node_id).second) {
		throw InvalidInputException("tree_from_json: duplicate node_id '%s' in tree '%s'", node_id, ctx.tree_id);
	}
	auto children = n.Get("children");
	auto attributes = n.Get("attributes");
	string kind = JsonString(n.Get("kind"));
	if (kind.empty()) {
		kind = JsonString(n.Get("node_kind"));
	}
	if (kind.empty()) {
		bool has_children = children && children->kind == Json::Kind::ARRAY && !children->items.empty();
		kind = has_children ? "container" : "item";
	}
	string edge = JsonString(n.Get("edge"));
	if (edge.empty()) {
		edge = JsonString(n.Get("edge_type"));
	}
	if (edge.empty()) {
		edge = "AND";
	}
	edge = StringUtil::Upper(edge);
	if (edge != "AND" && edge != "OR") {
		throw InvalidInputException("tree_from_json: node '%s': edge must be AND or OR, got '%s'", node_id, edge);
	}
	double count = 1;
	auto count_field = n.Get("count");
	if (count_field && !count_field->IsNull() && !JsonNumber(count_field, count)) {
		throw InvalidInputException("tree_from_json: node '%s': count must be a number", node_id);
	}
	string source = JsonString(n.Get("source"));
	if (source.empty()) {
		source = ctx.source;
	}
	vector<Value> attrs;
	if (attributes && attributes->kind == Json::Kind::ARRAY) {
		for (idx_t i = 0; i < attributes->items.size(); i++) {
			attrs.push_back(AttributeFromJson(attributes->items[i], i, node_id, ctx));
		}
	}
	child_list_t<Value> v;
	v.emplace_back("tree_id", Value(ctx.tree_id));
	v.emplace_back("node_id", Value(node_id));
	v.emplace_back("parent_id", OptVarchar(parent_id));
	v.emplace_back("node_kind", Value(kind));
	v.emplace_back("edge_type", Value(edge));
	v.emplace_back("label", OptVarchar(JsonString(n.Get("label"))));
	v.emplace_back("context", OptVarchar(JsonString(n.Get("context"))));
	v.emplace_back("class_id", OptVarchar(JsonString(n.Get("class_id"))));
	v.emplace_back("count", Value::DOUBLE(count));
	v.emplace_back("source", OptVarchar(source));
	v.emplace_back("attributes", Value::LIST(AttributeType(), std::move(attrs)));
	ctx.rows.push_back(Value::STRUCT(std::move(v)));
	if (children && children->kind == Json::Kind::ARRAY) {
		for (auto &c : children->items) {
			NodeFromJson(c, node_id, ctx, depth + 1);
		}
	}
}

void TreeFromJsonFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	for (idx_t i = 0; i < count; i++) {
		auto doc = args.data[0].GetValue(i);
		if (doc.IsNull()) {
			result.SetValue(i, Value(LogicalType::LIST(NodeType())));
			continue;
		}
		auto text = doc.ToString();
		auto root_doc = JsonReader(text).Parse();
		if (root_doc.kind != Json::Kind::OBJECT) {
			throw InvalidInputException("tree_from_json: invalid JSON document: expected an object with a 'root'");
		}
		const Json *root = root_doc.Get("root");
		if (!root) {
			root = &root_doc; // a bare node is accepted as the root
		}
		JsonTreeContext ctx;
		Value override_id = args.ColumnCount() > 1 ? args.data[1].GetValue(i) : Value();
		ctx.tree_id = !override_id.IsNull() ? override_id.ToString() : JsonString(root_doc.Get("tree_id"));
		if (ctx.tree_id.empty()) {
			ctx.tree_id = JsonString(root->Get("node_id"));
		}
		ctx.source = JsonString(root_doc.Get("source"));
		ctx.role = JsonString(root_doc.Get("role"));
		Value lang_arg = args.ColumnCount() > 2 ? args.data[2].GetValue(i) : Value();
		if (!lang_arg.IsNull()) {
			for (auto &l : StringListParameter(lang_arg, "lang")) {
				for (auto &part : StringUtil::Split(l, ',')) {
					ctx.langs.push_back(StringUtil::Lower(StringUtil::Replace(part, " ", "")));
				}
			}
		} else {
			auto lang = root_doc.Get("lang");
			if (lang && lang->kind == Json::Kind::ARRAY) {
				for (auto &l : lang->items) {
					ctx.langs.push_back(StringUtil::Lower(JsonString(&l)));
				}
			} else if (lang && lang->kind == Json::Kind::STRING) {
				ctx.langs.push_back(StringUtil::Lower(lang->text));
			}
		}
		if (ctx.langs.empty()) {
			ctx.langs = {"en", "de"};
		}
		UnitTable user_units;
		Value units_arg = args.ColumnCount() > 3 ? args.data[3].GetValue(i) : Value();
		if (units_arg.IsNull()) {
			ctx.units = &UnitTable::Seed();
		} else {
			user_units = UnitTable::WithUserUnits(units_arg);
			ctx.units = &user_units;
		}
		NodeFromJson(*root, "", ctx, 0);
		result.SetValue(i, Value::LIST(NodeType(), std::move(ctx.rows)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

//------------------------------------------------------------------------------
// tree_multiset: class multiset of one packed tree, max over OR alternatives
//------------------------------------------------------------------------------

using Multiset = std::map<string, double>;

void TreeMultisetFun(DataChunk &args, ExpressionState &, Vector &result) {
	auto count = args.size();
	for (idx_t row = 0; row < count; row++) {
		auto list = args.data[0].GetValue(row);
		if (list.IsNull()) {
			result.SetValue(row, Value(LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE)));
			continue;
		}
		struct N {
			string id, parent, kind, edge, cls;
			double count;
		};
		vector<N> nodes;
		for (auto &entry : ListValue::GetChildren(list)) {
			auto &types = StructType::GetChildTypes(entry.type());
			auto &vals = StructValue::GetChildren(entry);
			N n {"", "", "item", "AND", "", 1.0};
			for (idx_t i = 0; i < types.size(); i++) {
				auto name = StringUtil::Lower(static_cast<const string &>(types[i].first));
				auto &v = vals[i];
				if (v.IsNull()) {
					continue;
				}
				if (name == "node_id") {
					n.id = v.ToString();
				} else if (name == "parent_id") {
					n.parent = v.ToString();
				} else if (name == "node_kind") {
					n.kind = v.ToString();
				} else if (name == "edge_type") {
					n.edge = StringUtil::Upper(v.ToString());
				} else if (name == "class_id") {
					n.cls = v.ToString();
				} else if (name == "count") {
					n.count = v.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
				}
			}
			nodes.push_back(n);
		}
		std::unordered_map<string, idx_t> index;
		for (idx_t i = 0; i < nodes.size(); i++) {
			index[nodes[i].id] = i;
		}
		std::unordered_map<string, vector<idx_t>> children;
		vector<idx_t> roots;
		for (idx_t i = 0; i < nodes.size(); i++) {
			if (nodes[i].parent.empty() || !index.count(nodes[i].parent)) {
				roots.push_back(i);
			} else {
				children[nodes[i].parent].push_back(i);
			}
		}
		vector<bool> visiting(nodes.size(), false);
		std::function<Multiset(idx_t)> ms = [&](idx_t i) -> Multiset {
			Multiset out;
			if (visiting[i]) {
				return out; // a cycle contributes nothing
			}
			visiting[i] = true;
			auto &n = nodes[i];
			if (n.kind == "item" && !n.cls.empty()) {
				out[n.cls] += 1.0;
			}
			Multiset alternatives;
			for (auto c : children[n.id]) {
				auto sub = ms(c);
				if (nodes[c].edge == "OR") {
					for (auto &kv : sub) {
						alternatives[kv.first] = MaxValue(alternatives[kv.first], kv.second);
					}
				} else {
					for (auto &kv : sub) {
						out[kv.first] += kv.second;
					}
				}
			}
			for (auto &kv : alternatives) {
				out[kv.first] += kv.second;
			}
			for (auto &kv : out) {
				kv.second *= n.count;
			}
			visiting[i] = false;
			return out;
		};
		Multiset total;
		for (auto r : roots) {
			for (auto &kv : ms(r)) {
				total[kv.first] += kv.second;
			}
		}
		vector<Value> keys, values;
		for (auto &kv : total) {
			keys.emplace_back(kv.first);
			values.push_back(Value::DOUBLE(kv.second));
		}
		result.SetValue(row, Value::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE, std::move(keys), std::move(values)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> TreeFromJsonBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("tree_from_json");
	return nullptr;
}
unique_ptr<FunctionData> TreeMultisetBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("tree_multiset");
	return nullptr;
}

unique_ptr<SubqueryRef> ParseSubquery(const string &query, ClientContext &context, const string &fn) {
	auto statements = duckdb_compat::ParseStatements(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Failed to parse %s query", fn);
	}
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

string Quoted(const string &path) {
	string out = "\"";
	for (auto ch : path) {
		out += ch == '.' ? string("\".\"") : string(1, ch);
	}
	return out + "\"";
}

string OptionalString(const named_parameter_map_t &named, const string &name, const string &fallback) {
	auto v = duckdb_compat::FindNamedParameter(named, name);
	return v && !v->IsNull() ? v->GetValue<string>() : fallback;
}

TreeSource ReadTreeSource(ClientContext &context, TableFunctionBindInput &input, const string &fn) {
	TreeSource src;
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("%s: nodes_table must not be NULL", fn.c_str());
	}
	src.nodes_table = input.inputs[0].GetValue<string>();
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		src.attrs_table = input.inputs[1].GetValue<string>();
	}
	auto &named = input.named_parameters;
	if (auto v = duckdb_compat::FindNamedParameter(named, "attrs_table"); v && !v->IsNull()) {
		src.attrs_table = v->GetValue<string>();
	}
	src.overrides_table = OptionalString(named, "overrides_table", "");
	src.label_table = OptionalString(named, "label_table", "");
	if (auto v = duckdb_compat::FindNamedParameter(named, "tree_ids"); v) {
		src.tree_ids = StringListParameter(*v, "tree_ids");
	}
	ValidateTreeSource(context, src, fn);
	return src;
}

} // namespace

//------------------------------------------------------------------------------
// Shared SQL builders
//------------------------------------------------------------------------------

string QuotedList(const vector<string> &values) {
	string out = "[";
	for (idx_t i = 0; i < values.size(); i++) {
		out += (i ? ", " : "") + QuoteSQLStringLiteral(values[i]);
	}
	return out + "]";
}

vector<string> StringListParameter(const Value &value, const string &name) {
	vector<string> out;
	if (value.IsNull()) {
		return out;
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (auto &c : ListValue::GetChildren(value)) {
			if (!c.IsNull()) {
				out.push_back(c.ToString());
			}
		}
	} else {
		out.push_back(value.ToString());
	}
	return out;
}

void ValidateTreeSource(ClientContext &context, TreeSource &src, const string &fn) {
	src.nodes_table = ValidateSQLIdentifierPath(src.nodes_table, "nodes_table");
	ValidateTableColumns(context, src.nodes_table,
	                     {"tree_id", "node_id", "parent_id", "node_kind", "edge_type", "label", "class_id", "count"},
	                     "nodes_table");
	src.has_source = TableHasColumn(context, src.nodes_table, "source");
	src.has_context = TableHasColumn(context, src.nodes_table, "context");
	if (!src.attrs_table.empty()) {
		src.attrs_table = ValidateSQLIdentifierPath(src.attrs_table, "attrs_table");
		ValidateTableColumns(context, src.attrs_table, {"tree_id", "node_id", "attr_id"}, "attrs_table");
	}
	if (!src.overrides_table.empty()) {
		src.overrides_table = ValidateSQLIdentifierPath(src.overrides_table, "overrides_table");
		ValidateTableColumns(context, src.overrides_table, {"wording", "source", "class_id"}, "overrides_table");
	}
	if (!src.label_table.empty()) {
		src.label_table = ValidateSQLIdentifierPath(src.label_table, "label_table");
		ValidateTableColumns(context, src.label_table, {"tree_id", "node_id", "class_id", "p"}, "label_table");
	}
	(void)fn;
}

string ResolvedNodesSQL(const TreeSource &src) {
	string where = src.tree_ids.empty() ? "" : " WHERE CAST(tree_id AS VARCHAR) IN (SELECT unnest(" + QuotedList(src.tree_ids) + "))";
	string base = StringUtil::Format(
	    "SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id, "
	    "CAST(parent_id AS VARCHAR) AS parent_id, CAST(node_kind AS VARCHAR) AS node_kind, "
	    "upper(COALESCE(CAST(edge_type AS VARCHAR), 'AND')) AS edge_type, CAST(label AS VARCHAR) AS label, "
	    "%s AS context, %s AS source, COALESCE(CAST(\"count\" AS DOUBLE), 1.0) AS count, "
	    "CAST(class_id AS VARCHAR) AS given_class FROM %s%s",
	    src.has_context ? "CAST(context AS VARCHAR)" : "NULL::VARCHAR",
	    src.has_source ? "CAST(source AS VARCHAR)" : "NULL::VARCHAR", Quoted(src.nodes_table), where);
	string overrides = src.overrides_table.empty()
	                       ? "SELECT NULL::VARCHAR AS wording, NULL::VARCHAR AS source, NULL::VARCHAR AS class_id WHERE false"
	                       : "SELECT CAST(wording AS VARCHAR) AS wording, CAST(source AS VARCHAR) AS source, "
	                         "CAST(class_id AS VARCHAR) AS class_id FROM " +
	                             Quoted(src.overrides_table);
	string labels = src.label_table.empty()
	                    ? "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::VARCHAR AS class_id WHERE false"
	                    : "SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id, "
	                      "first(CAST(class_id AS VARCHAR) ORDER BY CAST(p AS DOUBLE) DESC, CAST(class_id AS VARCHAR)) "
	                      "AS class_id FROM " +
	                          Quoted(src.label_table) + " GROUP BY ALL";
	return StringUtil::Format(
	    "SELECT n.tree_id, n.node_id, n.parent_id, n.node_kind, n.edge_type, n.label, n.context, n.source, n.count, "
	    "COALESCE(n.given_class, o.class_id, l.class_id) AS class_id, "
	    "(n.given_class IS NOT NULL OR o.class_id IS NOT NULL) AS reviewed "
	    "FROM (%s) n "
	    "LEFT JOIN (%s) o ON o.wording = n.label AND (o.source IS NULL OR o.source = n.source) "
	    "LEFT JOIN (%s) l ON l.tree_id = n.tree_id AND l.node_id = n.node_id "
	    "QUALIFY row_number() OVER (PARTITION BY n.tree_id, n.node_id ORDER BY o.source IS NULL, o.class_id) = 1",
	    base, overrides, labels);
}

//------------------------------------------------------------------------------
// Table functions
//------------------------------------------------------------------------------

namespace {

unique_ptr<TableRef> TreeAnchorsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("tree_anchors");
	auto src = ReadTreeSource(context, input, "tree_anchors");
	string attr_counts = src.attrs_table.empty()
	                         ? "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, 0 AS n WHERE false"
	                         : "SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id, "
	                           "count(*) AS n FROM " +
	                               Quoted(src.attrs_table) + " GROUP BY ALL";
	auto sql = StringUtil::Format(R"(
		WITH RECURSIVE
			n AS (%s),
			walk AS (
				SELECT tree_id, node_id, node_kind, edge_type, label, context, source, class_id, reviewed, count,
				       0 AS depth, []::VARCHAR[] AS anchor_path,
				       CASE WHEN edge_type = 'OR' THEN [node_id] ELSE []::VARCHAR[] END AS variant_path,
				       count AS effective_count
				FROM n WHERE parent_id IS NULL
				UNION ALL
				SELECT c.tree_id, c.node_id, c.node_kind, c.edge_type, c.label, c.context, c.source, c.class_id,
				       c.reviewed, c.count, w.depth + 1, list_append(w.anchor_path, w.node_id),
				       CASE WHEN c.edge_type = 'OR' THEN list_append(w.variant_path, c.node_id) ELSE w.variant_path END,
				       w.effective_count * c.count
				FROM walk w JOIN n c ON c.tree_id = w.tree_id AND c.parent_id = w.node_id
				WHERE w.depth < 255
			),
			attr_counts AS (%s)
		SELECT w.tree_id, w.node_id, w.label, w.class_id, w.reviewed, w.count, w.effective_count, w.depth,
		       w.anchor_path, w.variant_path, w.context, w.source, COALESCE(a.n, 0)::BIGINT AS n_attributes
		FROM walk w LEFT JOIN attr_counts a USING (tree_id, node_id)
		WHERE w.node_kind = 'item'
		ORDER BY w.tree_id, w.node_id
	)",
	                              ResolvedNodesSQL(src), attr_counts);
	return ParseSubquery(sql, context, "tree_anchors");
}

unique_ptr<TableRef> TreeClassMultisetBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("tree_class_multiset");
	auto src = ReadTreeSource(context, input, "tree_class_multiset");
	auto sql = StringUtil::Format(R"(
		WITH n AS (%s),
		packed AS (
			SELECT tree_id, bool_and(reviewed) FILTER (WHERE node_kind = 'item') AS reviewed,
			       tree_multiset(list(struct_pack(node_id, parent_id, node_kind, edge_type, class_id, count))) AS m
			FROM n GROUP BY tree_id
		)
		SELECT tree_id AS entity_id, unnest(map_keys(m)) AS feature_id, unnest(map_values(m)) AS weight,
		       COALESCE(reviewed, true) AS reviewed
		FROM packed
		ORDER BY entity_id, feature_id
	)",
	                              ResolvedNodesSQL(src));
	return ParseSubquery(sql, context, "tree_class_multiset");
}

unique_ptr<TableRef> ValidateTreesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("validate_trees");
	auto src = ReadTreeSource(context, input, "validate_trees");
	string attr_checks;
	if (!src.attrs_table.empty()) {
		bool has_dimension = TableHasColumn(context, src.attrs_table, "dimension");
		bool has_lo = TableHasColumn(context, src.attrs_table, "lo");
		bool has_hi = TableHasColumn(context, src.attrs_table, "hi");
		bool has_name = TableHasColumn(context, src.attrs_table, "name");
		bool has_category = TableHasColumn(context, src.attrs_table, "category");
		bool has_bound = TableHasColumn(context, src.attrs_table, "bound");
		auto a = StringUtil::Format(
		    "SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id, "
		    "CAST(attr_id AS VARCHAR) AS attr_id, %s AS name, %s AS dimension, %s AS lo, %s AS hi, %s AS category, "
		    "%s AS bound FROM %s",
		    has_name ? "CAST(name AS VARCHAR)" : "NULL::VARCHAR",
		    has_dimension ? "CAST(dimension AS VARCHAR)" : "NULL::VARCHAR", has_lo ? "CAST(lo AS DOUBLE)" : "NULL::DOUBLE",
		    has_hi ? "CAST(hi AS DOUBLE)" : "NULL::DOUBLE", has_category ? "CAST(category AS VARCHAR)" : "NULL::VARCHAR",
		    has_bound ? "CAST(bound AS VARCHAR)" : "NULL::VARCHAR", Quoted(src.attrs_table));
		attr_checks = R"(
			UNION ALL
			SELECT a.tree_id, a.node_id, a.attr_id, 'attribute_on_container', 'error', 'attributes belong to item nodes'
			FROM a JOIN n USING (tree_id, node_id) WHERE n.node_kind <> 'item'
			UNION ALL
			SELECT a.tree_id, a.node_id, a.attr_id, 'orphan_attribute', 'error', 'no such node'
			FROM a WHERE NOT EXISTS (SELECT 1 FROM n WHERE n.tree_id = a.tree_id AND n.node_id = a.node_id)
			UNION ALL
			SELECT tree_id, node_id, attr_id, 'unknown_unit', 'warning', 'a value without a known dimension'
			FROM a WHERE lo IS NOT NULL AND dimension IS NULL AND category IS NULL
			UNION ALL
			SELECT tree_id, node_id, attr_id, 'bad_interval', 'error', 'lo is greater than hi'
			FROM a WHERE lo > hi
			UNION ALL
			SELECT tree_id, node_id, attr_id, 'bad_bound', 'error', 'bound must be at_least, at_most, within or equals'
			FROM a WHERE bound IS NOT NULL AND bound NOT IN ('at_least', 'at_most', 'within', 'equals')
			UNION ALL
			SELECT s.tree_id, s.node_id, s.attr_id, 'mixed_dimensions', 'warning',
			       'attribute name ' || s.name || ' has several dimensions in source ' || coalesce(s.source, 'NULL')
			FROM (SELECT a.*, n.source FROM a LEFT JOIN n USING (tree_id, node_id)) s
			JOIN (SELECT n.source, a.name FROM a LEFT JOIN n USING (tree_id, node_id)
			      WHERE a.dimension IS NOT NULL GROUP BY ALL HAVING count(DISTINCT a.dimension) > 1) m
			  ON m.name = s.name AND m.source IS NOT DISTINCT FROM s.source
			WHERE s.dimension IS NOT NULL
		)";
		attr_checks = ", a AS (" + a + ") SELECT * FROM (SELECT * FROM checks" + attr_checks + ")";
	} else {
		attr_checks = " SELECT * FROM checks";
	}
	string source_col = src.has_source ? "CAST(source AS VARCHAR)" : "NULL::VARCHAR";
	auto sql = StringUtil::Format(R"(
		WITH RECURSIVE
			raw AS (
				SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id,
				       CAST(parent_id AS VARCHAR) AS parent_id, CAST(node_kind AS VARCHAR) AS node_kind,
				       CAST(edge_type AS VARCHAR) AS edge_type, CAST("count" AS DOUBLE) AS count, %s AS source
				FROM %s%s
			),
			n AS (SELECT DISTINCT ON (tree_id, node_id) * FROM raw),
			up AS (
				SELECT tree_id, node_id AS start, parent_id AS cur, [node_id] AS path, 0 AS d
				FROM n WHERE parent_id IS NOT NULL
				UNION ALL
				SELECT u.tree_id, u.start, p.parent_id, list_append(u.path, u.cur), u.d + 1
				FROM up u JOIN n p ON p.tree_id = u.tree_id AND p.node_id = u.cur
				WHERE u.cur IS NOT NULL AND NOT list_contains(u.path, u.cur) AND u.d < 255
			),
			checks AS (
				SELECT tree_id, node_id, NULL::VARCHAR AS attr_id, 'duplicate_node' AS issue, 'error' AS severity,
				       'node_id used more than once' AS detail
				FROM raw GROUP BY tree_id, node_id HAVING count(*) > 1
				UNION ALL
				SELECT tree_id, node_id, NULL, 'bad_node_kind', 'error', 'node_kind must be container, item or variant_group'
				FROM n WHERE node_kind IS NULL OR node_kind NOT IN ('container', 'item', 'variant_group')
				UNION ALL
				SELECT tree_id, node_id, NULL, 'bad_edge_type', 'error', 'edge_type must be AND or OR'
				FROM n WHERE upper(COALESCE(edge_type, 'AND')) NOT IN ('AND', 'OR')
				UNION ALL
				SELECT tree_id, node_id, NULL, 'bad_count', 'error', 'count must be positive'
				FROM n WHERE count IS NOT NULL AND count <= 0
				UNION ALL
				SELECT c.tree_id, c.node_id, NULL, 'orphan', 'error', 'parent ' || c.parent_id || ' does not exist'
				FROM n c WHERE c.parent_id IS NOT NULL
				  AND NOT EXISTS (SELECT 1 FROM n p WHERE p.tree_id = c.tree_id AND p.node_id = c.parent_id)
				UNION ALL
				SELECT tree_id, NULL, NULL, 'no_root', 'error', 'no node without a parent'
				FROM n GROUP BY tree_id HAVING count(*) FILTER (WHERE parent_id IS NULL) = 0
				UNION ALL
				SELECT tree_id, NULL, NULL, 'multiple_roots', 'error',
				       count(*) FILTER (WHERE parent_id IS NULL) || ' nodes without a parent'
				FROM n GROUP BY tree_id HAVING count(*) FILTER (WHERE parent_id IS NULL) > 1
				UNION ALL
				SELECT DISTINCT tree_id, start, NULL, 'cycle', 'error', 'the node is its own ancestor'
				FROM up WHERE cur = start
				UNION ALL
				SELECT tree_id, parent_id, NULL, 'mixed_edge_types', 'warning', 'children joined by both AND and OR'
				FROM n WHERE parent_id IS NOT NULL GROUP BY tree_id, parent_id
				HAVING count(DISTINCT upper(COALESCE(edge_type, 'AND')))
				           FILTER (WHERE upper(COALESCE(edge_type, 'AND')) IN ('AND', 'OR')) > 1
			)
		%s
		ORDER BY tree_id, issue, node_id, attr_id
	)",
	                              source_col, Quoted(src.nodes_table),
	                              src.tree_ids.empty() ? ""
	                                                   : " WHERE CAST(tree_id AS VARCHAR) IN (SELECT unnest(" +
	                                                         QuotedList(src.tree_ids) + "))",
	                              attr_checks);
	return ParseSubquery(sql, context, "validate_trees");
}

void RegisterTreeTable(ExtensionLoader &loader, const string &name, table_function_bind_replace_t fn,
                       const string &description, vector<string> examples, bool with_attr_table) {
	TableFunction f(name, {LogicalType::VARCHAR}, nullptr, nullptr);
	if (with_attr_table) {
		f.varargs = LogicalType::VARCHAR;
	}
	f.bind_replace = fn;
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"coverage", "tree"};
	AddDocumentedNamedParameters(f, desc, {"nodes_table"},
	                             {{"attrs_table", LogicalType::VARCHAR},
	                              {"overrides_table", LogicalType::VARCHAR},
	                              {"label_table", LogicalType::VARCHAR},
	                              {"tree_ids", LogicalType::LIST(LogicalType::VARCHAR)}});
	desc.parameter_types = {LogicalType::VARCHAR};
	CreateTableFunctionInfo info(f);
	info.descriptions.push_back(std::move(desc));
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterPropertyTreeFunctions(ExtensionLoader &loader) {
	{
		ScalarFunctionSet set(duckdb_compat::FunctionName("tree_from_json"));
		auto vc = LogicalType::VARCHAR;
		auto any = LogicalType::ANY;
		for (auto args : {vector<LogicalType> {vc}, vector<LogicalType> {vc, vc}, vector<LogicalType> {vc, vc, any},
		                  vector<LogicalType> {vc, vc, any, any}}) {
			ScalarFunction f(duckdb_compat::FunctionName("tree_from_json"), args, LogicalType::LIST(NodeType()),
			                 DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, TreeFromJsonFun),
			                 compat_anofox::ScalarBind<&TreeFromJsonBind>());
			duckdb_compat::SetFallible(f);
			f.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING); // NULL tree_id or lang means "from the document"
			set.AddFunction(f);
		}
		CreateScalarFunctionInfo info(set);
		const string desc =
		    "Turns a tree from upstream extraction (JSON: {tree_id, source, lang, root: {node_id, kind, edge, label, "
		    "context, class_id, count, attributes: [{attr_id, name, text, lo, hi, unit, bound, category}], children: "
		    "[...]}}) into a LIST of node STRUCTs with normalised attributes (base units, dimension; text-only values "
		    "are parsed with parse_quantity in the document's lang, or lang if given; unit_table extends the units "
		    "seed as in parse_quantity). Attribute STRUCTs carry tree_id and node_id. Unnest it into tree_nodes and "
		    "tree_attributes. Schema: docs/requirements-matching/tree.schema.json.";
		for (auto names : {vector<string> {"json"}, vector<string> {"json", "tree_id"},
		                   vector<string> {"json", "tree_id", "lang"},
		                   vector<string> {"json", "tree_id", "lang", "unit_table"}}) {
			FunctionDescription d;
			d.description = desc;
			d.examples = {"SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n "
			              "FROM read_text('requests/*.json'));"};
			d.categories = {"coverage", "tree"};
			d.parameter_names = names;
			vector<LogicalType> types {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::ANY, LogicalType::ANY};
			types.resize(names.size());
			d.parameter_types = types;
			info.descriptions.push_back(std::move(d));
		}
		duckdb_compat::NameArgumentsFromDescriptions(info);
		loader.RegisterFunction(std::move(info));
	}
	{
		ScalarFunction f(duckdb_compat::FunctionName("tree_multiset"), {LogicalType::ANY},
		                 LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE),
		                 DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, TreeMultisetFun),
		                 compat_anofox::ScalarBind<&TreeMultisetBind>());
		duckdb_compat::SetFallible(f);
		CreateScalarFunctionInfo info(f);
		FunctionDescription d;
		d.description = "The class multiset of one tree given as a LIST of STRUCT(node_id, parent_id, node_kind, "
		                "edge_type, class_id, count): counts multiply down the tree, add across AND siblings and "
		                "take the elementwise maximum across OR alternatives. The building block of "
		                "tree_class_multiset.";
		d.examples = {"SELECT tree_multiset(list(struct_pack(node_id, parent_id, node_kind, edge_type, class_id, "
		              "count))) FROM tree_nodes GROUP BY tree_id;"};
		d.categories = {"coverage", "tree"};
		d.parameter_names = {"nodes"};
		d.parameter_types = {LogicalType::ANY};
		info.descriptions.push_back(std::move(d));
		duckdb_compat::NameArgumentsFromDescriptions(info);
		loader.RegisterFunction(std::move(info));
	}
	RegisterTreeTable(loader, "tree_anchors", TreeAnchorsBindReplace,
	                  "Flattens property trees: one row per item node (containers are transparent) with its "
	                  "resolved class, depth, anchor_path (ancestor ids from the root), variant_path (the nodes "
	                  "with an OR edge on the way, i.e. the alternatives chosen), effective_count (product of the "
	                  "counts along the path) and n_attributes. Works at any depth (BR-04).",
	                  {"SELECT * FROM tree_anchors('tree_nodes', 'tree_attributes');",
	                   "SELECT * FROM tree_anchors('tree_nodes', tree_ids := ['M1']);"},
	                  true);
	RegisterTreeTable(loader, "tree_class_multiset", TreeClassMultisetBindReplace,
	                  "The class multiset per tree for stage-1 recall: entity_id (tree), feature_id (class), weight "
	                  "(summed counts, maximum across OR alternatives), reviewed (every item's class came from "
	                  "class_id or the reviewed vocabulary). Classes resolve in the order class_id, "
	                  "overrides_table (wording, source, class_id), label_table (classifier arg-max).",
	                  {"SELECT * FROM tree_class_multiset('tree_nodes');",
	                   "SELECT * FROM tree_class_multiset('tree_nodes', overrides_table := 'vocabulary_overrides');"},
	                  false);
	RegisterTreeTable(loader, "validate_trees", ValidateTreesBindReplace,
	                  "Checks property trees and returns one row per issue (tree_id, node_id, attr_id, issue, severity, "
	                  "detail). Errors: duplicate_node, bad_node_kind, bad_edge_type, bad_count, orphan, no_root, "
	                  "multiple_roots, cycle, and with an attribute table attribute_on_container, orphan_attribute, "
	                  "bad_interval, bad_bound. Warnings: mixed_edge_types, unknown_unit, mixed_dimensions (one "
	                  "attribute name with several dimensions within a source).",
	                  {"SELECT * FROM validate_trees('tree_nodes', 'tree_attributes');"}, true);
}

} // namespace anofox
} // namespace duckdb
