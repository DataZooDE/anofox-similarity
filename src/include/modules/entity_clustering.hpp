#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Entity clustering (coverage matching, HLD §4.9)
//------------------------------------------------------------------------------

void RegisterEntityClusteringFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
