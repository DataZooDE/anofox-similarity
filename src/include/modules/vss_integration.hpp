#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// VSS Integration Module - Vector Similarity Search infrastructure
//
// Core VSS setup: Extension loading, table creation, and index management
// Related Modules:
//   - embedding_statistics: Statistics computation for z-score normalization
//   - incremental_updates: Dirty tracking triggers and incremental refresh
//------------------------------------------------------------------------------

// Initialize VSS extension (HNSW indexes for approximate k-NN search)
void InitializeVSSIntegration(Connection &conn);

// Create embedding storage and tracking tables
void CreateEmbeddingTables(Connection &conn);

// Create HNSW indexes for fast similarity search
void CreateHNSWIndexes(Connection &conn);

// Register core embedding macros (Jaccard embeddings, statistics freshness check)
// Note: Statistics computation macros are registered by embedding_statistics module
// Note: Incremental update macros are registered by incremental_updates module
void RegisterEmbeddingMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
