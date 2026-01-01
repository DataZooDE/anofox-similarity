#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Predecessor Inference Module - infer_predecessors temporal analysis
//------------------------------------------------------------------------------

// Registers predecessor inference macros using anti-correlation temporal analysis
void RegisterPredecessorInferenceMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
