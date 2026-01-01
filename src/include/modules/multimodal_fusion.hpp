#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Multi-Modal Fusion Module - Fusing structural, textual, and transactional embeddings
//------------------------------------------------------------------------------

// Registers the fuse_embeddings scalar function for combining embeddings
// Parameters:
//   - loader: ExtensionLoader for registering scalar functions
//
// Scalar Function: fuse_embeddings(
//   structural FLOAT[256],
//   textual FLOAT[384],
//   transactional FLOAT[128],
//   weights STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT)
// ) -> FLOAT[768]
//
//   - Implements late fusion formula: [√w_s · φ || √w_t · ψ || √w_x · χ]
//   - Returns 768-D fused embedding (256 + 384 + 128)
//   - Validates that weights sum to 1.0
//   - Applies square root scaling: √w_i to each weight component
void RegisterMultimodalFusionFunctions(ExtensionLoader &loader);

// Registers the compute_fused_embeddings SQL macro for batch fusion
// Parameters:
//   - conn: Database connection for registering SQL macros
//
// Macro: compute_fused_embeddings(
//   weights_structural := 0.5,
//   weights_textual := 0.5,
//   weights_transactional := 0.0
// ) -> TABLE
//   - Batch fusion of materials from material_embeddings table
//   - Outputs: material_id, combined_embedding (FLOAT[768]), fusion_weights
//   - Default: equal weight to structural and textual, zero weight to transactional
void RegisterFusionMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
