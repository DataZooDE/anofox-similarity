#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Similarity Search Module - find_similar_materials & cold_start_analogs
//------------------------------------------------------------------------------

// Registers similarity search table functions with telemetry tracking
void RegisterSimilaritySearchFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
