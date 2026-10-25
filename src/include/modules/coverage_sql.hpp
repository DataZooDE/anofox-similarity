#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "core/sql_safety.hpp"
#include "core/named_parameters.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include <cmath>

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

//! A SQL list literal of quoted strings, e.g. ['a', 'b'].
inline string QuotedList(const vector<string> &values) {
	string out = "[";
	for (idx_t i = 0; i < values.size(); i++) {
		out += (i ? ", " : "") + QuoteSQLStringLiteral(values[i]);
	}
	return out + "]";
}

//! One vector per attribute name from a name_vectors table (name, vec); different vectors for
//! one name are an error, so name similarity does not depend on the run.
inline string NameVectorsSQL(const string &table) {
	return "SELECT CAST(name AS VARCHAR) AS name, CASE WHEN count(DISTINCT CAST(vec AS DOUBLE[])) > 1 THEN "
	       "error('name_vectors has different vectors for the name ''' || CAST(name AS VARCHAR) || '''') ELSE "
	       "any_value(CAST(vec AS DOUBLE[])) END AS vec FROM " +
	       QuotedIdentifier(table) + " GROUP BY CAST(name AS VARCHAR)";
}

//! A DOUBLE literal with full precision.
inline string DoubleLiteral(double v) {
	return StringUtil::Format("%.17g::DOUBLE", v);
}

//! A SQL expression turning a VARCHAR expression into a JSON string literal (or null).
inline string JsonStringSQL(const string &expr) {
	return "CASE WHEN (" + expr + ") IS NULL THEN 'null' ELSE '\"' || regexp_replace(replace(replace(replace(replace(replace(replace("
	       "replace(CAST(" + expr + " AS VARCHAR), '\\', '\\\\'), '\"', '\\\"'), chr(10), '\\n'), chr(13), '\\r'), "
	       "chr(9), '\\t'), chr(8), '\\b'), chr(12), '\\f'), '[\\x00-\\x1f]', '', 'g') || '\"' END";
}

//! A SQL expression turning a DOUBLE expression into a JSON number (or null).
inline string JsonNumberSQL(const string &expr) {
	return "CASE WHEN (" + expr + ") IS NULL OR NOT isfinite(" + expr + ") THEN 'null' ELSE CAST(round(" + expr +
	       ", 6) AS VARCHAR) END";
}

//! Named parameters and table checks of a coverage table function (bind_replace).
struct CoverageParamReader {
	ClientContext &context;
	const duckdb_compat::BindNamedParameters &named;
	string fn;

	const Value *Get(const char *name) const {
		return duckdb_compat::FindNamedParameter(named, name);
	}
	//! The parameter's string, the fallback when not given, "" when given as NULL.
	string Str(const char *name, const string &fallback = "") const {
		auto v = Get(name);
		if (!v) {
			return fallback;
		}
		return v->IsNull() ? string() : v->GetValue<string>();
	}
	double Dbl(const char *name, double fallback) const {
		auto v = Get(name);
		if (!v || v->IsNull()) {
			return fallback;
		}
		double d = v->GetValue<double>();
		if (!std::isfinite(d)) {
			throw BinderException("%s: %s must be finite", fn, name);
		}
		return d;
	}
	int64_t Int(const char *name, int64_t fallback) const {
		auto v = Get(name);
		return !v || v->IsNull() ? fallback : v->GetValue<int64_t>();
	}
	//! A validated, existing table with the required columns.
	string Check(string table, const char *name, const vector<string> &columns) const {
		if (table.empty()) {
			return table;
		}
		table = ValidateSQLIdentifierPath(table, name);
		if (!TableExists(context, table)) {
			throw BinderException("%s: %s '%s' does not exist", fn, name, table);
		}
		ValidateTableColumns(context, table, columns, name);
		return table;
	}
	//! A table parameter ("" when NULL, or when not given and there is no fallback).
	string Table(const char *name, const string &fallback, const vector<string> &columns) const {
		return Check(Str(name, fallback), name, columns);
	}
	//! A required positional table argument.
	string Positional(const TableFunctionBindInput &input, idx_t i, const char *name,
	                  const vector<string> &columns) const {
		if (input.inputs.size() <= i || input.inputs[i].IsNull()) {
			throw BinderException("%s: %s must not be NULL", fn, name);
		}
		return Check(input.inputs[i].GetValue<string>(), name, columns);
	}
	bool Has(const string &table, const char *column) const {
		return TableHasColumn(context, table, column);
	}
	//! A validated plain column name, quoted.
	string Column(const string &table, const char *name, const string &fallback) const {
		auto column = Str(name, fallback);
		if (column.empty() || column.find('.') != string::npos) {
			throw BinderException("%s: %s must be a column name", fn, name);
		}
		ValidateSQLIdentifierPath(column, name);
		ValidateTableColumns(context, table, {column}, name);
		return QuotedIdentifier(column);
	}
};

//! Register a coverage table function with its documentation.
inline void RegisterCoverageTableFunction(ExtensionLoader &loader, const string &name, vector<LogicalType> positional,
                                          vector<string> positional_names, table_function_bind_replace_t fn,
                                          vector<std::pair<string, LogicalType>> named, const string &description,
                                          vector<string> examples, vector<string> categories) {
	TableFunction f(duckdb_compat::FunctionName(name), positional, nullptr, nullptr);
	f.bind_replace = fn;
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = std::move(categories);
	desc.parameter_names = std::move(positional_names);
	for (auto &spec : named) {
		duckdb_compat::AddNamedParameter(f, spec.first, spec.second);
		desc.parameter_names.emplace_back(spec.first);
	}
	desc.parameter_types = positional;
	CreateTableFunctionInfo info(f);
	info.descriptions.push_back(std::move(desc));
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

//------------------------------------------------------------------------------
// Weighted set measures over shared = Σ min(q, c), query = Σ q and candidate = Σ c (R1, HLD §4.1)
//------------------------------------------------------------------------------

inline string ContainmentSQL(const string &shared, const string &query) {
	return shared + " / NULLIF(" + query + ", 0)";
}

inline string JaccardSQL(const string &shared, const string &query, const string &candidate) {
	return shared + " / NULLIF(" + query + " + " + candidate + " - " + shared + ", 0)";
}

//! Stage-1 score: containment plus tie_break x Jaccard (lean candidates first), or Jaccard.
inline string TieBrokenScoreSQL(bool containment, const string &measure, const string &jaccard, double tie_break) {
	return containment ? measure + " + " + DoubleLiteral(tie_break) + " * " + jaccard : jaccard;
}

//! One documented overload of a coverage scalar.
struct CoverageScalarOverload {
	vector<LogicalType> arguments;
	vector<string> names;
	vector<string> examples;
	string description; // empty: the function's description
};

//! A coverage scalar: a fallible function set with one documented signature per overload.
struct CoverageScalarSpec {
	string name;
	LogicalType return_type;
	scalar_function_t function;
	bind_scalar_function_t bind;
	string description;
	vector<string> categories;
	bool special_null_handling = false; // the function handles NULL arguments itself
};

inline void RegisterCoverageScalar(ExtensionLoader &loader, const CoverageScalarSpec &spec,
                                   const vector<CoverageScalarOverload> &overloads) {
	ScalarFunctionSet set(duckdb_compat::FunctionName(spec.name));
	for (auto &o : overloads) {
		ScalarFunction f(duckdb_compat::FunctionName(spec.name), o.arguments, spec.return_type, spec.function, spec.bind);
		duckdb_compat::SetFallible(f);
		if (spec.special_null_handling) {
			f.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		}
		set.AddFunction(f);
	}
	CreateScalarFunctionInfo info(set);
	for (auto &o : overloads) {
		FunctionDescription desc;
		desc.description = o.description.empty() ? spec.description : o.description;
		desc.examples = o.examples;
		desc.categories = spec.categories;
		desc.parameter_names = o.names;
		desc.parameter_types = o.arguments;
		info.descriptions.push_back(std::move(desc));
	}
	duckdb_compat::NameArgumentsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace anofox
} // namespace duckdb
