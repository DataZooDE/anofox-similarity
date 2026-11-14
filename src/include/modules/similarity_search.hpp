#pragma once

#include "duckdb.hpp"
#include "duckdb_compat.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Similarity Search Module - find_similar_materials & cold_start_analogs
//------------------------------------------------------------------------------

// Registers similarity search table functions with telemetry tracking
void RegisterSimilaritySearchFunctions(ExtensionLoader &loader);

//! The query materials of a search: one id (the positional material_id), or every material_id of
//! a table (materials_table, the batch form). Both forms run the same set-based SQL; the batch
//! form returns a leading query_id column and applies k per query.
//!
//! DuckDB table functions take constants, not a column of another table ("does not support
//! lateral join column parameters"), so the batch form is how a search runs for many materials
//! in one query.
struct QueryMaterials {
	bool batch = false;
	string literal; // single: the quoted material id
	string table;   // batch: the validated materials_table

	//! single: the id from inputs[0] when the call has `single_args` positional arguments;
	//! batch (one positional argument fewer): materials_table, required.
	static QueryMaterials FromInput(ClientContext &context, TableFunctionBindInput &input, const string &fn,
	                                idx_t single_args) {
		QueryMaterials q;
		if (input.inputs.size() >= single_args) {
			if (input.inputs[0].IsNull()) {
				throw BinderException("%s: material_id must not be NULL", fn);
			}
			q.literal = QuoteSQLStringLiteral(input.inputs[0].GetValue<string>());
			return q;
		}
		auto value = duckdb_compat::FindNamedParameter(input.named_parameters, "materials_table");
		if (!value || value->IsNull()) {
			throw BinderException("%s: give a material_id, or materials_table := '<table>' for many materials", fn);
		}
		q.batch = true;
		q.table = ValidateSQLIdentifierPath(value->GetValue<string>(), "materials_table");
		ValidateTableColumns(context, q.table, {"material_id"}, "materials_table");
		return q;
	}

	//! The body of a `queries(query_id)` CTE.
	string QueriesSQL() const {
		return batch ? "SELECT DISTINCT material_id AS query_id FROM query_table(" + QuoteSQLStringLiteral(table) +
		                   ") WHERE material_id IS NOT NULL"
		             : "SELECT " + literal + " AS query_id";
	}

	//! A condition that `column` is the query material of the `queries` row `alias`. The single
	//! form compares with the literal, as before; the batch form compares as text, so an INTEGER
	//! BOM id matches a VARCHAR id list.
	string Matches(const string &column, const string &alias) const {
		return batch ? column + "::VARCHAR = " + alias + ".query_id::VARCHAR" : column + " = " + literal;
	}
};

} // namespace anofox
} // namespace duckdb
