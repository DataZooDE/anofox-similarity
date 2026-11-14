#pragma once
// The child vector of an ARRAY (FLOAT[n]) vector on every DuckDB line.
//
// DuckDB 2.0.0 moved ArrayVector into its own header and deprecated ArrayVector::GetEntry in
// favour of GetChild / GetChildMutable; before 2.0 only GetEntry exists, declared in the
// vector header.
#include "duckdb.hpp"
#if __has_include("duckdb/common/vector/array_vector.hpp")
#include "duckdb/common/vector/array_vector.hpp"
#define COMPAT_ANOFOX_ARRAY_GET_CHILD 1
#endif

namespace compat_anofox {

inline duckdb::Vector &ArrayChild(duckdb::Vector &vector) {
#ifdef COMPAT_ANOFOX_ARRAY_GET_CHILD
	return duckdb::ArrayVector::GetChildMutable(vector);
#else
	return duckdb::ArrayVector::GetEntry(vector);
#endif
}

} // namespace compat_anofox
