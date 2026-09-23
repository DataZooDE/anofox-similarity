#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

// Register BOM utility macros:
// - aggregate_material_components: Centralize material component aggregation logic
// - filter_recent_movements: Centralize time window filtering for goods movements
void RegisterBOMUtilityMacros(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
