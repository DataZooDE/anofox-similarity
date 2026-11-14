#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/connection.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Dynamics 365 BOM Transformations Module
//------------------------------------------------------------------------------

// Registers the Dynamics 365 table functions that convert native D365 tables (table field or OData
// entity names) to the universal BOM schema:
//   - dynamics365_to_materials(invent_table)
//   - dynamics365_to_bom_header(bom_version, bom_table, reference_date)
//   - dynamics365_to_bom_component(bom_lines, bom_version, reference_date)
// One BOM per item on reference_date; see src/modules/dynamics365_transformations.cpp.
void RegisterDynamics365TransformationMacros(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
