#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Jaccard Min-Hash C++ Implementation - Optimized embedding computation
//------------------------------------------------------------------------------

// Register Jaccard min-hash computation as C++ scalar function
// - compute_jaccard_embedding_cpp: Optimized single-pass min-hash computation
// Input: component list (VARCHAR[])
// Output: 128-D min-hash embedding (FLOAT[128])
// Performance: 10-20x speedup vs SQL Cartesian product + aggregation
void RegisterJaccardCppFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
