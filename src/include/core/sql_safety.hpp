#pragma once

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include <cctype>

namespace duckdb {
namespace anofox {

inline string EscapeSQLStringLiteral(const string &input) {
	string escaped;
	escaped.reserve(input.size());
	for (auto ch : input) {
		if (ch == '\'') {
			escaped += "''";
		} else {
			escaped += ch;
		}
	}
	return escaped;
}

inline string QuoteSQLStringLiteral(const string &input) {
	return "'" + EscapeSQLStringLiteral(input) + "'";
}

inline string ValidateSQLIdentifierPath(const string &identifier, const string &param_name) {
	if (identifier.empty()) {
		throw BinderException("%s must not be empty", param_name.c_str());
	}

	bool expect_segment_start = true;
	for (auto ch : identifier) {
		if (ch == '.') {
			if (expect_segment_start) {
				throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
			}
			expect_segment_start = true;
			continue;
		}

		if (expect_segment_start) {
			if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_')) {
				throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
			}
			expect_segment_start = false;
			continue;
		}

		if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
			throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
		}
	}

	if (expect_segment_start) {
		throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
	}

	return identifier;
}

} // namespace anofox
} // namespace duckdb
