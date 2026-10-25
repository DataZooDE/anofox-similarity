#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Evaluation and calibration (coverage matching, HLD §4.8)
//------------------------------------------------------------------------------

void RegisterEvaluationFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
