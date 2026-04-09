#include "modules/vss_integration.hpp"
#include "core/error_handling.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "telemetry.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Helper: Parse SQL to SubqueryRef
//------------------------------------------------------------------------------

static unique_ptr<SubqueryRef> ParseSubquery(const string &query, const ParserOptions &options, const string &err_msg) {
	Parser parser(options);
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException(err_msg);
	}
	auto &select_stmt = parser.statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

//------------------------------------------------------------------------------
// VSS Extension Initialization
//------------------------------------------------------------------------------

void InitializeVSSIntegration(Connection &conn) {
	// Best-effort loading strategy for restricted/offline environments:
	// 1) Try loading an already-installed extension.
	// 2) If that fails, try installing from repository, then load again.
	// All steps are optional to avoid hard-failing extension load.
	auto vss_result = conn.Query("LOAD vss;");
	if (vss_result->HasError()) {
		CheckQueryResult(vss_result, "load VSS extension", FailureMode::OPTIONAL);

		auto install_result = conn.Query("INSTALL vss;");
		CheckQueryResult(install_result, "install VSS extension", FailureMode::OPTIONAL);

		vss_result = conn.Query("LOAD vss;");
		CheckQueryResult(vss_result, "load VSS extension after install", FailureMode::OPTIONAL);
	}

	// HNSW persistence is experimental - continue if unavailable
	vss_result = conn.Query("SET hnsw_enable_experimental_persistence = true;");
	CheckQueryResult(vss_result, "enable HNSW persistence", FailureMode::OPTIONAL);
}

void CreateEmbeddingTables(Connection &conn) {
	// Create main embeddings table with Phase 3 schema (structural + textual + transactional)
	auto schema_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings (
			material_id VARCHAR PRIMARY KEY,
			jaccard_embedding FLOAT[128],
			structural_embedding FLOAT[256],
			textual_embedding FLOAT[384],
			transactional_embedding FLOAT[128],
			combined_embedding FLOAT[768],
			updated_at TIMESTAMP DEFAULT NULL,
			num_components INTEGER
		)
	)");
	CheckQueryResult(schema_result, "create material_embeddings table");

	// Create statistics table for transactional embedding normalization (Phase 2A)
	auto stats_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS transactional_embedding_statistics (
			feature_name VARCHAR PRIMARY KEY,
			feature_index INTEGER,
			feature_category VARCHAR,
			mean_value DOUBLE,
			std_value DOUBLE,
			min_value DOUBLE,
			max_value DOUBLE,
			num_samples INTEGER,
			updated_at TIMESTAMP DEFAULT NULL,
			version INTEGER DEFAULT 1
		)
	)");
	CheckQueryResult(stats_result, "create transactional_embedding_statistics table");

	// Create feature mapping table for transactional embeddings (Phase 2A)
	auto mapping_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS transactional_feature_mapping (
			feature_index INTEGER PRIMARY KEY,
			feature_name VARCHAR,
			category VARCHAR,
			description VARCHAR,
			is_advanced BOOLEAN DEFAULT FALSE,
			created_at TIMESTAMP DEFAULT NULL
		)
	)");
	CheckQueryResult(mapping_result, "create transactional_feature_mapping table");

	auto tracking_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings_dirty (
			material_id VARCHAR PRIMARY KEY,
			reason VARCHAR,
			marked_at TIMESTAMP DEFAULT NULL
		)
	)");
	CheckQueryResult(tracking_result, "create material_embeddings_dirty table");

	// Schema migration: Handle old schema with wl_embedding
	auto migration_check = conn.Query(R"(
		SELECT COUNT(*) FROM information_schema.columns
		WHERE table_name = 'material_embeddings' AND column_name = 'wl_embedding'
	)");
	if (!migration_check || migration_check->HasError()) {
		CheckQueryResult(migration_check, "check wl_embedding migration state", FailureMode::OPTIONAL);
		return;
	}

	if (migration_check->GetValue(0, 0).GetValue<int64_t>() > 0) {
		auto migration_result = conn.Query(R"(
			UPDATE material_embeddings
			SET structural_embedding = wl_embedding
			WHERE wl_embedding IS NOT NULL AND structural_embedding IS NULL
		)");
		CheckQueryResult(migration_result, "migrate wl_embedding to structural_embedding", FailureMode::OPTIONAL);

		auto drop_result = conn.Query(R"(
			ALTER TABLE material_embeddings
			DROP COLUMN IF EXISTS wl_embedding
		)");
		CheckQueryResult(drop_result, "drop old wl_embedding column", FailureMode::OPTIONAL);
	}
}

void CreateHNSWIndexes(Connection &conn) {
	// HNSW indexes are performance optimizations - continue if creation fails

	auto idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS jaccard_idx ON material_embeddings
		USING HNSW(jaccard_embedding) WITH (metric = 'l2sq')
	)");
	CheckQueryResult(idx_result, "create jaccard HNSW index", FailureMode::OPTIONAL);

	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_structural_idx ON material_embeddings
		USING HNSW(structural_embedding) WITH (metric = 'l2sq')
	)");
	CheckQueryResult(idx_result, "create structural HNSW index", FailureMode::OPTIONAL);

	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_textual_idx ON material_embeddings
		USING HNSW(textual_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create textual HNSW index", FailureMode::OPTIONAL);

	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_combined_idx ON material_embeddings
		USING HNSW(combined_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create combined HNSW index", FailureMode::OPTIONAL);

	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS wl_idx ON material_embeddings
		USING HNSW(structural_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create WL legacy HNSW index", FailureMode::OPTIONAL);
}

//------------------------------------------------------------------------------
// compute_jaccard_embeddings TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> ComputeJaccardEmbeddingsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("compute_jaccard_embeddings");

	// Parameters:
	// Named: bom_table (VARCHAR, default 'bom_items')

	string bom_table = "bom_items";

	if (input.named_parameters.count("bom_table")) {
		bom_table = input.named_parameters.at("bom_table").GetValue<string>();
	}
	bom_table = ValidateSQLIdentifierPath(bom_table, "bom_table");

	string sql = StringUtil::Format(R"(
		WITH
			material_components AS (
				SELECT
					parent_id AS material_id,
					COUNT(DISTINCT child_id)::INTEGER AS num_components,
					list(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table('%s')
				GROUP BY parent_id
			),
			minhash_by_seed AS (
				SELECT
					mc.material_id,
					t.seed,
					MIN(CAST(ABS(HASH(c || ':' || t.seed::VARCHAR)) AS FLOAT)) AS minhash_value,
					mc.num_components
				FROM material_components mc
				CROSS JOIN generate_series(0, 127) AS t(seed)
				CROSS JOIN UNNEST(mc.components) AS u(c)
				GROUP BY mc.material_id, t.seed, mc.num_components
			)
		SELECT
			material_id,
			seed,
			minhash_value,
			num_components
		FROM minhash_by_seed
		ORDER BY material_id, seed
	)",
	                                bom_table);

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse compute_jaccard_embeddings query");
}

//------------------------------------------------------------------------------
// check_statistics_freshness TableFunction
//------------------------------------------------------------------------------

static unique_ptr<TableRef> CheckStatisticsFreshnessBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().CaptureFunctionExecution("check_statistics_freshness");

	string sql = R"(
		SELECT
			COALESCE(COUNT(*), 0) AS stat_count,
			COALESCE(MAX(num_samples), 0) AS max_samples,
			COALESCE(MAX(version), 0) AS current_version,
			COALESCE(MAX(updated_at), CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '8 days') AS last_updated,
			(COALESCE(COUNT(*), 0) >= 30
			 AND COALESCE(MAX(updated_at), CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '8 days') > CAST(CURRENT_TIMESTAMP AS TIMESTAMP) - INTERVAL '7 days') AS is_fresh
		FROM transactional_embedding_statistics
	)";

	return ParseSubquery(sql, context.GetParserOptions(), "Failed to parse check_statistics_freshness query");
}

//------------------------------------------------------------------------------
// Module Registration
//------------------------------------------------------------------------------

void RegisterEmbeddingFunctions(ExtensionLoader &loader) {
	// compute_jaccard_embeddings
	{
		TableFunction compute_jaccard("compute_jaccard_embeddings", {}, nullptr, nullptr);
		compute_jaccard.bind_replace = ComputeJaccardEmbeddingsBindReplace;
		compute_jaccard.named_parameters["bom_table"] = LogicalType::VARCHAR;

		CreateTableFunctionInfo info(compute_jaccard);
		FunctionDescription desc;
		desc.description = "Computes 128-dimensional MinHash embeddings from BOM component sets for all materials "
		                   "and upserts them into material_embeddings. Each material's embedding is derived from "
		                   "its set of direct child component material numbers. Requires bom_table with columns "
		                   "matnr and idnrk.";
		desc.examples    = {"SELECT * FROM compute_jaccard_embeddings();",
		                    "SELECT * FROM compute_jaccard_embeddings(bom_table := 'sap_stpo');"};
		desc.categories  = {"embeddings", "structural"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}

	// check_statistics_freshness
	{
		TableFunction check_freshness("check_statistics_freshness", {}, nullptr, nullptr);
		check_freshness.bind_replace = CheckStatisticsFreshnessBindReplace;

		CreateTableFunctionInfo info(check_freshness);
		FunctionDescription desc;
		desc.description = "Returns a single-row summary of the transactional_embedding_statistics table including "
		                   "stat_count, max_samples, current_version, last_updated, and is_fresh. "
		                   "is_fresh is TRUE when >= 30 statistics rows exist and last_updated is within 7 days.";
		desc.examples    = {"SELECT * FROM check_statistics_freshness();"};
		desc.categories  = {"embeddings", "transactional", "diagnostics"};
		info.descriptions.push_back(std::move(desc));
		loader.RegisterFunction(std::move(info));
	}
}

} // namespace anofox
} // namespace duckdb
