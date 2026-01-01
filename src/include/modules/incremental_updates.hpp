#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Incremental Updates Module - Dirty Tracking & Batch Refresh Infrastructure
//------------------------------------------------------------------------------

// Creates triggers for automatic tracking of material embedding changes
// Parameters:
//   - conn: Database connection for creating triggers
//
// This module manages dirty material tracking for incremental embedding updates.
// Triggers track changes to bom_items table and mark affected materials for refresh.
//
// Tables Used:
//   - material_embeddings_dirty: Tracks which materials need recomputation
void CreateIncrementalUpdateTriggers(Connection &conn);

// Registers SQL macros for managing incremental embedding updates
// Parameters:
//   - conn: Database connection for registering SQL macros
//
// Macros Registered:
//   - refresh_transactional_embeddings(): Recompute embeddings for dirty materials
//   - clear_dirty_materials(material_ids): Remove materials from dirty tracking after refresh
void RegisterIncrementalUpdateMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
