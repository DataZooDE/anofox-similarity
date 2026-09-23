#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// SAP Transformations Module - sap_to_* macros for ERP integration
//------------------------------------------------------------------------------

// Registers SAP transformation macros for materials, BOMs, and descriptive data
void RegisterSAPTransformationMacros(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
