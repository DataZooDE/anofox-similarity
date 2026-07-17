#include "modules/incremental_updates.hpp"
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

// NOTE ON DIRTY-TRACKING / INCREMENTAL UPDATES
// --------------------------------------------
// DuckDB has no CREATE TRIGGER support, and table macros (CREATE MACRO ... AS TABLE) may not
// contain DML (UPDATE/DELETE/INSERT). The previous implementation of this module relied on both,
// so its triggers and its refresh_transactional_embeddings / clear_dirty_materials /
// refresh_dirty_embeddings macros always failed to register — silently, because the failures were
// swallowed. There was therefore no mechanism that could ever populate material_embeddings_dirty,
// making the whole subsystem inert. Rather than ship functions that never register, we no longer
// create them. Callers who need incremental refresh should recompute embeddings for the changed
// materials explicitly (e.g. INSERT OR REPLACE INTO material_embeddings SELECT ... FROM
// compute_transactional_embeddings(...)). These entry points are retained as no-ops so the
// extension load sequence is unchanged.

void CreateIncrementalUpdateTriggers(Connection &conn) {
	// Intentionally a no-op: DuckDB does not support CREATE TRIGGER.
	(void)conn;
}

void RegisterIncrementalUpdateMacros(Connection &conn) {
	// Intentionally a no-op: DuckDB table macros cannot contain DML, so dirty-tracking refresh
	// macros cannot be expressed here. See the note above.
	(void)conn;
}

} // namespace anofox
} // namespace duckdb
