#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Pivot classifier (coverage matching, HLD §4.4)
//------------------------------------------------------------------------------

// Registers maxsim, bhattacharyya, calibrated_cosine (scalars), cosine_floor and
// classify_to_catalogue (table functions, inlined SQL).
void RegisterPivotClassifierFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
