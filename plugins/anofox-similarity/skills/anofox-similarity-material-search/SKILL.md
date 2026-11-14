---
name: anofox-similarity-material-search
description: >
  Set similarity scalars (`jaccard_similarity`, `tversky_similarity`, `containment`, `weighted_jaccard`,
  `weighted_containment`, `weighted_tversky`, `wl_kernel_similarity`), BOM-based material search
  (`find_similar_materials_jaccard`, `find_similar_materials_wl_kernel`), predecessor inference
  (`infer_predecessors`), cold-start analogs (`cold_start_analogs`) and BOM traversal (`bom_explosion_multilevel`,
  `bom_where_used`, `bom_dfs_neighborhood`) in the anofox_similarity DuckDB extension. Use when asked to
  "find materials with a similar BOM", "which part did this new one replace", "find analogs to forecast a
  new product", "explode a BOM", "where is this component used", or to compare two component lists.
version: 2026.11.13
user-invocable: false
---

# Anofox Similarity — Material Search, Predecessors & BOM Cheat Sheet

**Extension:** `anofox_similarity` v2026.11.13 | **DuckDB:** v2.0 / v1.5.6 | **Reference:** [`docs/api/01-similarity-scalars.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/01-similarity-scalars.md), [`docs/api/02-material-search.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/02-material-search.md), [`docs/api/03-bom-traversal.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/03-bom-traversal.md)

> Paths like `test/data/...` are relative to a clone of the anofox-similarity repository; in
> your own project, substitute your tables. Examples that build inline tables run anywhere.

Compare materials by their bills of materials: score two component sets, rank the materials most
similar to one material (direct-children Jaccard or the multi-level WL kernel), detect which old
material a new one replaced (BOM similarity + anti-correlated weekly consumption), find analogs with
history for a new product, and walk BOMs up and down. Guides: `docs/guides/02-material-similarity.md`,
`docs/guides/03-predecessors-and-cold-start.md`; runnable scripts in `examples/material_similarity/`
and `examples/predecessors_cold_start/`.

## Critical gotchas

- **Two BOM shapes.** The search, WL, predecessor and cold-start functions and `bom_dfs_neighborhood`
  read a flat edge list `bom_table` (default `'bom_items'`) with columns `parent_id`, `child_id`
  (a `quantity` column is ignored). The traversal macros `bom_explosion_1level`,
  `bom_explosion_multilevel`, `bom_where_used`, `bom_common_components` and `create_bom_property_graph`
  read the universal pair `bom_header` (columns `bom_id`, `parent_material_id`, optional `source_system`) +
  `bom_component` (`bom_id`, `child_material_id`, `quantity_per`). Convert with `bom_to_items` /
  `items_to_bom_header` + `items_to_bom_component` (see the **anofox-similarity-erp-embeddings** skill).
- **`material_id` and `k` are positional-only** in the table functions. `k := 5` gives
  `Binder Error: No function matches ... cold_start_analogs(VARCHAR)`; `material_id := 'X', 5` gives
  `Unnamed parameters cannot come after named parameters`.
  Only the trailing tuning arguments (`min_similarity`, `bom_table`, ...) take `name := value`.
- **Scalar functions/macros bind named args by position on DuckDB 1.5.6** (by name on 2.0). Always
  write them in signature order: `tversky_similarity(q, c, beta := 0.0, alpha := 1.0)` silently
  swaps alpha and beta on 1.5.6. Table functions and table macros bind by name on both.
- **`goods_movements` contract:** `material_id`, `movement_date` (DATE or TIMESTAMP), `quantity`
  (numeric). A VARCHAR date fails with `column movement_date is VARCHAR, expected DATE or TIMESTAMP`
  — convert in a view (`strptime(budat, '%Y%m%d')::DATE AS movement_date`). Every row counts,
  receipts included: pass a view with consumption (issues) only.
- **`infer_predecessors` needs dense weekly history.** Quantities are summed per ISO week, the
  candidate series is shifted by `lag_weeks` (default 8), and at least `min_overlapping_weeks`
  (default 8) aligned non-empty weeks must exist. A handful of scattered movements, or a clean
  cut-over with no ramp, returns **nothing** — an empty result is not "no predecessor".
- **Caps:** WL `iterations` is exact for 1–5 and capped at 5 (`iterations := 50` = 5).
  `max_depth` of `bom_explosion_multilevel` (default 10) and `bom_dfs_neighborhood` (default 3) is
  clamped to 64 — deeper BOMs are silently truncated, no error.
- **Searches fill `k` rows with zero-similarity materials** (the query itself is excluded). Filter
  with `min_similarity`. Unknown material id → empty result, no error. Candidates are every
  `parent_id` in the BOM table, subassemblies included.
- **Read-only databases work.** All functions in this skill, macros included, are available on a
  read-only database; table functions just cannot write — `CREATE TABLE ... AS SELECT` elsewhere.

## Sample data used below

```sql
-- PUMP-C reaches the same parts as PUMP-A, but through the GEARBOX-G subassembly.
CREATE TABLE bom_items AS SELECT * FROM (VALUES
  ('PUMP-A', 'SEAL-001', 2.0), ('PUMP-A', 'BEARING-X', 1.0), ('PUMP-A', 'MOTOR-M1', 1.0), ('PUMP-A', 'HOUSING-1', 1.0),
  ('PUMP-B', 'SEAL-001', 2.0), ('PUMP-B', 'BEARING-X', 1.0), ('PUMP-B', 'MOTOR-M1', 1.0), ('PUMP-B', 'HOUSING-2', 1.0),
  ('PUMP-C', 'SEAL-001', 1.0), ('PUMP-C', 'GEARBOX-G', 1.0),
  ('GEARBOX-G', 'BEARING-X', 2.0), ('GEARBOX-G', 'MOTOR-M1', 1.0),
  ('MOTOR-M1', 'COIL-7', 1.0), ('MOTOR-M1', 'SHAFT-3', 1.0),
  ('OLD-PUMP', 'SEAL-002', 1.0), ('OLD-PUMP', 'BEARING-Y', 1.0), ('OLD-PUMP', 'IMPELLER-1', 1.0),
  ('NEW-PUMP', 'SEAL-002', 1.0), ('NEW-PUMP', 'BEARING-Y', 1.0), ('NEW-PUMP', 'IMPELLER-2', 1.0),
  ('NEXT-PUMP', 'SEAL-002', 1.0), ('NEXT-PUMP', 'BEARING-Y', 1.0), ('NEXT-PUMP', 'IMPELLER-2', 1.0),
  ('NEXT-PUMP', 'VALVE-9', 1.0)
) t(parent_id, child_id, quantity);
```

```sql
-- Weekly consumption: OLD-PUMP falls for 40 weeks; NEW-PUMP starts in week 10 and rises.
CREATE TABLE goods_movements AS
SELECT 'OLD-PUMP' AS material_id, DATE '2023-01-02' + 7 * w::INTEGER AS movement_date, 200.0 - 5 * w AS quantity
FROM range(0, 40) t(w)
UNION ALL
SELECT 'NEW-PUMP', DATE '2023-01-02' + 7 * w::INTEGER, 5.0 * w - 50
FROM range(10, 52) t(w);
```

```sql
-- Universal header + component shape for the traversal macros, derived from bom_items.
CREATE TABLE bom_header AS
SELECT DISTINCT parent_id AS bom_id, 'SAP' AS source_system, parent_id AS parent_material_id FROM bom_items;
CREATE TABLE bom_component AS
SELECT parent_id AS bom_id, child_id AS child_material_id, quantity AS quantity_per FROM bom_items;
```

## Set measures (scalars)

```text
jaccard_similarity(list_a ANY[], list_b ANY[])                              → DOUBLE  |a∩b| / |a∪b|
tversky_similarity(query ANY[], candidate ANY[] [, alpha DOUBLE, beta DOUBLE]) → DOUBLE
    |q∩c| / (|q∩c| + alpha·|q\c| + beta·|c\q|); default alpha = beta = 1 (= Jaccard)
containment(query ANY[], candidate ANY[])                                   → DOUBLE  |q∩c| / |q|
weighted_jaccard(query MAP, candidate MAP)                                  → DOUBLE  Σmin / Σmax
weighted_containment(query MAP, candidate MAP)                              → DOUBLE  Σmin / Σq
weighted_tversky(query MAP, candidate MAP [, alpha DOUBLE, beta DOUBLE])    → DOUBLE
    Σmin / (Σmin + alpha·Σ(q−c)⁺ + beta·Σ(c−q)⁺); default alpha = beta = 1
```

| Case | Result |
|---|---|
| NULL argument | NULL |
| Both lists / maps empty (Jaccard, Tversky, weighted Jaccard/Tversky) | 0.0 |
| Empty query in `containment` / `weighted_containment` (or all-zero weights) | NULL |
| Duplicates / NULL elements in lists | collapse / ignored (lists are sets) |
| Map weights | numeric, finite, ≥ 0; NULL weight = 1; 0 = absent; negative → `Invalid Input Error: weights must be non-negative` |
| Mixed element types | `jaccard_similarity` compares typed values (`[1,2]` vs `['1','2']` = 0.0); `tversky_similarity`/`containment` and map keys compare text form (= 1.0). INTEGER vs DOUBLE never match (`'1'` vs `'1.0'`): cast both sides, e.g. `::VARCHAR[]` |

Use `containment` / Tversky with `beta` < 1 when a richer candidate should not be penalised
("does B contain my parts?"); use the `weighted_*` forms when counts or quantities matter (4
requested units are half covered by 2). alpha = 1, beta = 0 is containment.

```sql
SELECT jaccard_similarity(['A','B','C'], ['B','C','D'])                         AS jaccard,       -- 0.5
       containment(['a','b'], ['a','b','c','d'])                                AS contained,     -- 1.0
       tversky_similarity(['a','b'], ['a','b','c','d'], 1.0, 0.25)              AS tversky,       -- 0.8
       weighted_jaccard(MAP {'heating': 2, 'massage': 4}, MAP {'heating': 4, 'ventilation': 2}) AS wj, -- 0.2
       weighted_containment(MAP {'massage': 4}, MAP {'massage': 2})             AS wc,            -- 0.5
       weighted_tversky(MAP {'a': 3}, MAP {'a': 2, 'b': 3}, 1.0, 0.0)           AS wt;            -- 0.6667
```

**Many pairs at once** — build component lists with `list()` (or the helper
`aggregate_material_components(bom_table[, material_filter])`, which returns
`material_id, components VARCHAR[]`) and join:

```sql
WITH sets AS (SELECT parent_id, list(child_id) AS parts FROM bom_items GROUP BY parent_id)
SELECT a.parent_id AS a, b.parent_id AS b, round(jaccard_similarity(a.parts, b.parts), 4) AS jaccard
FROM sets a JOIN sets b ON a.parent_id < b.parent_id
WHERE a.parent_id LIKE 'PUMP-%' AND b.parent_id LIKE 'PUMP-%'
ORDER BY jaccard DESC, a, b;
-- PUMP-A | PUMP-B | 0.6 ;  PUMP-A | PUMP-C | 0.2 ;  PUMP-B | PUMP-C | 0.2
-- => PUMP-A | PUMP-B | 0.6
-- => PUMP-A | PUMP-C | 0.2
-- => PUMP-B | PUMP-C | 0.2
```

## WL kernel: `wl_kernel_similarity`, `wl_fingerprint`

```text
wl_kernel_similarity(material_a, material_b, iterations := 3, bom_table := 'bom_items') → DOUBLE   -- scalar macro
wl_fingerprint(root, iters, bom_table) → TABLE(component VARCHAR, occurrence BIGINT)               -- table macro
```

- The fingerprint of a material is every component reachable at BOM depth 0 … `iterations`−1
  (capped at 5 levels), weighted by the number of distinct depths it occurs at. The kernel is the
  weighted Jaccard of two fingerprints: Σmin / (Σa + Σb − Σmin). Same material → 1.0; both empty → 0.0.
- `iterations := 1` = Jaccard of the direct children. Higher values find parts that moved into or
  out of subassemblies.
- Works per row inside joins with column arguments (no correlated-subquery limit).
- Pass the arguments in order (`iterations` before `bom_table`); naming only `bom_table := ...` is fine.

```sql
SELECT round(jaccard_similarity(
         (SELECT list(child_id) FROM bom_items WHERE parent_id = 'PUMP-A'),
         (SELECT list(child_id) FROM bom_items WHERE parent_id = 'PUMP-C')), 4)       AS jaccard, -- 0.2
       round(wl_kernel_similarity('PUMP-A', 'PUMP-C'), 4)                             AS wl,      -- 0.7143
       round(wl_kernel_similarity('PUMP-A', 'PUMP-C', 1, 'bom_items'), 4)             AS wl_1;    -- 0.2
```

```sql
SELECT * FROM wl_fingerprint('PUMP-C', 3, 'bom_items') ORDER BY component;
-- BEARING-X, COIL-7, GEARBOX-G, MOTOR-M1, SEAL-001, SHAFT-3; occurrence 1 each
```

## `find_similar_materials_jaccard`

```text
find_similar_materials_jaccard(material_id VARCHAR, k BIGINT,
    bom_table := 'bom_items', min_similarity := 0.0, mode := 'jaccard')
  → TABLE(material_id VARCHAR, similarity DOUBLE, shared_components BIGINT, total_components BIGINT)
```

| Arg | Default | Notes |
|---|---|---|
| `material_id`, `k` | required, positional | query excluded from output; ties ordered by material id |
| `bom_table` | `'bom_items'` | needs `parent_id`, `child_id` (else a Binder Error naming the missing column) |
| `min_similarity` | 0.0 | drop weak matches (the search otherwise fills `k` rows with 0.0) |
| `mode` | `'jaccard'` | `'containment'` = share of the query's components the candidate has; ties by Jaccard. Anything else → Binder Error |

`total_components` = size of the union. Exact brute force over all BOMs; counts components, not
quantities (for quantities use `find_covering_entities`, skill **anofox-similarity-coverage**).

```sql
SELECT * FROM find_similar_materials_jaccard('PUMP-A', 3, bom_table := 'bom_items');
-- PUMP-B 0.6 3 5 | GEARBOX-G 0.5 2 4 | PUMP-C 0.2 1 5
SELECT material_id, similarity, shared_components
FROM find_similar_materials_jaccard('PUMP-A', 5, mode := 'containment', min_similarity := 0.01, bom_table := 'bom_items');
-- PUMP-B 0.75 3 | GEARBOX-G 0.5 2 | PUMP-C 0.25 1
```

## `find_similar_materials_wl_kernel`

```text
find_similar_materials_wl_kernel(material_id VARCHAR, k BIGINT,
    bom_table := 'bom_items', min_similarity := 0.0, iterations := 3)
  → TABLE(material_id VARCHAR, similarity DOUBLE)
```

Same scores as `wl_kernel_similarity` against every parent in the table, set-based (safe on
cycles and dense BOMs). Use it when BOMs are deep or subassemblies move between levels.

```sql
SELECT material_id, round(similarity, 4) AS similarity
FROM find_similar_materials_wl_kernel('PUMP-A', 3, bom_table := 'bom_items');
-- PUMP-B 0.7143 | PUMP-C 0.7143 | GEARBOX-G 0.6667   (Jaccard ranked PUMP-C last at 0.2)
-- => PUMP-B | 0.7143
-- => PUMP-C | 0.7143
-- => GEARBOX-G | 0.6667
```

| Situation | Use |
|---|---|
| Flat, one-level BOMs; explainable score | `find_similar_materials_jaccard` |
| Deep BOMs, subassemblies restructured | `find_similar_materials_wl_kernel` |
| "Does B contain my parts?" | `mode := 'containment'` |
| Quantities matter | `find_covering_entities` (coverage skill) |
| Many pairs / custom candidate sets | scalars in a join |
| Millions of materials, approximate | MinHash/LSH (skill **anofox-similarity-evaluation-scale**) |

## `infer_predecessors`

```text
infer_predecessors(material_id VARCHAR,            -- positional: the successor
    lookback_months := 24, min_similarity := 0.3, min_confidence := 0.5,
    lag_weeks := 8, min_overlapping_weeks := 8,
    bom_table := 'bom_items', movements_table := 'goods_movements')
  → TABLE(predecessor_id VARCHAR, confidence DOUBLE, correlation DOUBLE, similarity DOUBLE,
          temporal_score DOUBLE, predecessor_first_usage DATE, predecessor_last_usage DATE,
          successor_start DATE, overlapping_weeks BIGINT)          -- ordered by confidence DESC
```

How it scores (from the build):

1. Candidates: the 100 materials most Jaccard-similar to the successor with similarity ≥
   `min_similarity`. With many near-identical variants a predecessor can fall outside them.
2. Weekly sums of both series; candidate weeks joined to successor weeks shifted by `lag_weeks`;
   candidate history limited to `lookback_months` before the successor's first movement. Pairs with
   fewer than `min_overlapping_weeks` aligned weeks are dropped.
3. `temporal_score`: high when the candidate's last usage lies within 3 months before to 6 months
   after `successor_start`; 0.3 if it keeps running longer; 0 (dropped) if it stopped earlier.
4. `confidence = 0.4·max(0, −correlation) + 0.3·similarity + 0.3·temporal_score`. Kept only if
   correlation < 0, temporal_score > 0, the candidate was in use **before** the successor started,
   and confidence ≥ `min_confidence`. Scores are rounded to 4 decimals.

```sql
SELECT predecessor_id, confidence, correlation, similarity, temporal_score, overlapping_weeks
FROM infer_predecessors('NEW-PUMP', bom_table := 'bom_items', movements_table := 'goods_movements');
-- OLD-PUMP | 0.64 | -1.0 | 0.5 | 0.3 | 38
SELECT count(*) AS n FROM infer_predecessors('OLD-PUMP', movements_table := 'goods_movements'); -- 0: direction matters
```

Tuning: lower `min_confidence` to see weak candidates for review; lower `min_overlapping_weeks`
only with care (unstable correlation); shorten `lag_weeks` for fast changeovers.

**Empty result? Check density first:**

```sql
SELECT material_id, count(*) AS movements,
       count(DISTINCT date_trunc('week', movement_date)) AS weeks,
       min(movement_date) AS first, max(movement_date) AS last
FROM goods_movements GROUP BY material_id ORDER BY material_id;
```

**Clean cut-over** (the old product stops when the new one starts, no overlapping ramp, so no
anti-correlation for `infer_predecessors` to find): rank by BOM similarity and require the
candidate's last movement to fall in a window that **ends** at the successor's launch. Here
NEXT-PUMP launches on 2023-10-09 and has no movements yet; NEW-PUMP, still selling, is excluded:

```sql
SET VARIABLE launch = DATE '2023-10-09';
SELECT s.material_id AS predecessor_id, s.similarity, h.last_usage
FROM find_similar_materials_jaccard('NEXT-PUMP', 50, min_similarity := 0.3, bom_table := 'bom_items') s
JOIN (SELECT material_id, max(movement_date) AS last_usage FROM goods_movements GROUP BY material_id) h
  USING (material_id)
WHERE h.last_usage BETWEEN getvariable('launch') - INTERVAL 12 WEEK AND getvariable('launch')
ORDER BY s.similarity DESC;
-- OLD-PUMP | 0.4 | 2023-10-02   (set the window to your changeover practice)
-- => OLD-PUMP | 0.4 | 2023-10-02
```

## `cold_start_analogs`

```text
cold_start_analogs(material_id VARCHAR, k BIGINT,  -- both positional
    min_history_months := 0, min_similarity := 0.0,
    bom_table := 'bom_items', movements_table := 'goods_movements')
  → TABLE(material_id VARCHAR, similarity DOUBLE, shared_components BIGINT, total_components BIGINT,
          history_months BIGINT, first_usage DATE, last_usage DATE)
```

- Jaccard-similar materials that have **at least one movement row**; the new material needs a BOM
  in `bom_table`, not history.
- `history_months` = `date_diff('month', first_usage, last_usage)` — the calendar span, not the
  number of months with movements. One movement → 0 and still passes the default 0. Check density
  (query above) before forecasting from an analog; check `last_usage` for stale analogs.
- No analog with enough history → empty result.

```sql
SELECT material_id, similarity, shared_components, history_months, first_usage, last_usage
FROM cold_start_analogs('NEXT-PUMP', 3, min_history_months := 6, min_similarity := 0.3,
                        bom_table := 'bom_items', movements_table := 'goods_movements');
-- NEW-PUMP | 0.75 | 3 | 9 | 2023-03-13 | 2023-12-25
-- OLD-PUMP | 0.4  | 2 | 9 | 2023-01-02 | 2023-10-02
-- => NEW-PUMP | 0.75 | 3 | 9 | 2023-03-13 | 2023-12-25
-- => OLD-PUMP | 0.4 | 2 | 9 | 2023-01-02 | 2023-10-02
```

Use the result as a surrogate: weight each analog's demand by `similarity`, then forecast (see the
anofox-forecast extension).

## BOM traversal macros (header + component shape)

```text
bom_explosion_1level(query_material_id, header_table := 'bom_header', component_table := 'bom_component')
bom_explosion_multilevel(query_material_id, max_depth := 10, header_table := 'bom_header', component_table := 'bom_component')
bom_where_used(child_material_id, header_table := 'bom_header', component_table := 'bom_component')
    → TABLE(parent_material_id, child_material_id, quantity_per)
bom_common_components(material_1, material_2, header_table := 'bom_header', component_table := 'bom_component')
    → TABLE(child_material_id_1, child_material_id_2)          -- one row per shared direct child
create_bom_property_graph(source_system := 'TEST')
    → TABLE(graph_name, source, num_nodes, num_edges)
```

- All are table macros: pass optional args by name. `quantity_per` keeps the column type of your
  `bom_component` (DECIMAL(18,6) from `items_to_bom_component` or the D365 macros).
- `bom_explosion_multilevel` returns the DISTINCT edges `(parent, child, quantity_per)` within
  `max_depth` levels (≤ 64) — no depth column, **no quantity roll-up**; a subassembly used by
  several parents appears once. Cycles and diamonds are safe (set-based recursion).
- `bom_where_used` is one level up (direct parents only). `bom_common_components` compares direct
  children; its row count equals `shared_components` of the Jaccard search.
- `create_bom_property_graph` only counts: distinct parents and component rows in the hard-wired
  tables `bom_header` / `bom_component` whose `source_system` matches (the default `'TEST'` usually
  gives 0 / 0). It creates nothing and does not need DuckPGQ.
- Missing tables → `Catalog Error: Table with name bom_header does not exist!`.

```sql
SELECT * FROM bom_explosion_1level('PUMP-A', header_table := 'bom_header', component_table := 'bom_component')
ORDER BY child_material_id;                                      -- 4 direct children
SELECT * FROM bom_explosion_multilevel('PUMP-C', max_depth := 5)
ORDER BY parent_material_id, child_material_id;                  -- 6 edges through GEARBOX-G and MOTOR-M1
SELECT parent_material_id FROM bom_where_used('MOTOR-M1') ORDER BY 1;   -- GEARBOX-G, PUMP-A, PUMP-B
SELECT count(*) AS shared FROM bom_common_components('PUMP-A', 'PUMP-B');  -- 3
SELECT * FROM create_bom_property_graph(source_system := 'SAP');  -- BOM_PROPERTY_GRAPH_SAP | SAP | 8 | 24
```

## `bom_dfs_neighborhood` (flat edge list)

```text
bom_dfs_neighborhood(root_material_id, max_depth := 3, bom_table := 'bom_items') → TABLE(component VARCHAR)
```

Distinct components reachable from the root in 1 … `max_depth` hops (clamped to 64) over
`parent_id`/`child_id`. Use it to flatten a BOM before a Jaccard comparison of all levels.

```sql
SELECT component FROM bom_dfs_neighborhood('PUMP-C', max_depth := 1, bom_table := 'bom_items') ORDER BY 1;
-- GEARBOX-G, SEAL-001
SELECT count(*) AS n FROM bom_dfs_neighborhood('PUMP-C', max_depth := 5, bom_table := 'bom_items');  -- 6
```

## Canonical material-search pipeline

```sql
-- 1. Map ERP tables onto the contracts (views keep it read-only-friendly).
CREATE OR REPLACE VIEW consumption AS
SELECT material_id, movement_date::DATE AS movement_date, quantity
FROM goods_movements WHERE quantity > 0;

-- 2. Shortlist structurally similar materials (Jaccard), re-score with the WL kernel.
CREATE OR REPLACE TABLE shortlist AS
SELECT s.material_id, s.similarity AS jaccard,
       wl_kernel_similarity('NEW-PUMP', s.material_id, 3, 'bom_items') AS wl
FROM find_similar_materials_jaccard('NEW-PUMP', 20, min_similarity := 0.2, bom_table := 'bom_items') s;

-- 3. Predecessor (needs dense weekly overlap) and analogs with history.
SELECT predecessor_id, confidence, overlapping_weeks
FROM infer_predecessors('NEW-PUMP', min_confidence := 0.5, movements_table := 'consumption');

SELECT a.material_id, a.similarity, a.history_months, sl.wl
FROM cold_start_analogs('NEW-PUMP', 5, min_history_months := 6, movements_table := 'consumption') a
LEFT JOIN shortlist sl USING (material_id)
ORDER BY a.similarity DESC;
```

Measuring search quality against known pairs (`eval_*`, thresholds, bootstrap CIs) is in
**anofox-similarity-evaluation-scale**; SAP / Dynamics 365 extraction into `bom_items` and the
`bom_to_items` / `items_to_materials` / `items_to_bom_header` / `items_to_bom_component` converters
are in **anofox-similarity-erp-embeddings**.
