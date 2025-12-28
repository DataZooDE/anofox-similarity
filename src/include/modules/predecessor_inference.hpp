#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Predecessor Inference Module - infer_predecessors temporal analysis
//------------------------------------------------------------------------------

// Registers predecessor inference macro using anti-correlation temporal analysis
void RegisterPredecessorInferenceMacro(Connection &conn);

} // namespace anofox
} // namespace duckdb
