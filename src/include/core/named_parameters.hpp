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

//! A VARCHAR or LIST(VARCHAR) parameter as a list of strings (NULL elements dropped).
inline vector<string> StringListParameter(const Value &value, const string &name) {
	vector<string> out;
	if (value.IsNull()) {
		return out;
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (auto &c : ListValue::GetChildren(value)) {
			if (!c.IsNull()) {
				out.push_back(c.ToString());
			}
		}
	} else {
		out.push_back(value.ToString());
	}
	(void)name;
	return out;
}

} // namespace anofox
} // namespace duckdb
