#pragma once
#include "duckdb/function/scalar_function.hpp"

namespace compat_anofox {
// Adapt a side-effect-only bind callback to the scalar bind function pointer
// on each DuckDB line. No bind argument is used by the telemetry callbacks.
template <auto FN, class BIND>
struct ScalarBindAdapter;

template <auto FN, class RESULT, class... ARGS>
struct ScalarBindAdapter<FN, RESULT (*)(ARGS...)> {
	static RESULT Call(ARGS...) {
		return FN();
	}
};

template <auto FN>
duckdb::bind_scalar_function_t ScalarBind() {
	return &ScalarBindAdapter<FN, duckdb::bind_scalar_function_t>::Call;
}
} // namespace compat_anofox
