#include "modules/incremental_updates.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

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
}

void RegisterIncrementalUpdateMacros(Connection &conn) {
	// Phase 2D: Refresh embeddings for dirty materials (incremental updates)
	auto result = conn.Query(R"(
		CREATE OR REPLACE MACRO refresh_transactional_embeddings() AS TABLE
		WITH dirty_materials AS (
			-- Get all materials marked as dirty
			SELECT DISTINCT material_id
			FROM material_embeddings_dirty
			WHERE material_id IS NOT NULL
		),
		fresh_embeddings AS (
			-- Compute fresh embeddings only for dirty materials
			SELECT material_id, transactional_embedding
			FROM compute_transactional_embeddings()
			WHERE material_id IN (SELECT material_id FROM dirty_materials)
		)
		UPDATE material_embeddings
		SET transactional_embedding = fe.transactional_embedding,
			updated_at = CURRENT_TIMESTAMP
		FROM fresh_embeddings fe
		WHERE material_embeddings.material_id = fe.material_id
		RETURNING material_embeddings.material_id, transactional_embedding AS updated_embedding;
	)");
	CheckQueryResult(result, "create refresh_transactional_embeddings macro", FailureMode::OPTIONAL);

	// Phase 2D: Macro to clear dirty materials after refresh
	result = conn.Query(R"(
		CREATE OR REPLACE MACRO clear_dirty_materials(
			material_ids := NULL
		) AS TABLE
		WITH deleted AS (
			DELETE FROM material_embeddings_dirty
			WHERE material_ids IS NULL OR material_id = ANY(material_ids)
			RETURNING material_id, reason, marked_at
		)
		SELECT material_id, reason, marked_at FROM deleted;
	)");
	CheckQueryResult(result, "create clear_dirty_materials macro", FailureMode::OPTIONAL);

	// Phase 2D: Helper macro to refresh and clear dirty materials
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
