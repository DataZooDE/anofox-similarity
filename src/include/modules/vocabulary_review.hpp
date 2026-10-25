#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Vocabulary review and the external re-rank hook (coverage matching, HLD §4.4, §4.5)
//------------------------------------------------------------------------------

// Registers vocabulary_review_queue, apply_vocabulary_review and uncertain_decisions
// (table functions, inlined SQL).
void RegisterVocabularyReviewFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
