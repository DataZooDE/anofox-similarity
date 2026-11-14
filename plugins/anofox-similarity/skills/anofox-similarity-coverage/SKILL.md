---
name: anofox-similarity-coverage
description: >
  Directional coverage matching for the anofox_similarity DuckDB extension: stage-1 search over
  feature multisets (`find_covering_entities`, `find_covering_entities_for`), quantities with units
  and bounds (`parse_quantity`, `normalize_quantity`, `value_satisfies`, `compat_score`,
  `quantity_units`), property trees (`tree_from_json`, `validate_trees`, `tree_anchors`,
  `tree_class_multiset`, `tree_multiset`) and the coverage kernel (`tree_coverage`,
  `tree_coverage_trace`, `coverage_recall`, `coverage_kernel`). Use when asked "which past
  programme/offer covers this request", "what is missing", "does 9 A meet max. 8 A", "parse
  'mindestens 24 cm'", "load spec trees from JSON", or "explain the gaps of this candidate".
version: 2026.11.13
user-invocable: false
---

# Anofox Similarity — Coverage Matching Cheat Sheet

**Extension:** `anofox_similarity` v2026.11.13 | **DuckDB:** v2.0 / v1.5.6 | **Reference:** [`docs/api/10-covering-search.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/10-covering-search.md), [`docs/api/11-quantities.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/11-quantities.md), [`docs/api/12-property-trees.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/12-property-trees.md), [`docs/api/13-coverage-kernel.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/13-coverage-kernel.md)

> Paths like `test/data/...` are relative to a clone of the anofox-similarity repository; in
> your own project, substitute your tables. Examples that build inline tables run anywhere.

Coverage is directional: does offer B provide everything request A asks for? Surplus does not
hurt; missing items and failed limits do. **Stage 1** ranks candidates by class multisets
(weighted containment); **stage 2** compares item by item (class, count, each value with unit and
direction) and returns coverage in [0, 1] plus a gap list. Numbers use rules, not embeddings: the
dimension gates, the bound gives the direction. Guides: `docs/guides/04-coverage-matching.md`,
`docs/guides/05-quantities-and-specs.md`; concepts: `docs/requirements-matching/HLD.md`.

## Critical gotchas

- **Table-function arguments must be constants**: a subquery fails with `Table function cannot
  contain subqueries`, and `LATERAL` with a column argument is not supported. Compute first, then
  pass `getvariable('x')` or a prepared `$1`. Table parameters take a table **name**, not a query.
- **1.5.6: `getvariable()` is evaluated at PREPARE.** A prepared statement re-runs the old value
  after `SET VARIABLE`. Use `$1` parameters for reusable statements (2.0 is fine either way).
- **1.5.6: scalar named arguments bind by position.** `value_satisfies(req, prov, scale, tol,
  soft_band, default_bound)` (pass `NULL` for `scale`), `parse_quantity(text, lang, unit_table)`.
  Table functions bind by name. Tversky: pass `alpha` and `beta`, `alpha` first.
- **Name both tables of each side** in `tree_coverage` / `tree_coverage_trace`: a non-default
  `nodes_table` without `attrs_table` (or `candidate_nodes_table` without
  `candidate_attrs_table`) is a binder error; `attrs_table := NULL` = no values.
  `coverage_recall` has no attrs parameters (passing one is an error).
- **Candidates default to the query table**: every other tree in it, other requests too. Keep
  requests and archive apart, or pass `candidates := [...]`.
- **Stage-1 `k` can drop a full cover**: stage 2 sees only the top `k` (default 20); with
  `k := 2` on the mini fixture `S6` (coverage 1.0) is never scored.
- **Coverage < 1 with no gaps** = name discount `mu` (0.2); without `name_vectors` names compare
  by word overlap (German never matches English). Use `mu := 0.0` or name vectors.
- **Dimension mismatch is not an error**: `value_satisfies` gives score 0, rule `gate` (A vs V,
  unknown unit, EUR vs USD). A categorical value (`IP54`) **is** an error there: use `compat_score`.
- **`within` = the offer covers the whole required range** (`25 cm` vs `von 22 bis 26 cm` →
  0.51). Store an acceptable band as two limits, `at_least` + `at_most`.
- **Unqualified values are points** (`38 dB(A)` vs `35 dB(A)` → 0): use `default_bound` or
  `default_bounds := 'catalogue'`. **Tolerance is soft** (`tol` 0.02, `soft_band` 0.05): 1.7 %
  over `max. 180 s` scores 1; hard limits need 0 and 0.
- **`compat_score` needs its rules table to exist** (may be empty). No rule: equal → 1, else 0.
- **Covering `score` can exceed 1** (containment + 0.01 × jaccard); a near-full lean cover can
  outrank a full bloated one. `min_score := 1.0` keeps full covers (it tests the raw measure).
- **Pass the document language**: `parse_quantity('2.500 Zyklen')` is 2.5 (English wins by
  default), 2500 with `'de'`.

## Input tables

**Feature multiset** (stage 1, `find_covering_entities*`): `(entity_id, feature_id, weight)`,
default table `feature_multiset`; remap with `feature_table`, `entity_col`, `feature_col`,
`weight_col` (`weight_col := NULL` → unit weights). Repeated rows add up; NULL weight = 1;
negative/non-finite weight anywhere → error naming the row.

**Property trees** (stage 2):

| Table | Required columns | Optional |
|---|---|---|
| nodes (`tree_nodes`) | `tree_id, node_id, parent_id, node_kind, edge_type, label, class_id, count` | `source`, `context` |
| attributes (`tree_attributes`) | `tree_id, node_id, attr_id` | `name, dimension, unit, lo, hi, bound, category, attr_key, role, qualifier` |
| `overrides_table` (reviewed vocabulary) | `wording, class_id` | `source` (NULL matches any source) |
| `label_table` (classifier output) | `tree_id, node_id, class_id, p` | |
| `rules_table` / `compatibility_rules` | `dimension, required, provided, score` | |

- `node_kind`: `item` (compared), `container` / `variant_group` (transparent); `edge_type` `AND`
  or `OR` (alternatives). `lo`/`hi` in **base units** (m, s, A, K). Counts multiply down the
  tree, add across AND siblings, never add across OR alternatives.
- **Class resolution order** (both stages): `class_id` in the nodes table → `overrides_table`
  (`wording` matched exactly against `label`) → `label_table` (arg-max of `p`, ties to the lowest
  class; duplicates add up, NULL class / `p ≤ 0` dropped). A tree is *reviewed* when every item
  got its class from `class_id` or the vocabulary; classifier labels are not reviewed.

```sql
-- Setup used below: the hand-made fixture (requests Q1-Q3, programmes S1-S6, modules M1-M5).
CREATE OR REPLACE TABLE tree_nodes AS
SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE OR REPLACE TABLE tree_attributes AS
SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');
```

```sql
CREATE OR REPLACE TABLE feature_multiset AS
SELECT * FROM read_csv_auto('test/data/coverage/mini/feature_multiset.csv');
```

## `find_covering_entities` / `find_covering_entities_for` — stage-1 search

```text
find_covering_entities(query_entity_id VARCHAR, k BIGINT, <named>)   -- query is an entity of the table
find_covering_entities_for(query MAP(feature, weight), k BIGINT, <named>)  -- query not in the table
  → TABLE(entity_id, score, containment, jaccard, shared_weight, query_weight,
          candidate_weight, surplus_weight)
```

| Named | Default | Notes |
|---|---|---|
| `mode` | `'containment'` | `'containment'` Σmin/Σq, `'jaccard'` Σmin/Σmax, `'tversky'` |
| `alpha`, `beta` | 1.0, 0.0 | Tversky only (missing / surplus weight) |
| `weighted` | true | false: every distinct (entity, feature) weighs 1 |
| `tie_break` | 0.01 | × jaccard added to score (containment, tversky) |
| `min_score` | 0.0 | threshold on the measure itself |
| `feature_table`, `entity_col`, `feature_col`, `weight_col` | `'feature_multiset'`, `'entity_id'`, `'feature_id'`, `'weight'` | column mapping |

Rows ordered `score DESC, entity_id`; only candidates sharing ≥ 1 feature; the query entity is
excluded. Unknown id or empty/NULL/all-zero MAP → no rows; NULL id → error.

```sql
-- Which programmes contain everything the request asks for, leanest first?
SELECT entity_id, round(score, 3) AS score, containment, round(jaccard, 3) AS jaccard, surplus_weight
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 1, 'memory_module': 1}, 4,
       feature_table := 'feature_multiset', min_score := 1.0);
-- S2 1.01 1.0 1.0 0 | S1 1.009 1.0 0.889 1 | S6 1.007 1.0 0.667 4
```

Your own ERP columns, the request as an entity of the table:

```sql
CREATE TABLE erp_offer_lines AS SELECT * FROM (VALUES
  ('REQ-7', 'MOTOR', 1), ('REQ-7', 'SEAL', 2),
  ('OFFER-A', 'MOTOR', 1), ('OFFER-A', 'SEAL', 2), ('OFFER-A', 'SENSOR', 1),
  ('OFFER-B', 'MOTOR', 1)
) t(OfferNumber, RequirementCode, Units);

SELECT entity_id, round(containment, 3) AS coverage
FROM find_covering_entities('REQ-7', 5,
  feature_table := 'erp_offer_lines', entity_col := 'OfferNumber',
  feature_col := 'RequirementCode', weight_col := 'Units');
-- OFFER-A 1.0 | OFFER-B 0.333
-- => OFFER-A | 1.0
-- => OFFER-B | 0.333
```

Reusable statement: use a prepared parameter, not `getvariable` (1.5.6 freezes it at PREPARE):

```sql
PREPARE cover AS
SELECT entity_id, containment FROM find_covering_entities_for($1, 3, feature_table := 'feature_multiset');
EXECUTE cover(MAP {'seat_track': 2.0});
EXECUTE cover(MAP {'seat_ventilation': 2.0});
```

**Many requests at once:** build one MAP per request and per candidate and score all pairs with
`weighted_containment` + 0.01 × `weighted_jaccard` (identical scores; scalars in
`anofox-similarity-material-search`). Approximate recall at scale: `find_covering_entities_lsh`
in `anofox-similarity-evaluation-scale`. Unreviewed labels: prefer `mode := 'jaccard'`.

## Quantities

### `quantity_units` — the units seed

```text
quantity_units() → TABLE(symbol, alias VARCHAR[], unit_id VARCHAR[], dimension, factor, offset, base_unit)
-- base = value × factor + offset; currencies have factor NULL and are never converted
```

SI with prefixes, time (s … year, Wochen/Tage), temperature (K, °C, °F, stored in K), dB(A),
percent/ppm, counts (pcs, Zyklen), data, ISO-4217 currencies. `unit_id` carries UN/ECE Rec 20
codes (`AMP`, `4K`, `CEL` ...). ECLASS unit IRDIs are **not** shipped (licensed); attach your own
through `unit_table`. Inspect: `SELECT * FROM quantity_units() WHERE dimension = 'current'`.

### `parse_quantity` — text to interval + bound

```text
parse_quantity(text [, lang [, unit_table]])
  → STRUCT(lo, hi, unit, dimension, bound, raw, source_unit, category)
-- lang: 'en' | 'de' | list, default ['en', 'de'] (first entry decides '2.500')
-- unit_table: LIST of STRUCT(symbol, alias, unit_id, dimension, factor, offset), e.g.
--             (SELECT list(u) FROM my_units u); extends/overrides the seed
```

`bound` ∈ `at_least`, `at_most`, `within`, `equals`, NULL (unqualified). `lo`/`hi` are in the
base unit. Category codes (`IP54`, `F1`, `M8`) go to `category` with `lo` NULL. A bare number is
a count (`unit` '1'). An unknown unit is never guessed: number kept, `dimension` NULL, every
comparison a gate. Negations flip (`nicht über`, `must not exceed` → `at_most`). Designations
(`M8x1,25`, `DIN 933`) come back empty — map them to categories yourself.

```sql
SELECT t AS text, q.lo, q.hi, q.unit, q.dimension, q.bound, q.category
FROM (VALUES (1, 'max. 38 dB(A)'), (2, 'mindestens 24 cm'), (3, 'zwischen 15 und 25 mm/s'),
             (4, '220 N ± 2 %'), (5, '37 °C'), (6, 'höchstens 7,5 A'), (7, 'IP 54 oder besser'),
             (8, '12 furlongs')) v(i, t),
     LATERAL (SELECT parse_quantity(t, 'de') AS q)
ORDER BY i;
-- 38 dB(A) at_most | 0.24 m at_least | 0.015-0.025 m/s within | 220 N equals
-- 310.15 K NULL | 7.5 A at_most | category IP54 at_least | 12 furlongs, dimension NULL
```

### `normalize_quantity` — value + unit to base unit

```text
normalize_quantity(value DOUBLE, unit VARCHAR [, unit_table]) → STRUCT(value, unit, dimension)
-- unit: symbol, alias or identifier (Rec 20 code or a unit_table unit_id, via semantic_key)
```

```sql
CREATE TABLE my_units AS SELECT * FROM (VALUES
  ('lanes', ['Spuren'], 'lane_count', 1.0, 0.0)) t(symbol, alias, dimension, factor, "offset");

SELECT normalize_quantity(2500, 'mA')            AS ma,        -- {2.5, A, current}
       normalize_quantity(5, 'AMP')              AS rec20,     -- {5.0, A, current}
       normalize_quantity(3, 'lanes')            AS unknown,   -- dimension NULL
       parse_quantity('mindestens 3 Spuren', 'de', (SELECT list(u) FROM my_units u)) AS custom;
```

A `unit_table` row with only `symbol, unit_id` attaches ids to a known unit (e.g. licensed ECLASS
IRDIs); two units claiming one id is an error.

### `value_satisfies` — direction-aware comparison

```text
value_satisfies(requirement, provided [, scale [, tol, soft_band [, default_bound]]])
  → STRUCT(score DOUBLE, distance DOUBLE, rule VARCHAR)
-- both args: STRUCT with lo, hi, dimension (optional unit; requirement also bound);
-- parse_quantity output plugs in directly; non-base units are normalised first
-- defaults: scale per dimension, tol 0.02, soft_band 0.05
```

| Rule | Satisfied when | `distance` |
|---|---|---|
| `at_least v` | provided high end ≥ v | `(v − p_hi)/|v|` |
| `at_most v` | provided low end ≤ v | `(p_lo − v)/|v|` |
| `within [lo, hi]` | provided range covers the required range | uncovered share |
| `equals v` | `|v − p|` ≤ tol relative (p = farther end of a range) | relative |
| `point` (no bound) | provided ≈ v (±0.01 decades log / ±1 % linear) | uncovered share |
| `gate` | different/unknown dimensions or currencies → score 0 | NULL |
| `missing` | no provided value → score 0 | NULL |

- `score` is 1 within `tol`, falls linearly to 0 across `soft_band`; `soft_band` 0 = strict.
- `scale`: linear for time, currency, percent, sound level; log otherwise (tol 0.02 decades ≈
  4.5 %). A limit of 0 uses an absolute distance and a strict score.
- Temperature: absolute 1 K tolerance, 2.5 K band, `distance` in K; `tol`/`soft_band` ignored.
- `default_bound` only applies when the requirement has no bound; a stated bound wins.

```sql
SELECT value_satisfies(parse_quantity('max. 180 s'), {'lo': 190.0, 'hi': 190.0, 'dimension': 'time'}).score AS soft,   -- 0.289
       value_satisfies(parse_quantity('max. 180 s'), parse_quantity('190 s'), NULL, 0.0, 0.0).score AS strict,           -- 0.0
       value_satisfies(parse_quantity('at least 5 A'), parse_quantity('5 V'))                      AS dim_mismatch,      -- {0.0, NULL, gate}
       value_satisfies(parse_quantity('max. 43 °C'), parse_quantity('45 °C'))                      AS temp,              -- {0.6, 2.0, at_most}
       value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)')).score               AS as_point,          -- 0.0
       value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)'),
                       NULL, 0.02, 0.05, 'at_most').score                                          AS with_bound;        -- 1.0
```

```text
SELECT value_satisfies(parse_quantity('IP54 or better'), parse_quantity('IP65'));
-- Invalid Input Error: value_satisfies compares numbers; for categorical values such as 'IP54' use compat_score
```

### `compat_score` — categorical / ordinal classes

```text
compat_score(dimension, required, provided [, rules_table := 'compatibility_rules']) → DOUBLE
-- rules table (dimension, required, provided, score) must exist (may be empty)
-- no rule: 1.0 if equal else 0.0; NULL category → NULL; text similarity never used
```

```sql
CREATE TABLE compatibility_rules AS
SELECT * FROM read_csv_auto('test/data/coverage/compatibility_rules.csv.gz');

SELECT provided, compat_score('protection_class', parse_quantity('IP54 or better').category, provided) AS score
FROM (VALUES (1, 'IP67'), (2, 'IP54'), (3, 'IP40')) t(i, provided)
ORDER BY i;
-- IP67 1.0 | IP54 1.0 | IP40 0.5
-- => IP67 | 1.0
-- => IP54 | 1.0
-- => IP40 | 0.5
```

## Property trees

### `tree_from_json` — extracted JSON to node rows

```text
tree_from_json(json [, tree_id [, lang [, unit_table]]])
  → LIST(STRUCT(<tree_nodes columns>, context, source, attributes LIST(STRUCT(<tree_attributes
         columns>, value_text, role, qualifier, attr_key, bound_source))))
-- JSON: {tree_id, source, lang, root: {node_id, kind, edge, label, context, class_id, count,
--        attributes: [{attr_id, name, text, lo, hi, unit, bound, category}], children: [...]}}
```

Structured value fields win over `text`; text-only values are parsed with `parse_quantity` in
the document's `lang` (or the `lang` argument). Errors name the node: non-positive `count`, or a
key close to a known one (`childs` → "did you mean 'children'?"). Other keys are ignored.

```sql
CREATE TABLE spec_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json('{"tree_id": "NEW-1", "source": "aurivon", "lang": "de",
  "root": {"node_id": "NEW-1", "kind": "container", "label": "Lastenheft Komfortsitz", "children": [
    {"node_id": "NEW-1.1", "kind": "item", "label": "Sitzheizung 2 Zonen", "count": 2, "class_id": "seat_heating",
     "attributes": [{"attr_id": "a1", "name": "Aufheizzeit auf 37 °C", "text": "max. 3 min"},
                    {"attr_id": "a2", "name": "Stromaufnahme", "text": "höchstens 7,5 A"}]},
    {"node_id": "NEW-1.2", "kind": "item", "label": "Massage 4 Zonen", "count": 4, "class_id": "massage"}]}}')) AS n);
```

```sql
CREATE TABLE spec_attrs AS SELECT unnest(attributes, recursive := true) FROM spec_nodes;
SELECT node_id, name, dimension, lo, unit, bound FROM spec_attrs;
-- NEW-1.1 | Aufheizzeit auf 37 °C | time | 180.0 | s | at_most
-- NEW-1.1 | Stromaufnahme         | current | 7.5 | A | at_most
```

Many files at once: `SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM
read_text('test/data/coverage/requests_json/*.json'))` (80 benchmark requests, 3128 nodes).

### `validate_trees` — run on every new batch

```text
validate_trees(nodes_table [, attrs_table], overrides_table, label_table, tree_ids)
  → TABLE(tree_id, node_id, attr_id, issue, severity, detail)
```

Errors: `duplicate_node, null_key, bad_node_kind, bad_edge_type, bad_count, orphan, no_root,
multiple_roots, cycle, unreachable, too_deep` (> 256 levels), and with attributes
`duplicate_attribute, attribute_on_container, orphan_attribute, bad_interval, bad_bound`.
Warnings: `mixed_edge_types, unknown_unit, category_without_dimension, mixed_roles,
mixed_dimensions`.

```sql
SELECT count(*) FILTER (WHERE severity = 'error') AS n_errors
FROM validate_trees('tree_nodes', 'tree_attributes');
-- 0
```

### `tree_anchors` — one row per item

```text
tree_anchors(nodes_table [, attrs_table], overrides_table, label_table, tree_ids)
  → TABLE(tree_id, node_id, label, class_id, reviewed, count, effective_count, depth,
          anchor_path VARCHAR[], variant_path VARCHAR[], context, source, n_attributes)
```

`effective_count` = product of counts along the path; `variant_path` = OR choices on the way.
List unclassified items (`WHERE class_id IS NULL`) before trusting a low score.

```sql
SELECT node_id, class_id, effective_count, anchor_path, variant_path
FROM tree_anchors('tree_nodes', 'tree_attributes', tree_ids := ['M1']);
-- M1.C1.A1 | seat_track | 1.0 | [M1, M1.C1] | [M1.C1, M1.C1.A1]   (three OR variants)
```

### `tree_class_multiset` / `tree_multiset` — stage-1 index

```text
tree_class_multiset(nodes_table, <named: attrs_table, overrides_table, label_table, tree_ids>)
  → TABLE(entity_id, feature_id, weight, reviewed)   -- max across OR alternatives (optimistic)
-- only nodes_table is positional: tree_class_multiset('t', 'attrs') is a binder error
tree_multiset(nodes LIST(STRUCT(node_id, parent_id, node_kind, edge_type, class_id, count)))
  → MAP(VARCHAR, DOUBLE)                             -- the scalar behind it
```

The output is a feature multiset: feed it to `find_covering_entities*` or to `tree_coverage`
as `candidate_multiset_table`. Pass the same `overrides_table` you rank with; rebuild when the
archive or vocabulary changes.

```sql
SELECT tree_id, tree_multiset(list(struct_pack(node_id, parent_id, node_kind, edge_type, class_id, count))) AS classes
FROM tree_nodes WHERE tree_id IN ('Q1', 'M1') GROUP BY tree_id ORDER BY tree_id;
-- M1 {seat_track=1.0} | Q1 {headrest_powered=1.0, massage=4.0, memory_module=1.0, seat_heating=2.0}
```

## Coverage kernel

```text
tree_coverage(query_tree_id VARCHAR | VARCHAR[], <named>)
  → TABLE(query_id, candidate_id, coverage, stage1_score, rank_score, rank, n_requirements,
          n_gaps, reviewed, assignment_used)
tree_coverage_trace(query_tree_id, <same named>)
  → TABLE(query_id, candidate_id, req_node_id, matched_node_id, matched_path, label_sim, attr_id,
          attr_name, req_value, prov_value, gate, name_sim, value_score, distance, rule, attr_score,
          count_needed, count_available, req_score, is_gap, name_rule, bound_source, label_rule)
coverage_recall(query_tree_id, candidates, k, nodes_table, candidate_nodes_table, overrides_table,
                label_table, mode, tie_break, candidate_multiset_table)   -- no attrs_table parameters
  → TABLE(query_id, candidate_id, score, containment, jaccard, reviewed, mode_used, rank)
coverage_kernel(query LIST(STRUCT), candidate LIST(STRUCT), options STRUCT)
  → STRUCT(coverage, n_requirements, n_gaps, reviewed, assignment_used, trace LIST(STRUCT))
```

```text
attr(x, y)  = gate · (mu·name_sim + 1 − mu) · value_satisfies(x, y).score   (categorical: compat_score)
a(q, c)     = mean over q's attributes of the best attr(x, ·)               (1 without attributes)
L(q, c)     = Bhattacharyya(p(q), p(c))  or 0/1 with labels := 'hard'
score(q, c) = label_weight·L + (1 − label_weight)·L·a
req(q)      = best score × min(1, Σ counts of compatible items with L ≥ count_gate / count(q))
coverage    = mean req(q);   rank_score = coverage + stage1_weight · stage1_score
```

| Named (`tree_coverage`, `tree_coverage_trace`) | Default | Notes |
|---|---|---|
| `candidates` | NULL | explicit ids; NULL → stage-1 top `k` (query never its own candidate) |
| `k` | 20 | stage-1 depth |
| `nodes_table`, `attrs_table` | `'tree_nodes'`, `'tree_attributes'` | query trees; `attrs_table := NULL` for none |
| `candidate_nodes_table`, `candidate_attrs_table` | the query's tables | archive |
| `overrides_table`, `label_table` | NULL | class resolution (both sides) |
| `candidate_multiset_table` | NULL | precomputed `tree_class_multiset` index; same results, faster |
| `recall_mode` | `'auto'` | containment if every candidate is reviewed, else jaccard (`mode` in `coverage_recall`) |
| `assignment` | `'auto'` | `'partition'` (one item serves one requirement, no substitution), `'best_match'` (sharing); auto = partition when both trees reviewed |
| `alternatives` | `'joint'` | one alternative per candidate OR group for the whole request; `'per_requirement'` |
| `mu` | 0.2 | attribute-name weight (names only discount) |
| `labels`, `label_weight` | `'soft'`, 0.0 | `'hard'` = 0/1 class agreement; λ = label credit independent of values |
| `partition_gate`, `count_gate` | 0.3, 0.5 | min label agreement for a block / for counting instances |
| `stage1_weight`, `tie_break` | 0.05, 0.01 | |
| `tol`, `soft_band` | 0.02, 0.05 | passed to `value_satisfies`; 0, 0 for hard limits |
| `default_bounds`, `templates_table` | NULL, `'catalogue_attributes'` | `'catalogue'`: unqualified values take their template's bound |
| `rules_table` | NULL | categorical rules |
| `name_vectors`, `name_floor` | NULL, 0.85 | `(name, vec)` table; floor = median cosine of unrelated names |
| `class_hierarchy`, `generalization_credit` | NULL, 0.0 | `(class_id, parent_class_id)`: subclass offer meets superclass requirement; superclass offer gets credit^distance |

**What `tree_coverage` errors on** (binder / invalid input):

```text
Invalid Input Error: tree_coverage: query tree 'NOPE' is not in nodes_table 'tree_nodes'
Invalid Input Error: tree_coverage: query tree 'Q1' has no classified items (give class_id, overrides_table or label_table)
Binder Error: tree_coverage: nodes_table 'spec_nodes' is given but attrs_table is not; name the attributes table (attrs_table := NULL for none)
Binder Error: nodes_table 'n' is missing required column(s): label (expected columns: tree_id, node_id, parent_id, node_kind, edge_type, label, class_id, count)
Binder Error: label_table 'lb' is missing required column(s): p (expected columns: tree_id, node_id, class_id, p)
Binder Error: tree_coverage: assignment must be one of 'auto', 'best_match', 'partition'
```

Not errors: a partially classified query (unclassified items become gaps with
`matched_node_id` NULL), an empty `candidates := []` list (no rows), unclassified candidate items
(match nothing). `n_gaps` counts requirements (items), not attributes; `is_gap` = no counterpart,
a count shortfall, or a value below 1. Unchosen request OR alternatives do not appear in
`n_requirements`, `n_gaps` or the trace.

```sql
-- Rank, then explain (README quick start)
SELECT candidate_id, round(coverage, 3) AS coverage, n_requirements, n_gaps
FROM tree_coverage('Q1', k := 3, mu := 0.0, nodes_table := 'tree_nodes', attrs_table := 'tree_attributes');
-- S1 1.0 4 0 | S6 1.0 4 0 | S2 0.5 4 2
SELECT req_node_id, attr_name, req_value, prov_value, value_score, round(distance, 3) AS distance
FROM tree_coverage_trace('Q1', candidates := ['S2'], mu := 0.0,
                         nodes_table := 'tree_nodes', attrs_table := 'tree_attributes')
WHERE is_gap
ORDER BY req_node_id, attr_name;
-- Q1.1 current draw  at_most 8 A     9 A    0.0 0.125
-- Q1.1 time to 37 °C at_most 180 s   240 s  0.0 0.333
-- Q1.3 height travel at_least 0.06 m 0.05 m 0.0 0.167
```

Many requests: pass a list, `tree_coverage(['Q1', 'Q2'], ...)`; `rank` is per `query_id`.
Stage 1 alone:

```sql
SELECT candidate_id, round(score, 3) AS score, containment, mode_used
FROM coverage_recall('Q1', k := 3, nodes_table := 'tree_nodes');
-- S2 1.01 1.0 containment | S1 1.009 | S6 1.007
-- => S2 | 1.01 | 1.0 | containment
-- => S1 | 1.009 | 1.0 | containment
-- => S6 | 1.007 | 1.0 | containment
```

Archive without `class_id`, classed through the reviewed vocabulary:

```sql
CREATE TABLE vocabulary_overrides AS
SELECT wording, source, class_id FROM read_csv_auto('test/data/coverage/mini/vocabulary.csv');
CREATE TABLE archive_nodes AS
SELECT * REPLACE (NULL::VARCHAR AS class_id) FROM tree_nodes WHERE tree_id LIKE 'S%';
SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps, reviewed, assignment_used
FROM tree_coverage('Q1', k := 3, nodes_table := 'tree_nodes', attrs_table := 'tree_attributes',
                   candidate_nodes_table := 'archive_nodes', candidate_attrs_table := 'tree_attributes',
                   overrides_table := 'vocabulary_overrides', mu := 0.0);
-- S1 1.0 0 true partition | S6 1.0 0 true partition | S2 0.5 2 true partition
```

**`coverage_kernel`** is the scalar behind both, for custom pipelines. Node fields: `node_id,
parent_id, node_kind, edge_type, class_id, reviewed, count, probs` (MAP), `attrs` (LIST of
STRUCT: `attr_id, name, dimension, unit, lo, hi, bound, category, default_bound, scale,
name_vec`). Options: `labels, assignment, label_weight, mu, partition_gate, count_gate,
name_floor, tol, soft_band, trace, rules`. Types are checked ("mu must be between 0 and 1").

```sql
SELECT r.coverage, r.n_gaps, r.trace[1].rule, r.trace[1].distance
FROM (SELECT coverage_kernel(
  [{'node_id': 'q', 'node_kind': 'item', 'class_id': 'heater', 'reviewed': true, 'count': 2.0,
    'attrs': [{'attr_id': 'a1', 'name': 'current', 'dimension': 'current', 'unit': 'A',
               'lo': 8.0, 'hi': 8.0, 'bound': 'at_most'}]}],
  [{'node_id': 'c', 'node_kind': 'item', 'class_id': 'heater', 'reviewed': true, 'count': 2.0,
    'attrs': [{'attr_id': 'b1', 'name': 'current', 'dimension': 'current', 'unit': 'A',
               'lo': 9.0, 'hi': 9.0}]}],
  {'labels': 'hard', 'mu': 0.0, 'trace': true}) AS r);
-- 0.0 | 1 | at_most | 0.125
```

**Dictionary keys:** an optional `attr_key` column (AAS semanticId / ECLASS IRDI, compared with
`semantic_key`) makes equal keys `name_sim = 1` (`name_rule = 'key'`) and different keys a hard
gate (`rule = 'key_mismatch'`). See `anofox-similarity-aas`.

## Canonical coverage pipeline

```sql
-- 1. Load: request from JSON (any lang), archive from tables (here: the mini fixture)
CREATE OR REPLACE TABLE req_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json('{"tree_id": "RFQ-1", "lang": "en",
  "root": {"node_id": "RFQ-1", "kind": "container", "label": "comfort seat", "children": [
    {"node_id": "RFQ-1.1", "kind": "item", "label": "seat heating", "count": 2, "class_id": "seat_heating",
     "attributes": [{"attr_id": "a1", "name": "current draw", "text": "max. 8 A"}]},
    {"node_id": "RFQ-1.2", "kind": "item", "label": "powered headrest", "count": 1, "class_id": "headrest_powered",
     "attributes": [{"attr_id": "a1", "name": "height travel", "text": "at least 60 mm"}]}]}}')) AS n);
CREATE OR REPLACE TABLE req_attrs AS SELECT unnest(attributes, recursive := true) FROM req_nodes;
CREATE OR REPLACE TABLE prog_nodes AS
SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv') WHERE tree_id LIKE 'S%';
CREATE OR REPLACE TABLE prog_attrs AS
SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv') WHERE tree_id LIKE 'S%';

-- 2. Validate (expect no rows), then 3. index the archive once (rebuild on archive/vocabulary change)
SELECT * FROM validate_trees('prog_nodes', 'prog_attrs') WHERE severity = 'error';
CREATE OR REPLACE TABLE prog_index AS SELECT * FROM tree_class_multiset('prog_nodes', attrs_table := 'prog_attrs');

-- 4. Rank: stage 1 on the index, stage 2 on the top k
SELECT candidate_id, round(coverage, 3) AS coverage, rank, n_gaps
FROM tree_coverage('RFQ-1', k := 5, nodes_table := 'req_nodes', attrs_table := 'req_attrs',
                   candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
                   candidate_multiset_table := 'prog_index', mu := 0.0);
-- S3 1.0 | S1 1.0 | S6 1.0 | S2 0.0 | S4 0.0   (S2: 9 A heater fails max. 8 A; 50 mm travel fails at least 60 mm)

-- 5. Explain the best candidate's gaps (same parameters as the ranking)
SET VARIABLE top = (SELECT candidate_id FROM tree_coverage('RFQ-1', k := 5,
                    nodes_table := 'req_nodes', attrs_table := 'req_attrs',
                    candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
                    candidate_multiset_table := 'prog_index', mu := 0.0) WHERE rank = 1);
SELECT req_node_id, matched_node_id, attr_name, req_value, prov_value, value_score, is_gap
FROM tree_coverage_trace('RFQ-1', candidates := [getvariable('top')],
                         nodes_table := 'req_nodes', attrs_table := 'req_attrs',
                         candidate_nodes_table := 'prog_nodes', candidate_attrs_table := 'prog_attrs',
                         mu := 0.0);
-- RFQ-1.1 S3.1 current draw at_most 8 A 7.5 A 1.0 false | RFQ-1.2 S3.3 height travel ... 1.0 false
```

Production additions: classes for unlabelled items via `overrides_table` / `label_table` (build
them with `anofox-similarity-classification-review`), `name_vectors` + `name_floor` for
multilingual attribute names, `default_bounds := 'catalogue'` with `templates_table`, and
`rules_table` for categorical attributes. Measure the ranking (recall@k, threshold at
precision, weight fitting) with `anofox-similarity-evaluation-scale`. Full benchmark:
`examples/coverage_matching/tree_coverage_benchmark.sql`.
