#pragma once

#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// DuckPGQ Property Graph Integration Module
//------------------------------------------------------------------------------

// Registers DuckPGQ property graph macros for BOM traversal with soft dependency:
//   - check_duckpgq_available() - Returns TRUE if DuckPGQ loaded, FALSE otherwise
//   - create_bom_property_graph(source_system) - Creates property graph from BOM
//   - bom_explosion_1level(material_id) - Single-level BOM explosion
//   - bom_explosion_multilevel(material_id, max_depth) - Multi-level BOM explosion
//   - bom_where_used(material_id) - Reverse BOM (where-used query)
//   - bom_common_components(material_1, material_2) - Find common components
//
// These macros enable efficient graph-based BOM traversal:
// - When DuckPGQ available: Uses SQL/PGQ graph queries (~5-10x faster on deep BOMs)
// - When DuckPGQ unavailable: Gracefully falls back to SQL recursive CTEs
//
// Parameters:
//   conn - DuckDB connection for macro registration
void RegisterCheckDuckPGQMacro(Connection &conn);

// Initializes DuckPGQ integration (soft dependency):
// - Attempts to load DuckPGQ extension if available
// - Continues gracefully if DuckPGQ not found
// - Returns success/failure status
void InitializeDuckPGQIntegration(Connection &conn);

// Registers property graph creation macros
void RegisterPropertyGraphMacros(Connection &conn);

// Registers BOM traversal macros (graph-based and SQL fallback)
void RegisterBOMTraversalMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
