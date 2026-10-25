#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Sub-linear recall accelerators (coverage matching, HLD §4.7)
//------------------------------------------------------------------------------

void RegisterRecallAcceleratorFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
