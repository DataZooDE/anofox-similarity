#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

// Register statistics helper functions/macros
// These support efficient computation of z-score normalization for embeddings
void RegisterStatisticsFunctions(Connection &conn);

} // namespace anofox
} // namespace duckdb
