#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Stage-1 recall over generic feature multisets (coverage matching, HLD §4.1)
//------------------------------------------------------------------------------

// Registers:
//
//   find_covering_entities(query_entity_id VARCHAR, k BIGINT, ...)
//   find_covering_entities_for(query MAP, k BIGINT, ...)
//
// Both rank the entities of a (entity, feature, weight) table by how well they cover
// the query: containment (default), weighted Jaccard or Tversky, with Jaccard as a
// tie-break so the leanest full cover comes first. Like the other search functions
// they inline their SQL (bind_replace), so they work on read-only databases.
void RegisterCoverageSearchFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
