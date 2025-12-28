#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// SAP Transformations Module - sap_to_* macros for ERP integration
//------------------------------------------------------------------------------

// Registers SAP transformation macros for materials, BOMs, and descriptive data
void RegisterSAPTransformationMacros(Connection &conn);

} // namespace anofox
} // namespace duckdb
