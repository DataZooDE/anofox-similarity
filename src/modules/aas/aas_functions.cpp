// SQL registration for the AAS integration. Only the SQL surface lives here: argument
// handling, documentation and telemetry. The logic is in the format-neutral modules next to it.
#include "modules/aas/aas_functions.hpp"
#include "modules/aas/semantic_key.hpp"
#include "modules/coverage_sql.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "duckdb_compat.hpp"
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

} // namespace

void RegisterAasFunctions(ExtensionLoader &loader) {
	RegisterSemanticKey(loader);
}

} // namespace anofox
} // namespace duckdb
