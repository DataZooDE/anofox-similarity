#include "modules/recall_accelerators.hpp"
#include "modules/coverage_sql.hpp"
#include "compat_anofox/scalar_bind.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "telemetry.hpp"
#include "anofox_similarity_banner.hpp"

#include <cmath>
#include <map>

namespace duckdb {
namespace anofox {

namespace {

//------------------------------------------------------------------------------
// Deterministic hashing and randomness (stable across DuckDB versions and platforms)
//------------------------------------------------------------------------------

uint64_t SplitMix(uint64_t &state) {
	state += 0x9E3779B97F4A7C15ULL;
	uint64_t z = state;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

uint64_t Mix(uint64_t a, uint64_t b) {
	uint64_t state = a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
	return SplitMix(state);
}

uint64_t Fnv1a(const string &s) {
	uint64_t h = 1469598103934665603ULL;
	for (unsigned char c : s) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	return h;
}

//! Uniform in (0, 1).
double Uniform(uint64_t &state) {
	return (double(SplitMix(state) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

//------------------------------------------------------------------------------
// weighted_minhash: improved consistent weighted sampling (Ioffe 2010)
//------------------------------------------------------------------------------

vector<uint64_t> WeightedMinhash(const std::map<string, double> &weights, idx_t num_hashes, uint64_t seed) {
	vector<std::pair<uint64_t, double>> elements;
	for (auto &kv : weights) {
		if (kv.second > 0) {
			elements.emplace_back(Fnv1a(kv.first), std::log(kv.second));
		}
	}
	vector<uint64_t> out(num_hashes);
	for (idx_t i = 0; i < num_hashes; i++) {
		double best_a = INFINITY;
		uint64_t best = 0;
		for (auto &e : elements) {
			uint64_t state = Mix(Mix(seed, i), e.first);
			double r = -std::log(Uniform(state) * Uniform(state));
			double c = -std::log(Uniform(state) * Uniform(state));
			double beta = Uniform(state);
			// log a = log c - r (t - beta) - r; t clamped to the int64 range
			double t = std::floor(e.second / r + beta);
			t = MaxValue(-9.0e18, MinValue(9.0e18, t));
			double log_a = std::log(c) - r * (t - beta) - r;
			if (log_a < best_a) {
				best_a = log_a;
				best = Mix(e.first, uint64_t(int64_t(t)));
			}
		}
		out[i] = best;
	}
	return out;
}

void WeightedMinhashFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto map = args.data[0].GetValue(row);
		auto n = args.data[1].GetValue(row);
		Value seed_v = args.ColumnCount() > 2 ? args.data[2].GetValue(row) : Value::BIGINT(7);
		if (map.IsNull() || n.IsNull() || seed_v.IsNull()) {
			result.SetValue(row, Value(LogicalType::LIST(LogicalType::UBIGINT)));
			continue;
		}
		auto num = n.GetValue<int64_t>();
		if (num <= 0 || num > 4096) {
			throw InvalidInputException("weighted_minhash: num_hashes must be between 1 and 4096");
		}
		std::map<string, double> weights;
		for (auto &kv : ListValue::GetChildren(map)) {
			auto &pair = StructValue::GetChildren(kv);
			if (pair[0].IsNull()) {
				continue;
			}
			double w = pair[1].IsNull() ? 1.0 : pair[1].GetValue<double>(); // a NULL weight counts as 1
			if (!std::isfinite(w) || w < 0) {
				throw InvalidInputException("weighted_minhash: weights must not be negative or non-finite");
			}
			weights[pair[0].ToString()] += w;
		}
		bool any = false;
		for (auto &kv : weights) {
			any = any || kv.second > 0;
		}
		if (!any) {
			result.SetValue(row, Value(LogicalType::LIST(LogicalType::UBIGINT)));
			continue;
		}
		auto sig = WeightedMinhash(weights, idx_t(num), uint64_t(seed_v.GetValue<int64_t>()));
		vector<Value> values;
		for (auto h : sig) {
			values.push_back(Value::UBIGINT(h));
		}
		result.SetValue(row, Value::LIST(LogicalType::UBIGINT, std::move(values)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

vector<uint64_t> Signature(const Value &v) {
	vector<uint64_t> out;
	for (auto &x : ListValue::GetChildren(v)) {
		out.push_back(x.IsNull() ? 0 : x.GetValue<uint64_t>());
	}
	return out;
}

void MinhashSimilarityFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto a = args.data[0].GetValue(row);
		auto b = args.data[1].GetValue(row);
		if (a.IsNull() || b.IsNull()) {
			result.SetValue(row, Value(LogicalType::DOUBLE));
			continue;
		}
		auto sa = Signature(a), sb = Signature(b);
		if (sa.size() != sb.size()) {
			throw InvalidInputException("minhash_similarity: signatures must have the same length");
		}
		if (sa.empty()) {
			result.SetValue(row, Value(LogicalType::DOUBLE));
			continue;
		}
		idx_t same = 0;
		for (idx_t i = 0; i < sa.size(); i++) {
			same += sa[i] == sb[i];
		}
		result.SetValue(row, Value::DOUBLE(double(same) / double(sa.size())));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void LshBandsFun(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto a = args.data[0].GetValue(row);
		auto b = args.data[1].GetValue(row);
		if (a.IsNull() || b.IsNull()) {
			result.SetValue(row, Value(LogicalType::LIST(LogicalType::UBIGINT)));
			continue;
		}
		auto sig = Signature(a);
		auto bands = b.GetValue<int64_t>();
		if (bands <= 0 || sig.empty() || sig.size() % idx_t(bands) != 0) {
			throw InvalidInputException("lsh_bands: the signature length must be divisible by bands");
		}
		idx_t rows = sig.size() / idx_t(bands);
		vector<Value> out;
		for (idx_t j = 0; j < idx_t(bands); j++) {
			uint64_t h = Mix(rows, j);
			for (idx_t k = 0; k < rows; k++) {
				h = Mix(h, sig[j * rows + k]);
			}
			out.push_back(Value::UBIGINT(h));
		}
		result.SetValue(row, Value::LIST(LogicalType::UBIGINT, std::move(out)));
	}
	if (args.AllConstant()) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

unique_ptr<FunctionData> WeightedMinhashBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("weighted_minhash");
	return nullptr;
}
unique_ptr<FunctionData> MinhashSimilarityBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("minhash_similarity");
	return nullptr;
}
unique_ptr<FunctionData> LshBandsBind() {
	PostHogTelemetry::Instance().RecordFunctionCall("lsh_bands");
	return nullptr;
}


void RegisterScalarSet(ExtensionLoader &loader, const string &name, LogicalType ret, scalar_function_t fn,
                       bind_scalar_function_t bind, const string &description, vector<CoverageScalarOverload> overloads) {
	RegisterCoverageScalar(loader, {name, std::move(ret), fn, bind, description, {"coverage", "recall"}}, overloads);
}

//------------------------------------------------------------------------------
// minhash_index and find_covering_entities_lsh (LSH-Ensemble style, HLD §4.7)
//------------------------------------------------------------------------------

unique_ptr<TableRef> MinhashIndexBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("minhash_index");
	CoverageParamReader p {context, input.named_parameters, "minhash_index"};
	auto table = p.Table("feature_table", "feature_multiset", {});
	if (table.empty()) {
		throw BinderException("%s: feature_table must not be NULL", p.fn);
	}
	auto entity = p.Column(table, "entity_col", "entity_id", "feature_table");
	auto feature = p.Column(table, "feature_col", "feature_id", "feature_table");
	string weight = "1.0::DOUBLE";
	string combine = "max"; // unit weights are a set, as in find_covering_entities
	auto weight_name = p.Str("weight_col", "weight");
	if (!weight_name.empty()) {
		if (!p.Get("weight_col") && !p.Has(table, "weight")) {
			throw BinderException("%s: feature_table '%s' has no column 'weight'; pass weight_col := '<column>', or "
			                      "weight_col := NULL for unit weights",
			                      p.fn, table);
		}
		weight = "COALESCE(CAST(" + p.Column(table, "weight_col", "weight", "feature_table") + " AS DOUBLE), 1.0)";
		combine = "sum";
	}
	int64_t nh = p.Int("num_hashes", 128);
	int64_t seed = p.Int("seed", 7);
	int64_t parts = p.Int("partitions", 4);
	if (nh <= 0 || nh > 4096 || nh % 8 != 0) {
		throw BinderException("%s: num_hashes must be a multiple of 8 between 8 and 4096", p.fn);
	}
	if (parts < 1 || parts > 64) {
		throw BinderException("%s: partitions must be between 1 and 64", p.fn);
	}
	// band hashes for 1, 2, 4 and 8 rows per band, concatenated in that order
	auto sql = StringUtil::Format(R"(
		WITH raw AS (
			SELECT CAST(%s AS VARCHAR) AS entity_id, CAST(%s AS VARCHAR) AS feature_id, %s AS w
			FROM %s WHERE %s IS NOT NULL AND %s IS NOT NULL
		),
		guard AS (
			SELECT CASE WHEN EXISTS (SELECT 1 FROM raw WHERE w < 0 OR NOT isfinite(w))
			            THEN error('minhash_index: weights must not be negative or non-finite') END AS g
		),
		f AS (SELECT entity_id, feature_id, %s(w) AS w FROM raw, guard WHERE guard.g IS NULL GROUP BY ALL),
		s AS (
			SELECT entity_id, sum(w) AS total_weight,
			       weighted_minhash(map_from_entries(list(struct_pack(k := feature_id, v := w))), %lld, %lld) AS signature
			FROM f WHERE w > 0 GROUP BY entity_id
		)
		SELECT entity_id, total_weight,
		       ntile(%lld) OVER (ORDER BY total_weight, entity_id) AS partition, signature,
		       list_concat(lsh_bands(signature, %lld), lsh_bands(signature, %lld), lsh_bands(signature, %lld),
		                   lsh_bands(signature, %lld)) AS band_hashes,
		       %lld AS num_hashes, %lld AS seed
		FROM s ORDER BY entity_id
	)",
	                              entity, feature, weight, QuotedIdentifier(table), entity, feature, combine, (long long)nh,
	                              (long long)seed, (long long)parts, (long long)nh, (long long)(nh / 2),
	                              (long long)(nh / 4), (long long)(nh / 8), (long long)nh, (long long)seed);
	return CoverageSubquery(sql, context, p.fn);
}

//! The band layout of minhash_index: band_hashes holds lsh_bands for 1, 2, 4 and 8 rows per
//! band, concatenated in that order. Long form: (entity_id, part, r, j, h).
string BandsSQL(const string &idx) {
	return StringUtil::Format(R"(SELECT * FROM (
		SELECT entity_id, part, r,
		       unnest(list_slice(band_hashes, o + 1, o + nh // r)) AS h,
		       generate_subscripts(list_slice(band_hashes, o + 1, o + nh // r), 1) AS j
		FROM (SELECT x.entity_id, x.part, x.band_hashes, x.nh, rr.r,
		             CASE rr.r WHEN 1 THEN 0 WHEN 2 THEN x.nh WHEN 4 THEN x.nh + x.nh // 2
		                       ELSE x.nh + x.nh // 2 + x.nh // 4 END AS o
		      FROM %s x CROSS JOIN (VALUES (1), (2), (4), (8)) rr(r))) b)",
	                          idx);
}

unique_ptr<TableRef> MinhashBandsBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("minhash_bands");
	CoverageParamReader p {context, input.named_parameters, "minhash_bands"};
	auto index = p.Positional(input, 0, "index_table", {"entity_id", "partition", "band_hashes", "num_hashes", "seed"});
	// num_hashes and seed travel along, so a search can check the band table against its index
	auto sql = StringUtil::Format(
	    "WITH idx AS (SELECT CAST(entity_id AS VARCHAR) AS entity_id, CAST(partition AS BIGINT) AS part, band_hashes, "
	    "CAST(num_hashes AS BIGINT) AS nh, CAST(seed AS BIGINT) AS sd FROM %s) "
	    "SELECT bands.entity_id, bands.part, bands.r, bands.j, bands.h, i.nh AS num_hashes, i.sd AS seed FROM (%s) bands "
	    "JOIN idx i ON i.entity_id = bands.entity_id ORDER BY bands.entity_id, bands.r, bands.j",
	    QuotedIdentifier(index), BandsSQL("idx"));
	return CoverageSubquery(sql, context, p.fn);
}

unique_ptr<TableRef> LshSearchBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	PostHogTelemetry::Instance().RecordFunctionCall("find_covering_entities_lsh");
	CoverageParamReader p {context, input.named_parameters, "find_covering_entities_lsh"};
	auto queries = p.Positional(input, 0, "queries_table", {"query_id", "feature_id"});
	auto index = p.Positional(input, 1, "index_table",
	                          {"entity_id", "total_weight", "partition", "signature", "band_hashes", "num_hashes", "seed"});
	int64_t k = p.Int("k", 20);
	if (k <= 0) {
		throw BinderException("%s: k must be positive", p.fn);
	}
	auto mode = StringUtil::Lower(p.Str("mode", "containment"));
	if (mode != "containment" && mode != "jaccard") {
		throw BinderException("%s: mode must be 'containment' or 'jaccard'", p.fn);
	}
	double t = p.Dbl("threshold", 0.5);
	if (!(t > 0 && t <= 1)) {
		throw BinderException("%s: threshold must be in (0, 1]", p.fn);
	}
	bool weighted_queries = p.Has(queries, "weight");
	string weight = weighted_queries ? "COALESCE(CAST(weight AS DOUBLE), 1.0)" : "1.0::DOUBLE";
	string combine = weighted_queries ? "sum" : "max"; // unweighted queries are sets
	// per query and size partition, the Jaccard threshold that the containment threshold implies
	// for the partition's largest set, and the rows per band whose S-curve threshold lies below it
	string jstar = mode == "jaccard" ? DoubleLiteral(t)
	                                 : StringUtil::Format("%s * q.total / (q.total + p.u - %s * q.total)",
	                                                      DoubleLiteral(t), DoubleLiteral(t));
	string estimate = mode == "jaccard" ? "j" : "least(1.0, j * (qt + ct) / ((1 + j) * qt))";
	// the index's band hashes in long form: from a stored minhash_bands table (a hash join,
	// sub-linear) or unnested from the index on the fly (linear in the index size)
	auto bands_table = p.Table("bands_table", "", {"entity_id", "part", "r", "j", "h", "num_hashes", "seed"});
	string bands_sql = bands_table.empty()
	                       ? BandsSQL("idx") + " WHERE EXISTS (SELECT 1 FROM qr WHERE qr.part = b.part AND qr.r = b.r)"
	                       : StringUtil::Format(
	                             "SELECT CAST(entity_id AS VARCHAR) AS entity_id, CAST(part AS BIGINT) AS part, CAST(r AS "
	                             "BIGINT) AS r, CAST(j AS BIGINT) AS j, h FROM %s b WHERE EXISTS (SELECT 1 FROM qr WHERE "
	                             "qr.part = b.part AND qr.r = b.r) AND (SELECT CASE WHEN EXISTS (SELECT 1 FROM %s x WHERE "
	                             "x.num_hashes <> (SELECT nh FROM prm) OR x.seed <> (SELECT sd FROM prm)) OR (SELECT "
	                             "count(DISTINCT entity_id) FROM %s) <> (SELECT count(*) FROM idx) THEN "
	                             "error('find_covering_entities_lsh: bands_table was built from another index') END) IS NULL",
	                             QuotedIdentifier(bands_table), QuotedIdentifier(bands_table), QuotedIdentifier(bands_table));
	auto sql = StringUtil::Format(R"(
		WITH idx AS (
			SELECT CAST(entity_id AS VARCHAR) AS entity_id, CAST(total_weight AS DOUBLE) AS total_weight,
			       CAST(partition AS BIGINT) AS part, signature, band_hashes, CAST(num_hashes AS BIGINT) AS nh,
			       CAST(seed AS BIGINT) AS sd
			FROM %s
		),
		prm AS (
			SELECT CASE WHEN count(DISTINCT nh) > 1 OR count(DISTINCT sd) > 1
			            THEN error('find_covering_entities_lsh: the index must have one num_hashes and seed') END AS g,
			       any_value(nh) AS nh, any_value(sd) AS sd
			FROM idx
		),
		parts AS (SELECT part, max(total_weight) AS u FROM idx GROUP BY ALL),
		qraw AS (SELECT CAST(query_id AS VARCHAR) AS query_id, CAST(feature_id AS VARCHAR) AS f, %s AS w FROM %s WHERE feature_id IS NOT NULL),
		qguard AS (
			SELECT CASE WHEN EXISTS (SELECT 1 FROM qraw WHERE w < 0 OR NOT isfinite(w))
			            THEN error('find_covering_entities_lsh: weights must not be negative or non-finite') END AS g
		),
		qf AS (SELECT query_id, f, %s(w) AS w FROM qraw, qguard WHERE qguard.g IS NULL GROUP BY ALL),
		qs AS (
			SELECT query_id, sum(w) AS total,
			       weighted_minhash(map_from_entries(list(struct_pack(k := f, v := w))), (SELECT nh FROM prm),
			                        (SELECT sd FROM prm)) AS sig
			FROM qf WHERE w > 0 GROUP BY query_id
		),
		qj AS (SELECT q.query_id, q.sig, p.part, least(1.0, greatest(0.0, %s)) AS j FROM qs q CROSS JOIN parts p),
		qr AS (
			SELECT qj.query_id, qj.sig, qj.part,
			       CASE WHEN pow(1.0 / (prm.nh // 8), 1.0 / 8) <= qj.j THEN 8 WHEN pow(1.0 / (prm.nh // 4), 1.0 / 4) <= qj.j THEN 4
			            WHEN pow(1.0 / (prm.nh // 2), 1.0 / 2) <= qj.j THEN 2 ELSE 1 END AS r
			FROM qj, prm WHERE prm.g IS NULL
		),
		qb AS (
			SELECT query_id, part, r, unnest(lsh_bands(sig, prm.nh // r)) AS h, generate_subscripts(lsh_bands(sig, prm.nh // r), 1) AS j
			FROM qr, prm
		),
		ib AS (%s),
		cand AS (
			SELECT DISTINCT qb.query_id, ib.entity_id
			FROM qb JOIN ib ON ib.part = qb.part AND ib.r = qb.r AND ib.j = qb.j AND ib.h = qb.h
		),
		n_cand AS (SELECT query_id, count(*) AS n_candidates FROM cand GROUP BY ALL),
		scored AS (
			SELECT c.query_id, c.entity_id, minhash_similarity(q.sig, i.signature) AS j, q.total AS qt, i.total_weight AS ct
			FROM cand c JOIN qs q USING (query_id) JOIN idx i USING (entity_id)
		)
		SELECT s.query_id, s.entity_id, %s AS estimate, s.j AS jaccard_estimate,
		       row_number() OVER (PARTITION BY s.query_id ORDER BY %s DESC, s.j DESC, s.entity_id) AS rank,
		       n.n_candidates
		FROM scored s JOIN n_cand n USING (query_id)
		QUALIFY rank <= %lld
		ORDER BY s.query_id, rank
	)",
	                              QuotedIdentifier(index), weight, QuotedIdentifier(queries), combine, jstar, bands_sql, estimate,
	                              estimate, (long long)k);
	return CoverageSubquery(sql, context, p.fn);
}

} // namespace

void RegisterRecallAcceleratorFunctions(ExtensionLoader &loader) {
	auto dist = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::DOUBLE);
	auto sig = LogicalType::LIST(LogicalType::UBIGINT);
	auto bi = LogicalType(LogicalTypeId::BIGINT);
	RegisterScalarSet(loader, "weighted_minhash", sig, DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, WeightedMinhashFun),
	                  compat_anofox::ScalarBind<&WeightedMinhashBind>(),
	                  "Weighted MinHash signature of a MAP(key, weight) multiset by improved consistent weighted "
	                  "sampling (ICWS, Ioffe 2010): the share of equal positions of two signatures estimates their "
	                  "weighted Jaccard. num_hashes positions (1..4096), seeded (default 7); hashing is our own (FNV-1a, "
	                  "SplitMix64), independent of DuckDB's hash. A NULL weight counts as 1, zero weights are absent, negative weights are an "
	                  "error; NULL or an empty multiset gives NULL.",
	                  {{{dist, bi}, {"weights", "num_hashes"}, {"SELECT weighted_minhash(MAP {'a': 1.0, 'b': 2.0}, 128);"}},
	                   {{dist, bi, bi},
	                    {"weights", "num_hashes", "seed"},
	                    {"SELECT weighted_minhash(MAP {'a': 1.0, 'b': 2.0}, 128, 7);"}}});
	RegisterScalarSet(loader, "minhash_similarity", LogicalType::DOUBLE,
	                  DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, MinhashSimilarityFun),
	                  compat_anofox::ScalarBind<&MinhashSimilarityBind>(),
	                  "Share of equal positions of two MinHash signatures of the same length: the estimate of their "
	                  "(weighted) Jaccard similarity.",
	                  {{{sig, sig},
	                    {"signature_a", "signature_b"},
	                    {"SELECT minhash_similarity(weighted_minhash(MAP {'a': 1.0}, 64), weighted_minhash(MAP {'a': 1.0, "
	                     "'b': 1.0}, 64)); -- about 0.5"}}});
	RegisterScalarSet(loader, "lsh_bands", sig, DATAZOO_GUARD(ANOFOX_SIMILARITY_BANNER, LshBandsFun),
	                  compat_anofox::ScalarBind<&LshBandsBind>(),
	                  "LSH band hashes of a MinHash signature: one hash per band of len(signature) / bands rows. Two "
	                  "multisets with Jaccard J share at least one band with probability 1 - (1 - J^rows)^bands.",
	                  {{{sig, bi}, {"signature", "bands"}, {"SELECT lsh_bands(weighted_minhash(MAP {'a': 1.0}, 64), 16);"}}});

	auto vc = LogicalType(LogicalTypeId::VARCHAR);
	RegisterCoverageTableFunction(
	    loader, "minhash_index", {}, {}, MinhashIndexBindReplace,
	    {{"feature_table", vc},
	     {"num_hashes", bi},
	     {"seed", bi},
	     {"partitions", bi},
	     {"entity_col", vc},
	     {"feature_col", vc},
	     {"weight_col", vc}},
	    "Builds a weighted-MinHash index of a feature multiset (feature_table, default 'feature_multiset'; "
	    "entity_col/feature_col/weight_col as in find_covering_entities; weight_col := NULL for unit weights, where "
	    "duplicate rows count once): one "
	    "row per entity with total_weight, its size partition (partitions quantiles, default 4), signature "
	    "(num_hashes, a multiple of 8, default 128; seed default 7), band_hashes for 1, 2, 4 and 8 rows per band "
	    "(concatenated), num_hashes and seed. Store it as a table; find_covering_entities_lsh searches it.",
	    {"CREATE TABLE programme_index AS SELECT * FROM minhash_index(feature_table := 'programme_class_multiset');"},
	    {"coverage", "recall"});
	RegisterCoverageTableFunction(
	    loader, "find_covering_entities_lsh", {vc, vc}, {"queries_table", "index_table"}, LshSearchBindReplace,
	    {{"k", bi}, {"mode", vc}, {"threshold", LogicalType::DOUBLE}, {"bands_table", vc}},
	    "Sub-linear candidate search over a minhash_index (LSH-Ensemble style). queries_table holds "
	    "(query_id, feature_id[, weight]) multisets. Per query and size partition, the containment threshold "
	    "(default 0.5) is turned into the Jaccard threshold for the partition's largest set, and the rows per band "
	    "are chosen so that the LSH S-curve passes below it; candidates share a band. Ranked by the estimate: "
	    "containment = J (|q| + |c|) / ((1 + J) |q|) (capped at 1) or J (mode 'jaccard'). Returns query_id, "
	    "entity_id, estimate, jaccard_estimate, rank (top k, default 20) and n_candidates (scored per query). An "
	    "approximate accelerator: exact find_covering_entities stays the reference. Without bands_table the "
	    "index's band hashes are unnested on every call (linear in the index size); store minhash_bands(index) "
	    "once and pass it as bands_table for a hash-join (sub-linear) search.",
	    {"SELECT * FROM find_covering_entities_lsh('request_multisets', 'programme_index', k := 20);"},
	    {"coverage", "recall"});
	RegisterCoverageTableFunction(
	    loader, "minhash_bands", {vc}, {"index_table"}, MinhashBandsBindReplace, {},
	    "The band hashes of a minhash_index in long form (entity_id, part, r, j, h, num_hashes, seed): one row per entity, rows per "
	    "band (1, 2, 4, 8) and band. Store it and pass it to find_covering_entities_lsh as bands_table, so the "
	    "search joins on the band hash instead of unnesting the index.",
	    {"CREATE TABLE programme_bands AS SELECT * FROM minhash_bands('programme_index');"}, {"coverage", "recall"});
}

} // namespace anofox
} // namespace duckdb
