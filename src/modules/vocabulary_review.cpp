#include "modules/vocabulary_review.hpp"
#include "modules/coverage_sql.hpp"
#include "modules/property_tree.hpp"
#include "duckdb_compat.hpp"
#include "core/sql_safety.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "telemetry.hpp"

#include <cmath>

namespace duckdb {
namespace anofox {

namespace {



//------------------------------------------------------------------------------
// vocabulary_review_queue
//------------------------------------------------------------------------------

unique_ptr<TableRef> QueueBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("vocabulary_review_queue");
	CoverageParamReader p {context, input.named_parameters, "vocabulary_review_queue"};
	auto trees = p.Table("trees_table", "tree_nodes", {"tree_id", "node_id", "node_kind", "label"});
	if (trees.empty()) {
		throw BinderException("%s: trees_table must not be NULL", p.fn);
	}
	auto decisions = p.Table("decisions", "", {"node_id", "class_id", "p"});
	if (decisions.empty()) {
		throw BinderException("%s: decisions (a classify_to_catalogue result) is required", p.fn);
	}
	auto overrides = p.Table("overrides_table", "", {"wording"});
	string source_filter;
	if (auto v = p.Get("sources"); v && !v->IsNull()) {
		source_filter = " AND source IN (SELECT unnest(" + QuotedList(StringListParameter(*v, "sources")) + "))";
	}
	string reviewed = "SELECT NULL::VARCHAR AS wording, NULL::VARCHAR AS source WHERE false";
	if (!overrides.empty()) {
		reviewed = StringUtil::Format("SELECT CAST(wording AS VARCHAR) AS wording, %s AS source FROM %s",
		                              OptionalColumnSQL(p.context, overrides, "source", "VARCHAR"), QuotedIdentifier(overrides));
	}
	auto sql = StringUtil::Format(R"(
		WITH items AS (
			SELECT * FROM (
				SELECT CAST(tree_id AS VARCHAR) AS tree_id, CAST(node_id AS VARCHAR) AS node_id,
				       CAST(label AS VARCHAR) AS label, %s AS source
				FROM %s WHERE lower(CAST(node_kind AS VARCHAR)) = 'item' AND label IS NOT NULL)
			WHERE true%s
		),
		d AS (
			SELECT %s AS tree_id, CAST(node_id AS VARCHAR) AS node_id, %s AS source,
			       CAST(class_id AS VARCHAR) AS class_id, CAST(p AS DOUBLE) AS p
			FROM %s WHERE class_id IS NOT NULL AND p IS NOT NULL
		),
		-- decisions per node: averaged over the decided occurrences of the wording
		node_d AS (
			SELECT i.label AS wording, i.source, d.tree_id, d.node_id, d.class_id, d.p
			FROM d JOIN items i ON i.tree_id = d.tree_id AND i.node_id = d.node_id
			WHERE d.tree_id IS NOT NULL
		),
		node_n AS (SELECT wording, source, count(DISTINCT (tree_id, node_id)) AS n FROM node_d GROUP BY ALL),
		node_dist AS (
			SELECT x.wording, x.source, x.class_id, sum(x.p) / any_value(n.n) AS p
			FROM node_d x JOIN node_n n ON n.wording = x.wording AND n.source IS NOT DISTINCT FROM x.source
			GROUP BY ALL
		),
		-- decisions per wording (tree_id NULL, node_id = the wording); a NULL source matches any
		occ AS (SELECT label AS wording, source, count(*) AS n_occurrences, count(DISTINCT tree_id) AS n_trees_affected
		        FROM items GROUP BY ALL),
		word_dist AS (
			SELECT o.wording, o.source, d.class_id, d.p, CASE WHEN d.source IS NULL THEN 3 ELSE 2 END AS prio
			FROM d JOIN occ o ON o.wording = d.node_id AND (d.source IS NULL OR d.source = o.source)
			WHERE d.tree_id IS NULL
		),
		-- one distribution per wording: node-level over wording-level, an exact source over NULL
		dist_all AS (SELECT wording, source, class_id, p, 1 AS prio FROM node_dist UNION ALL SELECT * FROM word_dist),
		dist AS (
			SELECT wording, source, class_id, avg(p) AS p FROM (
				SELECT *, min(prio) OVER (PARTITION BY wording, source) AS best FROM dist_all)
			WHERE prio = best GROUP BY ALL
		),
		guard AS (
			SELECT CASE WHEN EXISTS (SELECT 1 FROM d WHERE NOT (p >= 0 AND p <= 1))
			            THEN error('vocabulary_review_queue: p must be in [0, 1] in decisions') END AS g
		),
		ranked AS (
			SELECT *, row_number() OVER (PARTITION BY wording, source ORDER BY p DESC, class_id) AS r FROM dist
		),
		tops AS (
			SELECT wording, source,
			       max(class_id) FILTER (WHERE r = 1) AS top1, max(p) FILTER (WHERE r = 1) AS p1,
			       max(class_id) FILTER (WHERE r = 2) AS top2, max(p) FILTER (WHERE r = 2) AS p2
			FROM ranked GROUP BY ALL
		),
		reviewed AS (%s)
		SELECT t.wording, t.source, o.n_occurrences, o.n_trees_affected, t.top1, t.p1, t.top2, t.p2,
		       t.p1 - COALESCE(t.p2, 0) AS margin,
		       o.n_trees_affected * (1 - (t.p1 - COALESCE(t.p2, 0))) AS impact
		FROM tops t JOIN occ o ON o.wording = t.wording AND o.source IS NOT DISTINCT FROM t.source
		CROSS JOIN guard
		WHERE guard.g IS NULL AND NOT EXISTS (SELECT 1 FROM reviewed v WHERE v.wording = t.wording
		                  AND (v.source IS NULL OR v.source IS NOT DISTINCT FROM t.source))
		ORDER BY impact DESC, t.wording, t.source
	)",
	                              OptionalColumnSQL(p.context, trees, "source", "VARCHAR"), QuotedIdentifier(trees), source_filter,
	                              OptionalColumnSQL(p.context, decisions, "tree_id", "VARCHAR"), OptionalColumnSQL(p.context, decisions, "source", "VARCHAR"),
	                              QuotedIdentifier(decisions), reviewed);
	return CoverageSubquery(sql, context, p.fn);
}

//------------------------------------------------------------------------------
// apply_vocabulary_review
//------------------------------------------------------------------------------

unique_ptr<TableRef> ApplyReviewBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("apply_vocabulary_review");
	CoverageParamReader p {context, input.named_parameters, "apply_vocabulary_review"};
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("%s: review_table must not be NULL", p.fn);
	}
	auto review = p.Check(input.inputs[0].GetValue<string>(), "review_table", {"wording", "class_id"});
	// not given: merge into vocabulary_overrides when it exists (a fresh start otherwise);
	// NULL: no previous overrides
	string fallback = TableExists(context, "vocabulary_overrides") ? "vocabulary_overrides" : "";
	auto overrides = p.Table("overrides_table", fallback, {"wording", "class_id"});
	auto rows = [&](const string &table) {
		return StringUtil::Format(
		    "SELECT CAST(wording AS VARCHAR) AS wording, %s AS source, CAST(class_id AS VARCHAR) AS class_id, "
		    "%s AS reviewed_by, %s AS reviewed_at FROM %s WHERE wording IS NOT NULL",
		    OptionalColumnSQL(p.context, table, "source", "VARCHAR"), OptionalColumnSQL(p.context, table, "reviewed_by", "VARCHAR"),
		    OptionalColumnSQL(p.context, table, "reviewed_at", "TIMESTAMP"), QuotedIdentifier(table));
	};
	string old = overrides.empty() ? "SELECT NULL::VARCHAR AS wording, NULL::VARCHAR AS source, NULL::VARCHAR AS "
	                                 "class_id, NULL::VARCHAR AS reviewed_by, NULL::TIMESTAMP AS reviewed_at WHERE false"
	                               : rows(overrides);
	// one entry per (wording, source): the review wins; within the review, the last reviewed_at
	auto sql = StringUtil::Format(R"(
		WITH r_all AS (%s),
		r AS (
			SELECT * FROM r_all
			QUALIFY row_number() OVER (PARTITION BY wording, source ORDER BY reviewed_at DESC NULLS LAST, class_id) = 1
		),
		o AS (
			SELECT * FROM (%s)
			QUALIFY row_number() OVER (PARTITION BY wording, source ORDER BY reviewed_at DESC NULLS LAST, class_id) = 1
		),
		guard AS (
			SELECT CASE WHEN EXISTS (SELECT 1 FROM r_all WHERE class_id IS NULL)
			            THEN error('apply_vocabulary_review: class_id must not be NULL in review_table') END AS g
		)
		SELECT * EXCLUDE (g) FROM (
			SELECT * FROM o WHERE NOT EXISTS (SELECT 1 FROM r WHERE r.wording = o.wording
			                                  AND r.source IS NOT DISTINCT FROM o.source)
			UNION ALL
			SELECT * FROM r) CROSS JOIN guard
		WHERE guard.g IS NULL
		ORDER BY wording, source
	)",
	                              rows(review), old);
	return CoverageSubquery(sql, context, p.fn);
}

//------------------------------------------------------------------------------
// uncertain_decisions
//------------------------------------------------------------------------------

unique_ptr<TableRef> UncertainBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("uncertain_decisions");
	CoverageParamReader p {context, input.named_parameters, "uncertain_decisions"};
	auto decisions = p.Table("decisions", "", {"node_id", "class_id", "p", "rank", "margin"});
	if (decisions.empty()) {
		throw BinderException("%s: decisions (a classify_to_catalogue result) is required", p.fn);
	}
	auto threshold_v = p.Get("threshold");
	auto share_v = p.Get("share");
	bool has_threshold = threshold_v && !threshold_v->IsNull();
	bool has_share = share_v && !share_v->IsNull();
	if (has_threshold && has_share) {
		throw BinderException("%s: give either threshold or share, not both", p.fn);
	}
	double threshold = has_threshold ? threshold_v->GetValue<double>() : 0.2;
	double share = has_share ? share_v->GetValue<double>() : 0;
	if (!std::isfinite(threshold) || (has_share && !(share > 0 && share <= 1))) {
		throw BinderException("%s: threshold must be finite and share in (0, 1]", p.fn);
	}
	int64_t top = 3;
	if (auto v = p.Get("top"); v && !v->IsNull()) {
		top = v->GetValue<int64_t>();
		if (top <= 0) {
			throw BinderException("%s: top must be positive", p.fn);
		}
	}
	auto texts = p.Table("texts_table", "", {"node_id", "text"});
	auto catalogue = p.Table("catalogue_table", "", {"class_id"});
	bool dec_tree = p.Has(decisions, "tree_id");
	string texts_sql = "SELECT NULL::VARCHAR AS tree_id, NULL::VARCHAR AS node_id, NULL::VARCHAR AS text WHERE false";
	string text_guard = "SELECT NULL AS g";
	string text_join = "t.node_id = s.node_id AND t.tree_id IS NOT DISTINCT FROM s.tree_id";
	if (!texts.empty()) {
		bool text_tree = p.Has(texts, "tree_id");
		if (dec_tree && !text_tree) {
			// texts keyed by node_id alone are fine only when no decision has a tree_id
			text_guard = "SELECT CASE WHEN EXISTS (SELECT 1 FROM d WHERE tree_id IS NOT NULL) THEN error("
			             "'uncertain_decisions: texts_table needs a tree_id column when the decisions have tree ids') "
			             "END AS g";
		}
		texts_sql = StringUtil::Format(
		    "SELECT %s AS tree_id, CAST(node_id AS VARCHAR) AS node_id, min(CAST(text AS VARCHAR)) AS text "
		    "FROM %s GROUP BY ALL",
		    CastOrNullSQL(text_tree, "tree_id", "VARCHAR"), QuotedIdentifier(texts));
		if (!text_tree) {
			text_join = "t.node_id = s.node_id";
		}
	}
	string cat_sql = "SELECT NULL::VARCHAR AS class_id, NULL::VARCHAR AS label, NULL::VARCHAR AS definition WHERE false";
	if (!catalogue.empty()) {
		string label = p.Has(catalogue, "label_en") ? "CAST(label_en AS VARCHAR)" : OptionalColumnSQL(p.context, catalogue, "label", "VARCHAR");
		cat_sql = StringUtil::Format("SELECT CAST(class_id AS VARCHAR) AS class_id, min(%s) AS label, "
		                             "min(%s) AS definition FROM %s GROUP BY ALL",
		                             label, OptionalColumnSQL(p.context, catalogue, "definition", "VARCHAR"), QuotedIdentifier(catalogue));
	}
	string select = has_share ? StringUtil::Format("SELECT * FROM items QUALIFY row_number() OVER (ORDER BY margin, "
	                                               "tree_id, node_id) <= ceil(%s * count(*) OVER ())",
	                                               DoubleLiteral(share))
	                          : StringUtil::Format("SELECT * FROM items WHERE margin < %s", DoubleLiteral(threshold));
	string candidate =
	    "'{\"class_id\":' || " + JsonStringSQL("c.class_id") + " || ',\"p\":' || " + JsonNumberSQL("c.p") +
	    " || ',\"label\":' || " + JsonStringSQL("k.label") + " || ',\"definition\":' || " +
	    JsonStringSQL("k.definition") + " || '}'";
	string request = "'{\"tree_id\":' || " + JsonStringSQL("s.tree_id") + " || ',\"node_id\":' || " +
	                 JsonStringSQL("s.node_id") + " || ',\"text\":' || " + JsonStringSQL("any_value(t.text)") +
	                 " || ',\"margin\":' || " + JsonNumberSQL("s.margin") + " || ',\"candidates\":[' || string_agg(" +
	                 candidate + ", ',' ORDER BY c.rank) || ']}'";
	auto sql = StringUtil::Format(R"(
		WITH d AS (
			SELECT %s AS tree_id, CAST(node_id AS VARCHAR) AS node_id, CAST(class_id AS VARCHAR) AS class_id,
			       CAST(p AS DOUBLE) AS p, CAST(rank AS BIGINT) AS rank, CAST(margin AS DOUBLE) AS margin,
			       %s AS overridden
			FROM %s
		),
		items AS (
			SELECT tree_id, node_id, any_value(margin) AS margin FROM d
			WHERE NOT COALESCE(overridden, false) GROUP BY ALL
		),
		sel AS (%s),
		texts AS (%s),
		cat AS (%s),
		guard AS (%s)
		SELECT s.tree_id, s.node_id, s.margin, %s AS request
		FROM sel s CROSS JOIN guard
		JOIN d c ON c.node_id = s.node_id AND c.tree_id IS NOT DISTINCT FROM s.tree_id AND c.rank <= %lld
		LEFT JOIN cat k ON k.class_id = c.class_id
		LEFT JOIN texts t ON %s
		WHERE guard.g IS NULL
		GROUP BY s.tree_id, s.node_id, s.margin
		ORDER BY s.margin, s.tree_id, s.node_id
	)",
	                              CastOrNullSQL(dec_tree, "tree_id", "VARCHAR"),
	                              OptionalColumnSQL(p.context, decisions, "overridden", "BOOLEAN"), QuotedIdentifier(decisions), select,
	                              texts_sql, cat_sql, text_guard, request, (long long)top, text_join);
	return CoverageSubquery(sql, context, p.fn);
}


} // namespace

void RegisterVocabularyReviewFunctions(ExtensionLoader &loader) {
	auto vc = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCoverageTableFunction(
	    loader, "vocabulary_review_queue", {}, {}, QueueBindReplace,
	    {{"trees_table", vc},
	     {"decisions", vc},
	     {"sources", LogicalType::LIST(LogicalType::VARCHAR)},
	     {"overrides_table", vc}},
	    "The vocabulary review queue: one row per distinct wording and source of the item nodes in "
	    "trees_table (default 'tree_nodes'), with n_occurrences, n_trees_affected, the classifier's top1/p1 and "
	    "top2/p2, margin and impact = n_trees_affected x (1 - margin), highest impact first. decisions is a "
	    "classify_to_catalogue result: per wording (tree_id NULL, node_id = wording) or per node (averaged over the "
	    "wording's occurrences). Wordings in overrides_table are reviewed and leave the queue; sources restricts "
	    "the document families.",
	    {"SELECT * FROM vocabulary_review_queue(trees_table := 'programme_nodes', decisions := 'wording_decisions', "
	     "overrides_table := 'vocabulary_overrides') LIMIT 10;"},
	    {"coverage", "curation"});
	RegisterCoverageTableFunction(
	    loader, "apply_vocabulary_review", {vc}, {"review_table"}, ApplyReviewBindReplace,
	    {{"overrides_table", vc}},
	    "Applies a review (wording, class_id[, source, reviewed_by, reviewed_at]) to the overrides (default "
	    "'vocabulary_overrides' when that table exists; NULL for none): one entry per wording and source, the review wins. Returns the "
	    "new overrides table; table functions cannot write, so store it: CREATE OR REPLACE TABLE "
	    "vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review').",
	    {"CREATE OR REPLACE TABLE vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review');"},
	    {"coverage", "curation"});
	RegisterCoverageTableFunction(
	    loader, "uncertain_decisions", {}, {}, UncertainBindReplace,
	    {{"decisions", vc},
	     {"threshold", LogicalType::DOUBLE},
	     {"share", LogicalType::DOUBLE},
	     {"top", LogicalType::BIGINT},
	     {"texts_table", vc},
	     {"catalogue_table", vc}},
	    "The external re-rank hook: the low-margin items of a classify_to_catalogue result "
	    "(margin < threshold, default 0.2, or the lowest-margin share of the items), overridden items excluded, "
	    "lowest margin first. request is JSON text for an LLM or a human: tree_id, node_id, text (texts_table: "
	    "[tree_id,] node_id, text), margin and the top candidates with p, label and definition (catalogue_table: "
	    "class_id[, label_en|label, definition]). Write the answers back as an answers_table for "
	    "classify_to_catalogue, or as vocabulary_overrides per wording.",
	    {"SELECT request FROM uncertain_decisions(decisions := 'decided', share := 0.15, texts_table := 'texts', "
	     "catalogue_table := 'catalogue_classes');"},
	    {"coverage", "curation"});
}

} // namespace anofox
} // namespace duckdb
