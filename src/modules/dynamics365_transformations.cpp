#include "modules/dynamics365_transformations.hpp"
#include "duckdb/common/types/date.hpp"
#include "core/error_handling.hpp"
#include "core/sql_safety.hpp"
#include "duckdb_compat.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "telemetry.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/main/client_context.hpp"

#include <initializer_list>

namespace duckdb {
namespace anofox {

//------------------------------------------------------------------------------
// Dynamics 365 (Finance & Operations / SCM) -> universal materials, bom_header, bom_component
//------------------------------------------------------------------------------
//
// bind_replace table functions, like sap_to_bom_items: they read the D365 table fields
// (BOMVersion.ItemId, BOMId, FromDate, ToDate, Active, Approved; BOM.BOMId, ItemId, BOMQty,
// BOMQtySerie, UnitId, ...) and use the optional ones an export has. Data-entity (OData) exports
// name the fields differently; map them with a view (docs/api/05-erp-transformations.md).
//
// BOMVersion links items to BOMs (many-to-many). For each company and manufactured item one version
// is taken on reference_date: valid on the date (FromDate <= date, ToDate empty, 1900-01-01 or >=
// date), then active first, approved first, newest FromDate, lowest BOMId. A BOM shared by several
// items gives one header per item, so bom_id is '<item>/<BOMId>' ('<company>/<item>/<BOMId>' with
// DataAreaId), each segment escaped (a backslash and '/' get a backslash in front), so ids never
// collide. The company is also an output column (lower case); rows of different companies are never
// merged.

namespace {

using Names = std::initializer_list<const char *>;

// the first of the candidate column names the table has, or "" (column names are hard-coded here,
// so they are safe to inline)
string D365Column(ClientContext &context, const string &table, Names names) {
	for (auto name : names) {
		if (TableHasColumn(context, table, name)) {
			return name;
		}
	}
	return string();
}

string D365RequiredColumn(ClientContext &context, const string &fn, const string &param, const string &table,
                          Names names) {
	auto column = D365Column(context, table, names);
	if (!column.empty()) {
		return column;
	}
	string list;
	for (auto name : names) {
		list += (list.empty() ? "" : " or ") + string(name);
	}
	string hint;
	if (TableHasColumn(context, table, "ApprovedStatus") || TableHasColumn(context, table, "ActivationDate") ||
	    TableHasColumn(context, table, "VersionId") || TableHasColumn(context, table, "ScrapPercent")) {
		hint = " (this looks like the column contract of releases before v2026.11; see docs/UPGRADING.md for the "
		       "mapping to the D365 field names)";
	}
	throw BinderException("%s: %s '%s' has no column %s%s", fn, param, table, list, hint);
}

// D365 dates: DATE, TIMESTAMP or ISO text ('2024-01-01T00:00:00Z' from OData); 1900-01-01 means "none"
string D365Date(const string &column) {
	if (column.empty()) {
		return "NULL::DATE";
	}
	return "NULLIF(TRY_CAST(left(trim(" + column + "::VARCHAR), 10) AS DATE), DATE '1900-01-01')";
}

// NoYes: BOOLEAN, 0/1, or 'Yes'/'No' text
string D365Bool(const string &column) {
	if (column.empty()) {
		return "NULL::BOOLEAN";
	}
	auto v = "lower(trim(" + column + "::VARCHAR))";
	return "(CASE WHEN " + v + " IN ('1', 'true', 'yes') THEN TRUE WHEN " + v +
	       " IN ('0', 'false', 'no') THEN FALSE END)";
}

// one segment of a composed id with the backslash and '/' escaped, so 'P/A' + 'B' and 'P' + 'A/B'
// give different ids
string D365IdSegment(const string &expr) {
	return "replace(replace(" + expr + ", '\\', '\\\\'), '/', '\\/')";
}

// the company of a row: DataAreaId lower-cased and trimmed (exports differ in case), or NULL
string D365Company(ClientContext &context, const string &table) {
	return TableHasColumn(context, table, "DataAreaId") ? string("NULLIF(lower(trim(DataAreaId::VARCHAR)), '')")
	                                                    : string("NULL::VARCHAR");
}

string D365Text(const string &column) {
	return column.empty() ? string("NULL::VARCHAR") : "NULLIF(trim(" + column + "::VARCHAR), '')";
}

template <class NAMED>
Value D365Named(const NAMED &named, const string &name) {
	auto value = duckdb_compat::FindNamedParameter(named, name);
	return value ? *value : Value(LogicalType::SQLNULL);
}

// a table parameter: absent -> fallback (empty: optional), NULL -> error, else a validated path
template <class NAMED>
string D365Table(const NAMED &named, const string &fn, const string &name, const string &fallback,
                 const Value *positional = nullptr) {
	Value value;
	bool given = false;
	if (positional) {
		value = *positional;
		given = true;
	} else if (auto found = duckdb_compat::FindNamedParameter(named, name)) {
		value = *found;
		given = true;
	}
	if (!given) {
		return fallback;
	}
	if (value.IsNull()) {
		if (fallback.empty()) {
			return string(); // an optional table passed as NULL: not used
		}
		throw BinderException("%s: %s must not be NULL", fn, name);
	}
	return ValidateSQLIdentifierPath(value.ToString(), name);
}

// reference_date: YYYY-MM-DD (optionally followed by 'T' or ' ' and a time) or YYYYMMDD; anything
// else, trailing text included, is an error
template <class NAMED>
string D365ReferenceDate(const NAMED &named, const string &fn) {
	auto value = D365Named(named, "reference_date");
	string text = value.IsNull() ? string("9999-12-31") : value.ToString();
	auto digits = [&](idx_t from, idx_t count) {
		for (idx_t i = from; i < from + count; i++) {
			if (i >= text.size() || text[i] < '0' || text[i] > '9') {
				return false;
			}
		}
		return true;
	};
	string iso;
	if (text.size() == 8 && digits(0, 8)) {
		iso = text.substr(0, 4) + "-" + text.substr(4, 2) + "-" + text.substr(6, 2);
	} else if (text.size() >= 10 && digits(0, 4) && text[4] == '-' && digits(5, 2) && text[7] == '-' && digits(8, 2) &&
	           (text.size() == 10 || ((text[10] == 'T' || text[10] == ' ') && text.size() > 11))) {
		iso = text.substr(0, 10);
	} else {
		throw BinderException("%s: reference_date '%s' is not a date (YYYY-MM-DD or YYYYMMDD)", fn, text);
	}
	if (text.size() > 10) {
		// a time after the date must be a valid timestamp ('2024-06-30T12:00:00Z', not '2024-06-30Tjunk')
		string spelled = text;
		spelled[10] = ' ';
		try {
			Timestamp::FromString(spelled, true);
		} catch (std::exception &) {
			throw BinderException("%s: reference_date '%s' is not a date (YYYY-MM-DD or YYYYMMDD)", fn, text);
		}
	}
	try {
		return "DATE '" + Date::ToString(Date::FromString(iso, true)) + "'";
	} catch (std::exception &) {
		throw BinderException("%s: reference_date '%s' is not a date (YYYY-MM-DD or YYYYMMDD)", fn, text);
	}
}

unique_ptr<SubqueryRef> D365Subquery(const string &query, ClientContext &context, const string &fn) {
	auto statements = duckdb_compat::ParseStatements(context, query);
	if (statements.size() != 1 || statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw ParserException("%s: failed to parse the generated query", fn);
	}
	auto &select_stmt = statements[0]->Cast<SelectStatement>();
	auto new_stmt = make_uniq<SelectStatement>();
	new_stmt->node = std::move(select_stmt.node);
	return make_uniq<SubqueryRef>(std::move(new_stmt));
}

// The chosen version per company and item on the reference date (CTE "chosen"): area (the company,
// or NULL), parent_id, source_bom_id, valid_from, valid_to, is_active, is_approved; "chosen_ids"
// adds the escaped bom_id.
struct D365Versions {
	string sql;
	bool areas = false;
};

// The company (DataAreaId) is part of bom_id whenever BOMVersion has it, so both functions build the
// same ids; lines and BOMTable rows are matched on it when they have it too (with it on the lines
// or BOMTable but not on the versions, the binds raise an error).
D365Versions ChosenVersions(ClientContext &context, const string &fn, const string &versions, const string &reference) {
	// the old contract would pass the required columns and silently ignore its dates and status
	if ((TableHasColumn(context, versions, "ActivationDate") && !TableHasColumn(context, versions, "FromDate")) ||
	    (TableHasColumn(context, versions, "ApprovedStatus") && !TableHasColumn(context, versions, "Approved"))) {
		throw BinderException("%s: bom_version '%s' has ActivationDate / ApprovedStatus instead of FromDate / Approved: "
		                      "that is the column contract of releases before v2026.11; see docs/UPGRADING.md for the "
		                      "mapping to the D365 field names",
		                      fn, versions);
	}
	auto item = D365RequiredColumn(context, fn, "bom_version", versions, {"ItemId"});
	auto bom = D365RequiredColumn(context, fn, "bom_version", versions, {"BOMId"});
	auto from = D365Date(D365Column(context, versions, {"FromDate"}));
	auto to = D365Date(D365Column(context, versions, {"ToDate"}));
	auto active = D365Bool(D365Column(context, versions, {"Active"}));
	auto approved = D365Bool(D365Column(context, versions, {"Approved"}));
	D365Versions out;
	out.areas = TableHasColumn(context, versions, "DataAreaId");
	string area = D365Company(context, versions);
	out.sql = "chosen AS (SELECT * EXCLUDE (rn) FROM (SELECT *, row_number() OVER (PARTITION BY area, parent_id "
	          "ORDER BY COALESCE(is_active, FALSE) DESC, COALESCE(is_approved, FALSE) DESC, valid_from DESC NULLS LAST, "
	          "source_bom_id) AS rn FROM (SELECT " +
	          area + " AS area, trim(" + item + "::VARCHAR) AS parent_id, trim(" + bom +
	          "::VARCHAR) AS source_bom_id, " + from + " AS valid_from, " + to + " AS valid_to, " + active +
	          " AS is_active, " + approved + " AS is_approved FROM query_table(" + QuoteSQLStringLiteral(versions) +
	          ")) v WHERE parent_id IS NOT NULL AND source_bom_id IS NOT NULL AND (valid_from IS NULL OR valid_from <= " +
	          reference + ") AND (valid_to IS NULL OR valid_to >= " + reference +
	          ")) ranked WHERE rn = 1), "
	          "chosen_ids AS (SELECT *, CASE WHEN area IS NULL THEN '' ELSE " + D365IdSegment("area") +
	          " || '/' END || " + D365IdSegment("parent_id") + " || '/' || " + D365IdSegment("source_bom_id") +
	          " AS bom_id FROM chosen)";
	return out;
}

//------------------------------------------------------------------------------
// dynamics365_to_materials(invent_table)
//------------------------------------------------------------------------------

unique_ptr<TableRef> D365MaterialsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	const string fn = "dynamics365_to_materials";
	PostHogTelemetry::Instance().RecordFunctionCall(fn);
	auto table = D365Table(input.named_parameters, fn, "invent_table", "InventTable",
	                       input.inputs.empty() ? nullptr : &input.inputs[0]);
	auto item = D365RequiredColumn(context, fn, "invent_table", table, {"ItemId"});
	auto name = D365Text(D365Column(context, table, {"ItemName", "NameAlias"}));
	auto group = D365Text(D365Column(context, table, {"ItemGroupId"}));
	auto type_column = D365Column(context, table, {"ItemType"});
	// D365 ItemType: 0 Item, 1 BOM, 2 Service (numbers, or the enum names in an export)
	string type = "NULL::VARCHAR";
	if (!type_column.empty()) {
		auto v = "lower(trim(" + type_column + "::VARCHAR))";
		type = "CASE " + v + " WHEN '0' THEN 'ITEM' WHEN 'item' THEN 'ITEM' WHEN '1' THEN 'BOM' WHEN 'bom' THEN 'BOM' "
		       "WHEN '2' THEN 'SERVICE' WHEN 'service' THEN 'SERVICE' ELSE 'OTHER' END";
	}
	// one row per company and item (InventTable is keyed by DataAreaId, ItemId)
	string sql = "SELECT DISTINCT ON (company, material_id) * FROM (SELECT trim(" + item +
	             "::VARCHAR) AS material_id, trim(" + item + "::VARCHAR) AS material_number, " + name +
	             " AS description, " + type + " AS material_type, " + group +
	             " AS material_group, NULL::VARCHAR AS procurement_type, NULL::VARCHAR AS base_uom, "
	             "NULL::DECIMAL(12,4) AS weight, NULL::DECIMAL(12,4) AS cost_per_unit, 'DYNAMICS365' AS source_system, "
	             "TRUE AS is_active, CURRENT_TIMESTAMP::TIMESTAMP AS created_at, " +
	             D365Company(context, table) + " AS company FROM query_table(" + QuoteSQLStringLiteral(table) +
	             ") WHERE " + item + " IS NOT NULL) m ORDER BY company NULLS FIRST, material_id, description NULLS LAST";
	return D365Subquery(sql, context, fn);
}

//------------------------------------------------------------------------------
// dynamics365_to_bom_header(bom_version, bom_table, reference_date)
//------------------------------------------------------------------------------

unique_ptr<TableRef> D365HeaderBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	const string fn = "dynamics365_to_bom_header";
	PostHogTelemetry::Instance().RecordFunctionCall(fn);
	auto versions = D365Table(input.named_parameters, fn, "bom_version", "BOMVersion");
	auto headers = D365Table(input.named_parameters, fn, "bom_table", "");
	auto reference = D365ReferenceDate(input.named_parameters, fn);
	string site;
	if (!headers.empty()) {
		D365RequiredColumn(context, fn, "bom_table", headers, {"BOMId"});
		site = D365Column(context, headers, {"SiteId"});
	}
	const bool header_areas = !headers.empty() && TableHasColumn(context, headers, "DataAreaId");
	auto chosen = ChosenVersions(context, fn, versions, reference);
	if (header_areas && !chosen.areas) {
		throw BinderException("%s: bom_table '%s' has DataAreaId but bom_version '%s' does not: add DataAreaId to the "
		                      "versions, or filter bom_table to one company",
		                      fn, headers, versions);
	}
	const bool match_areas = header_areas && chosen.areas;
	string site_join, site_value = "NULL::VARCHAR";
	if (!headers.empty() && !site.empty()) {
		// one site per (company, BOMId): the lowest when BOMTable repeats a BOM
		site_join = " LEFT JOIN (SELECT DISTINCT ON (area, bom) * FROM (SELECT " +
		            string(match_areas ? D365Company(context, headers) : string("NULL::VARCHAR")) +
		            " AS area, trim(BOMId::VARCHAR) AS bom, " + D365Text(site) +
		            " AS site FROM query_table(" + QuoteSQLStringLiteral(headers) +
		            ")) ORDER BY area, bom, site NULLS LAST) t ON t.bom = c.source_bom_id" +
		            string(match_areas ? " AND t.area IS NOT DISTINCT FROM c.area" : "");
		site_value = "t.site";
	}
	string sql = "WITH " + chosen.sql +
	             " SELECT c.bom_id, 'DYNAMICS365' AS source_system, c.source_bom_id, c.parent_id AS parent_material_id, "
	             "'MANUFACTURING' AS bom_type, NULL::VARCHAR AS alternative_number, NULL::VARCHAR AS revision, "
	             "1::DECIMAL(18,6) AS base_quantity, NULL::VARCHAR AS base_uom, c.valid_from, c.valid_to, " +
	             site_value + "::VARCHAR AS plant_id, COALESCE(c.is_approved, FALSE) AS is_approved, "
	             "CURRENT_TIMESTAMP::TIMESTAMP AS created_at, c.area AS company FROM chosen_ids c" +
	             site_join;
	return D365Subquery(sql, context, fn);
}

//------------------------------------------------------------------------------
// dynamics365_to_bom_component(bom_lines, bom_version, reference_date)
//------------------------------------------------------------------------------

unique_ptr<TableRef> D365ComponentBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	const string fn = "dynamics365_to_bom_component";
	PostHogTelemetry::Instance().RecordFunctionCall(fn);
	auto lines = D365Table(input.named_parameters, fn, "bom_lines", "BOM");
	auto versions = D365Table(input.named_parameters, fn, "bom_version", "BOMVersion");
	auto reference = D365ReferenceDate(input.named_parameters, fn);

	// the old contract's scrap column would be read as 0: say what changed instead
	if ((TableHasColumn(context, lines, "ScrapPercent") && !TableHasColumn(context, lines, "ScrapVar")) ||
	    (TableHasColumn(context, lines, "Quantity") && !TableHasColumn(context, lines, "BOMQty"))) {
		throw BinderException("%s: bom_lines '%s' has ScrapPercent / Quantity instead of ScrapVar / BOMQty: that is the column contract of "
		                      "releases before v2026.11; see docs/UPGRADING.md for the mapping to the D365 field names",
		                      fn, lines);
	}
	auto bom = D365RequiredColumn(context, fn, "bom_lines", lines, {"BOMId"});
	auto item = D365RequiredColumn(context, fn, "bom_lines", lines, {"ItemId"});
	auto qty = D365RequiredColumn(context, fn, "bom_lines", lines, {"BOMQty"});
	auto serie = D365Column(context, lines, {"BOMQtySerie"});
	auto unit = D365Column(context, lines, {"UnitId"});
	auto line = D365Column(context, lines, {"LineNum"});
	auto type = D365Column(context, lines, {"BOMType"});
	auto consumption = D365Column(context, lines, {"BOMConsump"});
	auto scrap = D365Column(context, lines, {"ScrapVar"});
	auto operation = D365Column(context, lines, {"OprNum"});
	auto from = D365Date(D365Column(context, lines, {"FromDate"}));
	auto to = D365Date(D365Column(context, lines, {"ToDate"}));
	auto chosen = ChosenVersions(context, fn, versions, reference);
	if (!chosen.areas && TableHasColumn(context, lines, "DataAreaId")) {
		// the lines of several companies would merge under one company-less header
		throw BinderException("%s: bom_lines '%s' has DataAreaId but bom_version '%s' does not: add DataAreaId to the "
		                      "versions, or filter the lines to one company",
		                      fn, lines, versions);
	}
	const bool match_areas = chosen.areas && TableHasColumn(context, lines, "DataAreaId");

	// quantity per one parent: BOMQty / BOMQtySerie (a series of 0 or none counts as 1)
	string quantity = qty + "::DOUBLE" +
	                  (serie.empty() ? string() : " / COALESCE(NULLIF(" + serie + "::DOUBLE, 0), 1)");
	string type_value = "'ITEM'";
	if (!type.empty()) {
		// BOMType: 0 Item, 1 Phantom, 2 Pegged supply, 3 Vendor (numbers or enum names)
		auto v = "lower(trim(" + type + "::VARCHAR))";
		type_value = "CASE WHEN " + v + " IN ('0', 'item') THEN 'ITEM' WHEN " + v +
		             " IN ('1', 'phantom') THEN 'PHANTOM' WHEN " + v + " IN ('2', 'pegged', 'peggedsupply') THEN 'PEGGED' "
		             "WHEN " + v + " IN ('3', 'vendor') THEN 'VENDOR' WHEN " + type + " IS NULL THEN 'ITEM' ELSE 'OTHER' END";
	}
	string fixed = "FALSE";
	if (!consumption.empty()) {
		// BOMConsump: 0 Variable, 1 Constant
		auto v = "lower(trim(" + consumption + "::VARCHAR))";
		fixed = "COALESCE(" + v + " IN ('1', 'constant'), FALSE)";
	}
	string line_sql =
	    "line_rows AS (SELECT " + string(match_areas ? D365Company(context, lines) : string("NULL::VARCHAR")) +
	    " AS area, trim(" + bom + "::VARCHAR) AS source_bom_id, trim(" + item + "::VARCHAR) AS child_id, " +
	    (line.empty() ? string("NULL::DOUBLE") : "TRY_CAST(" + line + " AS DOUBLE)") + " AS line_num, " + quantity +
	    " AS qty, " + (unit.empty() ? string("'EA'") : "COALESCE(" + D365Text(unit) + ", 'EA')") + " AS uom, " +
	    (scrap.empty() ? string("0") : "COALESCE(TRY_CAST(" + scrap + " AS DOUBLE), 0)") + " AS scrap, " + from +
	    " AS effective_from, " + to + " AS effective_to, " + type_value + " AS component_type, " + fixed +
	    " AS is_fixed, " + (operation.empty() ? string("NULL::INTEGER") : "TRY_CAST(" + operation + " AS INTEGER)") +
	    " AS operation_sequence FROM query_table(" + QuoteSQLStringLiteral(lines) + ") WHERE " + item +
	    " IS NOT NULL AND (" + from + " IS NULL OR " + from + " <= " + reference + ") AND (" + to + " IS NULL OR " +
	    to + " >= " + reference + "))";

	// one row per BOM, child, unit and consumption type (constant lines do not scale with the
	// production quantity, so they are not added to variable ones); quantities of the child's lines
	// are added, the other fields come from its first line. Grouped on the chosen version's tuple,
	// not on the display id.
	string sql =
	    "WITH " + chosen.sql + ", " + line_sql +
	    ", merged AS (SELECT c.area, c.parent_id, c.source_bom_id, any_value(c.bom_id) AS bom_id, l.child_id, l.uom, "
	    "l.is_fixed, sum(l.qty) AS qty, min(l.line_num) AS first_line, "
	    "arg_min(l.scrap, COALESCE(l.line_num, 0)) AS scrap, min(l.effective_from) AS effective_from, "
	    "CASE WHEN count(*) > count(l.effective_to) THEN NULL ELSE max(l.effective_to) END AS effective_to, "
	    "arg_min(l.component_type, COALESCE(l.line_num, 0)) AS component_type, "
	    "arg_min(l.operation_sequence, COALESCE(l.line_num, 0)) AS operation_sequence "
	    "FROM line_rows l JOIN chosen_ids c ON c.source_bom_id = l.source_bom_id" +
	    string(match_areas ? " AND c.area IS NOT DISTINCT FROM l.area" : "") +
	    " GROUP BY c.area, c.parent_id, c.source_bom_id, l.child_id, l.uom, l.is_fixed) "
	    "SELECT 'COMP/' || bom_id || '/' || " + D365IdSegment("child_id") + " || '/' || " + D365IdSegment("uom") +
	    " || CASE WHEN is_fixed THEN '/constant' ELSE '/variable' END AS component_id, bom_id, "
	    "row_number() OVER (PARTITION BY bom_id ORDER BY first_line NULLS LAST, child_id, uom, is_fixed)::INTEGER "
	    "AS line_number, "
	    "child_id AS child_material_id, qty::DECIMAL(18,6) AS quantity_per, uom::VARCHAR AS quantity_uom, "
	    "is_fixed AS is_fixed_quantity, scrap::DECIMAL(8,4) AS scrap_percent, effective_from, effective_to, "
	    "component_type::VARCHAR AS component_type, NULL::VARCHAR AS supply_type, operation_sequence, "
	    "FALSE AS is_alternative, NULL::VARCHAR AS alternative_group, CURRENT_TIMESTAMP::TIMESTAMP AS created_at, "
	    "area AS company FROM merged";
	return D365Subquery(sql, context, fn);
}

void RegisterD365Function(ExtensionLoader &loader, const string &name, table_function_bind_replace_t bind,
                          vector<string> positional, vector<string> named, const string &description,
                          vector<string> examples) {
	TableFunctionSet set(duckdb_compat::FunctionName(name));
	vector<FunctionDescription> docs;
	// with the tables positional (where there are), and with every argument named
	vector<idx_t> overloads = positional.empty() ? vector<idx_t> {0} : vector<idx_t> {positional.size(), 0};
	for (auto count : overloads) {
		vector<LogicalType> args(count, LogicalType(LogicalTypeId::VARCHAR));
		TableFunction f(duckdb_compat::FunctionName(name), args, nullptr, nullptr);
		f.bind_replace = bind;
		FunctionDescription desc;
		desc.description = description;
		desc.examples = examples;
		desc.categories = {"similarity", "erp"};
		vector<string> named_here = named;
		if (count == 0) {
			named_here.insert(named_here.begin(), positional.begin(), positional.end());
		} else {
			desc.parameter_names = positional;
		}
		for (auto &n : named_here) {
			duckdb_compat::AddNamedParameter(f, n, LogicalType(LogicalTypeId::VARCHAR));
			desc.parameter_names.push_back(n);
		}
		desc.parameter_types = args;
		set.AddFunction(f);
		docs.push_back(std::move(desc));
	}
	CreateTableFunctionInfo info(set);
	for (auto &d : docs) {
		info.descriptions.push_back(std::move(d));
	}
	duckdb_compat::NameFunctionsFromDescriptions(info);
	loader.RegisterFunction(std::move(info));
}

} // namespace

void RegisterDynamics365TransformationMacros(ExtensionLoader &loader) {
	RegisterD365Function(
	    loader, "dynamics365_to_materials", D365MaterialsBindReplace, {"invent_table"}, {},
	    "Maps a Dynamics 365 InventTable to the universal materials shape, one row per company and item. Needs "
	    "ItemId; uses ItemName (or NameAlias), ItemGroupId, ItemType (0 Item, 1 BOM, 2 Service, or the enum names) "
	    "and DataAreaId (the company column, lower case) when present. Columns not in D365 are typed NULLs.",
	    {"SELECT * FROM dynamics365_to_materials('InventTable');"});
	RegisterD365Function(
	    loader, "dynamics365_to_bom_header", D365HeaderBindReplace, {}, {"bom_version", "bom_table", "reference_date"},
	    "Maps Dynamics 365 BOMVersion (ItemId, BOMId; optional FromDate, ToDate, Active, Approved, DataAreaId) to the "
	    "universal bom_header shape: one header per company and manufactured item, the version valid on "
	    "reference_date (YYYY-MM-DD or YYYYMMDD, default 9999-12-31; ToDate empty or 1900-01-01 = open), active "
	    "first, approved first, newest FromDate, lowest BOMId. bom_id is '<item>/<BOMId>' ('<company>/<item>/<BOMId>' "
	    "with DataAreaId; a backslash or '/' inside a segment gets a backslash in front), source_bom_id the BOMId, "
	    "company the lower-cased DataAreaId. bom_table (BOMTable, optional) gives plant_id from SiteId.",
	    {"SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', bom_table := 'BOMTable');",
	     "SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', reference_date := '2024-06-30');"});
	RegisterD365Function(
	    loader, "dynamics365_to_bom_component", D365ComponentBindReplace, {},
	    {"bom_lines", "bom_version", "reference_date"},
	    "Maps Dynamics 365 BOM lines (BOMId, ItemId, BOMQty; optional BOMQtySerie, UnitId, LineNum, FromDate, ToDate, "
	    "BOMType, BOMConsump, ScrapVar, OprNum, DataAreaId) of the BOMs that dynamics365_to_bom_header chooses (same "
	    "bom_version and reference_date) to the universal bom_component shape. quantity_per = BOMQty / BOMQtySerie "
	    "(per one parent), lines valid on reference_date only, one row per BOM, child, unit and consumption type "
	    "(the quantities of a child's lines are added; constant and variable lines stay apart), company the "
	    "lower-cased DataAreaId. bom_to_items over the two gives one edge per component row (constant and variable "
	    "lines of a child are two edges; sum them for one) and has no "
	    "company column: filter one company first.",
	    {"SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion');"});
}

} // namespace anofox
} // namespace duckdb
