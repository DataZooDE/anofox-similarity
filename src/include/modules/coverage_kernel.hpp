#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Coverage kernel, stage 2 (coverage matching, HLD §4.6 with the §11 amendments)
//------------------------------------------------------------------------------

// Registers:
//
//   coverage_kernel(query LIST, candidate LIST, options STRUCT) -> STRUCT
//       the in-memory kernel over two packed trees (the building block)
//   tree_coverage(query_tree_id, ...)        ranked candidates with coverage and gaps
//   tree_coverage_trace(query_tree_id, ...)  per requirement and attribute: why
//   coverage_recall(query_tree_id, ...)      stage 1 on its own
//
// The table functions inline their SQL (bind_replace): stage 1 over class multisets,
// then the kernel over the top-k candidates. query_tree_id may be a list, so many
// requests are ranked in one call.
void RegisterCoverageKernelFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
