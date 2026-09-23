#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// WL Kernel Module - wl_kernel_similarity graph structural similarity
//------------------------------------------------------------------------------

// Registers Weisfeiler-Lehman kernel macros (bom_dfs_neighborhood helper + wl_kernel_similarity)
// - bom_dfs_neighborhood: Reusable BOM depth-first traversal helper
// - wl_kernel_similarity: Graph-structural similarity computation
void RegisterWLKernelMacros(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
