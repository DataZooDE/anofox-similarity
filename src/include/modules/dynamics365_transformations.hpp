#pragma once

#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Dynamics 365 BOM Transformations Module
//------------------------------------------------------------------------------

// Registers Dynamics 365 transformation macros for converting ERP structures
// to the universal BOM schema:
//   - dynamics365_to_materials(invent_table)
//   - dynamics365_to_bom_header(bom_table, bom_version)
//   - dynamics365_to_bom_component(bom_lines)
//
// These macros enable integration with Microsoft Dynamics 365 SCM systems,
// converting D365 InventTable, BOMTable, BOMVersion, and BOM structures
// to the universal BOM schema for multi-ERP similarity analysis.
//
// Parameters:
//   conn - DuckDB connection for macro registration
void RegisterDynamics365TransformationMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
