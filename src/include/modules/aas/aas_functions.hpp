#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace anofox {

//! SQL surface of the Asset Administration Shell (AAS) integration (GitHub #37).
void RegisterAasFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
