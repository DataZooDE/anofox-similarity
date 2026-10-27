#include "modules/evaluation.hpp"
#include "modules/coverage_sql.hpp"
#include "duckdb/common/string_util.hpp"
#include "telemetry.hpp"

namespace duckdb {
namespace anofox {

namespace {

//! results (query_id, candidate_id, rank | score) and gold (query_id, candidate_id, true_coverage).
struct EvalInput {
	string results, gold, rank_expr, score_expr;
};

EvalInput ReadEvalInput(const CoverageParamReader &p, TableFunctionBindInput &input, bool need_rank, bool need_score) {
	EvalInput e;
	e.results = p.Positional(input, 0, "results", {"query_id", "candidate_id"});
	e.gold = p.Positional(input, 1, "gold", {"query_id", "candidate_id", "true_coverage"});
	// a column named explicitly must exist; the defaults are used when present
	auto column = [&](const char *param, const char *fallback) -> string {
		auto v = p.Get(param);
		if (v && !v->IsNull()) {
			return p.Column(e.results, param, fallback, "results");
		}
		if (v || !p.Has(e.results, fallback)) {
			return string();
		}
		return QuotedIdentifier(fallback);
	};
	auto rank_col = column("rank_col", "rank");
	auto score_col = column("score_col", "coverage");
	if (!score_col.empty()) {
		e.score_expr = "CAST(" + score_col + " AS DOUBLE)";
	}
	if (!rank_col.empty()) {
		e.rank_expr = "CAST(" + rank_col + " AS BIGINT)";
	}
	if (need_rank && rank_col.empty() && score_col.empty()) {
		throw BinderException("%s: results needs a rank column (rank_col) or a score column (score_col)", p.fn);
	}
	if (need_score && score_col.empty()) {
		throw BinderException("%s: results needs a score column (score_col, default 'coverage')", p.fn);
	}
	return e;
}

//! r: one row per (query, candidate) with a strict position: the given rank (ties by candidate
//! id) or the score (highest first, ties by candidate id). g: gold, the best row per pair.
string EvalCTEs(const EvalInput &e, const string &fn) {
	string order = e.rank_expr.empty() ? "score DESC NULLS LAST, candidate_id" : "rank_in, candidate_id";
	return StringUtil::Format(
	    "r_raw AS (SELECT CAST(query_id AS VARCHAR) AS query_id, CAST(candidate_id AS VARCHAR) AS candidate_id, %s AS "
	    "rank_in, %s AS score FROM %s), "
	    "r_dedup AS (SELECT query_id, candidate_id, min(rank_in) AS rank_in, max(score) AS score FROM r_raw GROUP BY ALL), "
	    "rank_guard AS (SELECT CASE WHEN EXISTS (SELECT 1 FROM r_dedup WHERE rank_in < 1) THEN error('%s: rank must be "
	    "at least 1') END AS g), "
	    "r AS (SELECT query_id, candidate_id, score, row_number() OVER (PARTITION BY query_id ORDER BY %s) AS rank "
	    "FROM r_dedup, rank_guard WHERE rank_guard.g IS NULL), "
	    "g AS (SELECT CAST(query_id AS VARCHAR) AS query_id, CAST(candidate_id AS VARCHAR) AS candidate_id, "
	    "max(CAST(true_coverage AS DOUBLE)) AS t FROM %s GROUP BY ALL)",
	    e.rank_expr.empty() ? "NULL::BIGINT" : e.rank_expr, e.score_expr.empty() ? "NULL::DOUBLE" : e.score_expr,
	    QuotedIdentifier(e.results), fn, order, QuotedIdentifier(e.gold));
}

//------------------------------------------------------------------------------
// eval_ranking
//------------------------------------------------------------------------------

unique_ptr<TableRef> EvalRankingBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("eval_ranking");
	CoverageParamReader p {context, input.named_parameters, "eval_ranking"};
	auto e = ReadEvalInput(p, input, true, false);
	vector<int64_t> ks {1, 5, 10, 20};
	if (auto v = p.Get("k"); v && !v->IsNull()) {
		ks.clear();
		for (auto &x : ListValue::GetChildren(*v)) {
			if (x.IsNull() || x.GetValue<int64_t>() <= 0) {
				throw BinderException("%s: k must be positive", p.fn);
			}
			ks.push_back(x.GetValue<int64_t>());
		}
		if (ks.empty()) {
			throw BinderException("%s: k must not be empty", p.fn);
		}
	}
	string k_list;
	for (auto k : ks) {
		k_list += (k_list.empty() ? "" : ", ") + std::to_string(k);
	}
	// best = every gold candidate within 1e-9 of the query's gold best; a ranked candidate
	// without gold has true coverage 0
	auto sql = StringUtil::Format(R"(
		WITH %s,
		best AS (SELECT query_id, max(t) AS b FROM g GROUP BY ALL),
		best_set AS (SELECT g.query_id, g.candidate_id FROM g JOIN best USING (query_id) WHERE g.t >= best.b - 1e-9),
		n_best AS (SELECT query_id, count(*) AS n FROM best_set GROUP BY ALL),
		-- the queries with gold; a gold query without results is a miss with full regret
		queries AS (SELECT query_id FROM best),
		rr AS (
			SELECT r.query_id, r.candidate_id, r.rank, bs.candidate_id IS NOT NULL AS is_best, COALESCE(g.t, 0) AS t
			FROM r LEFT JOIN best_set bs USING (query_id, candidate_id) LEFT JOIN g USING (query_id, candidate_id)
		),
		regret AS (
			SELECT q.query_id, b.b - COALESCE((SELECT min(t) FROM rr WHERE rr.query_id = q.query_id AND rr.rank = 1), 0) AS regret
			FROM queries q JOIN best b USING (query_id)
		),
		ks AS (SELECT unnest([%s]) AS k),
		per AS (
			SELECT ks.k, q.query_id,
			       COALESCE(bool_or(rr.is_best AND rr.rank <= ks.k), false) AS hit,
			       count(*) FILTER (WHERE rr.is_best AND rr.rank <= ks.k) / any_value(nb.n) AS rec,
			       COALESCE(max(1.0 / rr.rank) FILTER (WHERE rr.is_best AND rr.rank <= ks.k), 0.0) AS rr_k
			FROM ks CROSS JOIN queries q
			LEFT JOIN rr ON rr.query_id = q.query_id
			LEFT JOIN n_best nb ON nb.query_id = q.query_id
			GROUP BY ks.k, q.query_id
		)
		SELECT per.k, count(*) AS n_queries, avg(per.hit::INT)::DOUBLE AS top_k_accuracy, avg(per.rec)::DOUBLE AS recall_at_k,
		       avg(per.rr_k)::DOUBLE AS mrr, avg(rg.regret)::DOUBLE AS mean_regret,
		       avg((rg.regret <= 0.05)::INT)::DOUBLE AS share_regret_le_005
		FROM per LEFT JOIN regret rg USING (query_id)
		GROUP BY per.k ORDER BY per.k
	)",
	                              EvalCTEs(e, p.fn), k_list);
	return CoverageSubquery(sql, context, p.fn);
}

//------------------------------------------------------------------------------
// eval_calibration
//------------------------------------------------------------------------------

unique_ptr<TableRef> EvalCalibrationBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("eval_calibration");
	CoverageParamReader p {context, input.named_parameters, "eval_calibration"};
	auto e = ReadEvalInput(p, input, false, true);
	auto sql = StringUtil::Format(R"(
		WITH %s,
		pairs AS (
			SELECT r.score AS p, g.t FROM r JOIN g USING (query_id, candidate_id)
			WHERE r.score IS NOT NULL AND g.t IS NOT NULL
		),
		ranked AS (
			SELECT p, t,
			       rank() OVER (ORDER BY p) + (count(*) OVER (PARTITION BY p) - 1) / 2.0 AS rp,
			       rank() OVER (ORDER BY t) + (count(*) OVER (PARTITION BY t) - 1) / 2.0 AS rt
			FROM pairs
		)
		SELECT count(*) AS n_pairs, avg(abs(p - t)) AS mae, avg(p - t) AS bias,
		       CASE WHEN stddev_pop(p) > 0 AND stddev_pop(t) > 0 THEN corr(p, t) END AS pearson,
		       CASE WHEN stddev_pop(rp) > 0 AND stddev_pop(rt) > 0 THEN corr(rp, rt) END AS spearman
		FROM ranked
	)",
	                              EvalCTEs(e, p.fn));
	return CoverageSubquery(sql, context, p.fn);
}

//------------------------------------------------------------------------------
// threshold_at_precision
//------------------------------------------------------------------------------

unique_ptr<TableRef> ThresholdBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("threshold_at_precision");
	CoverageParamReader p {context, input.named_parameters, "threshold_at_precision"};
	auto e = ReadEvalInput(p, input, true, true);
	double target = p.Dbl("target_precision", 0.95);
	double good = p.Dbl("good", 0.9);
	if (target < 0 || target > 1) {
		throw BinderException("%s: target_precision must be between 0 and 1", p.fn);
	}
	if (good < 0 || good > 1) {
		throw BinderException("%s: good must be between 0 and 1", p.fn);
	}
	// over the top-1 picks: the lowest threshold whose precision meets the target (the largest
	// auto-accept share); tied scores enter together
	auto sql = StringUtil::Format(R"(
		WITH %s,
		picks AS (
			SELECT r.query_id, r.score AS s, COALESCE(g.t, 0) AS t FROM r LEFT JOIN g USING (query_id, candidate_id)
			WHERE r.rank = 1 AND r.score IS NOT NULL
			QUALIFY row_number() OVER (PARTITION BY r.query_id ORDER BY r.score DESC, r.candidate_id) = 1
		),
		n AS (SELECT count(*) AS n FROM picks),
		cum AS (
			SELECT DISTINCT s AS threshold,
			       sum((t >= %s)::INT) OVER w / count(*) OVER w AS precision,
			       count(*) OVER w / (SELECT n FROM n) AS share
			FROM picks WINDOW w AS (ORDER BY s DESC RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
		),
		chosen AS (SELECT min(threshold) AS threshold FROM cum WHERE precision >= %s - 1e-12)
		SELECT c.threshold, k.precision::DOUBLE AS precision, k.share::DOUBLE AS accepted_share, (SELECT n FROM n) AS n_queries,
		       (SELECT list(struct_pack("threshold" := threshold, "precision" := precision::DOUBLE, "share" := share::DOUBLE)
		                    ORDER BY threshold DESC) FROM cum) AS curve
		FROM chosen c LEFT JOIN cum k ON k.threshold = c.threshold
	)",
	                              EvalCTEs(e, p.fn), DoubleLiteral(good), DoubleLiteral(target));
	return CoverageSubquery(sql, context, p.fn);
}

//------------------------------------------------------------------------------
// bootstrap_ci
//------------------------------------------------------------------------------

unique_ptr<TableRef> BootstrapBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("bootstrap_ci");
	CoverageParamReader p {context, input.named_parameters, "bootstrap_ci"};
	if (input.inputs.size() < 2 || input.inputs[1].IsNull()) {
		throw BinderException("%s: cluster_col must not be NULL", p.fn);
	}
	auto cluster = input.inputs[1].GetValue<string>();
	if (cluster.find('.') != string::npos) {
		throw BinderException("%s: cluster_col must be a column name", p.fn);
	}
	ValidateSQLIdentifierPath(cluster, "cluster_col");
	auto table = p.Positional(input, 0, "metric_table", {cluster});
	int64_t n = p.Int("n", 1000);
	int64_t seed = p.Int("seed", 7);
	double level = p.Dbl("level", 0.95);
	if (n < 1 || n > 100000) {
		throw BinderException("%s: n must be between 1 and 100000", p.fn);
	}
	if (!(level > 0 && level < 1)) {
		throw BinderException("%s: level must be in (0, 1)", p.fn);
	}
	auto qc = QuotedIdentifier(cluster);
	string columns;
	if (auto v = p.Get("metrics"); v && !v->IsNull()) {
		auto metrics = StringListParameter(*v, "metrics");
		if (metrics.empty()) {
			throw BinderException("%s: metrics must not be empty", p.fn);
		}
		ValidateTableColumns(context, table, metrics, "metrics");
		for (auto &m : metrics) {
			ValidateSQLIdentifierPath(m, "metrics");
			columns += StringUtil::Format(", TRY_CAST(%s AS DOUBLE) AS %s", QuotedIdentifier(m), QuotedIdentifier(m));
		}
	} else {
		// every other column that casts to a number
		columns = StringUtil::Format(", TRY_CAST(COLUMNS(* EXCLUDE (%s)) AS DOUBLE)", qc);
	}
	// percentile intervals of the pooled mean over resampled clusters (with replacement, drawn
	// from a seeded hash)
	double alpha = (1 - level) / 2;
	auto sql = StringUtil::Format(R"(
		WITH wide AS (SELECT CAST(%s AS VARCHAR) AS __anofox_cluster%s FROM %s),
		long AS (UNPIVOT wide ON COLUMNS(* EXCLUDE (__anofox_cluster)) INTO NAME metric VALUE v),
		guard AS (
			SELECT CASE WHEN EXISTS (SELECT 1 FROM wide WHERE __anofox_cluster IS NULL)
			            THEN error('bootstrap_ci: cluster_col must not be NULL')
			            WHEN (SELECT count(DISTINCT __anofox_cluster) FROM wide) * %lld > 100000000
			            THEN error('bootstrap_ci: n x clusters must not exceed 10^8') END AS g
		),
		cl AS (SELECT metric, __anofox_cluster, sum(v) AS s, count(*) AS c FROM long, guard WHERE guard.g IS NULL GROUP BY ALL),
		ids AS (SELECT __anofox_cluster, row_number() OVER (ORDER BY __anofox_cluster) - 1 AS i FROM (SELECT DISTINCT __anofox_cluster FROM long)),
		m AS (SELECT count(*) AS m FROM ids),
		draws AS (
			SELECT b, CAST(hash(%lld, b, j) %% m.m AS BIGINT) AS i
			FROM (SELECT b, unnest(range(m.m)) AS j FROM range(%lld) t(b), m), m
		),
		boot AS (
			SELECT d.b, cl.metric, sum(cl.s) / sum(cl.c) AS est
			FROM draws d JOIN ids USING (i) JOIN cl USING (__anofox_cluster) GROUP BY ALL
		),
		point AS (SELECT metric, sum(s) / sum(c) AS estimate FROM cl GROUP BY ALL)
		SELECT b.metric, any_value(pt.estimate) AS estimate, quantile_cont(b.est, %s) AS lo,
		       quantile_cont(b.est, %s) AS hi, count(*) AS n_boot, (SELECT m FROM m) AS n_clusters
		FROM boot b JOIN point pt USING (metric)
		GROUP BY b.metric ORDER BY b.metric
	)",
	                              qc, columns, QuotedIdentifier(table), (long long)n, (long long)seed, (long long)n,
	                              DoubleLiteral(alpha), DoubleLiteral(1 - alpha));
	return CoverageSubquery(sql, context, p.fn);
}

} // namespace

void RegisterEvaluationFunctions(ExtensionLoader &loader) {
	auto vc = LogicalType(LogicalTypeId::VARCHAR);
	vector<string> categories {"coverage", "evaluation"};
	RegisterCoverageTableFunction(
	    loader, "eval_ranking", {vc, vc}, {"results", "gold"}, EvalRankingBindReplace,
	    {{"k", LogicalType::LIST(LogicalType::BIGINT)}, {"rank_col", vc}, {"score_col", vc}},
	    "Ranking quality against gold coverage (HLD §4.8). results: query_id, candidate_id and a rank (rank_col, "
	    "default 'rank') or a score (score_col, default 'coverage', highest first); gold: query_id, candidate_id, "
	    "true_coverage. The best candidates of a query are all gold candidates within 1e-9 of its gold best; a ranked "
	    "candidate without gold counts as 0. Per k (default 1, 5, 10, 20): n_queries, top_k_accuracy (share of "
	    "queries with a best candidate in the top k, BRD 'recall@k of the best candidate'), recall_at_k (share of "
	    "each query's best candidates in the top k), mrr (cut at k), mean_regret and share_regret_le_005 of the top "
	    "pick.",
	    {"SELECT * FROM eval_ranking('results', 'gold', k := [1, 20]);"}, categories);
	RegisterCoverageTableFunction(
	    loader, "eval_calibration", {vc, vc}, {"results", "gold"}, EvalCalibrationBindReplace,
	    {{"score_col", vc}},
	    "Calibration of the predicted coverage (score_col, default 'coverage') against gold true_coverage over the "
	    "pairs in both tables: n_pairs, mae, bias (predicted - true), pearson, spearman (average ranks for ties); "
	    "correlations are NULL for a constant input.",
	    {"SELECT * FROM eval_calibration('results', 'gold');"}, categories);
	RegisterCoverageTableFunction(
	    loader, "threshold_at_precision", {vc, vc}, {"results", "gold"}, ThresholdBindReplace,
	    {{"target_precision", LogicalType::DOUBLE}, {"good", LogicalType::DOUBLE}, {"rank_col", vc}, {"score_col", vc}},
	    "The auto-accept threshold: over the top-1 picks, the lowest score threshold whose precision (share of picks "
	    "with true coverage >= good, default 0.9) reaches target_precision (default 0.95; 'target' is a reserved word). Returns threshold (NULL when "
	    "unreachable), precision, accepted_share, n_queries and the precision curve (threshold, precision, share).",
	    {"SELECT threshold, precision, accepted_share FROM threshold_at_precision('results', 'gold', target_precision := 0.95);"},
	    categories);
	RegisterCoverageTableFunction(
	    loader, "bootstrap_ci", {vc, vc}, {"metric_table", "cluster_col"}, BootstrapBindReplace,
	    {{"metrics", LogicalType::LIST(LogicalType::VARCHAR)},
	     {"n", LogicalType::BIGINT},
	     {"seed", LogicalType::BIGINT},
	     {"level", LogicalType::DOUBLE}},
	    "Cluster bootstrap intervals (BR-51): resamples the clusters (cluster_col) with replacement n times (default "
	    "1000, seeded) and reports per metric the pooled mean (estimate) and the percentile interval lo/hi at level "
	    "(default 0.95), with n_boot and n_clusters. metrics defaults to every other numeric column.",
	    {"SELECT * FROM bootstrap_ci('per_request', 'maker', metrics := ['hit', 'regret']);"}, categories);
}

} // namespace anofox
} // namespace duckdb
