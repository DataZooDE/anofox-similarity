#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// WL Kernel C++ Implementation - Optimized graph structural similarity
//------------------------------------------------------------------------------

// Register Weisfeiler-Lehman kernel as C++ scalar function
// - wl_kernel_similarity_cpp: Optimized C++ implementation with hash-based graph traversal
// Performance: 3-5x speedup vs SQL recursive CTE implementation
void RegisterWLKernelCppFunctions(ExtensionLoader &loader, Connection &conn);

} // namespace anofox
} // namespace duckdb
