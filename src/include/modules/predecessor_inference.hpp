#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Predecessor Inference Module - infer_predecessors temporal analysis
//------------------------------------------------------------------------------

// Registers predecessor inference table functions using anti-correlation temporal analysis
void RegisterPredecessorInferenceFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
