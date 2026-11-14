---
name: anofox-similarity-classification-review
description: >
  Pivot classifier and human-in-the-loop review for the anofox_similarity DuckDB extension:
  `classify_to_catalogue` (items onto catalogue classes as p(class), rank, margin), the scalars
  `maxsim`, `bhattacharyya`, `calibrated_cosine`, the `cosine_floor` estimator, and the review
  cycle `vocabulary_review_queue` → `apply_vocabulary_review`, `uncertain_decisions`.
  Use when the user asks "classify these items/requirements onto our catalogue", "which wordings
  should a reviewer look at", "export low-confidence items to an LLM", "where do label_table /
  overrides come from", or "calibrate cosine similarity of a small sentence encoder".
version: 2026.11.13
user-invocable: false
---

# Anofox Similarity — Classification & Review Cheat Sheet

**Extension:** `anofox_similarity` v2026.11.13 | **DuckDB:** v2.0 / v1.5.6 | **Reference:** [`docs/api/14-classification-and-review.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/14-classification-and-review.md) (guide: [`docs/guides/06-classification-and-review.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/guides/06-classification-and-review.md))

> Paths like `test/data/...` are relative to a clone of the anofox-similarity repository; in
> your own project, substitute your tables. Examples that build inline tables run anywhere.

Coverage matching (`tree_coverage`, see **anofox-similarity-coverage**) compares items by
class. Specifications and archives rarely carry classes, so this toolkit gives them classes: the
pivot classifier turns item vectors into a distribution over catalogue classes, the margin
(p1 − p2) says how sure it is, and the review functions turn that uncertainty into a short work
list for a human or an LLM. The extension ships **no language model** — embed texts with any
model you like and pass `FLOAT[n]` vectors (the type DuckDB `vss` indexes).

## Critical gotchas

- **Bring your own vectors.** Items and catalogue must come from the same model and have the
  same dimension (checked: error mentions `dimension`). `FLOAT[n]` arrays are preferred (what a vss
  HNSW index holds; token sets `FLOAT[n][]`); lists (`FLOAT[]`, `DOUBLE[]`) work too.
  Items with a NULL `vec` are skipped.
- **`classify_to_catalogue`, `vocabulary_review_queue` and `uncertain_decisions` take named
  arguments only.** `classify_to_catalogue('items', 'catalogue')` fails with "No function matches";
  write `items_table := 'items', catalogue_table := 'catalogue'`. `cosine_floor('tbl', 'col')` and
  `apply_vocabulary_review('review')` take their table positionally (only `sample`, `seed` and
  `overrides_table` are named).
- **Arguments are table names (strings), not queries.** Materialise inputs as tables first.
  Table function arguments must be constants: compute a floor into a variable
  (`SET VARIABLE f = (SELECT floor FROM cosine_floor(...))`, then `getvariable('f')`).
- **Table functions cannot write.** `apply_vocabulary_review` only *returns* the merged overrides;
  keep them with `CREATE OR REPLACE TABLE vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review')`.
- **Centring needs a batch.** `center_by_source` (default true) subtracts the source mean only
  when the source has ≥ `min_source_items` (default 10) items, else the batch mean; a batch smaller
  than that is effectively not centred. Classifying one item alone gives different `p`/`margin` than
  classifying it with its source — re-classify the whole source so stored decisions stay comparable.
- **There is no "abstain" class.** Abstention is your policy on `margin`: treat a low margin as
  "ask someone" and route it through `uncertain_decisions` (default `threshold` 0.2).
- **Ties go to the lower class id** (e.g. `motor` before `pump` at p = 0.5/0.5, margin 0).
- **Overrides and answers must name catalogue classes** ("is not in catalogue_table" otherwise).
  Overrides match the item's `label` exactly (needs a `label` column in `items_table`); a NULL
  `source` in the overrides matches any source. Answers beat overrides beat the classifier.
- **`uncertain_decisions`: `threshold` or `share`, not both** (error "give either threshold or
  share, not both"). If the decisions carry `tree_id`s, `texts_table` needs a `tree_id` column.
- **`calibrated_cosine` floor must be in [0, 1).** `cosine_floor` returns NULL with fewer than two
  vectors and can return 1.0 for an all-identical corpus — clamp with `GREATEST(0.0, LEAST(floor, 0.999))` (a corpus of opposed vectors gives a negative
  floor, which `calibrated_cosine` rejects; fewer than two vectors give NULL, so `COALESCE` a default) or pick
  a model-specific floor instead of the NULL.
- **Classifier labels never make a tree *reviewed*.** Only `class_id` or the reviewed vocabulary
  (`overrides_table`) do, so `tree_coverage(assignment := 'auto')` falls back to `best_match`.

## Bring your own vectors — the input tables

| Table | Columns | Used by |
|---|---|---|
| items | `node_id, vec[, tree_id, source, label, tokens]` | `classify_to_catalogue(items_table := ...)` |
| catalogue | `class_id, vec[, exemplar_tokens]` | `catalogue_table` |
| overrides (reviewed vocabulary) | `wording, source, class_id[, reviewed_by, reviewed_at]` | `overrides_table` |
| answers (per item) | `[tree_id,] node_id, class_id` | `answers_table` |
| item attributes | `[tree_id,] node_id, name, dimension` | `attrs_table` (attribute bonus) |
| class templates | `class_id, dimension[, name, name_en, name_de]` | `templates_table` |
| name vectors | `name, vec` | `name_vectors` (with `name_floor`) |
| texts for reviewers | `[tree_id,] node_id, text` | `uncertain_decisions(texts_table := ...)` |
| class descriptions | `class_id[, label_en \| label, definition]` | `uncertain_decisions(catalogue_table := ...)` |

Setup used by the examples below (three-dimensional toy vectors):

```sql
CREATE TABLE catalogue AS SELECT * FROM (VALUES
  ('pump',  [1.0, 0.0, 0.0]::FLOAT[3], [[1.0, 0.0, 0.0]]::FLOAT[3][]),
  ('motor', [0.0, 1.0, 0.0]::FLOAT[3], [[0.0, 1.0, 0.0]]::FLOAT[3][]),
  ('valve', [0.0, 0.0, 1.0]::FLOAT[3], [[0.0, 0.0, 1.0], [0.3, 0.0, 0.95]]::FLOAT[3][]))
  t(class_id, vec, exemplar_tokens);

CREATE TABLE items AS SELECT * FROM (VALUES
  ('I1', 'Kreiselpumpe 3 kW', [0.9, 0.3, 0.1]::FLOAT[3]),
  ('I2', 'Drehstrommotor',    [0.2, 0.9, 0.1]::FLOAT[3]),
  ('I3', 'Pumpe mit Motor',   [0.6, 0.6, 0.0]::FLOAT[3])) t(node_id, label, vec);
```

## `classify_to_catalogue` — the pivot classifier

```text
classify_to_catalogue(items_table := 'item_vectors', catalogue_table := 'catalogue_vectors',
    mode := 'centroid', center_by_source := true, attr_bonus := 1.0, temperature := 0.3,
    overrides_table := NULL, answers_table := NULL, attrs_table := NULL, templates_table := NULL,
    name_vectors := NULL, name_floor := 0.85, top := NULL, min_source_items := 10)
  → TABLE(tree_id, node_id, source, class_id, p DOUBLE, rank BIGINT, margin DOUBLE, overridden BOOLEAN)
```

One row per item and class (or the best `top` classes). Pipeline per item:
centring → similarity → z-score across the catalogue (+ `attr_bonus` × attribute-template match)
→ softmax of score / `temperature` → `margin = p1 − p2` (same value on every row of the item).
`tree_id`/`source` are NULL when the items table has none.

| Parameter | Default | Effect |
|---|---|---|
| `mode` | `'centroid'` | `'centroid'`: cosine to the class `vec`. `'late_interaction'`: `maxsim(tokens, exemplar_tokens)`; needs `tokens` on items and `exemplar_tokens` on the catalogue. Better on unseen wordings |
| `center_by_source` | true | subtract the mean vector of the item's source (removes a shared sentence frame); centroid mode |
| `min_source_items` | 10 | sources smaller than this use the batch mean |
| `attr_bonus` | 1.0 | β × mean over the item's attributes of the best same-dimension template; needs `attrs_table` + `templates_table`; 0 switches it off |
| `name_vectors`, `name_floor` | NULL, 0.85 | name similarity for the bonus via `calibrated_cosine`; without vectors a dimension match counts 1 |
| `temperature` | 0.3 | lower = sharper `p`; must be positive |
| `overrides_table` | NULL | reviewed wordings → one-hot `p`, `overridden = true` |
| `answers_table` | NULL | per-item answers → one-hot `p`, `overridden = true`; beat overrides |
| `top` | NULL | keep the best `top` classes per item |

```sql
SELECT node_id, class_id, round(p, 3) AS p, rank, round(margin, 3) AS margin
FROM classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue', top := 2)
ORDER BY node_id, rank;
-- I1 pump 0.997 / motor 0.003, margin 0.994
-- I2 motor 0.998 / pump 0.001, margin 0.997
-- I3 motor 0.5   / pump 0.5,   margin 0.0   <- ask someone
```

Late interaction scores token vectors (here one item whose tokens are close to the second
`valve` exemplar):

```sql
CREATE TABLE token_items AS SELECT * FROM (VALUES
  ('U1', [0.2, 0.2, 0.9]::FLOAT[3], [[0.3, 0.0, 0.95], [0.0, 0.2, 0.9]]::FLOAT[3][]),
  ('U2', [1.0, 0.0, 0.0]::FLOAT[3], [[1.0, 0.0, 0.0]]::FLOAT[3][])) t(node_id, vec, tokens);

SELECT node_id, class_id, round(p, 3) AS p
FROM classify_to_catalogue(items_table := 'token_items', catalogue_table := 'catalogue',
                           mode := 'late_interaction', top := 1)
ORDER BY node_id;
-- U1 valve 0.998, U2 pump 0.996
-- => U1 | valve | 0.998
-- => U2 | pump | 0.996
```

Attribute bonus — a `length` attribute only fits class A's template, which flips a B-leaning item
(from `test/sql/coverage/r4/classify_to_catalogue.test`):

```sql
CREATE TABLE cat2 AS SELECT * FROM (VALUES ('A', [1.0, 0.0]), ('B', [0.0, 1.0]), ('C', [-1.0, 0.0]))
  t(class_id, vec);
CREATE TABLE bonus_items AS SELECT * FROM (VALUES ('T', 'x', [0.6, 0.8])) t(tree_id, node_id, vec);
CREATE TABLE bonus_attrs AS SELECT * FROM (VALUES ('T', 'x', 'travel', 'length'))
  t(tree_id, node_id, name, dimension);
CREATE TABLE class_templates AS SELECT * FROM (VALUES
  ('A', 'length', 'adjustment travel'), ('B', 'temperature', 'surface temperature'))
  t(class_id, dimension, name_en);

SELECT 'no bonus' AS setting, class_id, round(p, 3) AS p
FROM classify_to_catalogue(items_table := 'bonus_items', catalogue_table := 'cat2',
                           center_by_source := false) WHERE rank = 1
UNION ALL
SELECT 'bonus', class_id, round(p, 3)
FROM classify_to_catalogue(items_table := 'bonus_items', catalogue_table := 'cat2',
                           center_by_source := false, attrs_table := 'bonus_attrs',
                           templates_table := 'class_templates') WHERE rank = 1;
-- no bonus B 0.746 | bonus A 0.905
```

Answers and overrides give a one-hot distribution:

```sql
CREATE TABLE item_overrides AS SELECT * FROM (VALUES ('Pumpe mit Motor', NULL, 'pump'))
  t(wording, source, class_id);
CREATE TABLE item_answers AS SELECT 'I3' AS node_id, 'valve' AS class_id;

SELECT node_id, class_id, p, overridden
FROM classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue',
                           overrides_table := 'item_overrides', answers_table := 'item_answers', top := 1)
ORDER BY node_id;
-- I3 valve 1.0 true  (the answer beats the override's 'pump')
```

Input checks (errors): unknown `mode`, `temperature` ≤ 0, `late_interaction` without `tokens`,
dimension mismatch, empty catalogue, duplicate `class_id` or item, NULL `node_id`, non-finite vector
elements, override/answer class not in the catalogue, `overrides_table` without a `label` column.

## Scalars: `maxsim`, `bhattacharyya`, `calibrated_cosine`

```text
maxsim(query_tokens DOUBLE[][], document_tokens DOUBLE[][]) → DOUBLE
bhattacharyya(p MAP(VARCHAR, DOUBLE), q MAP(VARCHAR, DOUBLE)) → DOUBLE
calibrated_cosine(a DOUBLE[], b DOUBLE[], floor DOUBLE) → DOUBLE
```

`FLOAT[n]` arrays (and `FLOAT[n][]` token sets) cast in implicitly, so vss-style columns work as they are.

| Function | Meaning | Edge cases |
|---|---|---|
| `maxsim` | ColBERT-style late interaction: mean over query tokens of the best cosine over document tokens. Asymmetric | tokens must share one dimension; zero token → cosine 0; NULL/empty list → NULL; NaN → error |
| `bhattacharyya` | Σ √(p_k q_k): 1 identical, 0 disjoint. The soft label agreement L(q, c) inside `tree_coverage` | distributions used as given (not normalised); negative → error; NULL → NULL |
| `calibrated_cosine` | (cos − floor) / (1 − floor), clipped to [0, 1]: cosine rescaled above the corpus floor | floor in [0, 1) else error; NULL floor → NULL; NULL element → error |

```sql
SELECT maxsim([[1.0, 0.0], [0.0, 1.0]], [[1.0, 0.0], [0.6, 0.8]])                 AS maxsim_score,  -- 0.9
       bhattacharyya(MAP {'heating': 0.6, 'ventilation': 0.4},
                     MAP {'heating': 0.5, 'ventilation': 0.5})                     AS agreement,     -- 0.9949
       bhattacharyya(MAP {'heating': 1.0}, MAP {'ventilation': 1.0})               AS disjoint,      -- 0.0
       calibrated_cosine([1.0, 0.0], [0.8, 0.6], 0.5)                              AS calibrated;    -- 0.6
```

## `cosine_floor` — calibrate a sentence encoder

```text
cosine_floor(table_name VARCHAR, column_name VARCHAR, sample := 10000, seed := 7)   -- table/column positional
  → TABLE(floor DOUBLE, n_pairs BIGINT)
```

Median cosine of distinct vector pairs. Exact when all pairs fit into `sample`, otherwise a
deterministic sample from `seed`. Small sentence encoders give unrelated texts a cosine around
0.85; the floor makes only real synonyms count. Use the same floor for `classify_to_catalogue`
and `tree_coverage` (`name_floor := ...`).

```sql
CREATE TABLE name_vecs AS SELECT * FROM (VALUES
  ('current draw',        [0.90, 0.40, 0.10]::FLOAT[3]),
  ('max current',         [0.88, 0.45, 0.12]::FLOAT[3]),
  ('travel',              [0.50, 0.80, 0.30]::FLOAT[3]),
  ('surface temperature', [0.60, 0.30, 0.70]::FLOAT[3])) t(name, vec);

SET VARIABLE name_floor = (SELECT COALESCE(GREATEST(0.0, LEAST(floor, 0.999)), 0.0) FROM cosine_floor('name_vecs', 'vec'));
SELECT round(getvariable('name_floor'), 3) AS floor,                                  -- 0.799 (6 pairs)
       round(calibrated_cosine([0.90, 0.40, 0.10], [0.88, 0.45, 0.12],
                               getvariable('name_floor')), 3) AS current_vs_max_current;
```

On the benchmark name vectors the floor is 0.853 from 14 365 pairs (`sample := 20000`).

## Thresholds and abstention — `uncertain_decisions`

```text
uncertain_decisions(decisions := <classify_to_catalogue result table>,
    threshold := 0.2 | share := <fraction>, top := <n candidates>,
    texts_table := NULL, catalogue_table := NULL)
  → TABLE(tree_id, node_id, margin DOUBLE, request VARCHAR)   -- request is JSON text
```

Selects the low-margin items of a decision table, **overridden items excluded**, lowest margin
first: `margin < threshold` (default 0.2) or the lowest `ceil(share × n)` items. `request` is one
JSON object per item — `tree_id, node_id, text, margin, candidates[{class_id, p, label, definition}]`
— ready for a colleague or an LLM. JSON escapes quotes, backslashes and control characters.

```sql
CREATE TABLE decided AS
SELECT * FROM classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue');
CREATE TABLE item_texts AS SELECT node_id, label AS text FROM items;
CREATE TABLE class_info AS SELECT * FROM (VALUES
  ('pump', 'centrifugal pump', 'Moves liquid'), ('motor', 'electric motor', 'Drives a shaft'),
  ('valve', 'shut-off valve', 'Stops a flow')) t(class_id, label_en, definition);

SELECT node_id, round(margin, 3) AS margin, request
FROM uncertain_decisions(decisions := 'decided', top := 2,
                         texts_table := 'item_texts', catalogue_table := 'class_info');
-- I3 | 0.0 | {"tree_id":null,"node_id":"I3","text":"Pumpe mit Motor","margin":0.0,
--            "candidates":[{"class_id":"motor","p":0.499788,"label":"electric motor",...},...]}
```

Write the reviewer's answers to a table `answers` with columns `tree_id, node_id, class_id` and classify again with
`answers_table := 'answers'` (see the canonical pipeline). On the benchmark, answering the 15 %
lowest-margin items (`share := 0.15`) lifts recall@20 from 0.86 to 0.91; the exported items have
6× the overall error rate (`examples/classification_review/uncertain_decisions.sql`).

## The vocabulary review cycle — queue → decisions → apply

Archives reuse a closed internal vocabulary: the same wordings in thousands of trees. Classify
each **distinct wording once**, rank the wordings by impact, let a reviewer confirm the few that
matter, and store the answers as the reviewed vocabulary.

```text
vocabulary_review_queue(trees_table := 'tree_nodes', decisions := <table>,
    sources := NULL /* VARCHAR[] */, overrides_table := NULL)
  → TABLE(wording, source, n_occurrences, n_trees_affected, top1, p1, top2, p2, margin, impact)

apply_vocabulary_review(review_table [, overrides_table := 'vocabulary_overrides' if it exists])
  → TABLE(wording, source, class_id, reviewed_by, reviewed_at)
```

- **Queue:** one row per distinct `(label, source)` of the item nodes (`node_kind = 'item'`) of
  `trees_table`. `decisions` is a `classify_to_catalogue` result, either per wording
  (`node_id` = the wording, `tree_id` NULL) or per node (averaged over the wording's occurrences).
  `impact = n_trees_affected × (1 − margin)`, highest first. Wordings in `overrides_table` leave the
  queue; `sources := ['internal']` restricts document families.
- **Apply:** the review table needs `wording, class_id` (plus optional `source, reviewed_by,
  reviewed_at`). One entry per wording and source; the review wins. Without `overrides_table`
  it merges into an existing `vocabulary_overrides` table (or starts one); `overrides_table := NULL`
  starts fresh. A review table without a `wording` column is an error.

Toy archive: two programmes use the in-house wording "KP-Aggregat" (a pump that reads half like a motor).

```sql
CREATE TABLE archive_nodes AS SELECT * FROM (VALUES
  ('R1', 'R1',   NULL, 'container', 'AND', 'Request R1',      NULL, 1, 'req'),
  ('R1', 'R1.1', 'R1', 'item',      'AND', 'Kreiselpumpe',    NULL, 1, 'req'),
  ('R1', 'R1.2', 'R1', 'item',      'AND', 'Pumpe mit Motor', NULL, 1, 'req'),
  ('P1', 'P1',   NULL, 'container', 'AND', 'Programme P1',    NULL, 1, 'internal'),
  ('P1', 'P1.1', 'P1', 'item',      'AND', 'KP-Aggregat',     NULL, 1, 'internal'),
  ('P1', 'P1.2', 'P1', 'item',      'AND', 'Antriebseinheit', NULL, 1, 'internal'),
  ('P2', 'P2',   NULL, 'container', 'AND', 'Programme P2',    NULL, 1, 'internal'),
  ('P2', 'P2.1', 'P2', 'item',      'AND', 'KP-Aggregat',     NULL, 1, 'internal'),
  ('P2', 'P2.2', 'P2', 'item',      'AND', 'Absperrorgan',    NULL, 1, 'internal'))
  t(tree_id, node_id, parent_id, node_kind, edge_type, label, class_id, count, source);

-- one vector per distinct wording; node_id and label are the wording itself
CREATE TABLE wordings AS SELECT * FROM (VALUES
  ('KP-Aggregat',     'KP-Aggregat',     'internal', [0.55, 0.50, 0.10]::FLOAT[3]),
  ('Antriebseinheit', 'Antriebseinheit', 'internal', [0.10, 0.95, 0.10]::FLOAT[3]),
  ('Absperrorgan',    'Absperrorgan',    'internal', [0.10, 0.10, 0.95]::FLOAT[3]))
  t(node_id, label, source, vec);

CREATE TABLE wording_decisions AS
SELECT * FROM classify_to_catalogue(items_table := 'wordings', catalogue_table := 'catalogue');
```

```sql
SELECT wording, top1, top2, round(margin, 3) AS margin, n_trees_affected, round(impact, 3) AS impact
FROM vocabulary_review_queue(trees_table := 'archive_nodes', decisions := 'wording_decisions');
-- KP-Aggregat     pump  motor 0.392 2 1.217   <- review first
-- Absperrorgan    valve motor 0.997 1 0.003
-- Antriebseinheit motor pump  0.997 1 0.003
-- => KP-Aggregat | pump | motor | 0.392 | 2 | 1.217
-- => Absperrorgan | valve | motor | 0.997 | 1 | 0.003
-- => Antriebseinheit | motor | pump | 0.997 | 1 | 0.003
```

```sql
CREATE TABLE review AS SELECT * FROM (VALUES ('KP-Aggregat', 'internal', 'pump', 'jr'))
  t(wording, source, class_id, reviewed_by);
```

```sql
-- table functions cannot write: store the merged overrides yourself
-- (first review; on later reviews replace the table with the merged result)
CREATE TABLE vocabulary_overrides AS
SELECT * FROM apply_vocabulary_review('review', overrides_table := NULL);
```

```sql
SELECT wording FROM vocabulary_review_queue(trees_table := 'archive_nodes', decisions := 'wording_decisions',
                                            overrides_table := 'vocabulary_overrides');
-- Absperrorgan, Antriebseinheit   (the reviewed wording left the queue)
```

On the benchmark archive (`examples/classification_review/vocabulary_review.sql`) the four
misleading wordings ("Klimapaket Heizung" is a ventilation module) top the queue, their `top2` is
the true class, and four answers fix 225 of 300 programmes.

## How classification feeds the coverage kernel

`tree_coverage` and friends resolve each item's class in this order
(see **anofox-similarity-coverage**):

1. `class_id` on the node (reviewed),
2. `overrides_table` — the reviewed vocabulary `(wording, source, class_id)`, `wording` matched
   exactly on the node's `label`, NULL `source` matches any (reviewed),
3. `label_table` — classifier output `(tree_id, node_id, class_id, p)` (not reviewed).
   `labels := 'soft'` (default) uses the whole distribution via `bhattacharyya`; `'hard'` the arg-max.

The output of `classify_to_catalogue` *is* a `label_table` (columns `tree_id, node_id, class_id, p`).
For wording-level decisions, join them back onto the nodes. One `label_table` serves both the
request and the candidate side — `UNION ALL` them. Soft labels keep coverage below 1 even without
gaps, and since classifier labels are never *reviewed*, `assignment := 'auto'` uses `best_match`.
To let a confirmed request use `partition`, write its classes into the nodes' `class_id`:

```text
CREATE TABLE request_nodes_reviewed AS
SELECT n.* REPLACE (COALESCE(n.class_id, l.class_id) AS class_id)
FROM request_nodes n LEFT JOIN (SELECT * FROM request_labels WHERE p = 1.0) l USING (tree_id, node_id);
```

The same `label_table` / `overrides_table` pair is accepted by `tree_class_multiset`,
`tree_anchors`, `validate_trees`, `tree_coverage_trace` and `fit_coverage_weights`.

## Canonical classification-and-review pipeline

Uses the setup blocks above (`catalogue`, `archive_nodes`, `wording_decisions`,
`vocabulary_overrides`).

```sql
-- 1. Classify the request items (unique sentences: review per item, not per wording)
CREATE TABLE request_items AS SELECT * FROM (VALUES
  ('R1', 'R1.1', 'req', 'Kreiselpumpe',    [0.9, 0.3, 0.1]::FLOAT[3]),
  ('R1', 'R1.2', 'req', 'Pumpe mit Motor', [0.6, 0.6, 0.0]::FLOAT[3])) t(tree_id, node_id, source, label, vec);
CREATE TABLE request_decisions AS
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue');

-- 2. Export the uncertain ones (texts need tree_id because the decisions have one)
CREATE TABLE request_texts AS SELECT tree_id, node_id, label AS text FROM request_items;
CREATE TABLE export AS
SELECT * FROM uncertain_decisions(decisions := 'request_decisions', threshold := 0.2, top := 2,
                                  texts_table := 'request_texts');

-- 3. The reviewer's answers come back as (tree_id, node_id, class_id)
CREATE TABLE answers AS SELECT tree_id, node_id, 'pump' AS class_id FROM export;

-- 4. One label_table for both sides: archive items take their wording's distribution
CREATE TABLE labels AS
SELECT n.tree_id, n.node_id, d.class_id, d.p
FROM archive_nodes n JOIN wording_decisions d ON d.node_id = n.label AND d.source = n.source
WHERE n.node_kind = 'item'
UNION ALL
SELECT tree_id, node_id, class_id, p
FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                           answers_table := 'answers');

-- 5. Rank archive programmes for the request; reviewed wordings win over the labels
SELECT candidate_id, round(coverage, 3) AS coverage, rank, n_gaps, reviewed, assignment_used
FROM tree_coverage('R1', k := 5, nodes_table := 'archive_nodes', attrs_table := NULL,
                   overrides_table := 'vocabulary_overrides', label_table := 'labels');
-- P1 0.999 1 0 false best_match | P2 0.999 2 0 false best_match
```

Production shape (benchmark files in `test/data/coverage/`): `catalogue_vectors.csv.gz`
(64-dimensional: `vec::FLOAT[64]`, `exemplar_tokens::FLOAT[64][]`), `vocabulary_vectors.csv.gz`,
`request_item_vectors.csv.gz`, `programme_nodes.csv.gz`, `request_nodes.csv.gz`,
`catalogue_classes.csv.gz`, `attribute_name_vectors.csv.gz`. Benchmark figures (asserted in
`test/sql/coverage/r4/uc_01`…`uc_07`): top-1 0.593 raw → 0.853 centred → 0.953 with the attribute
bonus; late interaction beats centroid on 200 unseen phrasings (0.94 vs 0.75); with classifier
labels recall@20 0.86, after answering the 15 % lowest-margin items 0.91.

## Related skills

- **anofox-similarity-coverage** — `tree_coverage`, property trees, class resolution, assignment
- **anofox-similarity-evaluation-scale** — measure each step (`eval_*`, `threshold_at_precision`, `fit_coverage_weights`)
- **anofox-similarity-erp-embeddings** — computing/storing embeddings, VSS
- **anofox-similarity-aas** — suggesting semantic ids for uncoded AAS attributes with the same classifier
- Runnable walkthroughs: `examples/classification_review/{classify_to_catalogue,vocabulary_review,uncertain_decisions}.sql`
