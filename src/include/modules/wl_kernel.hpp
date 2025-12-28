#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// WL Kernel Module - wl_kernel_similarity graph structural similarity
//------------------------------------------------------------------------------

// Registers Weisfeiler-Lehman kernel macro for graph-structural similarity
void RegisterWLKernelMacro(Connection &conn);

} // namespace anofox
} // namespace duckdb
