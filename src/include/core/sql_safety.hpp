#pragma once

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
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

		// Bytes with the high bit set are UTF-8 continuation/lead bytes of a non-ASCII identifier
		// (e.g. an emoji or CJK table name). They are allowed because every SQL metacharacter that
		// could enable injection (quote, semicolon, whitespace, parenthesis, dot) is ASCII < 0x80.
		const bool is_utf8 = static_cast<unsigned char>(ch) >= 0x80;

		if (expect_segment_start) {
			if (!(std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || is_utf8)) {
				throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
			}
			expect_segment_start = false;
			continue;
		}

		if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || is_utf8)) {
			throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
		}
	}

	if (expect_segment_start) {
		throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), identifier.c_str());
	}

	return identifier;
}

//! Validate that a (possibly qualified) table exists with the columns a
//! table-name parameter requires, so users get "bom_table 'x' is missing
//! column parent_id" instead of a cryptic binder error from the internal
//! SQL. Non-table entries (views, CTE-provided relations) are left to the
//! inner query to validate.
inline void ValidateTableColumns(ClientContext &context, const string &table_path, const vector<string> &required,
                                 const string &param_name) {
	// split the pre-validated identifier path (1-3 dot-separated segments)
	vector<string> parts;
	string current;
	for (auto ch : table_path) {
		if (ch == '.') {
			parts.push_back(current);
			current.clear();
		} else {
			current += ch;
		}
	}
	parts.push_back(current);
	if (parts.size() > 3) {
		throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), table_path.c_str());
	}
	string catalog_name = parts.size() == 3 ? parts[0] : INVALID_CATALOG;
	string schema_name = parts.size() >= 2 ? parts[parts.size() - 2] : INVALID_SCHEMA;
	auto &table_name = parts.back();

	optional_ptr<CatalogEntry> entry;
	try {
		EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, table_name);
		entry = Catalog::GetEntry(context, catalog_name, schema_name, lookup_info, OnEntryNotFound::RETURN_NULL);
		if (!entry && parts.size() == 2) {
			// two-part paths can also mean catalog.table (binder semantics)
			entry = Catalog::GetEntry(context, parts[0], INVALID_SCHEMA, lookup_info, OnEntryNotFound::RETURN_NULL);
		}
	} catch (...) {
		return; // ambiguous/odd paths: defer to the inner query's own error
	}
	if (!entry || entry->type != CatalogType::TABLE_ENTRY) {
		return;
	}
	auto &table_entry = entry->Cast<TableCatalogEntry>();
	string missing;
	for (auto &column : required) {
		if (!table_entry.ColumnExists(column)) {
			if (!missing.empty()) {
				missing += ", ";
			}
			missing += column;
		}
	}
	if (!missing.empty()) {
		string expected;
		for (auto &column : required) {
			if (!expected.empty()) {
				expected += ", ";
			}
			expected += column;
		}
		throw BinderException("%s '%s' is missing required column(s): %s (expected columns: %s)", param_name.c_str(),
		                      table_path.c_str(), missing.c_str(), expected.c_str());
	}
}

} // namespace anofox
} // namespace duckdb
