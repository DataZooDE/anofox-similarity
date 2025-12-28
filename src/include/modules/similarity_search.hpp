#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Similarity Search Module - find_similar_materials & cold_start_analogs
//------------------------------------------------------------------------------

// Registers hybrid similarity search macros that combine VSS (when available) with brute-force
void RegisterSimilaritySearchMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
