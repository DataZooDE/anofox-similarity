#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Transactional Embeddings: Generate 128-D vectors from goods movements data
//
// Uses anofox-forecast time series features to extract consumption patterns,
// demand volatility, frequency domain features, and temporal autocorrelation.
//
// Note: This module has a soft dependency on anofox-forecast. It gracefully
// handles the case where anofox-forecast is not loaded by providing helpful
// error messages to the user.
//------------------------------------------------------------------------------

// Registers check_anofox_forecast_available() SQL macro
// Returns: BOOLEAN - true if anofox_fcst_ts_features function is available
// Purpose: Allows graceful fallback when anofox-forecast extension not loaded
void RegisterCheckAnofoxForecastMacro(Connection &conn);

// Registers compute_transactional_embeddings as a TableFunction with telemetry
// (
//   movements_table := 'goods_movements',
//   time_window_days := 365
// ) SQL macro
//
// Returns: TABLE with columns:
//   - material_id VARCHAR: Unique material identifier
//   - transactional_embedding FLOAT[128]: 128-dimensional embedding vector
//
// Algorithm:
// 1. Aggregates goods movements per material into time series arrays
// 2. Calls anofox_fcst_ts_features() to extract 76+ time series statistics
// 3. Selects 30 key features and pads remaining 98 dimensions with zeros
// 4. Returns deterministic FLOAT[128] arrays suitable for similarity search
//
// Feature Breakdown (30 actual features + 98 zero-padding):
//   - Consumption patterns (10): mean, median, variance, trend, sum, length, etc.
//   - Demand volatility (8): coefficient of variation, skewness, entropy, etc.
//   - Frequency domain (8): FFT coefficients (real & imaginary), aggregates
//   - Temporal patterns (4): Autocorrelation at lags 1, 7, 14, 28
//   - Reserved (98): Zero-padding for future feature expansion
//
// Edge Cases:
//   - Materials with < 3 data points: Returns NULL (insufficient history)
//   - Materials with sparse movements: Features computed on available data
//   - Zero or negative quantities: Filtered out (require positive quantity)
//   - anofox-forecast not loaded: Raises informative error with install instructions
//
// Performance:
//   - Time: O(N × M × log M) where N = materials, M = avg movements per material
//   - Space: O(N × 128) for output embeddings
//   - Estimated throughput: 100-500 materials/second on 8-core CPU
//
// Integration with Similarity Search:
//   - Designed to integrate with material_embeddings table
//   - Works with existing fuse_embeddings() fusion function
//   - Supports HNSW index creation for fast similarity search
void RegisterTransactionalEmbeddingFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
