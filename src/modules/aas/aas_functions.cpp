// SQL registration for the AAS integration. Only the SQL surface lives here: argument
// handling, documentation and telemetry. The logic is in the format-neutral modules next to it.
#include "modules/aas/aas_functions.hpp"
#include "modules/aas/aas_model.hpp"
#include "modules/aas/aas_tree_mapper.hpp"
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
	AasMapping Map(DataChunk &args, idx_t row) {
		AasMapOptions options;
		if (args.ColumnCount() > 1) {
			auto id = args.data[1].GetValue(row);
			options.tree_id = id.IsNull() ? string() : id.ToString();
		}
		if (args.ColumnCount() > 2) {
			auto units = args.data[2].GetValue(row);
			if (!has_units || !Value::NotDistinctFrom(units, units_value)) {
				unit_table = UnitTable::WithUserUnits(units);
				units_value = units;
				has_units = true;
			}
			options.units = &unit_table;
		} else {
			options.units = &UnitTable::Seed();
		}
		return MapAasToTrees(ReadAas(ContentOf(args.data[0].GetValue(row))), options);
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
	    "Store it as a table and pass it to tree_coverage as class_hierarchy with a generalization_credit, so a "
	    "requirement for a specific capability gets partial credit from a more general offer and vice versa. Same "
	    "arguments as aas_to_trees.",
	    AAS_CATEGORIES};
	hierarchy.special_null_handling = true;
	RegisterCoverageScalar(loader, hierarchy, ContentOverloads("aas_class_hierarchy"));
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

} // namespace

void RegisterAasFunctions(ExtensionLoader &loader) {
	RegisterSemanticKey(loader);
	RegisterAasReader(loader);
	RegisterAasIdB64Url(loader);
}

} // namespace anofox
} // namespace duckdb
