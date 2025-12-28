#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Embedding Dimension Constants
//------------------------------------------------------------------------------

// Jaccard Min-Hash embedding dimension
// 128-D provides ~2% approximation error for Jaccard similarity
constexpr idx_t JACCARD_EMBEDDING_DIM = 128;

// Weisfeiler-Lehman kernel embedding dimension (reserved for future use)
constexpr idx_t WL_EMBEDDING_DIM = 256;

// Combined embedding (Jaccard + WL concatenated)
constexpr idx_t COMBINED_EMBEDDING_DIM = 384;

//------------------------------------------------------------------------------
// Temporal Analysis Thresholds
//------------------------------------------------------------------------------

// Minimum overlap window (in weeks) for correlation computation
// Correlation requires sufficient data for meaningful coefficient
constexpr int MIN_OVERLAP_WEEKS = 8;

// Recent consumption window (in days) for short-term trend analysis
constexpr int RECENT_WINDOW_DAYS = 90;

// Long-term consumption window (in days) for historical trend analysis
constexpr int LONG_WINDOW_DAYS = 180;

//------------------------------------------------------------------------------
// VSS (Vector Similarity Search) Parameters
//------------------------------------------------------------------------------

// Over-fetch factor for HNSW k-NN search
// Retrieve k*factor candidates to account for HNSW approximation
// Then filter by min_similarity threshold
constexpr int VSS_OVERFETCH_FACTOR = 2;

//------------------------------------------------------------------------------
// Weisfeiler-Lehman Kernel Parameters
//------------------------------------------------------------------------------

// Weight decay offset for WL kernel iterations
// Controls how much weight earlier iterations contribute
// Higher values = more recent iterations weighted
constexpr double WL_WEIGHT_DECAY_OFFSET = 2.0;

} // namespace anofox
} // namespace duckdb
