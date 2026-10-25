#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Directional and weighted set measures (coverage matching, HLD §4.1)
//------------------------------------------------------------------------------

// Registers the set-measure scalar functions:
//
//   tversky_similarity(query LIST, candidate LIST[, alpha DOUBLE, beta DOUBLE]) -> DOUBLE
//       |q∩c| / (|q∩c| + alpha|q\c| + beta|c\q|); alpha = beta = 1 is Jaccard
//   containment(query LIST, candidate LIST) -> DOUBLE
//       |q∩c| / |q|; NULL for an empty query
//   weighted_jaccard(query MAP, candidate MAP) -> DOUBLE
//       Σ min(q_k, c_k) / Σ max(q_k, c_k)
//   weighted_containment(query MAP, candidate MAP) -> DOUBLE
//       Σ min(q_k, c_k) / Σ q_k; NULL for an empty query
//   weighted_tversky(query MAP, candidate MAP[, alpha DOUBLE, beta DOUBLE]) -> DOUBLE
//       Σmin / (Σmin + alpha Σ(q-c)+ + beta Σ(c-q)+)
//
// Lists are treated as sets (duplicates and NULL elements dropped). Map values are
// non-negative weights; a NULL weight counts as 1. NULL inputs give NULL. Two empty
// inputs give 0.0, as jaccard_similarity does; any other zero denominator gives NULL.
void RegisterSetMeasureFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
