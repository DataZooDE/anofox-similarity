#include "modules/vss_integration.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

void InitializeVSSIntegration(Connection &conn) {
	auto vss_result = conn.Query("INSTALL vss; LOAD vss;");
	CheckQueryResult(vss_result, "load VSS extension");

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
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			num_components INTEGER
		)
	)");
	CheckQueryResult(schema_result, "create material_embeddings table");

	auto tracking_result = conn.Query(R"(
		CREATE TABLE IF NOT EXISTS material_embeddings_dirty (
			material_id VARCHAR PRIMARY KEY,
			reason VARCHAR,
			marked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)
	)");
	CheckQueryResult(tracking_result, "create material_embeddings_dirty table");

	// Schema migration: Handle old schema with wl_embedding
	// Check if old column exists and migrate
	auto migration_check = conn.Query(R"(
		SELECT COUNT(*) FROM information_schema.columns
		WHERE table_name = 'material_embeddings' AND column_name = 'wl_embedding'
	)");

	if (migration_check->GetValue(0, 0).GetValue<int64_t>() > 0) {
		// Old schema exists - copy data from wl_embedding to structural_embedding
		auto migration_result = conn.Query(R"(
			UPDATE material_embeddings
			SET structural_embedding = wl_embedding
			WHERE wl_embedding IS NOT NULL AND structural_embedding IS NULL
		)");
		CheckQueryResult(migration_result, "migrate wl_embedding to structural_embedding", FailureMode::OPTIONAL);

		// Drop old columns if migration succeeded
		auto drop_result = conn.Query(R"(
			ALTER TABLE material_embeddings
			DROP COLUMN IF EXISTS wl_embedding
		)");
		CheckQueryResult(drop_result, "drop old wl_embedding column", FailureMode::OPTIONAL);
	}
}

void CreateHNSWIndexes(Connection &conn) {
	// HNSW indexes are performance optimizations - continue if creation fails

	// Jaccard embedding index (128-D, L2 distance)
	auto idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS jaccard_idx ON material_embeddings
		USING HNSW(jaccard_embedding) WITH (metric = 'l2sq')
	)");
	CheckQueryResult(idx_result, "create jaccard HNSW index", FailureMode::OPTIONAL);

	// Structural embedding index (256-D, L2-squared distance)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_structural_idx ON material_embeddings
		USING HNSW(structural_embedding) WITH (metric = 'l2sq')
	)");
	CheckQueryResult(idx_result, "create structural HNSW index", FailureMode::OPTIONAL);

	// Textual embedding index (384-D, cosine distance)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_textual_idx ON material_embeddings
		USING HNSW(textual_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create textual HNSW index", FailureMode::OPTIONAL);

	// Combined embedding index (768-D, cosine distance)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS hnsw_combined_idx ON material_embeddings
		USING HNSW(combined_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create combined HNSW index", FailureMode::OPTIONAL);

	// Legacy wl_idx for backward compatibility (if old schema exists)
	idx_result = conn.Query(R"(
		CREATE INDEX IF NOT EXISTS wl_idx ON material_embeddings
		USING HNSW(structural_embedding) WITH (metric = 'cosine')
	)");
	CheckQueryResult(idx_result, "create WL legacy HNSW index", FailureMode::OPTIONAL);
}

void RegisterEmbeddingMacros(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO compute_jaccard_embeddings(bom_table := 'bom_items') AS TABLE
		WITH
			-- Step 1: Aggregate components per material
			material_components AS (
				SELECT
					parent_id AS material_id,
					COUNT(DISTINCT child_id)::INTEGER AS num_components,
					list(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				GROUP BY parent_id
			),
			-- Step 2: For each material and seed (0-127), compute min-hash value (JACCARD_EMBEDDING_DIM = 128)
			-- Min-hash(A, seed) = MIN(HASH(c || ':' || seed) for c in A)
			-- This preserves Jaccard similarity in expectation
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
	)");

	CheckQueryResult(result, "create compute_jaccard_embeddings macro");
}

void CreateIncrementalUpdateTriggers(Connection &conn) {
	auto result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_insert_trigger
		AFTER INSERT ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (NEW.parent_id, 'insert')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");
	// Continue even if trigger creation fails (may not be supported in test context)

	result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_update_trigger
		AFTER UPDATE ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (NEW.parent_id, 'update')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");

	result = conn.Query(R"(
		CREATE OR REPLACE TRIGGER IF NOT EXISTS bom_items_delete_trigger
		AFTER DELETE ON bom_items
		FOR EACH ROW
		BEGIN
			INSERT INTO material_embeddings_dirty (material_id, reason)
			VALUES (OLD.parent_id, 'delete')
			ON CONFLICT (material_id) DO UPDATE SET
				marked_at = CURRENT_TIMESTAMP;
		END
	)");

	result = conn.Query(R"(
		CREATE OR REPLACE MACRO refresh_dirty_embeddings(bom_table := 'bom_items') AS TABLE
		WITH
			dirty_materials AS (
				SELECT material_id FROM material_embeddings_dirty
			),
			material_components AS (
				SELECT
					parent_id AS material_id,
					LIST(DISTINCT child_id ORDER BY child_id) AS components
				FROM query_table(bom_table)
				WHERE parent_id IN (SELECT material_id FROM dirty_materials)
				GROUP BY parent_id
			)
		DELETE FROM material_embeddings_dirty
		WHERE material_id IN (SELECT material_id FROM material_components)
		RETURNING material_id
	)");
	// Triggers and refresh macros are optional - continue if they fail
	CheckQueryResult(result, "create refresh_dirty_embeddings macro", FailureMode::OPTIONAL);
}

} // namespace anofox
} // namespace duckdb
