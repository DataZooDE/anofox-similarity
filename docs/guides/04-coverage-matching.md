# Coverage Matching

> Does a past solution cover what a new request asks for, and where are the gaps?

**Use this guide to:**
- Understand directional (coverage) matching and how it differs from similarity
- Rank feature multisets with `containment`, the `weighted_*` measures and `find_covering_entities`
- Load property trees from JSON and check them with `validate_trees` and `tree_anchors`
- Rank candidate trees with `tree_coverage` and list the gaps with `tree_coverage_trace`
- Give every item a class, choose the assignment rule and index a large archive

The examples use the hand-made fixture in `test/data/coverage/mini`: requests `Q1`–`Q3`, seat
programmes `S1`–`S6` and modules `M1`–`M5`. Run them from the repository root.

## The Idea

Similarity is symmetric: how alike are A and B? Coverage is directional: does the offer B provide
everything the request A asks for? Extra items in B do not hurt coverage. Missing items do.

```sql
SELECT containment(['heating', 'massage'], ['heating', 'massage', 'track']) AS request_in_offer,
       containment(['heating', 'massage', 'track'], ['heating', 'massage']) AS offer_in_request,
       jaccard_similarity(['heating', 'massage'], ['heating', 'massage', 'track']) AS jaccard;
```

| request_in_offer | offer_in_request | jaccard            |
|-----------------:|-----------------:|-------------------:|
| 1.0              | 0.6666666666666666 | 0.6666666666666666 |

Counts matter too. A request for four massage zones is half covered by two:

```sql
SELECT weighted_containment(MAP {'massage': 4}, MAP {'massage': 2}) AS counts_matter;
```

| counts_matter |
|--------------:|
| 0.5           |

The first argument is always the query (the request), the second the candidate (the offer).

## Stage 1: Rank Feature Multisets

A feature multiset is a table `(entity_id, feature_id, weight)`. Here: the classes of six past
programmes, with counts.

```sql
CREATE TABLE feature_multiset AS
SELECT * FROM read_csv_auto('test/data/coverage/mini/feature_multiset.csv');

SELECT entity_id, round(score, 3) AS score, containment, round(jaccard, 3) AS jaccard, surplus_weight
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 1, 'memory_module': 1}, 4);
```

| entity_id | score | containment | jaccard | surplus_weight |
|-----------|------:|------------:|--------:|---------------:|
| S2        | 1.01  | 1.0         | 1.0     | 0.0            |
| S1        | 1.009 | 1.0         | 0.889   | 1.0            |
| S6        | 1.007 | 1.0         | 0.667   | 4.0            |
| S3        | 0.631 | 0.625       | 0.625   | 0.0            |

Three programmes contain the request completely. The `score` adds 0.01 × Jaccard as a tie-break, so
the leanest full cover comes first. `surplus_weight` is what the candidate brings on top.

- `find_covering_entities('S5', 3)` takes an entity of the table as the query.
- `min_score := 1.0` keeps only full covers.
- `mode := 'jaccard'` or `'tversky'` changes the measure.
- `feature_table`, `entity_col`, `feature_col` and `weight_col` map an existing table, for example
  a BOM: `feature_table := 'bom_items', entity_col := 'parent_id', feature_col := 'child_id', weight_col := 'quantity'`.

The query argument must be a constant: a literal or `getvariable('name')`.

## Property Trees

A request or an offer is a tree. Containers group items. Items carry a class, a count and
attributes. `variant_group` nodes with `OR` edges hold alternatives.

```sql
CREATE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');

SELECT count(*) AS n_errors FROM validate_trees('tree_nodes', 'tree_attributes') WHERE severity = 'error';
```

| n_errors |
|---------:|
| 0        |

`validate_trees` lists structural errors (orphans, cycles, bad counts, ...) and warnings (unknown
units, mixed dimensions). Run it on every new batch.

`tree_anchors` flattens a tree to its items:

```sql
SELECT node_id, class_id, effective_count, anchor_path, variant_path
FROM tree_anchors('tree_nodes', 'tree_attributes', tree_ids := ['M1']);
```

| node_id  | class_id   | effective_count | anchor_path | variant_path      |
|----------|------------|----------------:|-------------|-------------------|
| M1.C1.A1 | seat_track | 1.0             | [M1, M1.C1] | [M1.C1, M1.C1.A1] |
| M1.C1.A2 | seat_track | 1.0             | [M1, M1.C1] | [M1.C1, M1.C1.A2] |
| M1.C2.A1 | seat_track | 1.0             | [M1, M1.C2] | [M1.C2, M1.C2.A1] |

Module `M1` offers a seat track in three variants. Counts multiply down the tree and never add
across `OR` alternatives.

### From JSON

An extraction service usually delivers JSON. `tree_from_json` turns one document into node rows.
Text values are parsed with `parse_quantity` in the document's language:

```sql
CREATE TABLE spec_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json('{"tree_id": "NEW-1", "source": "aurivon", "lang": "de",
  "root": {"node_id": "NEW-1", "kind": "container", "label": "Lastenheft Komfortsitz", "children": [
    {"node_id": "NEW-1.1", "kind": "item", "label": "Sitzheizung 2 Zonen", "count": 2, "class_id": "seat_heating",
     "attributes": [{"attr_id": "a1", "name": "Aufheizzeit auf 37 °C", "text": "max. 3 min"},
                    {"attr_id": "a2", "name": "Stromaufnahme", "text": "höchstens 7,5 A"}]},
    {"node_id": "NEW-1.2", "kind": "item", "label": "Massage 4 Zonen", "count": 4, "class_id": "massage"}]}}')) AS n);
CREATE TABLE spec_attrs AS SELECT unnest(attributes, recursive := true) FROM spec_nodes;

SELECT node_id, name, dimension, lo, unit, bound FROM spec_attrs;
```

| node_id | name                  | dimension | lo    | unit | bound   |
|---------|-----------------------|-----------|------:|------|---------|
| NEW-1.1 | Aufheizzeit auf 37 °C | time      | 180.0 | s    | at_most |
| NEW-1.1 | Stromaufnahme         | current   | 7.5   | A    | at_most |

For many files: `SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM read_text('specs/*.json'))`.
The schema is in [tree.schema.json](../requirements-matching/tree.schema.json).

## Stage 2: Coverage With Gaps

`tree_coverage` ranks candidate trees. Stage 1 picks the top `k` by class multiset. Stage 2 compares
item by item: class, count and every attribute value.

```sql
CREATE TABLE programme_nodes AS SELECT * FROM tree_nodes WHERE tree_id LIKE 'S%';

SELECT candidate_id, round(coverage, 3) AS coverage, rank, n_requirements, n_gaps
FROM tree_coverage('NEW-1', k := 3, nodes_table := 'spec_nodes', attrs_table := 'spec_attrs',
                   candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'tree_attributes',
                   mu := 0.0);
```

| candidate_id | coverage | rank | n_requirements | n_gaps |
|--------------|---------:|-----:|---------------:|-------:|
| S1           | 1.0      | 1    | 2              | 0      |
| S6           | 1.0      | 2    | 2              | 0      |
| S2           | 0.5      | 3    | 2              | 1      |

`candidate_nodes_table` defaults to the query table, so name the archive explicitly. The trace
explains a score. It takes the same parameters:

```sql
SELECT req_node_id, matched_node_id, attr_name, req_value, prov_value,
       round(value_score, 3) AS value_score, round(distance, 3) AS distance
FROM tree_coverage_trace('NEW-1', candidates := ['S2'], nodes_table := 'spec_nodes', attrs_table := 'spec_attrs',
                         candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'tree_attributes',
                         mu := 0.0)
WHERE is_gap;
```

| req_node_id | matched_node_id | attr_name             | req_value     | prov_value | value_score | distance |
|-------------|-----------------|-----------------------|---------------|------------|------------:|---------:|
| NEW-1.1     | S2.1            | Aufheizzeit auf 37 °C | at_most 180 s | 240 s      | 0.0         | 0.333    |
| NEW-1.1     | S2.1            | Stromaufnahme         | at_most 7.5 A | 9 A        | 0.0         | 0.2      |

S2's heating takes 33 % too long and draws 20 % too much current. `is_gap` also marks requirements
without a counterpart (`matched_node_id` NULL) and count shortfalls (`count_available < count_needed`).

### Attribute Names

`mu` (default 0.2) discounts a value match by the similarity of the attribute names. Without
name vectors the similarity is word overlap, so a German offer name never matches an English
request name. With the default, `Q1` scores 0.858 against `S1` with no gaps, and 1.0 with
`mu := 0.0`. Pass `name_vectors` and a calibrated `name_floor` for multilingual data (see
[Classification and review](06-classification-and-review.md)), or set `mu := 0.0`.

## Where Classes Come From

Stage 1 and stage 2 match items by class. Each item gets its class in this order:

1. `class_id` in the nodes table (counts as reviewed).
2. `overrides_table`: the reviewed vocabulary `(wording, source, class_id)`, matched on `label`.
3. `label_table`: classifier output `(tree_id, node_id, class_id, p)`.

A query tree without any classified item is an error:

```sql
CREATE TABLE request_nodes AS SELECT * REPLACE (NULL::VARCHAR AS class_id) FROM tree_nodes WHERE tree_id = 'Q1';
SELECT * FROM tree_coverage('Q1', nodes_table := 'request_nodes', candidate_nodes_table := 'programme_nodes');
```

```text
Invalid Input Error: tree_coverage: query tree 'Q1' has no classified items (give class_id, overrides_table or label_table)
```

Candidate items without a class match nothing, so such candidates do not appear. Give the archive
its classes through the reviewed vocabulary:

```sql
CREATE TABLE archive_nodes AS SELECT * REPLACE (NULL::VARCHAR AS class_id) FROM programme_nodes;
CREATE TABLE vocabulary_overrides AS
SELECT wording, source, class_id FROM read_csv_auto('test/data/coverage/mini/vocabulary.csv');

SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps, reviewed, assignment_used
FROM tree_coverage('Q1', k := 3, candidate_nodes_table := 'archive_nodes', candidate_attrs_table := 'tree_attributes',
                   overrides_table := 'vocabulary_overrides', mu := 0.0);
```

| candidate_id | coverage | n_gaps | reviewed | assignment_used |
|--------------|---------:|-------:|----------|-----------------|
| S1           | 1.0      | 0      | true     | partition       |
| S6           | 1.0      | 0      | true     | partition       |
| S2           | 0.5      | 2      | true     | partition       |

The same `overrides_table` and `label_table` apply to the query and the candidate trees.
[Classification and review](06-classification-and-review.md) shows how to build both.

## Assignment

`assignment` decides how offer items are given to requirements:

| Value | Rule | Use when |
|---|---|---|
| `'partition'` | Each offer item serves one requirement of its own class. No substitution. | Both sides are reviewed. |
| `'best_match'` | Requirements may share offer items. | Classes come from a classifier. |
| `'auto'` (default) | `partition` when both trees are reviewed, else `best_match`. | Almost always. |

A tree is reviewed when every item got its class from `class_id` or the vocabulary. Classifier
labels are not reviewed. `assignment_used` shows the decision for each row.

## Large Archives

Stage 1 resolves the classes of every candidate tree on each call. Index the archive once:

```sql
CREATE TABLE programme_index AS SELECT * FROM tree_class_multiset('programme_nodes');

SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('NEW-1', k := 3, nodes_table := 'spec_nodes', attrs_table := 'spec_attrs',
                   candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'tree_attributes',
                   mu := 0.0, candidate_multiset_table := 'programme_index');
```

The result is the same as without the index. On 100 000 archive trees, stage 1 drops from about 2 s
to 0.1 s. Rebuild the index when the archive or the vocabulary changes. Pass the same
`overrides_table` to `tree_class_multiset` that you use for ranking.

## Pitfalls

| Pitfall | Solution |
|---|---|
| Empty result | `candidate_nodes_table` defaults to the query table. Name the archive. |
| "has no classified items" | Give `class_id`, `overrides_table` or `label_table`. |
| Coverage below 1 without gaps | Attribute-name discount. Use `name_vectors` or `mu := 0.0`. |
| A nearly full cover outranks a full one | The tie-break is additive. Filter with `min_score := 1.0` in stage 1. |
| Many requests at once | `tree_coverage` accepts a list of ids: `tree_coverage(['R01', 'R02'], ...)`. |
| Column parameter in a lateral join | Table functions take constants only. Use a list of ids or `getvariable`. |

## Next Steps

- [Quantities and specs](05-quantities-and-specs.md): how values are parsed and compared
- [Classification and review](06-classification-and-review.md): classes for unlabelled items
- [Evaluation and tuning](07-evaluation-and-tuning.md): measure the ranking
- API: [Covering search](../api/10-covering-search.md), [Property trees](../api/12-property-trees.md), [Coverage kernel](../api/13-coverage-kernel.md)
- [Examples](../../examples/coverage_matching)
