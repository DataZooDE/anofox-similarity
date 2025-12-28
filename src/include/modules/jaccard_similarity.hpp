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
void RegisterJaccardFunctions(ExtensionLoader &loader);

// Registers the compute_jaccard_embeddings SQL macro for min-hash embedding generation
// Parameters:
//   - conn: Database connection for registering SQL macros
//
// Macro: compute_jaccard_embeddings(bom_table := 'bom_items') -> TABLE
//   - Generates 128-D min-hash embeddings for Jaccard similarity approximation
//   - Outputs: material_id, seed (0-127), minhash_value, num_components
//   - One row per material per seed (128 rows per material)
//   - Supports VSS (Vector Similarity Search) via HNSW indexes
//   - Theoretical approximation error: ±2% with 128 dimensions
void RegisterJaccardEmbeddingMacro(Connection &conn);

} // namespace anofox
} // namespace duckdb
