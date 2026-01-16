#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

// Register BOM utility macros:
// - aggregate_material_components: Centralize material component aggregation logic
// - filter_recent_movements: Centralize time window filtering for goods movements
void RegisterBOMUtilityMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
