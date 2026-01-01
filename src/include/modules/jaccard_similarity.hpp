#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Jaccard Similarity Module - C++ Scalar Function & Min-Hash Embeddings
//------------------------------------------------------------------------------

// Registers the jaccard_similarity scalar function (C++ implementation)
// Parameters:
//   - loader: ExtensionLoader for registering scalar functions
//
// Scalar Function: jaccard_similarity(list_a, list_b) -> DOUBLE
//   - Computes exact Jaccard similarity between two sets (represented as lists)
//   - Returns intersection_size / union_size
//   - Handles NULL inputs and empty lists
//   - Used for brute-force similarity computation and validation
//
// Note: compute_jaccard_embeddings macro is registered by vss_integration module
// to ensure centralized embedding infrastructure management
void RegisterJaccardFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
