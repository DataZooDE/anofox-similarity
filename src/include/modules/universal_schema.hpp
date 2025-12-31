#pragma once

#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Universal BOM Schema Module - ERP-agnostic BOM structures
//------------------------------------------------------------------------------

// Creates universal BOM schema tables (materials, bom_header, bom_component)
// Supports SAP, Dynamics 365, and other ERP systems with standardized schema
// Parameters:
//   conn - DuckDB connection
//   drop_existing - If true, drops existing tables before creating (for clean migration)
void CreateUniversalBOMSchema(Connection &conn, bool drop_existing = false);

// Registers conversion macros between flat bom_items and universal schema
// Macros:
//   bom_to_items(header_table, component_table) - Converts universal to flat format
//   items_to_bom(items_table, source_system) - Converts flat to universal format
void RegisterBOMConversionMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
