#pragma once

// A private copy of DuckDB's bundled miniz, compiled into this extension under its own
// namespace (src/modules/aas/aasx_miniz.cpp). The core library exports duckdb_miniz only
// internally; renaming avoids both duplicate symbols in static builds and host-symbol
// dependencies in loadable builds (Windows exports only DUCKDB_API symbols).
// Include this header before any DuckDB header in a translation unit.
#define duckdb_miniz anofox_aas_miniz
#include "miniz.hpp"
#undef duckdb_miniz

namespace aasx_zip = anofox_aas_miniz;
