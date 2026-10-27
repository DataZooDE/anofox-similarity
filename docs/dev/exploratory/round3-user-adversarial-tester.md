# Round 3 exploratory testing: adversarial inputs

Tested the supplied DuckDB 1.5.6 linked shell and DuckDB 2.0 explicitly loaded shell on 2026-10-27. SQL was run from `/home/jr/Projects/datazoo/anofox-similarity`; the source and fixture paths inspected were from this crew worktree. The 79 documented function names were matched against `duckdb_functions()` on each shell (79/79 present). Both runs of `scripts/run_examples.sh` passed 17/17 examples, including the `-- Expected:` checks. The 1.5.6 API generation `--check` passed all 13 pages.

## Findings

### A1 — medium — NULL hash values produce false MinHash matches and identical LSH bands

Location: `src/modules/recall_accelerators.cpp:128-132` (`Signature`). It changes every NULL element to numeric zero before both consumers. A corrupted or partly missing stored signature can therefore compare as an exact match and be admitted to the same LSH bucket as a valid signature. Both shells gave the same result:

```sql
SELECT minhash_similarity([NULL::UBIGINT], [0::UBIGINT]) AS similarity,
       lsh_bands([NULL::UBIGINT], 1) = lsh_bands([0::UBIGINT], 1) AS same_band;
-- actual: 1.0 | true
SELECT 1; -- still works
```

Expected: reject a signature containing NULL with an error naming the element, or return NULL rather than claiming a match. Suggested fix: validate non-NULL elements in `Signature` before hashing/comparing; test NULL against zero and NULL against NULL in both functions.

### A2 — medium — a prepared `getvariable()` search uses a stale input on DuckDB 1.5.6

Location: `src/modules/similarity_search.cpp:56-58` and `src/modules/coverage_search.cpp:217-219` (the search input is turned into a SQL literal during binding). A session can change a query variable between executions and silently search for the former value on 1.5.6. DuckDB 2.0 refreshes the value. Copy-pasteable repro, in a fresh shell:

```sql
CREATE TABLE feature_multiset(entity_id VARCHAR, feature_id VARCHAR, weight DOUBLE);
INSERT INTO feature_multiset VALUES ('A','x',1), ('B','y',1);
SET VARIABLE q = MAP {'x': 1};
PREPARE p AS SELECT entity_id FROM find_covering_entities_for(getvariable('q'), 1);
EXECUTE p;                    -- A on both shells
SET VARIABLE q = MAP {'y': 1};
EXECUTE p;                    -- A on 1.5.6; B on 2.0
SELECT entity_id FROM find_covering_entities_for(getvariable('q'), 1); -- B on both
```

The neighbouring `find_similar_materials_jaccard` pattern also stayed on the old material on 1.5.6. Expected: the second execution searches for `y` and returns B. Suggested fix: arrange rebind on changed variables in 1.5.6, or document the version limitation and use `PREPARE ... $1` for changing search inputs. The latter workaround was added to `docs/api/00-conventions.md` and verified on both shells: `EXECUTE search(MAP {'x': 1})` returned A and `EXECUTE search(MAP {'y': 1})` returned B.

### A3 — low — `cosine_floor` can return a value rejected by its documented consumer

Location: `docs/api/14-classification-and-review.md:18` (the two functions are presented as a pipeline). With two identical vectors, the estimated floor is 1.0; `calibrated_cosine` requires a floor below 1.0. With zero or one row, the estimate is NULL. Both shells reproduced the two-row case:

```sql
CREATE TABLE v(vec DOUBLE[]);
INSERT INTO v VALUES ([1.0,0.0]), ([1.0,0.0]);
SELECT floor, n_pairs FROM cosine_floor('v','vec'); -- 1.0 | 1
SELECT calibrated_cosine([1.0,0.0], [1.0,0.0],
       (SELECT floor FROM cosine_floor('v','vec')));
-- actual: Invalid Input Error: calibrated_cosine: floor must be in [0, 1)
```

Expected: the docs tell users how to handle an all-identical or too-small sample. Suggested fix: cap a 1.0 estimate below 1.0 and supply a model-specific floor when no pairs exist. Added that guidance to the page; `LEAST(floor, 0.999)` produced 0.999 and a valid calibrated score of 1.0 on both shells.

## Regression adjacency and other probes

- UR-2 G1/G2/G12 held for `mind. 1 000 N`, `ab 1 000 N` (narrow no-break space), `≤ 1 000 N`, `größer als 5 A`, `bis 5 A`, `1 000 N` (no-break space), `1 000 000 N`, `10 U/min`, `2 Zoll`, `3 M`, and `5 G`. Both shells parsed the bounds, grouping, and converted units as intended.
- G5 held with a repeated unit-weight `(entity_id, feature_id)` row: `minhash_index(feature_table := 'f', weight_col := NULL, num_hashes := 8)` gave total weight 1 and the same signature as a single row on both shells. Deliberately weighted duplicate rows still summed to 2, as documented.
- G6 held for a view backed by the mini tree fixture: passing `nodes_table := 'vnodes'` without `attrs_table` raised the explicit missing-attributes error when the default attributes table existed. G7 rejected `child` as a likely misspelling of `children` and count -1. G8 rejected a view's VARCHAR `movement_date` with the `strptime` conversion hint. The shell remained usable after errors.
- G3/G10/G11: all 79 documented names were present in both catalogues; the reference generator's `--check` passed 13 pages on 1.5.6. G4/G9: all 17 executable examples passed both shells; this did not rerun every moved tutorial or benchmark the guide's performance claim. This round found no contrary evidence for G3–G12. The deferred UR-2 D-1..D-5 items were excluded from new findings.
- Zero and negative `k` still differ between search families (`find_similar_materials_jaccard`/WL return no rows for zero and expose DuckDB's LIMIT error for negative; covering searches reject both). This is UR-1's already deferred uniform-`k` validation item, so it is not relisted as a finding. NULL search IDs raised named binder errors; integer IDs required an explicit cast; repeated calls in one query returned stable ties. Vector functions rejected mismatched dimensions and non-finite elements without an INTERNAL error. Empty and one-row `cosine_floor` returned `(NULL, 0)`.
- Quoted names with spaces and Unicode still raise `Invalid identifier` in table-name parameters, the previously deferred UR-1 quoted-identifier item. Simple schema-qualified and attached-database table names worked. No crash or hang was observed in these probes.

## Function inventory

Names below were read from the generated API pages and checked against `duckdb_functions()` on both shells (79 distinct names, 79 present on each). This is a catalogue check, not a claim that every signature was fuzzed.

`aggregate_material_components`, `apply_vocabulary_review`, `bhattacharyya`, `bom_common_components`, `bom_dfs_neighborhood`, `bom_explosion_1level`, `bom_explosion_multilevel`, `bom_to_items`, `bom_where_used`, `bootstrap_ci`, `calibrated_cosine`, `check_anofox_forecast_available`, `check_duckpgq_available`, `check_statistics_freshness`, `classify_to_catalogue`, `cluster_matches`, `cold_start_analogs`, `compat_score`, `compute_domain_specific_statistics`, `compute_fused_embeddings`, `compute_jaccard_embeddings`, `compute_textual_embeddings`, `compute_transactional_embeddings`, `connected_components`, `containment`, `cosine_floor`, `coverage_kernel`, `coverage_recall`, `create_bom_property_graph`, `dynamics365_to_bom_component`, `dynamics365_to_bom_header`, `dynamics365_to_materials`, `embed_text`, `embedding_backend`, `eval_calibration`, `eval_ranking`, `extract_material_descriptions`, `extract_ts_features`, `filter_recent_movements`, `find_covering_entities`, `find_covering_entities_for`, `find_covering_entities_lsh`, `find_similar_materials_jaccard`, `find_similar_materials_wl_kernel`, `fit_coverage_weights`, `fuse_embeddings`, `infer_predecessors`, `items_to_bom`, `jaccard_similarity`, `lsh_bands`, `maxsim`, `minhash_bands`, `minhash_index`, `minhash_similarity`, `normalize_quantity`, `parse_quantity`, `quantity_units`, `recompute_embedding_statistics`, `sap_to_bom_items`, `sap_to_materials`, `sap_to_materials_with_desc`, `threshold_at_precision`, `tree_anchors`, `tree_class_multiset`, `tree_coverage`, `tree_coverage_trace`, `tree_from_json`, `tree_multiset`, `tversky_similarity`, `uncertain_decisions`, `validate_trees`, `value_satisfies`, `vocabulary_review_queue`, `weighted_containment`, `weighted_jaccard`, `weighted_minhash`, `weighted_tversky`, `wl_fingerprint`, `wl_kernel_similarity`.

## Verdict

The common examples and edge-input validation let this user continue working. The most serious remaining robustness risk is silent false similarity from NULL elements in a stored MinHash signature. On 1.5.6, changing a variable used by a prepared search can also return stale results without warning; a bound `$1` parameter avoids it.
