#include "modules/sap_transformations.hpp"
#include "duckdb/common/types/date.hpp"

#include <algorithm>
#include "core/error_handling.hpp"
#include "duckdb/main/connection.hpp"
#include "datazoo_function_doc.hpp"
#include "duckdb_compat.hpp"
#include "core/named_parameters.hpp"
#include "core/sql_safety.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"

namespace ddoc = datazoo::doc;

namespace duckdb {
namespace anofox {

static const DefaultTableMacro SAP_TO_MATERIALS_MACRO = {
    DEFAULT_SCHEMA,
    "sap_to_materials",
    {"mara_table", nullptr},
    {{"makt_table", "NULL"}, {"language", "'E'"}, {nullptr, nullptr}},
    R"(
		SELECT
			TRIM(matnr) AS material_id,
			mtart AS material_type,
			matkl AS material_group,
			'' AS description,
			-- SAP YYYYMMDD text, or an ersda that is already a DATE / ISO string
			COALESCE(TRY_STRPTIME(ersda::VARCHAR, '%Y%m%d')::DATE, TRY_CAST(ersda AS DATE)) AS created_date
		FROM query_table(mara_table)
		WHERE lvorm IS NULL OR lvorm = '' OR lvorm = ' ')"};

static const DefaultTableMacro SAP_TO_MATERIALS_WITH_DESC_MACRO = {
    DEFAULT_SCHEMA,
    "sap_to_materials_with_desc",
    {"mara_table", "makt_table", nullptr},
    {{"language", "'E'"}, {nullptr, nullptr}},
    R"(
		SELECT
			m.material_id,
			m.material_type,
			m.material_group,
			COALESCE(k.maktx, '') AS description,
			m.created_date
		FROM sap_to_materials(mara_table := mara_table) m
		LEFT JOIN (
			-- One description per material: real MAKT can have several rows per (matnr, spras)
			-- (e.g. across MANDT/client), which would otherwise fan the material out into duplicates.
			SELECT material_id, ANY_VALUE(maktx) AS maktx
			FROM (SELECT TRIM(matnr) AS material_id, maktx FROM query_table(makt_table) WHERE spras = COALESCE(language, 'E'))
			GROUP BY material_id
		) k ON m.material_id = k.material_id)"};

static const DefaultTableMacro EXTRACT_MATERIAL_DESCRIPTIONS_MACRO = {
    DEFAULT_SCHEMA,
    "extract_material_descriptions",
    {nullptr},
    {{"makt_table", "'sap_makt'"}, {"language", "'EN'"}, {nullptr, nullptr}},
    R"(
		WITH descriptions AS (
			-- Collapse multiple MAKT rows per (matnr, spras) to a single description so a material
			-- is not duplicated (real MAKT repeats per MANDT/client).
			SELECT
				TRIM(matnr) AS material_id,
				ANY_VALUE(TRIM(maktx)) AS description,
				ANY_VALUE(TRIM(maktg)) AS short_text
			FROM query_table(makt_table)
			WHERE spras = COALESCE(language, 'EN')
			GROUP BY TRIM(matnr)
		),
		combined_text AS (
			SELECT
				material_id,
				CONCAT_WS(' ', description, short_text) AS full_text
			FROM descriptions
			WHERE description IS NOT NULL OR short_text IS NOT NULL
		)
		SELECT * FROM combined_text)"};

//------------------------------------------------------------------------------
// sap_to_bom_items: MAST / STKO / STPO (/ STAS) -> flat BOM edges
//------------------------------------------------------------------------------
//
// A bind_replace table function rather than a macro, so it can use the SAP columns an extract
// has and skip the ones it lacks: a minimal STPO with stlnr, idnrk, menge, meins still works, a
// full extract gets the category, alternative, versioning and deletion rules applied.

static unique_ptr<SubqueryRef> ParseSapSubquery(const string &query, ClientContext &context) {
	auto statements = duckdb_compat::ParseStatements(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("sap_to_bom_items: failed to parse the generated query");
	}
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

// SAP YYYYMMDD text, or a DATE / ISO string
static string SapDate(const string &expr) {
	return "COALESCE(TRY_STRPTIME(" + expr + "::VARCHAR, '%Y%m%d')::DATE, TRY_CAST(" + expr + " AS DATE))";
}

// TRUE when any of the given flag columns the table has (lkenz, loekz: 'X' = deleted) is set
static string SapDeleted(ClientContext &context, const string &table, std::initializer_list<const char *> flags) {
	string expr;
	for (auto flag : flags) {
		if (TableHasColumn(context, table, flag)) {
			// ::VARCHAR: a flag column of only NULLs may be typed INTEGER
			expr += (expr.empty() ? "" : " OR ") + string("COALESCE(TRIM(") + flag + "::VARCHAR), '') <> ''";
		}
	}
	return expr.empty() ? "FALSE" : "(" + expr + ")";
}

// a template: the named-parameter map type differs between DuckDB 1.5.6 and 2.0
template <class NAMED>
static string SapNamedString(const NAMED &named, const string &name, const string &fallback) {
	auto value = duckdb_compat::FindNamedParameter(named, name);
	if (!value || value->IsNull()) {
		return fallback;
	}
	return value->ToString();
}

// reference_date: SAP YYYYMMDD or an ISO date; checked here so a bad value is an error even when no
// table has datuv
static date_t SapParseReferenceDate(const string &text) {
	string iso = text;
	if (text.size() == 8 && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
		iso = text.substr(0, 4) + "-" + text.substr(4, 2) + "-" + text.substr(6, 2);
	}
	try {
		return Date::FromString(iso, true);
	} catch (std::exception &) {
		throw BinderException("sap_to_bom_items: reference_date '%s' is not a date (YYYYMMDD or YYYY-MM-DD)", text);
	}
}

static unique_ptr<TableRef> SapToBomItemsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("sap_to_bom_items");
	const char *names[] = {"mast_table", "stko_table", "stpo_table"};
	string tables[3];
	for (idx_t i = 0; i < 3; i++) {
		// positional, or (the overload without positional arguments) named
		Value table = input.inputs.size() > i ? input.inputs[i] : Value();
		if (input.inputs.size() <= i) {
			auto named = duckdb_compat::FindNamedParameter(input.named_parameters, names[i]);
			table = named ? *named : Value();
		}
		if (table.IsNull()) {
			throw BinderException("sap_to_bom_items: %s must not be NULL", names[i]);
		}
		tables[i] = ValidateSQLIdentifierPath(table.GetValue<string>(), names[i]);
	}
	const string &mast = tables[0], &stko = tables[1], &stpo = tables[2];
	ValidateTableColumns(context, mast, {"matnr", "stlnr"}, "mast_table");
	ValidateTableColumns(context, stko, {"stlnr", "stlal"}, "stko_table");
	ValidateTableColumns(context, stpo, {"stlnr", "idnrk", "menge", "meins"}, "stpo_table");

	// only an absent or NULL stas_table turns the item selection off; '' is an invalid name
	auto stas_value = duckdb_compat::FindNamedParameter(input.named_parameters, "stas_table");
	string stas;
	if (stas_value && !stas_value->IsNull()) {
		stas = ValidateSQLIdentifierPath(stas_value->ToString(), "stas_table");
		ValidateTableColumns(context, stas, {"stlnr", "stlal", "stlkn"}, "stas_table");
		ValidateTableColumns(context, stpo, {"stlkn"}, "stpo_table (with stas_table)");
	}
	auto alternative = QuoteSQLStringLiteral(SapNamedString(input.named_parameters, "bom_alternative", "01"));
	auto reference_text = SapNamedString(input.named_parameters, "reference_date", "9999-12-31");
	string reference = "DATE '" + Date::ToString(SapParseReferenceDate(reference_text)) + "'";
	auto usage_value = duckdb_compat::FindNamedParameter(input.named_parameters, "bom_usage");
	const bool by_usage = usage_value && !usage_value->IsNull();
	if (by_usage) {
		ValidateTableColumns(context, mast, {"stlan"}, "mast_table (with bom_usage)");
	}

	auto has = [&](const string &table, const char *column) {
		return TableHasColumn(context, table, column);
	};
	// The BOM category: STKO, STPO and STAS share stlnr across categories (M, D, K, ...). Filtering
	// the header but not the items would let a document item replace a material item of the same
	// node, so with stlty on STKO the item tables need it too.
	if (has(stko, "stlty")) {
		ValidateTableColumns(context, stpo, {"stlty"}, "stpo_table (STKO has stlty)");
		if (!stas.empty()) {
			ValidateTableColumns(context, stas, {"stlty"}, "stas_table (STKO has stlty)");
		}
	}
	// SAP clients: with mandt in every table, rows of different clients never join
	const bool clients = has(mast, "mandt") && has(stko, "mandt") && has(stpo, "mandt") &&
	                     (stas.empty() || has(stas, "mandt"));
	const string client_col = clients ? "mandt, " : "";
	// newest datuv first, then the highest version counter (stkoz, stpoz, stasz) as a number
	// ('10' after '9'), then the given columns so the choice is deterministic
	auto newest_first = [&](const string &table, const char *counter, const string &tie_break) {
		string order = has(table, "datuv") ? SapDate("datuv") + " DESC NULLS LAST, " : string();
		if (has(table, counter)) {
			order += "TRY_CAST(" + string(counter) + " AS BIGINT) DESC NULLS LAST, " + counter + " DESC, ";
		}
		return order + tie_break;
	};
	// a blank or SAP-initial ('00000000') datuv parses to NULL and counts as undated, not as invalid
	auto valid_on_reference = [&](const string &table) {
		return has(table, "datuv") ? " AND (" + SapDate("datuv") + " IS NULL OR " + SapDate("datuv") + " <= " +
		                                 reference + ")"
		                           : string();
	};
	auto material_boms = [&](const string &table) {
		return has(table, "stlty") ? string(" AND stlty = 'M'") : string();
	};
	const bool mast_alternative = has(mast, "stlal");
	const bool item_nodes = has(stpo, "stlkn");

	// MAST: material -> BOM number of the alternative (and usage); plants collapse to one row
	string mast_sql = "SELECT DISTINCT " + client_col + "TRIM(matnr) AS parent_id, stlnr" + string(mast_alternative ? ", stlal" : "") +
	                  " FROM query_table(" + QuoteSQLStringLiteral(mast) + ") WHERE TRUE" +
	                  (mast_alternative ? " AND stlal = " + alternative : string()) +
	                  (by_usage ? " AND stlan = " + QuoteSQLStringLiteral(usage_value->ToString()) : string());

	// STKO: the header version valid on the reference date; the BOM is dropped when it is deleted
	string stko_sql = "SELECT " + client_col + "stlnr, stlal, " + (has(stko, "datuv") ? SapDate("datuv") : string("NULL::DATE")) +
	                  " AS valid_from, " + SapDeleted(context, stko, {"lkenz", "loekz"}) +
	                  " AS deleted, row_number() OVER (PARTITION BY " + client_col + "stlnr, stlal ORDER BY " +
	                  newest_first(stko, "stkoz", "stlnr") + ") AS rn FROM query_table(" + QuoteSQLStringLiteral(stko) +
	                  ") WHERE stlal = " + alternative + material_boms(stko) + valid_on_reference(stko);

	// STPO: per item node (stlkn) the version valid on the reference date; dropped when deleted
	string stpo_sql = "SELECT " + client_col + "stlnr, " + string(item_nodes ? "stlkn" : "NULL AS stlkn") +
	                  ", TRIM(idnrk) AS child_id, menge AS qty, meins AS unit, " +
	                  SapDeleted(context, stpo, {"lkenz"}) + " AS deleted, " +
	                  (item_nodes ? "row_number() OVER (PARTITION BY " + client_col + "stlnr, stlkn ORDER BY " +
	                                    newest_first(stpo, "stpoz", "TRIM(idnrk), menge, meins") + ")"
	                              : string("1")) +
	                  " AS rn FROM query_table(" + QuoteSQLStringLiteral(stpo) + ") WHERE TRUE" +
	                  material_boms(stpo) + valid_on_reference(stpo);

	string stas_cte, stas_join;
	if (!stas.empty()) {
		// STAS: the items the alternative selects, from the newest selection row on the date
		stas_cte = ",\n stas AS (SELECT " + client_col + "stlnr, stlkn, " + SapDeleted(context, stas, {"lkenz"}) +
		           " AS deleted, row_number() OVER (PARTITION BY " + client_col + "stlnr, stlkn ORDER BY " +
		           newest_first(stas, "stasz", "deleted") +
		           ") AS rn FROM query_table(" + QuoteSQLStringLiteral(stas) + ") WHERE stlal = " + alternative +
		           material_boms(stas) + valid_on_reference(stas) + ")";
		stas_join = " JOIN stas a ON a.stlnr = i.stlnr AND a.stlkn = i.stlkn AND a.rn = 1 AND NOT a.deleted" +
		            string(clients ? " AND a.mandt = i.mandt" : "");
	}

	string sql = "WITH mast AS (" + mast_sql +
	             "),\n header AS (" + stko_sql + "),\n item AS (" + stpo_sql + ")" + stas_cte +
	             "\nSELECT h.stlnr AS bom_id, m.parent_id, i.child_id, i.qty, i.unit, h.valid_from, "
	             "'9999-12-31'::DATE AS valid_to, h.stlal AS bom_version"
	             " FROM mast m JOIN header h ON h.stlnr = m.stlnr" + string(clients ? " AND h.mandt = m.mandt" : "") +
	             string(mast_alternative ? " AND h.stlal = m.stlal" : "") +
	             " AND h.rn = 1 AND NOT h.deleted"
	             " JOIN item i ON i.stlnr = h.stlnr AND i.rn = 1 AND NOT i.deleted" +
	             string(clients ? " AND i.mandt = h.mandt" : "") +
	             stas_join;
	return ParseSapSubquery(sql, context);
}

void RegisterSAPTransformationMacros(ExtensionLoader &loader) {
	ddoc::Registrar reg(loader, {"similarity", "erp"});
	reg.RegisterTableMacro(SAP_TO_MATERIALS_MACRO,
	           {ddoc::Doc()
	                .Describe("Extracts materials from a SAP MARA table into the universal materials shape. Requires MARA columns matnr, mtart, matkl, ersda and lvorm; rows flagged deleted via lvorm are excluded. The description column is always empty here -- use sap_to_materials_with_desc to populate it from MAKT; the makt_table and language parameters are accepted for compatibility and ignored.")
	                .Example("SELECT * FROM sap_to_materials('MARA')")});
	reg.RegisterTableMacro(SAP_TO_MATERIALS_WITH_DESC_MACRO,
	           {ddoc::Doc()
	                .Describe("Like sap_to_materials, but joins MAKT to populate the description column in the requested 'language' (SAP single-character language key, e.g. 'E').")
	                .Example("SELECT * FROM sap_to_materials_with_desc('MARA', 'MAKT', language := 'E')")});
	reg.RegisterTableMacro(EXTRACT_MATERIAL_DESCRIPTIONS_MACRO,
	           {ddoc::Doc()
	                .Describe("Extracts material descriptions from a SAP MAKT table for one language, returning (material_id, full_text): the description (MAKTX) and the search text (MAKTG) joined by a space, one row per material.")
	                .Example("SELECT * FROM extract_material_descriptions(makt_table := 'sap_makt', language := 'EN')")});
	// Two overloads, as the macro allowed: positional tables, and (2.0 has no varargs and 1.5.6
	// cannot pass a positional argument by name) the three tables as named parameters.
	TableFunctionSet bom_items_set("sap_to_bom_items");
	vector<FunctionDescription> bom_items_docs;
	for (idx_t positional : {idx_t(3), idx_t(0)}) {
		vector<LogicalType> args(positional, LogicalType(LogicalTypeId::VARCHAR));
		TableFunction f("sap_to_bom_items", args, nullptr, nullptr);
		f.bind_replace = SapToBomItemsBindReplace;
		FunctionDescription desc;
		desc.description =
		    "Converts SAP MAST/STKO/STPO (and optionally STAS) into flat BOM edges (bom_id, parent_id, child_id, qty, "
		    "unit, valid_from, valid_to, bom_version). Needs MAST matnr, stlnr; STKO stlnr, stlal; STPO stlnr, idnrk, "
		    "menge, meins. Each further SAP column is used when the table has it: stlty keeps material BOMs (M) only; "
		    "MAST.stlal must equal bom_alternative (default '01', also matched on STKO.stlal); datuv picks the STKO "
		    "header and, per STPO item node stlkn, the item version valid on reference_date (YYYYMMDD or a date, "
		    "default 9999-12-31), stkoz / stpoz breaking ties; a header version with lkenz or loekz set and an item "
		    "version with lkenz set are dropped. bom_usage filters MAST.stlan. stas_table := 'STAS' applies the item "
		    "selection: an item is kept only when its newest STAS row for the alternative on reference_date exists "
		    "and is not deleted (lkenz). With stlty on STKO, STPO (and STAS) need it too; with mandt in every table, "
		    "clients are kept apart. Plants collapse to one row per material and BOM, but without bom_usage every usage "
		    "and every BOM number of a material is merged (pass bom_usage, e.g. '1' for production). qty keeps the "
		    "MENGE type.";
		desc.examples = {"SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO');",
		                 "SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', stas_table := 'STAS', "
		                 "reference_date := '2024-01-01', bom_usage := '1');"};
		desc.categories = {"similarity", "erp"};
		vector<NamedParameterSpec> named;
		if (positional == 0) {
			named.emplace_back("mast_table", LogicalType(LogicalTypeId::VARCHAR));
			named.emplace_back("stko_table", LogicalType(LogicalTypeId::VARCHAR));
			named.emplace_back("stpo_table", LogicalType(LogicalTypeId::VARCHAR));
		} else {
			desc.parameter_names = {"mast_table", "stko_table", "stpo_table"};
		}
		named.emplace_back("bom_alternative", LogicalType(LogicalTypeId::VARCHAR));
		named.emplace_back("reference_date", LogicalType(LogicalTypeId::VARCHAR));
		named.emplace_back("bom_usage", LogicalType(LogicalTypeId::VARCHAR));
		named.emplace_back("stas_table", LogicalType(LogicalTypeId::VARCHAR));
		for (auto &spec : named) {
			duckdb_compat::AddNamedParameter(f, spec.first, spec.second);
			desc.parameter_names.emplace_back(spec.first);
		}
		desc.parameter_types = args;
		bom_items_set.AddFunction(f);
		bom_items_docs.push_back(std::move(desc));
	}
	CreateTableFunctionInfo info(bom_items_set);
	for (auto &d : bom_items_docs) {
		info.descriptions.push_back(std::move(d));
	}
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace anofox
} // namespace duckdb
