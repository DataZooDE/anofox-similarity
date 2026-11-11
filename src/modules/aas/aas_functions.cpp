// SQL registration for the AAS integration. Only the SQL surface lives here: argument
// handling, documentation and telemetry. The logic is in the format-neutral modules next to it.
#include "modules/aas/aas_functions.hpp"
#include "modules/aas/aas_model.hpp"
#include "modules/aas/aas_tree_mapper.hpp"
#include "modules/aas/aas_writer.hpp"
#include "modules/aas/aas_quality.hpp"
#include "modules/aas/semantic_key.hpp"
#include "modules/property_tree.hpp"
#include "modules/quantity.hpp"
#include "modules/coverage_sql.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/common/error_data.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

namespace duckdb {
namespace anofox {

namespace {

const vector<string> AAS_CATEGORIES = {"coverage", "aas"};

//------------------------------------------------------------------------------
// semantic_key(id [, keep_version])
//------------------------------------------------------------------------------

void SemanticKeyFun(DataChunk &args, ExpressionState &, Vector &result) {
	bool has_flag = args.ColumnCount() > 1;
	for (idx_t row = 0; row < args.size(); row++) {
		auto id = args.data[0].GetValue(row);
		if (id.IsNull()) {
			result.SetValue(row, Value(LogicalType::VARCHAR));
			continue;
		}
		bool keep_version = false;
		if (has_flag) {
			auto flag = args.data[1].GetValue(row);
			keep_version = !flag.IsNull() && flag.GetValue<bool>();
		}
		auto key = SemanticKey(id.ToString(), keep_version);
		result.SetValue(row, key.has_key ? Value(key.key) : Value(LogicalType::VARCHAR));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> SemanticKeyBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("semantic_key");
	return nullptr;
}

void RegisterSemanticKey(ExtensionLoader &loader) {
	auto vc = LogicalType::VARCHAR;
	CoverageScalarSpec spec {
	    "semantic_key",
	    vc,
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, SemanticKeyFun),
	    compat_anofox::ScalarBind<&SemanticKeyBind>(),
	    "Canonical join key for an AAS semanticId. An ECLASS or IEC CDD IRDI ('0173-1#02-AAC895#008', "
	    "'0112/2///61360_4#AAA001#001') and the ECLASS IRI form ('https://api.eclass-cdp.com/0173-1-02-AAC895-008') "
	    "give the same key: the upper-cased IRDI without its version suffix. keep_version (default false) keeps the "
	    "version; use it when a dictionary release changed the meaning of a property. Other URIs pass through with "
	    "scheme and host lower-cased, any other text is only trimmed, and NULL or a blank give NULL (no key).",
	    AAS_CATEGORIES};
	spec.special_null_handling = true;
	RegisterCoverageScalar(
	    loader, spec,
	    {{{vc}, {"id"}, {"SELECT semantic_key('https://api.eclass-cdp.com/0173-1-02-AAC895-008'); -- 0173-1#02-AAC895"}, ""},
	     {{vc, LogicalType::BOOLEAN},
	      {"id", "keep_version"},
	      {"SELECT semantic_key('0173-1#02-AAC895#008', true); -- 0173-1#02-AAC895#008"},
	      ""}});
}

//------------------------------------------------------------------------------
// aas_to_trees(content [, tree_id [, unit_table]]) and aas_issues(content [, tree_id [, unit_table]])
//------------------------------------------------------------------------------

//! The raw bytes of a VARCHAR or BLOB argument.
string ContentOf(const Value &v) {
	return StringValue::Get(v);
}

Value Text(const string &s) {
	return s.empty() ? Value(LogicalType::VARCHAR) : Value(s);
}

Value AttributeValue(const TreeNodeRow &n, const TreeAttrRow &a) {
	child_list_t<Value> v;
	v.emplace_back("tree_id", Value(n.tree_id));
	v.emplace_back("node_id", Value(n.node_id));
	v.emplace_back("attr_id", Value(a.attr_id));
	v.emplace_back("name", Text(a.name));
	v.emplace_back("value_text", Text(a.value_text));
	v.emplace_back("dimension", Text(a.dimension));
	v.emplace_back("unit", Text(a.unit));
	v.emplace_back("lo", a.has_value ? Value::DOUBLE(a.lo) : Value(LogicalType::DOUBLE));
	v.emplace_back("hi", a.has_value ? Value::DOUBLE(a.hi) : Value(LogicalType::DOUBLE));
	v.emplace_back("role", Text(a.role));
	v.emplace_back("bound", Text(a.bound));
	v.emplace_back("qualifier", Text(a.qualifier));
	v.emplace_back("category", Text(a.category));
	v.emplace_back("attr_key", Text(a.attr_key));
	v.emplace_back("bound_source", Text(a.bound_source));
	return Value::STRUCT(std::move(v));
}

Value NodeValue(const TreeNodeRow &n) {
	vector<Value> attrs;
	for (auto &a : n.attrs) {
		attrs.push_back(AttributeValue(n, a));
	}
	child_list_t<Value> v;
	v.emplace_back("tree_id", Value(n.tree_id));
	v.emplace_back("node_id", Value(n.node_id));
	v.emplace_back("parent_id", Text(n.parent_id));
	v.emplace_back("node_kind", Value(n.node_kind));
	v.emplace_back("edge_type", Value(n.edge_type));
	v.emplace_back("label", Text(n.label));
	v.emplace_back("context", Text(n.context));
	v.emplace_back("class_id", Text(n.class_id));
	v.emplace_back("count", Value::DOUBLE(n.count));
	v.emplace_back("source", Value(LogicalType::VARCHAR));
	v.emplace_back("attributes", Value::LIST(TreeAttributeType(), std::move(attrs)));
	return Value::STRUCT(std::move(v));
}

LogicalType IssueType() {
	child_list_t<LogicalType> c {{"tree_id", LogicalType::VARCHAR},
	                             {"path", LogicalType::VARCHAR},
	                             {"issue", LogicalType::VARCHAR},
	                             {"severity", LogicalType::VARCHAR},
	                             {"detail", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(c);
}

Value IssueValue(const AasIssue &i) {
	child_list_t<Value> v;
	v.emplace_back("tree_id", Text(i.tree_id));
	v.emplace_back("path", Text(i.path));
	v.emplace_back("issue", Value(i.issue));
	v.emplace_back("severity", Value(i.severity));
	v.emplace_back("detail", Text(i.detail));
	return Value::STRUCT(std::move(v));
}

//! Reads one row's AAS input and maps it. The unit table is rebuilt only when it changes.
class AasRowMapper {
public:
	//! The environment of the last Map call (for reports that need the model as well).
	AasEnvironment environment;

	AasMapping Map(DataChunk &args, idx_t row, idx_t unit_arg = 2) {
		AasMapOptions options;
		if (args.ColumnCount() > 1 && unit_arg != 1) {
			auto id = args.data[1].GetValue(row);
			options.tree_id = id.IsNull() ? string() : id.ToString();
		}
		if (args.ColumnCount() > unit_arg) {
			auto units = args.data[unit_arg].GetValue(row);
			if (!has_units || !Value::NotDistinctFrom(units, units_value)) {
				unit_table = UnitTable::WithUserUnits(units);
				units_value = units;
				has_units = true;
			}
			options.units = &unit_table;
		} else {
			options.units = &UnitTable::Seed();
		}
		environment = ReadAas(ContentOf(args.data[0].GetValue(row)));
		return MapAasToTrees(environment, options);
	}

private:
	bool has_units = false;
	Value units_value;
	UnitTable unit_table;
};

//! Malformed input: strict functions (aas_to_trees) raise the reader's error; the audit
//! (aas_issues) reports it as a parse_error row, so a folder load can name the bad file.
enum class OnReadError { RAISE, REPORT };

template <class EMIT>
void ForEachMapping(DataChunk &args, Vector &result, EMIT emit, OnReadError on_error = OnReadError::RAISE) {
	AasRowMapper mapper;
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		AasMapping mapping;
		if (on_error == OnReadError::RAISE) {
			mapping = mapper.Map(args, row);
		} else {
			try {
				mapping = mapper.Map(args, row);
			} catch (std::exception &ex) {
				ErrorData error(ex);
				mapping.issues.push_back({"", "", "parse_error", "error", error.RawMessage()});
			}
		}
		result.SetValue(row, emit(mapping));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void AasToTreesFun(DataChunk &args, ExpressionState &, Vector &result) {
	ForEachMapping(args, result, [](const AasMapping &m) {
		vector<Value> nodes;
		for (auto &n : m.nodes) {
			nodes.push_back(NodeValue(n));
		}
		return Value::LIST(TreeNodeType(), std::move(nodes));
	});
}

void AasIssuesFun(DataChunk &args, ExpressionState &, Vector &result) {
	ForEachMapping(
	    args, result,
	    [](const AasMapping &m) {
		    vector<Value> issues;
		    for (auto &i : m.issues) {
			    issues.push_back(IssueValue(i));
		    }
		    return Value::LIST(IssueType(), std::move(issues));
	    },
	    OnReadError::REPORT);
}

LogicalType ClassEdgeType() {
	child_list_t<LogicalType> c {{"class_id", LogicalType::VARCHAR}, {"parent_class_id", LogicalType::VARCHAR}};
	return LogicalType::STRUCT(c);
}

void AasClassHierarchyFun(DataChunk &args, ExpressionState &, Vector &result) {
	ForEachMapping(args, result, [](const AasMapping &m) {
		vector<Value> edges;
		for (auto &e : m.class_hierarchy) {
			edges.push_back(Value::STRUCT({{"class_id", Value(e.class_id)}, {"parent_class_id", Value(e.parent_class_id)}}));
		}
		return Value::LIST(ClassEdgeType(), std::move(edges));
	});
}

unique_ptr<FunctionData> AasClassHierarchyBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_class_hierarchy");
	return nullptr;
}

unique_ptr<FunctionData> AasToTreesBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_to_trees");
	return nullptr;
}

unique_ptr<FunctionData> AasIssuesBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_issues");
	return nullptr;
}

//! The overloads shared by aas_to_trees and aas_issues: VARCHAR (JSON, XML) or BLOB (also AASX).
vector<CoverageScalarOverload> ContentOverloads(const string &fn) {
	auto vc = LogicalType::VARCHAR;
	auto blob = LogicalType::BLOB;
	auto any = LogicalType::ANY;
	vector<CoverageScalarOverload> out;
	for (auto &content : {vc, blob}) {
		bool text = content == vc;
		out.push_back({{content},
		               {"content"},
		               {text ? "SELECT " + fn + "(content) FROM read_text('offers/*.json');"
		                     : "SELECT " + fn + "(content) FROM read_blob('offers/*.aasx');"},
		               ""});
		out.push_back({{content, vc},
		               {"content", "tree_id"},
		               {"SELECT " + fn + "(content, 'OFF-A') FROM " + (text ? "read_text('offer_a.xml');" : "read_blob('offer_a.aasx');")},
		               ""});
		out.push_back({{content, vc, any},
		               {"content", "tree_id", "unit_table"},
		               {"SELECT " + fn + "(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM " +
		                (text ? "read_text('offer_a.json');" : "read_blob('offer_a.aasx');")},
		               ""});
	}
	return out;
}

void RegisterAasReader(ExtensionLoader &loader) {
	CoverageScalarSpec trees {
	    "aas_to_trees",
	    LogicalType::LIST(TreeNodeType()),
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, AasToTreesFun),
	    compat_anofox::ScalarBind<&AasToTreesBind>(),
	    "Reads Asset Administration Shell data (AAS v3.x: Part 1 JSON or XML, an AASX package, a single Submodel or a "
	    "Part 2 paged result) into property trees: a LIST of the node STRUCTs of tree_from_json with their attributes. "
	    "One tree per shell (tree_id: the shell id, or the tree_id argument), submodels and collections as nodes; a "
	    "collection that directly holds values is an item (class_id: its semanticId), otherwise a container. Property "
	    "and Range give attributes: attr_key is the semanticId (the supplementalSemanticId for IDTA-02020 property "
	    "types), the unit comes from the IEC 61360 data specification (unitId first, then unit) and values are "
	    "converted to base units. An IDTA-02020 CapabilityContainer is an item whose class is the Capability's "
	    "supplementalSemanticId and whose attributes come from its PropertySets; the CapabilityRoleQualifier gives "
	    "role. Pass read_blob(...) for AASX packages. Findings about the input are in aas_issues(). unit_table as "
	    "in parse_quantity.",
	    AAS_CATEGORIES};
	trees.special_null_handling = true;
	RegisterCoverageScalar(loader, trees, ContentOverloads("aas_to_trees"));

	CoverageScalarSpec issues {
	    "aas_issues",
	    LogicalType::LIST(IssueType()),
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, AasIssuesFun),
	    compat_anofox::ScalarBind<&AasIssuesBind>(),
	    "Findings of aas_to_trees about the input: LIST of STRUCT(tree_id, path, issue, severity, detail). It never "
	    "raises on malformed input: a broken file gives one parse_error row with the reader's message, so "
	    "SELECT filename, unnest(aas_issues(content)) FROM read_blob('offers/*') audits a folder. See the API page for "
	    "the issue codes (bad_value, empty_value, decimal_comma, missing_concept_description, "
	    "ambiguous_concept_description, unknown_unit_id, assumed_dimensionless, duplicate_id_short, ...). path is "
	    "the idShort path, submodel first. Same arguments as aas_to_trees.",
	    AAS_CATEGORIES};
	issues.special_null_handling = true;
	RegisterCoverageScalar(loader, issues, ContentOverloads("aas_issues"));

	CoverageScalarSpec hierarchy {
	    "aas_class_hierarchy",
	    LogicalType::LIST(ClassEdgeType()),
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, AasClassHierarchyFun),
	    compat_anofox::ScalarBind<&AasClassHierarchyBind>(),
	    "The capability class hierarchy of IDTA-02020 CapabilityGeneralizedBy relations: LIST of STRUCT(class_id, "
	    "parent_class_id), class ids as in aas_to_trees (the second element may also be an external ontology IRI). "
	    "Store it as a table and pass it to tree_coverage as class_hierarchy: an offer of a subclass then fully meets "
	    "a requirement of its superclass (is-a), and with a generalization_credit c a superclass offer gets c^d for "
	    "a subclass requirement. Unresolvable references are in aas_issues (unresolved_relation).",
	    AAS_CATEGORIES};
	hierarchy.special_null_handling = true;
	RegisterCoverageScalar(
	    loader, hierarchy,
	    {{{LogicalType::VARCHAR},
	      {"content"},
	      {"SELECT unnest(aas_class_hierarchy(content), recursive := true) FROM read_text('capability_taxonomy.aas.json');"},
	      ""},
	     {{LogicalType::BLOB},
	      {"content"},
	      {"SELECT unnest(aas_class_hierarchy(content), recursive := true) FROM read_blob('taxonomy.aasx');"},
	      ""}});
}

//------------------------------------------------------------------------------
// aas_id_b64url(id): identifiers in AAS Part 2 REST paths
//------------------------------------------------------------------------------

string Base64Url(const string &in) {
	static const char *ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	string out;
	idx_t i = 0;
	while (i + 2 < in.size()) {
		uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
		out += ALPHABET[(n >> 18) & 63];
		out += ALPHABET[(n >> 12) & 63];
		out += ALPHABET[(n >> 6) & 63];
		out += ALPHABET[n & 63];
		i += 3;
	}
	if (i + 1 == in.size()) {
		uint32_t n = uint8_t(in[i]) << 16;
		out += ALPHABET[(n >> 18) & 63];
		out += ALPHABET[(n >> 12) & 63];
	} else if (i + 2 == in.size()) {
		uint32_t n = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
		out += ALPHABET[(n >> 18) & 63];
		out += ALPHABET[(n >> 12) & 63];
		out += ALPHABET[(n >> 6) & 63];
	}
	return out;
}

void AasIdB64UrlFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto id = args.data[0].GetValue(row);
		result.SetValue(row, id.IsNull() ? Value(LogicalType::VARCHAR) : Value(Base64Url(id.ToString())));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> AasIdB64UrlBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_id_b64url");
	return nullptr;
}

void RegisterAasIdB64Url(ExtensionLoader &loader) {
	CoverageScalarSpec spec {
	    "aas_id_b64url",
	    LogicalType::VARCHAR,
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, AasIdB64UrlFun),
	    compat_anofox::ScalarBind<&AasIdB64UrlBind>(),
	    "An AAS identifier encoded for an AAS Part 2 REST path: base64url (RFC 4648 §5) without padding, as in "
	    "GET /submodels/{submodelIdentifier}. Use it with read_text over httpfs to load a submodel from an AAS "
	    "repository (BaSyx, FA³ST).",
	    AAS_CATEGORIES};
	RegisterCoverageScalar(
	    loader, spec,
	    {{{LogicalType::VARCHAR},
	      {"id"},
	      {"SELECT aas_to_trees(content) FROM read_text('http://localhost:8081/submodels/' || "
	       "aas_id_b64url('https://example.com/ids/sm/TechnicalData'));"},
	      ""}});
}

//------------------------------------------------------------------------------
// aas_quality(content [, unit_table]): how much of an AAS the matching can use (GitHub #36)
//------------------------------------------------------------------------------

LogicalType QualityType() {
	child_list_t<LogicalType> c {{"tree_id", LogicalType::VARCHAR},
	                             {"submodel_id", LogicalType::VARCHAR},
	                             {"submodel", LogicalType::VARCHAR},
	                             {"kind", LogicalType::VARCHAR},
	                             {"n_values", LogicalType::BIGINT},
	                             {"n_with_semantic_id", LogicalType::BIGINT},
	                             {"semantic_id_share", LogicalType::DOUBLE},
	                             {"n_missing_concept_description", LogicalType::BIGINT},
	                             {"n_unresolved_units", LogicalType::BIGINT},
	                             {"n_unparseable", LogicalType::BIGINT},
	                             {"n_mixed_releases", LogicalType::BIGINT},
	                             {"n_issues", LogicalType::BIGINT}};
	return LogicalType::STRUCT(c);
}

void AasQualityFun(DataChunk &args, ExpressionState &, Vector &result) {
	AasRowMapper mapper;
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		auto mapping = mapper.Map(args, row, 1);
		vector<Value> rows;
		for (auto &q : AssessAasQuality(mapper.environment, mapping)) {
			auto share = q.n_values ? Value::DOUBLE(double(q.n_with_semantic_id) / double(q.n_values))
			                        : Value(LogicalType::DOUBLE);
			rows.push_back(Value::STRUCT({{"tree_id", Text(q.tree_id)},
			                              {"submodel_id", Text(q.submodel_id)},
			                              {"submodel", Text(q.submodel)},
			                              {"kind", Text(q.kind)},
			                              {"n_values", Value::BIGINT(int64_t(q.n_values))},
			                              {"n_with_semantic_id", Value::BIGINT(int64_t(q.n_with_semantic_id))},
			                              {"semantic_id_share", share},
			                              {"n_missing_concept_description", Value::BIGINT(int64_t(q.n_missing_concept_description))},
			                              {"n_unresolved_units", Value::BIGINT(int64_t(q.n_unresolved_units))},
			                              {"n_unparseable", Value::BIGINT(int64_t(q.n_unparseable))},
			                              {"n_mixed_releases", Value::BIGINT(int64_t(q.n_mixed_releases))},
			                              {"n_issues", Value::BIGINT(int64_t(q.n_issues))}}));
		}
		result.SetValue(row, Value::LIST(QualityType(), std::move(rows)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> AasQualityBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_quality");
	return nullptr;
}

void RegisterAasQuality(ExtensionLoader &loader) {
	CoverageScalarSpec spec {
	    "aas_quality",
	    LogicalType::LIST(QualityType()),
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, AasQualityFun),
	    compat_anofox::ScalarBind<&AasQualityBind>(),
	    "Data quality of an AAS input per submodel, without the coverage kernel: LIST of STRUCT(tree_id, submodel_id, "
	    "submodel, kind, n_values (Property, Range, MultiLanguageProperty), n_with_semantic_id, semantic_id_share, "
	    "n_missing_concept_description, n_unresolved_units (numbers whose unit is unknown), n_unparseable (bad_value), "
	    "n_mixed_releases (concepts referenced in more than one dictionary release in the input), n_issues). The "
	    "counts come from the same reading as aas_to_trees and aas_issues. unit_table as in parse_quantity.",
	    AAS_CATEGORIES};
	spec.special_null_handling = true;
	auto any = LogicalType::ANY;
	vector<CoverageScalarOverload> overloads;
	for (auto &content : {LogicalType::VARCHAR, LogicalType::BLOB}) {
		bool text = content == LogicalType::VARCHAR;
		overloads.push_back({{content},
		                     {"content"},
		                     {string("SELECT filename, unnest(aas_quality(content), recursive := true) FROM ") +
		                      (text ? "read_text('offers/*.json');" : "read_blob('offers/*');")},
		                     ""});
		overloads.push_back({{content, any},
		                     {"content", "unit_table"},
		                     {string("SELECT unnest(aas_quality(content, (SELECT list(u) FROM eclass_units u)), recursive := "
		                             "true) FROM ") +
		                      (text ? "read_text('offer.json');" : "read_blob('offer.aasx');")},
		                     ""});
	}
	RegisterCoverageScalar(loader, spec, overloads);
}

//------------------------------------------------------------------------------
// suggest_semantic_id(items_table, dictionary_table): dictionary ids for uncoded wordings,
// as a review queue (GitHub #36)
//------------------------------------------------------------------------------

unique_ptr<TableRef> SuggestSemanticIdBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("suggest_semantic_id");
	CoverageParamReader r {context, input.named_parameters, "suggest_semantic_id"};
	auto items = r.Positional(input, 0, "items_table", {"node_id", "vec"});
	auto dictionary = r.Positional(input, 1, "dictionary_table", {"class_id", "vec"});
	auto overrides = r.Table("overrides_table", "", {"wording", "class_id"});
	auto mode = StringUtil::Lower(r.Str("mode", "late_interaction"));
	if (mode != "centroid" && mode != "late_interaction") {
		throw BinderException("suggest_semantic_id: mode must be 'centroid' or 'late_interaction'");
	}
	if (mode == "late_interaction" && (!r.Has(items, "tokens") || !r.Has(dictionary, "exemplar_tokens"))) {
		throw BinderException("suggest_semantic_id: mode 'late_interaction' needs items_table.tokens and "
		                      "dictionary_table.exemplar_tokens (or use mode := 'centroid')");
	}
	double temperature = r.Dbl("temperature", 0.1);
	auto min_sim = r.Get("min_similarity");
	string min_similarity = !min_sim || min_sim->IsNull() ? "NULL::DOUBLE" : DoubleLiteral(r.Dbl("min_similarity", 0));
	// the absolute fit of a suggestion: the classifier's p is relative to the dictionary, so the best
	// of only poor candidates looks certain; the raw similarity says whether anything fits
	string similarity = mode == "late_interaction" ? "maxsim(CAST(i.tokens AS DOUBLE[][]), CAST(d.exemplar_tokens AS DOUBLE[][]))"
	                                               : "list_cosine_similarity(CAST(i.vec AS DOUBLE[]), CAST(d.vec AS DOUBLE[]))";
	auto center = r.Get("center_by_source");
	bool center_by_source = center && !center->IsNull() && center->GetValue<bool>();
	auto label = r.Has(items, "label") ? "coalesce(CAST(i.label AS VARCHAR), CAST(i.node_id AS VARCHAR))"
	                                   : "CAST(i.node_id AS VARCHAR)";
	auto source = CastOrNullSQL(r.Has(items, "source"), "source", "VARCHAR", "i");
	auto tree = CastOrNullSQL(r.Has(items, "tree_id"), "tree_id", "VARCHAR", "i");
	string reviewed;
	if (!overrides.empty()) {
		reviewed = r.Has(overrides, "source")
		               ? StringUtil::Format(" WHERE NOT EXISTS (SELECT 1 FROM %s o WHERE CAST(o.wording AS VARCHAR) = "
		                                    "q.wording AND (o.source IS NULL OR CAST(o.source AS VARCHAR) IS NOT DISTINCT "
		                                    "FROM q.source))",
		                                    QuotedIdentifier(overrides))
		               : StringUtil::Format(" WHERE q.wording NOT IN (SELECT CAST(wording AS VARCHAR) FROM %s)",
		                                    QuotedIdentifier(overrides));
	}
	auto sql = StringUtil::Format(R"(
		WITH decisions AS (
			SELECT * FROM classify_to_catalogue(items_table := %s, catalogue_table := %s, mode := %s, top := 2,
			                                    temperature := %s, center_by_source := %s)),
		occurrences AS (
			SELECT %s AS tree_id, CAST(i.node_id AS VARCHAR) AS node_id, %s AS wording, %s AS source FROM %s i),
		fit AS (
			SELECT o.wording, o.source, CAST(d.class_id AS VARCHAR) AS class_id, avg(%s) AS similarity
			FROM occurrences o JOIN %s i ON CAST(i.node_id AS VARCHAR) = o.node_id AND %s IS NOT DISTINCT FROM o.tree_id,
			     %s d
			GROUP BY ALL),
		per_class AS (
			SELECT o.wording, o.source, d.class_id, avg(d.p) AS p
			FROM decisions d JOIN occurrences o ON o.node_id = CAST(d.node_id AS VARCHAR)
			     AND o.tree_id IS NOT DISTINCT FROM CAST(d.tree_id AS VARCHAR)
			GROUP BY ALL),
		ranked AS (
			SELECT *, row_number() OVER (PARTITION BY wording, source ORDER BY p DESC, class_id) AS rank FROM per_class),
		counts AS (
			SELECT wording, source, count(*) AS n_occurrences, count(DISTINCT coalesce(tree_id, node_id)) AS n_trees_affected
			FROM occurrences GROUP BY ALL),
		q AS (
			SELECT c.wording, c.source, c.n_occurrences, c.n_trees_affected,
			       r1.class_id AS top1, r1.p AS p1, r2.class_id AS top2, r2.p AS p2,
			       r1.p - coalesce(r2.p, 0) AS margin, f.similarity, f.similarity >= %s AS fits
			FROM counts c
			JOIN ranked r1 ON r1.wording = c.wording AND r1.source IS NOT DISTINCT FROM c.source AND r1.rank = 1
			LEFT JOIN ranked r2 ON r2.wording = c.wording AND r2.source IS NOT DISTINCT FROM c.source AND r2.rank = 2
			LEFT JOIN fit f ON f.wording = c.wording AND f.source IS NOT DISTINCT FROM c.source
			     AND f.class_id = CAST(r1.class_id AS VARCHAR))
		SELECT wording, source, n_occurrences, n_trees_affected, top1, p1, top2, p2, margin, similarity, fits,
		       CASE WHEN fits = false THEN n_trees_affected ELSE n_trees_affected * (1 - margin) END AS impact
		FROM q%s
		ORDER BY impact DESC, wording)",
	    QuoteSQLStringLiteral(items), QuoteSQLStringLiteral(dictionary), QuoteSQLStringLiteral(mode),
	    DoubleLiteral(temperature), center_by_source ? "true" : "false", tree, label, source, QuotedIdentifier(items),
	    similarity, QuotedIdentifier(items), tree, QuotedIdentifier(dictionary), min_similarity, reviewed);
	return CoverageSubquery(sql, context, "suggest_semantic_id");
}

void RegisterSuggestSemanticId(ExtensionLoader &loader) {
	auto vc = LogicalType::VARCHAR;
	RegisterCoverageTableFunction(
	    loader, "suggest_semantic_id", {vc, vc}, {"items_table", "dictionary_table"}, SuggestSemanticIdBindReplace,
	    {{"mode", vc}, {"temperature", LogicalType::DOUBLE}, {"center_by_source", LogicalType::BOOLEAN},
	     {"overrides_table", vc}, {"min_similarity", LogicalType::DOUBLE}},
	    "Suggests dictionary ids (ECLASS or your own catalogue) for uncoded attribute names or capability labels, as "
	    "a review queue in the shape of vocabulary_review_queue: wording, source, n_occurrences, n_trees_affected, "
	    "top1, p1, top2, p2, margin, similarity (the raw maxsim or cosine of top1: an absolute fit, since p is relative "
	    "to the dictionary and the best of poor candidates looks certain), fits (similarity >= min_similarity, NULL "
	    "without one) and impact = n_trees_affected x (1 - margin), or n_trees_affected when fits is false, highest "
	    "impact first. It runs "
	    "classify_to_catalogue: items_table (node_id, vec[, tree_id, source, label, tokens]; the wording is label, "
	    "else node_id) against dictionary_table (class_id = the dictionary id, vec[, exemplar_tokens]). mode "
	    "'late_interaction' (default; maxsim of the wording's tokens over example phrasings, the strongest signal) "
	    "or 'centroid'; temperature default 0.1; center_by_source default false. Wordings in overrides_table "
	    "(wording[, source], class_id) are reviewed and leave the queue. Confirm suggestions with "
	    "apply_vocabulary_review and use the result as attr_key or as overrides_table. No dictionary content ships "
	    "with the extension: bring your own vectors.",
	    {"SELECT wording, top1, p1, margin FROM suggest_semantic_id('uncoded_names', 'eclass_examples') LIMIT 20;"},
	    AAS_CATEGORIES);
}

//------------------------------------------------------------------------------
// Writers: trees_to_aas(nodes [, catalogue]), aas_export(...), aas_coverage_result(trace_table)
//------------------------------------------------------------------------------

//! A STRUCT field by name (case-insensitive), or nullptr.
const Value *StructField(const Value &v, const char *name) {
	if (v.IsNull() || v.type().id() != LogicalTypeId::STRUCT) {
		return nullptr;
	}
	auto &types = StructType::GetChildTypes(v.type());
	auto &values = StructValue::GetChildren(v);
	for (idx_t i = 0; i < types.size(); i++) {
		if (StringUtil::CIEquals(types[i].first, name)) {
			return &values[i];
		}
	}
	return nullptr;
}

string FieldString(const Value &v, const char *name) {
	auto f = StructField(v, name);
	return f && !f->IsNull() ? f->ToString() : string();
}

bool FieldDouble(const Value &v, const char *name, double &out) {
	auto f = StructField(v, name);
	if (!f || f->IsNull()) {
		return false;
	}
	out = f->DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	return true;
}

//! A node STRUCT as aas_to_trees and tree_from_json produce it (missing fields are empty).
TreeNodeRow NodeRowFrom(const Value &v) {
	TreeNodeRow n;
	n.tree_id = FieldString(v, "tree_id");
	n.node_id = FieldString(v, "node_id");
	n.parent_id = FieldString(v, "parent_id");
	n.node_kind = FieldString(v, "node_kind");
	n.edge_type = FieldString(v, "edge_type");
	n.label = FieldString(v, "label");
	n.context = FieldString(v, "context");
	n.class_id = FieldString(v, "class_id");
	if (!FieldDouble(v, "count", n.count)) {
		n.count = 1;
	}
	auto attrs = StructField(v, "attributes");
	if (attrs && !attrs->IsNull()) {
		for (auto &a : ListValue::GetChildren(*attrs)) {
			if (a.IsNull()) {
				continue;
			}
			TreeAttrRow r;
			r.attr_id = FieldString(a, "attr_id");
			r.name = FieldString(a, "name");
			r.value_text = FieldString(a, "value_text");
			r.dimension = FieldString(a, "dimension");
			r.unit = FieldString(a, "unit");
			bool has_lo = FieldDouble(a, "lo", r.lo), has_hi = FieldDouble(a, "hi", r.hi);
			r.has_value = has_lo || has_hi;
			if (!has_lo) {
				r.lo = r.hi;
			}
			if (!has_hi) {
				r.hi = r.lo;
			}
			r.role = FieldString(a, "role");
			r.bound = FieldString(a, "bound");
			r.bound_source = FieldString(a, "bound_source");
			r.qualifier = FieldString(a, "qualifier");
			r.category = FieldString(a, "category");
			r.attr_key = FieldString(a, "attr_key");
			n.attrs.push_back(std::move(r));
		}
	}
	return n;
}

void TreesToAasFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto nodes = args.data[0].GetValue(row);
		if (nodes.IsNull()) {
			result.SetValue(row, Value(LogicalType::VARCHAR));
			continue;
		}
		if (nodes.type().id() != LogicalTypeId::LIST) {
			throw InvalidInputException("trees_to_aas: nodes must be a LIST of node STRUCTs (aas_to_trees output)");
		}
		vector<TreeNodeRow> rows;
		for (auto &n : ListValue::GetChildren(nodes)) {
			if (!n.IsNull()) {
				rows.push_back(NodeRowFrom(n));
			}
		}
		vector<CatalogueEntry> catalogue;
		if (args.ColumnCount() > 1) {
			auto list = args.data[1].GetValue(row);
			if (!list.IsNull() && list.type().id() == LogicalTypeId::LIST) {
				for (auto &c : ListValue::GetChildren(list)) {
					auto label = FieldString(c, "label");
					catalogue.push_back({FieldString(c, "class_id"), label.empty() ? FieldString(c, "name") : label,
					                     FieldString(c, "definition")});
				}
			}
		}
		result.SetValue(row, Value(WriteTreesAsAas(rows, catalogue)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> TreesToAasBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("trees_to_aas");
	return nullptr;
}

void CoverageResultToAasFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto query = args.data[0].GetValue(row), candidate = args.data[1].GetValue(row);
		auto coverage = args.data[2].GetValue(row), trace = args.data[3].GetValue(row);
		if (query.IsNull() || candidate.IsNull() || trace.IsNull()) {
			result.SetValue(row, Value(LogicalType::VARCHAR));
			continue;
		}
		std::map<string, TraceRequirementEntry> by_req;
		vector<string> order;
		for (auto &t : ListValue::GetChildren(trace)) {
			auto req = FieldString(t, "req_node_id");
			if (!by_req.count(req)) {
				order.push_back(req);
				auto &r = by_req[req];
				r.req_node_id = req;
				r.matched_node_id = FieldString(t, "matched_node_id");
				r.matched_path = FieldString(t, "matched_path");
				r.label_rule = FieldString(t, "label_rule");
				FieldDouble(t, "label_sim", r.label_sim);
				FieldDouble(t, "req_score", r.req_score);
				FieldDouble(t, "count_needed", r.count_needed);
				FieldDouble(t, "count_available", r.count_available);
				auto gap = StructField(t, "is_gap");
				r.is_gap = gap && !gap->IsNull() && gap->GetValue<bool>();
			}
			if (FieldString(t, "attr_id").empty()) {
				continue;
			}
			TraceAttrEntry a;
			a.attr_id = FieldString(t, "attr_id");
			a.attr_name = FieldString(t, "attr_name");
			a.required = FieldString(t, "req_value");
			a.offered = FieldString(t, "prov_value");
			a.rule = FieldString(t, "rule");
			a.name_rule = FieldString(t, "name_rule");
			a.bound_source = FieldString(t, "bound_source");
			a.has_score = FieldDouble(t, "attr_score", a.score);
			by_req[req].attrs.push_back(std::move(a));
		}
		vector<TraceRequirementEntry> reqs;
		for (auto &id : order) {
			reqs.push_back(std::move(by_req[id]));
		}
		double cov = coverage.IsNull() ? 0.0 : coverage.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		result.SetValue(row, Value(WriteCoverageResult(query.ToString(), candidate.ToString(), cov, reqs)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> CoverageResultToAasBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("coverage_result_to_aas");
	return nullptr;
}

//! The node STRUCT list of a tree from nodes/attributes tables (optional columns as NULL).
string PackedTreesSQL(const CoverageParamReader &r, const string &nodes, const string &attrs, const string &filter) {
	auto n = [&](const char *col, const char *type) { return CastOrNullSQL(r.Has(nodes, col), col, type, "n"); };
	auto a = [&](const char *col, const char *type) { return CastOrNullSQL(r.Has(attrs, col), col, type, "a"); };
	string attrs_cte = attrs.empty()
	                       ? "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL AS attributes WHERE false"
	                       : StringUtil::Format(
	                             "SELECT CAST(a.tree_id AS VARCHAR) AS tree_id, CAST(a.node_id AS VARCHAR) AS node_id, "
	                             "list(struct_pack(attr_id := CAST(a.attr_id AS VARCHAR), name := %s, value_text := %s, "
	                             "dimension := %s, unit := %s, lo := %s, hi := %s, role := %s, bound := %s, "
	                             "bound_source := %s, category := %s, attr_key := %s) ORDER BY CAST(a.attr_id AS VARCHAR)) "
	                             "AS attributes FROM %s a GROUP BY ALL",
	                             a("name", "VARCHAR"), a("value_text", "VARCHAR"), a("dimension", "VARCHAR"),
	                             a("unit", "VARCHAR"), a("lo", "DOUBLE"), a("hi", "DOUBLE"), a("role", "VARCHAR"),
	                             a("bound", "VARCHAR"), a("bound_source", "VARCHAR"), a("category", "VARCHAR"),
	                             a("attr_key", "VARCHAR"), QuotedIdentifier(attrs));
	return StringUtil::Format(
	    "WITH packed_attrs AS (%s) "
	    "SELECT CAST(n.tree_id AS VARCHAR) AS tree_id, list(struct_pack(tree_id := CAST(n.tree_id AS VARCHAR), "
	    "node_id := CAST(n.node_id AS VARCHAR), parent_id := CAST(n.parent_id AS VARCHAR), node_kind := %s, "
	    "edge_type := %s, label := %s, class_id := %s, count := %s, attributes := pa.attributes) "
	    "ORDER BY CAST(n.node_id AS VARCHAR)) AS nodes "
	    "FROM %s n LEFT JOIN packed_attrs pa ON pa.tree_id = CAST(n.tree_id AS VARCHAR) "
	    "AND pa.node_id = CAST(n.node_id AS VARCHAR) %s GROUP BY 1",
	    attrs_cte, n("node_kind", "VARCHAR"), n("edge_type", "VARCHAR"), n("label", "VARCHAR"),
	    n("class_id", "VARCHAR"), n("count", "DOUBLE"), QuotedIdentifier(nodes), filter);
}

unique_ptr<TableRef> AasExportBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_export");
	CoverageParamReader r {context, input.named_parameters, "aas_export"};
	auto nodes = r.Table("nodes_table", "tree_nodes", {"tree_id", "node_id", "parent_id"});
	auto attrs = r.Table("attrs_table", "tree_attributes", {"tree_id", "node_id", "attr_id"});
	auto catalogue = r.Table("catalogue_table", "", {"class_id"});
	string filter;
	auto ids = r.Get("tree_ids");
	if (ids && !ids->IsNull()) {
		vector<string> list;
		for (auto &v : ListValue::GetChildren(*ids)) {
			if (!v.IsNull()) {
				list.push_back(v.ToString());
			}
		}
		filter = "WHERE list_contains(" + QuotedList(list) + ", CAST(n.tree_id AS VARCHAR))";
		if (list.empty()) {
			filter = "WHERE false";
		}
	}
	string catalogue_sql = "NULL";
	if (!catalogue.empty()) {
		catalogue_sql = StringUtil::Format(
		    "(SELECT list(struct_pack(class_id := CAST(class_id AS VARCHAR), label := %s, definition := %s)) FROM %s)",
		    r.Has(catalogue, "label") ? "CAST(label AS VARCHAR)"
		                              : (r.Has(catalogue, "name") ? "CAST(name AS VARCHAR)" : "NULL::VARCHAR"),
		    r.Has(catalogue, "definition") ? "CAST(definition AS VARCHAR)" : "NULL::VARCHAR",
		    QuotedIdentifier(catalogue));
	}
	auto sql = "SELECT tree_id, trees_to_aas(nodes, " + catalogue_sql + ") AS aas_json FROM (" +
	           PackedTreesSQL(r, nodes, attrs, filter) + ") ORDER BY tree_id";
	return CoverageSubquery(sql, context, "aas_export");
}

unique_ptr<TableRef> AasCoverageResultBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("aas_coverage_result");
	CoverageParamReader r {context, input.named_parameters, "aas_coverage_result"};
	auto trace = r.Positional(input, 0, "trace_table", {"query_id", "candidate_id", "req_node_id"});
	auto t = [&](const char *col, const char *type) { return CastOrNullSQL(r.Has(trace, col), col, type, "t"); };
	auto sql = StringUtil::Format(
	    "SELECT query_id, candidate_id, coverage_result_to_aas(query_id, candidate_id, "
	    "(SELECT avg(s) FROM (SELECT any_value(x.req_score) AS s FROM unnest(rows) AS u(x) GROUP BY x.req_node_id)), rows) "
	    "AS aas_json FROM (SELECT CAST(t.query_id AS VARCHAR) AS query_id, CAST(t.candidate_id AS VARCHAR) AS candidate_id, "
	    "list(struct_pack(req_node_id := CAST(t.req_node_id AS VARCHAR), matched_node_id := %s, matched_path := %s, "
	    "label_sim := %s, label_rule := %s, req_score := %s, is_gap := %s, count_needed := %s, count_available := %s, "
	    "attr_id := %s, attr_name := %s, req_value := %s, prov_value := %s, rule := %s, name_rule := %s, "
	    "bound_source := %s, attr_score := %s) ORDER BY CAST(t.req_node_id AS VARCHAR), %s) AS rows "
	    "FROM %s t GROUP BY ALL) ORDER BY query_id, candidate_id",
	    t("matched_node_id", "VARCHAR"), t("matched_path", "VARCHAR"), t("label_sim", "DOUBLE"),
	    t("label_rule", "VARCHAR"), t("req_score", "DOUBLE"), t("is_gap", "BOOLEAN"), t("count_needed", "DOUBLE"),
	    t("count_available", "DOUBLE"), t("attr_id", "VARCHAR"), t("attr_name", "VARCHAR"), t("req_value", "VARCHAR"),
	    t("prov_value", "VARCHAR"), t("rule", "VARCHAR"), t("name_rule", "VARCHAR"), t("bound_source", "VARCHAR"),
	    t("attr_score", "DOUBLE"), t("attr_id", "VARCHAR"), QuotedIdentifier(trace));
	return CoverageSubquery(sql, context, "aas_coverage_result");
}

void RegisterAasWriters(ExtensionLoader &loader) {
	auto any = LogicalType::ANY;
	CoverageScalarSpec trees {
	    "trees_to_aas",
	    LogicalType::VARCHAR,
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, TreesToAasFun),
	    compat_anofox::ScalarBind<&TreesToAasBind>(),
	    "Writes property trees as an AAS v3 Environment (Part 1 JSON): one shell per tree_id, the level below the root "
	    "as submodels, collections below. nodes is a LIST of node STRUCTs as aas_to_trees and tree_from_json produce "
	    "them. Items whose attributes carry a role (required, provided) become IDTA-02020 CapabilityContainers "
	    "(Required / Offered, class as supplementalSemanticId, properties as PropertyRange / PropertyProperty); other "
	    "items SubmodelElementCollections with their Property / Range elements. Bounds become ranges (at_least: min "
	    "only, at_most: max only), equals a levelType nom; variant groups and counts use the DataZoo AAS profile; "
	    "ComposedOf parts are written next to their composite with CapabilityComposedOf relations. Every attr_key "
	    "becomes a ConceptDescription (IEC 61360 name and unit); catalogue (LIST of STRUCT(class_id, label, "
	    "definition)) adds class entries. A tree read from AAS reads back the same: aas_to_trees(trees_to_aas(t)).",
	    AAS_CATEGORIES};
	trees.special_null_handling = true;
	RegisterCoverageScalar(
	    loader, trees,
	    {{{any}, {"nodes"}, {"SELECT trees_to_aas(aas_to_trees(content)) FROM read_text('offer.aas.json');"}, ""},
	     {{any, any},
	      {"nodes", "catalogue"},
	      {"SELECT trees_to_aas(aas_to_trees(content), (SELECT list(c) FROM catalogue c)) FROM read_text('offer.aas.json');"},
	      ""}});

	CoverageScalarSpec result {
	    "coverage_result_to_aas",
	    LogicalType::VARCHAR,
	    DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, CoverageResultToAasFun),
	    compat_anofox::ScalarBind<&CoverageResultToAasBind>(),
	    "The explanation of one ranking as an AAS Submodel (Part 1 JSON) 'CoverageResult': Query, Candidate, "
	    "Coverage, GapCount and a list of Requirements with matched node and path, label similarity and rule, counts, "
	    "score, IsGap and per attribute the required and offered value, rule and score. semanticIds in the "
	    "https://data-zoo.de/anofox/aas/CoverageResult/ namespace. rows are tree_coverage_trace rows (STRUCTs); "
	    "aas_coverage_result builds them from a trace table.",
	    AAS_CATEGORIES};
	result.special_null_handling = true;
	RegisterCoverageScalar(loader, result,
	                       {{{LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::DOUBLE, any},
	                         {"query_id", "candidate_id", "coverage", "rows"},
	                         {"SELECT * FROM aas_coverage_result('trace');"},
	                         ""}});

	auto vc = LogicalType::VARCHAR;
	RegisterCoverageTableFunction(
	    loader, "aas_export", {}, {}, AasExportBindReplace,
	    {{"nodes_table", vc}, {"attrs_table", vc}, {"catalogue_table", vc}, {"tree_ids", LogicalType::LIST(vc)}},
	    "Exports property trees as AAS: one row per tree with tree_id and aas_json, an AAS v3 Environment (Part 1 "
	    "JSON) as trees_to_aas writes it. Trees come from nodes_table / attrs_table (default 'tree_nodes' / "
	    "'tree_attributes'), tree_ids selects some; catalogue_table (class_id[, label | name][, definition]) adds "
	    "ConceptDescriptions for the classes.",
	    {"SELECT tree_id, aas_json FROM aas_export(tree_ids := ['RFQ-2026-117']);"}, AAS_CATEGORIES);
	RegisterCoverageTableFunction(
	    loader, "aas_coverage_result", {vc}, {"trace_table"}, AasCoverageResultBindReplace, {},
	    "The rankings of a tree_coverage_trace result as AAS: one row per query and candidate with aas_json, a "
	    "'CoverageResult' Submodel (coverage_result_to_aas). coverage is the mean requirement score of the trace.",
	    {"CREATE TABLE trace AS SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := ['OFF-A']);",
	     "SELECT aas_json FROM aas_coverage_result('trace');"},
	    AAS_CATEGORIES);
}

//------------------------------------------------------------------------------
// attrs_satisfying(attr_key, requirement): a unit- and direction-aware filter over loaded
// attributes (GitHub #35), a thin wrapper over value_satisfies
//------------------------------------------------------------------------------

unique_ptr<TableRef> AttrsSatisfyingBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("attrs_satisfying");
	CoverageParamReader r {context, input.named_parameters, "attrs_satisfying"};
	if (input.inputs.size() < 2 || input.inputs[0].IsNull()) {
		throw BinderException("attrs_satisfying: attr_key must not be NULL");
	}
	if (input.inputs[1].IsNull()) {
		throw BinderException("attrs_satisfying: requirement must not be NULL (e.g. parse_quantity('at least 7.6 A'))");
	}
	auto key = QuoteSQLStringLiteral(input.inputs[0].ToString());
	auto &requirement = input.inputs[1];
	auto req = "CAST(" + requirement.ToSQLString() + " AS " + requirement.type().ToString() + ")";
	auto attrs = r.Table("attrs_table", "tree_attributes", {"tree_id", "node_id", "attr_id", "lo", "hi", "dimension"});
	bool keyed = r.Has(attrs, "attr_key");
	bool named = r.Has(attrs, "name");
	auto name = CastOrNullSQL(named, "name", "VARCHAR", "a");
	auto attr_key = CastOrNullSQL(keyed, "attr_key", "VARCHAR", "a");
	auto unit = CastOrNullSQL(r.Has(attrs, "unit"), "unit", "VARCHAR", "a");
	auto opt = [&](const char *param, bool numeric) {
		auto v = r.Get(param);
		if (!v || v->IsNull()) {
			return string(numeric ? "NULL::DOUBLE" : "NULL::VARCHAR");
		}
		return numeric ? DoubleLiteral(r.Dbl(param, 0)) : QuoteSQLStringLiteral(r.Str(param));
	};
	// keyed rows match by semantic_key (any release, IRDI or IRI); unkeyed rows by name
	auto match = StringUtil::Format(
	    "(%s IS NOT NULL AND semantic_key(%s) = semantic_key(%s)) OR (%s IS NULL AND lower(%s) = lower(trim(%s)))",
	    attr_key, attr_key, key, attr_key, name, key);
	auto sql = StringUtil::Format(R"(
		SELECT tree_id, node_id, attr_id, name, attr_key, value, s.score AS score, s.distance AS distance,
		       s.rule AS rule, s.score >= 1 - 1e-9 AS satisfied
		FROM (
			SELECT CAST(a.tree_id AS VARCHAR) AS tree_id, CAST(a.node_id AS VARCHAR) AS node_id,
			       CAST(a.attr_id AS VARCHAR) AS attr_id, %s AS name, %s AS attr_key,
			       CASE WHEN a.lo IS NULL THEN NULL
			            WHEN a.lo = a.hi THEN printf('%%g', CAST(a.lo AS DOUBLE))
			            ELSE printf('%%g..%%g', CAST(a.lo AS DOUBLE), CAST(a.hi AS DOUBLE)) END
			       || CASE WHEN a.lo IS NULL OR %s IS NULL OR %s = '1' THEN '' ELSE ' ' || %s END AS value,
			       value_satisfies(%s, struct_pack(lo := CAST(a.lo AS DOUBLE), hi := CAST(a.hi AS DOUBLE),
			                                       dimension := CAST(a.dimension AS VARCHAR),
			                                       unit := CASE WHEN a.dimension = 'currency' THEN %s END),
			                       %s, %s, %s, %s) AS s
			FROM %s a WHERE %s)
		ORDER BY score DESC, tree_id, node_id, attr_id)",
	    name, attr_key, unit, unit, unit, req, unit, opt("scale", false), opt("tol", true), opt("soft_band", true),
	    opt("default_bound", false), QuotedIdentifier(attrs), match);
	return CoverageSubquery(sql, context, "attrs_satisfying");
}

void RegisterAttrsSatisfying(ExtensionLoader &loader) {
	auto vc = LogicalType::VARCHAR;
	auto dbl = LogicalType::DOUBLE;
	RegisterCoverageTableFunction(
	    loader, "attrs_satisfying", {vc, LogicalType::ANY}, {"attr_key", "requirement"}, AttrsSatisfyingBindReplace,
	    {{"attrs_table", vc}, {"tol", dbl}, {"soft_band", dbl}, {"scale", vc}, {"default_bound", vc}},
	    "Which attributes satisfy a requirement: every row of attrs_table (default 'tree_attributes') whose attr_key "
	    "equals attr_key after semantic_key (any release, IRDI or ECLASS IRI; rows without attr_key match by name, "
	    "case-insensitively), checked with value_satisfies against requirement (a STRUCT such as parse_quantity("
	    "'mindestens 7637,1 mA')): direction (at_least, at_most, within, equals), units and the dimension gate (5 V "
	    "never satisfies 5 A). Returns tree_id, node_id, attr_id, name, attr_key, value (the offered value in base "
	    "units), score, distance, rule and satisfied (score 1), best first. tol, soft_band, scale and default_bound "
	    "as in value_satisfies; tol := 0, soft_band := 0 for hard limits. A unit- and direction-aware query over "
	    "AAS data that the AAS Query Language does not offer.",
	    {"SELECT tree_id, node_id, value FROM attrs_satisfying('0173-1#02-BAA120#008', parse_quantity('mindestens "
	     "7637,1 mA', 'de')) WHERE satisfied;"},
	    AAS_CATEGORIES);
}

} // namespace

void RegisterAasFunctions(ExtensionLoader &loader) {
	RegisterSemanticKey(loader);
	RegisterAasReader(loader);
	RegisterAasIdB64Url(loader);
	RegisterAttrsSatisfying(loader);
	RegisterAasWriters(loader);
	RegisterAasQuality(loader);
	RegisterSuggestSemanticId(loader);
}

} // namespace anofox
} // namespace duckdb
