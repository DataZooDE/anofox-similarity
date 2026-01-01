#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Embedding Statistics Module - Normalization & Feature Statistics Computation
//------------------------------------------------------------------------------

// Registers SQL macros for computing and managing embedding statistics
// Parameters:
//   - conn: Database connection for registering SQL macros
//
// This module manages statistics used for z-score normalization of transactional
// embeddings across 98 features (30 Phase 2A + 68 Phase 2B + 6 reserved Phase 2C)
//
// Macros Registered:
//   - recompute_embedding_statistics(): Computes mean/stddev for all 98 features
//   - compute_phase2c_statistics(): Advanced domain-specific features (indices 92-97)
void RegisterStatisticsMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
