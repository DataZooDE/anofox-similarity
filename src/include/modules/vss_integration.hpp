#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// VSS Integration Module - Vector Similarity Search infrastructure
//------------------------------------------------------------------------------

// Initialize VSS extension (HNSW indexes for approximate k-NN search)
void InitializeVSSIntegration(Connection &conn);

// Create embedding storage and tracking tables
void CreateEmbeddingTables(Connection &conn);

// Create HNSW indexes for fast similarity search
void CreateHNSWIndexes(Connection &conn);

// Register embedding generation and refresh macros
void RegisterEmbeddingMacros(Connection &conn);

// Create triggers for incremental embedding updates
void CreateIncrementalUpdateTriggers(Connection &conn);

} // namespace anofox
} // namespace duckdb
