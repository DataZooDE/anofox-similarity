#pragma once
#include "duckdb_compat.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/standard_entry.hpp"
#include "duckdb/common/string_util.hpp"
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

//! The 1-3 segments of a pre-validated, dot-separated identifier path.
inline vector<string> SplitIdentifierPath(const string &path) {
	vector<string> parts;
	string current;
	for (auto ch : path) {
		if (ch == '.') {
			parts.push_back(current);
			current.clear();
		} else {
			current += ch;
		}
	}
	parts.push_back(current);
	return parts;
}

//! Look up a table or view by a (possibly qualified) path as the binder does: a two-part
//! path is schema.name, else catalog.name. Returns nullptr when there is no such entry or
//! the catalog or schema does not exist.
inline optional_ptr<CatalogEntry> LookupRelation(ClientContext &context, const string &path, CatalogType type) {
	auto parts = SplitIdentifierPath(path);
	if (parts.size() > 3) {
		return nullptr;
	}
	string catalog_name = parts.size() == 3 ? parts[0] : INVALID_CATALOG;
	string schema_name = parts.size() >= 2 ? parts[parts.size() - 2] : INVALID_SCHEMA;
	try {
		EntryLookupInfo lookup(type, duckdb_compat::EntryName(parts.back()));
		auto entry = Catalog::GetEntry(context, duckdb_compat::Ident(catalog_name), duckdb_compat::Ident(schema_name),
		                               lookup, OnEntryNotFound::RETURN_NULL);
		if (!entry && parts.size() == 2) {
			entry = Catalog::GetEntry(context, duckdb_compat::Ident(parts[0]), duckdb_compat::Ident(INVALID_SCHEMA),
			                          lookup, OnEntryNotFound::RETURN_NULL);
		}
		return entry && entry->type == type ? entry : nullptr;
	} catch (CatalogException &) {
		return nullptr;
	} catch (BinderException &) {
		return nullptr;
	}
}

//! Whether a table or view exists.
inline bool TableExists(ClientContext &context, const string &path) {
	return LookupRelation(context, path, CatalogType::TABLE_ENTRY) ||
	       LookupRelation(context, path, CatalogType::VIEW_ENTRY);
}

//! The fully qualified, lower-case name (catalog.schema.name) of a table or view, or the path
//! as given when it cannot be resolved. Two paths name the same relation when these agree.
inline string ResolvedRelationName(ClientContext &context, const string &path) {
	auto entry = LookupRelation(context, path, CatalogType::TABLE_ENTRY);
	if (!entry) {
		entry = LookupRelation(context, path, CatalogType::VIEW_ENTRY);
	}
	if (!entry) {
		return StringUtil::Lower(path);
	}
	auto &standard = entry->Cast<StandardEntry>();
	return StringUtil::Lower(static_cast<const string &>(standard.ParentCatalog().GetName()) + "." +
	                         static_cast<const string &>(standard.ParentSchema().name) + "." +
	                         static_cast<const string &>(entry->name));
}

//! inner query to validate.
inline void ValidateTableColumns(ClientContext &context, const string &table_path, const vector<string> &required,
                                 const string &param_name) {
	if (SplitIdentifierPath(table_path).size() > 3) {
		throw BinderException("Invalid identifier for %s: %s", param_name.c_str(), table_path.c_str());
	}
	auto entry = LookupRelation(context, table_path, CatalogType::TABLE_ENTRY);
	if (!entry) {
		return; // views and unresolvable paths: the inner query reports its own error
	}
	auto &table_entry = entry->Cast<TableCatalogEntry>();
	string missing;
	for (auto &column : required) {
		if (!table_entry.ColumnExists(duckdb_compat::Ident(column))) {
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

//! Whether a (possibly qualified) table has a column. Relations that are not tables
//! (views, unresolvable paths) report true and leave the check to the inner query.
inline bool TableHasColumn(ClientContext &context, const string &table_path, const string &column) {
	auto entry = LookupRelation(context, table_path, CatalogType::TABLE_ENTRY);
	if (entry) {
		return entry->Cast<TableCatalogEntry>().ColumnExists(duckdb_compat::Ident(column));
	}
	// a view: compare against its column names (or aliases)
	try {
		auto view = LookupRelation(context, table_path, CatalogType::VIEW_ENTRY);
		if (view) {
			auto &v = view->Cast<ViewCatalogEntry>();
			v.BindView(context);
			auto info = v.GetColumnInfo();
			if (!info) {
				return true;
			}
			for (idx_t i = 0; i < info->names.size(); i++) {
				const string &name = i < v.aliases.size() ? static_cast<const string &>(v.aliases[i])
				                                          : static_cast<const string &>(info->names[i]);
				if (StringUtil::CIEquals(name, column)) {
					return true;
				}
			}
			return false;
		}
	} catch (...) {
	}
	return true;
}

//! The type of a table's or view's column; false when it cannot be determined.
inline bool RelationColumnType(ClientContext &context, const string &path, const string &column, LogicalType &type) {
	auto entry = LookupRelation(context, path, CatalogType::TABLE_ENTRY);
	if (entry) {
		auto &table = entry->Cast<TableCatalogEntry>();
		if (!table.ColumnExists(duckdb_compat::Ident(column))) {
			return false;
		}
		type = table.GetColumn(duckdb_compat::Ident(column)).Type();
		return true;
	}
	try {
		auto view = LookupRelation(context, path, CatalogType::VIEW_ENTRY);
		if (!view) {
			return false;
		}
		auto &v = view->Cast<ViewCatalogEntry>();
		v.BindView(context);
		auto info = v.GetColumnInfo();
		if (!info) {
			return false;
		}
		for (idx_t i = 0; i < info->names.size() && i < info->types.size(); i++) {
			const string &name = i < v.aliases.size() ? static_cast<const string &>(v.aliases[i])
			                                          : static_cast<const string &>(info->names[i]);
			if (StringUtil::CIEquals(name, column)) {
				type = info->types[i];
				return true;
			}
		}
	} catch (...) {
	}
	return false;
}

//! A date column (movement_date) must be a DATE or TIMESTAMP; text dates get a conversion hint.
inline void ValidateDateColumn(ClientContext &context, const string &table_path, const string &column,
                               const string &param_name) {
	LogicalType type;
	if (!RelationColumnType(context, table_path, column, type)) {
		return;
	}
	switch (type.id()) {
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		return;
	default:
		throw BinderException("%s '%s': column %s is %s, expected DATE or TIMESTAMP; convert it in a view, e.g. "
		                      "strptime(budat, '%%Y%%m%%d')::DATE AS %s",
		                      param_name.c_str(), table_path.c_str(), column.c_str(), type.ToString().c_str(),
		                      column.c_str());
	}
}

} // namespace anofox
} // namespace duckdb
