# Covering search

> Which entities contain a request: directional search over feature multisets.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Coverage Matching

Asymmetric, explainable matching of a **request** (a specification, a BOM to source, a
profile) against **offers** (products, past solutions, modules). It answers "does B
satisfy what A asks for?", where the similarity functions above answer "how alike are A
and B?". Design: [requirements-matching/HLD.md](requirements-matching/HLD.md) (§11 lists
the amendments decided during implementation). Test dataset:
[test/data/coverage](../test/data/coverage/README.md).

On DuckDB 1.5.6, named arguments of **scalar** functions bind by position (the table functions
bind by name on both lines). For `tversky_similarity` and `weighted_tversky`, pass both `alpha`
and `beta`, `alpha` first: on 1.5.6 `(…, beta := 0.0, alpha := 1.0)` silently swaps them, and
passing only one of them does not bind.

### find_covering_entities / find_covering_entities_for

Stage-1 recall: rank the entities of a generic `(entity, feature, weight)` table by how well
they cover a query. `find_covering_entities` takes an entity of the table as the query;
`find_covering_entities_for` takes a `MAP(feature, weight)`, e.g. the class multiset of a new
specification that is not in the table yet.

**Signature:**
```sql
find_covering_entities(query_entity_id VARCHAR, k BIGINT, <named>)
find_covering_entities_for(query MAP, k BIGINT, <named>)
  → TABLE(entity_id VARCHAR, score DOUBLE, containment DOUBLE, jaccard DOUBLE,
          shared_weight DOUBLE, query_weight DOUBLE, candidate_weight DOUBLE, surplus_weight DOUBLE)
```

**Named parameters:**
| Parameter | Default | Description |
|---|---|---|
| `mode` | `'containment'` | `'containment'` (Σmin/Σq), `'jaccard'` (Σmin/Σmax) or `'tversky'`; case-insensitive |
| `alpha`, `beta` | 1.0, 0.0 | Tversky weights of the missing and the surplus weight; used in mode `'tversky'` only (ignored otherwise) |
| `weighted` | true | false: every distinct (entity, feature) weighs 1 |
| `tie_break` | 0.01 | weight of Jaccard added to the score in modes `containment` and `tversky`; ignored in mode `'jaccard'` |
| `min_score` | 0.0 | threshold on the measure itself (before the tie-break) |
| `feature_table` | `'feature_multiset'` | table or view, optionally schema-qualified |
| `entity_col`, `feature_col`, `weight_col` | `'entity_id'`, `'feature_id'`, `'weight'` | column mapping, so `bom_items(parent_id, child_id, quantity)` works as is; `weight_col := NULL` means unit weights |

**Returns:** `score` is the ranking key: the measure plus `tie_break × jaccard` (jaccard in mode
`'jaccard'`). It can exceed 1. `containment` and `jaccard` are the raw measures. `shared_weight`
= Σmin, `query_weight` = Σq, `candidate_weight` = Σc, `surplus_weight` = Σc − Σmin (what the
candidate brings on top). Rows are ordered by `score DESC, entity_id`. Only candidates that
share at least one feature with the query are returned, and the query entity itself is
excluded.

**Semantics:** weights of repeated (entity, feature) rows add up; a NULL weight counts as 1;
rows with a NULL entity or feature are ignored. A negative or non-finite weight anywhere in
the table (also on rows that are ignored) raises an error that names the row. Features
compare by their text form. `find_covering_entities`: an unknown id returns no rows, a NULL
id is an error. `find_covering_entities_for`: an empty, NULL, all-zero or unknown query
returns no rows; zero-weight entries of the query are dropped.

**Known limitation:** the tie-break is additive, so a nearly
complete partial cover can outrank a full but much larger cover (containment 0.995 with
jaccard 0.995 scores 1.00495; a full cover with jaccard 0.2 scores 1.002). Filter with
`min_score := 1.0` when only full covers count.

**Read-only databases:** the SQL is inlined, so both functions work where macros cannot be
created. The query argument must be constant: a literal, a parameter, or
`getvariable('name')`.

**Performance:** an inverted join on the feature. 100 000 entities with 15 features each
(1.5 M rows) take about 0.15–0.2 s per query on a laptop.

**Examples:**
```sql
-- Which past programmes contain everything a new request asks for, leanest first?
SELECT entity_id, round(score, 3), round(jaccard, 3)
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 1, 'memory_module': 1}, 3,
       feature_table := 'prog_classes');

-- An existing BOM table, quantities included
SELECT * FROM find_covering_entities('F-100', 5, feature_table := 'frame_bom',
       entity_col := 'frame_no', feature_col := 'part_no', weight_col := 'quantity_per');

-- A request tree turned into a class multiset (repeated classes add up)
SET VARIABLE request = (
    SELECT map_from_entries(list((class_id, n)))
    FROM (SELECT class_id, sum(count)::DOUBLE AS n FROM spec_nodes
          WHERE node_kind = 'item' GROUP BY class_id));
SELECT * FROM find_covering_entities_for(getvariable('request'), 20);
```

**Many requests at once.** A table function takes one constant query, and `LATERAL` with a
column argument is not supported. To rank the archive for many requests, build one map per
request and per candidate and score all pairs with the scalars:

```sql
CREATE TABLE requests AS
SELECT request_id, map_from_entries(list((class_id, n))) AS classes
FROM (SELECT request_id, class_id, sum(count)::DOUBLE AS n FROM request_items GROUP BY ALL)
GROUP BY request_id;
CREATE TABLE candidates AS
SELECT entity_id, map_from_entries(list((feature_id, weight))) AS classes
FROM feature_multiset GROUP BY entity_id;

SELECT r.request_id, c.entity_id,
       weighted_containment(r.classes, c.classes) + 0.01 * weighted_jaccard(r.classes, c.classes) AS score
FROM requests r, candidates c
QUALIFY row_number() OVER (PARTITION BY r.request_id ORDER BY score DESC, c.entity_id) <= 20;
```

The scores equal those of `find_covering_entities_for`. The scalars process about 10⁵ pairs in
0.15 s.

**Choosing the measure:** containment is the right question once the
candidates' labels are reliable (reviewed vocabulary). On unreviewed labels a mislabelled
item can make a candidate look complete, and weighted Jaccard recalls the best candidate more
often (on the benchmark, recall@20 is 0.55 for containment vs 0.64 for Jaccard; with reviewed
labels it is 0.925 vs 0.85).

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `find_covering_entities`

*Table function*

Ranks the entities of an (entity, feature, weight) table by how well they cover a query entity of the same table: weighted containment Σmin/Σq by default, so a richer candidate is not penalised, with weighted Jaccard as tie-break so the leanest full cover comes first. Returns entity_id, score, containment, jaccard, shared_weight, query_weight, candidate_weight, surplus_weight. Named parameters: mode ('containment' | 'jaccard' | 'tversky'), alpha, beta (Tversky weights, used in mode 'tversky' only), weighted (use the weight column, default true), tie_break (weight of Jaccard added to the score in modes 'containment' and 'tversky', default 0.01), min_score (threshold on the measure itself), feature_table (default 'feature_multiset'), entity_col, feature_col, weight_col (NULL for unit weights).

```sql
find_covering_entities(query_entity_id, k, mode, alpha, beta, weighted, tie_break, min_score, feature_table, entity_col, feature_col, weight_col)
```

**Examples**

```sql
SELECT * FROM find_covering_entities('P-100', 10);
SELECT * FROM find_covering_entities('F-100', 5, feature_table := 'frame_bom', entity_col := 'frame_no', feature_col := 'part_no', weight_col := 'quantity_per');
```

### `find_covering_entities_for`

*Table function*

Like find_covering_entities, for a query that is not in the table: query is a MAP(feature, weight), e.g. the class multiset of a new specification. The argument must be constant (a literal or getvariable(...)). Named parameters: mode ('containment' | 'jaccard' | 'tversky'), alpha, beta (Tversky weights, used in mode 'tversky' only), weighted (use the weight column, default true), tie_break (weight of Jaccard added to the score in modes 'containment' and 'tversky', default 0.01), min_score (threshold on the measure itself), feature_table (default 'feature_multiset'), entity_col, feature_col, weight_col (NULL for unit weights).

```sql
find_covering_entities_for(query, k, mode, alpha, beta, weighted, tie_break, min_score, feature_table, entity_col, feature_col, weight_col)
```

**Examples**

```sql
SELECT * FROM find_covering_entities_for(MAP {'seat_heating': 2, 'massage': 4}, 10);
SELECT * FROM find_covering_entities_for(getvariable('request'), 20, mode := 'jaccard');
```

<!-- generated:functions:end -->
