#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "core/sql_safety.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// SQL helpers shared by the coverage-matching table functions (bind_replace)
//------------------------------------------------------------------------------

//! Parse one SELECT into the subquery a bind_replace returns.
inline unique_ptr<SubqueryRef> CoverageSubquery(const string &query, ClientContext &context, const string &fn) {
	auto statements = duckdb_compat::ParseStatements(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("Failed to parse %s query", fn);
	}
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

//! A validated identifier path, each segment double-quoted ("schema"."table").
inline string QuotedIdentifier(const string &path) {
	string out = "\"";
	for (auto ch : path) {
		out += ch == '.' ? string("\".\"") : string(1, ch);
	}
	return out + "\"";
}

//! A DOUBLE literal with full precision.
inline string DoubleLiteral(double v) {
	return StringUtil::Format("%.17g::DOUBLE", v);
}

//! A SQL expression turning a VARCHAR expression into a JSON string literal (or null).
inline string JsonStringSQL(const string &expr) {
	return "CASE WHEN (" + expr + ") IS NULL THEN 'null' ELSE '\"' || regexp_replace(replace(replace(replace(replace("
	       "replace(CAST(" + expr + " AS VARCHAR), '\\', '\\\\'), '\"', '\\\"'), chr(10), '\\n'), chr(13), '\\r'), "
	       "chr(9), '\\t'), '[\\x00-\\x1f]', '', 'g') || '\"' END";
}

//! A SQL expression turning a DOUBLE expression into a JSON number (or null).
inline string JsonNumberSQL(const string &expr) {
	return "CASE WHEN (" + expr + ") IS NULL OR NOT isfinite(" + expr + ") THEN 'null' ELSE CAST(round(" + expr +
	       ", 6) AS VARCHAR) END";
}

} // namespace anofox
} // namespace duckdb
