# Round 5: demand planner, launch and phase-out

## Goal

Plan a launch and a phase-out at a pump maker with the extension, on DuckDB v1.5.6
(`./build/release/duckdb`, extension 29ad5b7). Find the predecessors of two new pumps, pick a
cold-start analog for a pump with no history, build transactional embeddings, and combine BOM and
consumption similarity into one decision. Check every result against a planted truth.

## Setup

- Generator: `/tmp/claude-1000/-home-jr-Projects-datazoo-anofox-similarity/8931a555-d34f-4741-ab0b-88e11223db86/scratchpad/r5planner/gen.py`
  (seeded). It copies `examples/erp_extract/data` (MARA, MAKT, MAST, STKO, STPO, STAS), adds 16
  materials and 6 BOMs in SAP format, and writes a new MSEG.
- Load script: `.../scratchpad/r5planner/load.sql` (guide 12 steps; the goods_movements view also
  maps `bwart AS movement_type`). Database: `.../scratchpad/r5planner/r5.duckdb`.
- Sizes: 29 MARA rows (client 100), 9 pumps with BOMs (43 edges), 5 567 MSEG rows over 158 weeks
  (2023-11-06 to 2026-11-13): 788 × 101, 3 016 × 261, 165 × 262, 1 554 × 601, 44 × 602, plus
  client-200 rows. After the guide-12 filter: 4 726 movements of 25 materials.
- Scale run: the same data ×200 (1 800 parents, 8 600 BOM rows, 284 000 movements).
- Planted truth:
  - KP-45 (`…100400`) replaces KP-40 (`…100100`): a 16-week hand-over from 2025-09-01.
  - KP-80 (`…100500`) replaces KP-70 (`…100600`): an 18-week hand-over from 2026-03-02.
  - KP-75 (`…100700`) is KP-80's sibling with the same Jaccard as KP-70, still selling. Not a predecessor.
  - KP-40 gets one customer return (602, S, 3 pcs) on 2026-08-12, eight months after its last delivery.
  - KP-90 (`…100800`) is new with no history. The right analog is KP-75 (29 months, seasonal).
  - KP-92 (`…101000`) is KP-90's near twin, with 6 sporadic deliveries over 14 months.
  - KP-65 (`…100300`) launched 2026-10-12 and has 4 weeks of history.
  - Plant 1000 posts nothing in the weeks of 2024-03-04, 03-11 and 03-18 (strike).
  - All pumps have summer seasonality (±35 %).
- The anofox-forecast extension: community build `e7f3138`, from `INSTALL anofox_forecast FROM community`.

## What worked

- Guide 12's SAP path scaled to the larger extract without changes. `sap_to_bom_items` with STAS,
  `bom_usage := '1'` and `client = '100'` gave the expected edges. KP-40 has 4 components,
  because the extract deletes its bearing in 2023.
- `infer_predecessors` found both planted predecessors and dropped the sibling KP-75, also with
  `min_confidence := 0` and `min_similarity := 0.1`. KP-80 → KP-70: confidence 0.6759,
  correlation −0.6461, temporal 0.82, 24 weeks.
- The strike gap and the seasonality did not create false predecessors.
- The batch form `infer_predecessors(materials_table := ...)` returned the same rows as the
  single calls.
- `cold_start_analogs('…100800', 5)` ranked KP-75 first (0.6667, 29 months). That matches the truth.
- The doc claims about embedding positions match the source. 30 features sit at 0–29, the six ERP
  features at 92–97, and the rest are 0. A feature with no statistics row comes out as 0.
- `check_statistics_freshness` reported the stored statistics correctly (98 rows, `is_fresh` true).
- Performance, ×200 scale: batch `infer_predecessors` for 400 new pumps over 284 000 movements took
  0.47 s. Batch `cold_start_analogs` took 0.06 s. On the base data each call took 0.02–0.05 s.
  Transactional embeddings for 1 600 materials took 1.06 s, but only with the shim and patch
  described in F1 and F2.

## Findings

### F1 [high] Transactional embeddings cannot run with the community anofox-forecast, and the error says it is not loaded

The extension calls a scalar `anofox_fcst_ts_features(values, dates)`. The community build
`e7f3138` does not have it. It has only the aggregate `anofox_fcst_ts_features_agg` (and
`ts_features`). The error tells you to install and load it, but it is already loaded.

```sql
INSTALL anofox_forecast FROM community; LOAD anofox_forecast;
SELECT check_anofox_forecast_available();
-- false
SELECT count(*) FROM compute_transactional_embeddings();
-- Binder Error: compute_transactional_embeddings requires the anofox-forecast extension, which is not
-- loaded. Install and load it first: INSTALL anofox_forecast FROM community; LOAD anofox_forecast;
SELECT count(*) FROM recompute_embedding_statistics();   -- same for extract_ts_features(), compute_domain_specific_statistics()
-- Catalog Error: Scalar Function with name anofox_fcst_ts_features does not exist!
-- Did you mean "anofox_fcst_ts_features_agg"?
```

The feature names do not match either. The macros read 30 struct fields (`transactional_embeddings.cpp`).
Only 12 of them exist in the output of `anofox_fcst_ts_features_agg`: mean, median, variance,
standard_deviation, length, the two maximum locations, kurtosis, skewness, both entropies and
benford_correlation. The other 18 are named differently or do not exist:
`autocorrelation__lag_1` against `autocorrelation_lag1`, `sum_values` against `sum`,
`coefficient_variation` against `variation_coefficient`, and `fft_coefficient__attr_real__coeff_0`
against `fft_coefficient_0_real`. `autocorrelation__lag_14`/`_28`, `range_count` and the
`fft_aggregated__*` fields do not exist at all. The forecast repo dropped the scalar in commit
`28d37b5e` ("Fix table macros ... C++ API compatibility").

The test `test/sql/functions/embeddings/transactional_embeddings.test` only checks that the
functions are registered. No test ever runs them against anofox-forecast.

- src: `src/modules/transactional_embeddings.cpp:41-45` (check macro), `:87-107` (catalog check and error text),
  `src/modules/embedding_statistics.cpp:28` (the call)
- doc: `docs/api/04-embeddings.md` (Status table says "real, needs `anofox-forecast`", with no
  version); `docs/concepts/embeddings-and-fusion.md:43`.
- Fix: call `anofox_fcst_ts_features_agg(movement_date::TIMESTAMP, quantity::DOUBLE)` as an
  aggregate and map its field names. Make the check look for the function that is actually used.
  When the extension is loaded but the function is missing, say "anofox_forecast <version> lacks
  <function>". Add a CI test that loads the community anofox_forecast and runs the pipeline.
- To test F2–F6 I wrote a test shim, a user macro `anofox_fcst_ts_features` that maps the `_agg`
  output to the expected names (`.../scratchpad/r5planner/mkshim.py`, `shim.sql`). Fields that
  cannot be mapped are NULL: lags 14 and 28, `range_count` and the FFT aggregates. Everything below
  that involves embeddings used this shim.

### F2 [high] compute_transactional_embeddings does not bind on 1.5.6 even with the dependency present

With the shim loaded (`check_anofox_forecast_available()` = true) the function fails with an error
about its own SQL:

```sql
SELECT * FROM compute_transactional_embeddings();
-- Binder Error: UNNEST requires a single list as input
```

`raw_embedding` is a `FLOAT[128]` array, and `UNNEST` in FROM takes only a list:

```sql
SELECT * FROM (SELECT [1,2]::FLOAT[2] a) CROSS JOIN LATERAL UNNEST(a) AS t(v);
-- Binder Error: UNNEST requires a single list as input
SELECT * FROM (SELECT [1,2]::FLOAT[2] a) CROSS JOIN LATERAL UNNEST(a::FLOAT[]) AS t(v);   -- works
```

- src: `src/modules/transactional_embeddings.cpp:312` and `:323`
- Fix: `UNNEST(raw_embedding::FLOAT[])`. Better, normalise with `list_transform` and
  `list_sum(x*x)` and skip the unnest/regroup. I extracted the SQL template with that one change
  (`.../scratchpad/r5planner/extract_te_sql.py`, `te_fixed.sql`) to test F3–F6.

### F3 [high] Positions 92–97 are always 0 when you follow the documented statistics step

The docs say: "`recompute_embedding_statistics` returns them; store them in that table". That macro
emits placeholder rows named `reserved_domain_specific_92` … `_97`. The embedding looks the stats up
under other names: `movement_type_issue_ratio`, `movement_type_reversal_ratio`,
`weekday_weekend_ratio`, `weekday_concentration`, `lifecycle_trend_strength` and
`lifecycle_growth_indicator`. No row matches, so every ERP feature becomes 0. Only the
undocumented extra step `compute_domain_specific_statistics` fills them.

```sql
INSERT OR REPLACE INTO transactional_embedding_statistics
SELECT *, CURRENT_TIMESTAMP, 1 FROM recompute_embedding_statistics();
SELECT feature_index, feature_name, mean_value, std_value, num_samples FROM transactional_embedding_statistics
WHERE feature_index BETWEEN 92 AND 97;
-- 92 | reserved_domain_specific_92 | 0.0 | 1.0 | 1   (same for 93-97)
-- embedding (te_fixed.sql), non-zero 0-based positions of KP-50 and seal 200010:
-- [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 20, 21, 22, 23, 26, 27]
INSERT OR REPLACE INTO transactional_embedding_statistics
SELECT *, CURRENT_TIMESTAMP, 1 FROM compute_domain_specific_statistics();
-- now feature_index 92..97 each have 2 rows; non-zero positions add 92, 94, 95, 96, 97
```

- src: `src/modules/embedding_statistics.cpp:431-441` (reserved rows),
  `src/modules/transactional_embeddings.cpp:260-275` (lookup names)
- doc: `docs/concepts/embeddings-and-fusion.md:50-56`, `docs/api/04-embeddings.md` (Rules: Statistics)
- Fix: have `recompute_embedding_statistics` emit the six domain rows under the names the embedding
  reads (union in `compute_domain_specific_statistics`). Or document both inserts. Also give
  `transactional_embedding_statistics` one row per `feature_index`.

### F4 [medium] Feature 93 (262 share) is always 0 with the documented MSEG view, and feature 92 is 0 for every finished good

Guide 12 signs MSEG by SHKZG, so 262 reversals become negative. The ERP features and
`filter_recent_movements` keep only `quantity > 0`, so every 262 row is dropped. Feature 93 then has
std 0 and becomes 0 for all materials. Guide 03 says returns "net against the issues of the same
week". That holds for `infer_predecessors`, but the embedding drops them.

Feature 92 counts only 261 rows. Pumps (finished goods) move with 601, so their share is always 0,
and feature 92 just separates components from finished goods. Guide 12's view also has no
`movement_type` column. With it, 92 and 93 are NULL → 0 for every material, and
`compute_domain_specific_statistics` fails with `Binder Error: Referenced column "movement_type"
not found`.

```sql
SELECT feature_name, feature_index, round(mean_value,4), round(std_value,4), num_samples
FROM compute_domain_specific_statistics();
-- movement_type_issue_ratio    | 92 | 0.6676 | 0.4598 | 25
-- movement_type_reversal_ratio | 93 | 0.0    | 0.0    | 25
SELECT material_id, count(*) FILTER (WHERE movement_type='262') n262,
       count(*) FILTER (WHERE movement_type='262' AND quantity>0) n262_pos
FROM goods_movements WHERE material_id = '000000000000200010' GROUP BY 1;
-- 000000000000200010 | 16 | 0
```

- src: `src/modules/transactional_embeddings.cpp:181,216`, `src/modules/embedding_statistics.cpp:464-479`
- doc: `docs/concepts/embeddings-and-fusion.md:47-49`, `docs/guides/12-from-erp-extract.md:120-126`
- Fix: count movement types on the absolute quantity, or before the sign filter. Count 601 next to
  261 (or "issue or delivery"). Add `bwart AS movement_type` to the guide 12 view and say that the
  ERP features need it.

### F5 [medium] "Cosine compares the profile of demand, not its volume" is false

Positions 0–9 are z-scores of mean, median, variance, std, sum, intercept and length. The vector
is scaled to length 1 only after the z-scoring, which does not take volume out. The same weekly
pattern ×10 is almost orthogonal to the original:

```sql
CREATE VIEW gm_scaled AS SELECT * FROM goods_movements
UNION ALL SELECT 'KP75_X10', movement_date, quantity*10, movement_type FROM goods_movements
WHERE material_id = '000000000000100700';
-- te_scaled.sql (F2-patched template over gm_scaled, stored statistics, shim)
SELECT round(array_cosine_similarity(a.transactional_embedding, b.transactional_embedding),3)
FROM te_s a, te_s b WHERE a.material_id='000000000000100700' AND b.material_id='KP75_X10';
-- 0.006
```

- doc: `docs/concepts/embeddings-and-fusion.md:54-56`, and the table row "transactional … raw
  volume comparisons (the vector is normalised)"
- Fix: correct the text ("volume features count; scale-free comparison needs …"). Or drop or
  normalise the level features (use CV and share-based features) when a scale-free profile is wanted.

### F6 [medium] Short histories give NaN: the statistics refresh fails and new materials get NaN vectors

The forecast aggregate returns NaN for short series. I saw it for sample_entropy and lags 4–8 on
KP-65 (4 weeks), KP-92 (6 movements) and four components. `STDDEV_POP` over NaN aborts the whole
refresh. `COALESCE` does not catch NaN, so the embedding of those materials is NaN. Those are
exactly the new materials a planner asks about.

```sql
-- shim.sql (raw forecast values)
SELECT * FROM recompute_embedding_statistics();
-- Out of Range Error: STDDEV_POP is out of range!
-- embedding, te_fixed.sql with shim.sql:
SELECT material_id FROM te_nan WHERE list_bool_or(list_transform(transactional_embedding::FLOAT[], lambda x: isnan(x)));
-- 000000000000100300, 000000000000101000, 000000000000200021, 000000000000200030, 000000000000200031, 000000000000200099
```

With a NaN→NULL shim (`shim_nonan.sql`) both work. This depends on the F1 fix: once the extension
reads the forecast output, it has to handle NaN.

- src: `src/modules/embedding_statistics.cpp:117` (and every `STDDEV_POP`),
  `src/modules/transactional_embeddings.cpp:225-262` (`COALESCE` per feature)
- Fix: `CASE WHEN isnan(x) OR isinf(x) THEN NULL ELSE x END` per feature before the statistics and
  the z-scores. Raise `min_observations` for the embedding, or document it.

### F7 [medium] One late customer return moves the phase-out date and costs the true predecessor its temporal score

`predecessor_last_usage`, the temporal score and cold start's `last_usage` take the latest row of
any sign. KP-40's only movement after December 2025 is one return (602, −3) on 2026-08-12. It counts
as last usage, so the time order looks wrong. It also adds a 24th "overlap week" with quantity −3.

```sql
SELECT predecessor_id, confidence, correlation, similarity, temporal_score, predecessor_last_usage, overlapping_weeks
FROM infer_predecessors('000000000000100400');
-- 000000000000100100 | 0.5653 | -0.8131 | 0.5 | 0.3 | 2026-08-12 | 24
CREATE VIEW gm_no_return AS SELECT * FROM goods_movements
WHERE NOT (material_id='000000000000100100' AND movement_date = DATE '2026-08-12');
SELECT predecessor_id, confidence, correlation, similarity, temporal_score, predecessor_last_usage, overlapping_weeks
FROM infer_predecessors('000000000000100400', movements_table := 'gm_no_return');
-- 000000000000100100 | 0.7142 | -0.7854 | 0.5 | 0.8333 | 2025-12-17 | 23
SELECT material_id, round(similarity,4), history_months, last_usage FROM cold_start_analogs('000000000000100300', 5);
-- 000000000000100100 | 0.1429 | 33 | 2026-08-12   (KP-40 looks active until August)
```

Guide 03 says "Review pairs below about 0.7". With the return, the right pair falls into review.

- src: `src/modules/predecessor_inference.cpp:116-117,134-135,172-182`, `src/modules/similarity_search.cpp:286-289`
- doc: `docs/guides/03-predecessors-and-cold-start.md:47` (netting per week), `:250-251` ("Check `last_usage`")
- Fix: take first and last usage from weeks with a positive net quantity. Join only positive weeks
  in the correlation. Or document that returns must be filtered out of the view.

### F8 [medium] BOM + consumption similarity ranks the sibling above the true predecessor, and the docs do not say how to combine them

KP-70 and KP-75 tie on BOM (0.5714). Consumption cosine picks the sibling, because a predecessor
hands over: its profile is complementary, not similar. A planner who adds the two scores gets the
wrong answer:

```sql
SELECT j.material_id, round(j.similarity,4) AS bom_jaccard,
       round(array_cosine_similarity(q.transactional_embedding, c.transactional_embedding),3) AS consumption_cos,
       round(0.5*j.similarity + 0.5*array_cosine_similarity(q.transactional_embedding, c.transactional_embedding),3) AS combined
FROM find_similar_materials_jaccard('000000000000100500', 10) j JOIN te_b c USING (material_id)
JOIN te_b q ON q.material_id='000000000000100500' ORDER BY combined DESC;
-- 000000000000100700 (KP-75, sibling)      | 0.5714 | 0.648  | 0.61
-- 000000000000100600 (KP-70, predecessor)  | 0.5714 | 0.473  | 0.522
-- 000000000000101000 (KP-92)               | 0.5714 | -0.174 | 0.199
```

For cold start the combination does help. KP-75 and KP-92 tie for KP-90 at 0.6667. KP-92's profile
is unlike KP-75's (cos −0.1), and KP-75 has the dense history, so KP-75 is the analog. Both answers
came from SQL I wrote myself. None of the concepts pages (`predecessors-and-cold-start.md`,
`similarity-measures.md`, `erp-bom-models.md`, `embeddings-and-fusion.md`) explains how to put BOM
Jaccard and consumption cosine together. `fuse_embeddings` has no BOM slot: Jaccard has no place in
it, and structural vectors are not computed. So I could defend the `infer_predecessors` numbers
(formula and worked example), but not a combined score.

- doc: `docs/concepts/predecessors-and-cold-start.md:105-111`, `docs/concepts/embeddings-and-fusion.md` (Fusion)
- Fix: add a section "BOM and consumption together". Use the cosine to rank cold-start analogs.
  Never use it as evidence for a predecessor (use `infer_predecessors`' anti-correlation).
  Mention the different scales (Jaccard 0..1, cosine −1..1).

### F9 [low] Autocorrelation "lags 1, 7, 14, 28 days" are lags in movement rows

`extract_ts_features` passes the raw movement rows, `LIST(quantity ORDER BY movement_date)`, with
no daily or weekly bucketing. Lag 7 is seven postings back. For a pump with 1–3 deliveries a week,
that is 2–7 weeks. `length` is the number of postings (KP-50: 101 rows in 365 days), not of
periods. `infer_predecessors` buckets by week, so the two features see different series.

```sql
SELECT material_id, features.mean, features.length FROM extract_ts_features() ORDER BY 1 LIMIT 2;  -- shim
-- 000000000000100100 | 3.0769 | 13.0
-- 000000000000100200 | 12.9505 | 101.0
```

- src: `src/modules/embedding_statistics.cpp:26-35`; doc: `docs/concepts/embeddings-and-fusion.md:46-47`
- Fix: bucket per day or week (with zero fill) before the features, or reword the doc as "lags in postings".

### F10 [low] lookback_months clips predecessor_first_usage, and it does not "search older materials"

```sql
SELECT predecessor_id, confidence, predecessor_first_usage FROM infer_predecessors('000000000000100500');
-- 000000000000100600 | 0.6759 | 2024-03-25
SELECT predecessor_id, confidence, predecessor_first_usage FROM infer_predecessors('000000000000100500', lookback_months := 48);
-- 000000000000100600 | 0.6759 | 2023-11-06   (the real first movement)
SELECT predecessor_id, confidence, predecessor_first_usage FROM infer_predecessors('000000000000100500', lookback_months := 3);
-- 000000000000100600 | 0.6759 | 2025-12-16
```

The parameter only cuts the candidate rows before the window statistics. The column is documented
as the "usage window of the candidate".

- src: `src/modules/predecessor_inference.cpp:116-121`; doc: `docs/guides/03-predecessors-and-cold-start.md:114` and the output table
- Fix: compute first and last usage before the lookback cut, or document the clipping.

### F11 [low] A candidate still selling at the extract end scores a near-perfect temporal score

The temporal score cannot tell "ended" from "still running". If the successor started less than six
months before the extract ends, every active sibling's last row is the extract end. That scores
0.95–1.0. KP-65 (4 weeks), relaxed:

```sql
SELECT predecessor_id, confidence, correlation, similarity, temporal_score, predecessor_last_usage, overlapping_weeks
FROM infer_predecessors('000000000000100300', min_confidence := 0, min_similarity := 0.01, min_overlapping_weeks := 1, lag_weeks := 0);
-- 000000000000100200 | 0.7278 | -0.6582 | 0.6   | 0.9483 | 2026-11-12 | 4
-- 000000000000100400 | 0.6046 | -0.7079 | 0.125 | 0.9467 | 2026-11-13 | 4
```

KP-50 and KP-45 are both still selling. Both are found with confidence above 0.6, from four weeks
of data.

- src: `src/modules/predecessor_inference.cpp:172-182`
- Fix: count a candidate whose last movement lies within a few weeks of the extract end as not
  ended (temporal 0.3). Or return a `still_active` flag.

### F12 [low] Cold-start ties are broken by material number, and a sparse analog ties a dense one

```sql
SELECT material_id, round(similarity,4), shared_components, total_components, history_months, first_usage, last_usage
FROM cold_start_analogs('000000000000100800', 5);
-- 000000000000100700 | 0.6667 | 4 | 6 | 29 | 2024-06-06 | 2026-11-13   (269 movements, 128 weeks)
-- 000000000000101000 | 0.6667 | 4 | 6 | 14 | 2025-09-10 | 2026-11-04   (6 movements, 6 weeks)
-- 000000000000100500 | 0.375  | 3 | 8 |  8 | 2026-03-16 | 2026-11-13
-- 000000000000100600 | 0.25   | 2 | 8 | 32 | 2023-11-06 | 2026-07-02
```

KP-92 passes `min_history_months := 12` with six deliveries. The right analog is first only because
`…100700` sorts before `…101000`. The docs warn about the span and show the density query, so this
works as documented. But a planner who takes the top row has no density in the result.

- src: `src/modules/similarity_search.cpp:283-307` (order `similarity DESC, material_id`)
- Fix: add `movement_weeks` (distinct weeks with movements) and break ties by it.
