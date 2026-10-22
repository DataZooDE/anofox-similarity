#pragma once

#include "duckdb_compat.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include <initializer_list>
#include <utility>

namespace duckdb {
namespace anofox {

using NamedParameterSpec = std::pair<const char *, LogicalType>;

inline void AddDocumentedNamedParameters(TableFunction &function, FunctionDescription &description,
                                          std::initializer_list<const char *> positional,
                                          std::initializer_list<NamedParameterSpec> named) {
	for (auto name : positional) {
		description.parameter_names.emplace_back(name);
	}
	for (const auto &parameter : named) {
		duckdb_compat::AddNamedParameter(function, parameter.first, parameter.second);
		description.parameter_names.emplace_back(parameter.first);
	}
}

} // namespace anofox
} // namespace duckdb
