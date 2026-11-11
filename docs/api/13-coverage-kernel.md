# Coverage kernel

> Rank candidates by how much of a request they cover, with gaps and an explanation.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Coverage: tree_coverage, tree_coverage_trace, coverage_recall

```sql
tree_coverage(query_tree_id VARCHAR | VARCHAR[], <named>)
  → TABLE(query_id, candidate_id, coverage, stage1_score, rank_score, rank, n_requirements, n_gaps,
          reviewed, assignment_used)
tree_coverage_trace(query_tree_id, <named>)
  → TABLE(query_id, candidate_id, req_node_id, matched_node_id, matched_path, label_sim, attr_id, attr_name,
          req_value, prov_value, gate, name_sim, value_score, distance, rule, attr_score,
          count_needed, count_available, req_score, is_gap)
coverage_recall(query_tree_id, <named>)
  → TABLE(query_id, candidate_id, score, containment, jaccard, reviewed, mode_used, rank)
```

`n_gaps` counts requirements (items), not attributes; the trace lists the failing attributes.

**Pipeline.** Stage 1 ranks the candidate trees by their class multisets, using containment
(+ `tie_break` × Jaccard) when every candidate tree is reviewed and weighted Jaccard otherwise
(on unreviewed labels containment admits wrong matches). It keeps the top `k` per query. Stage 2 runs the kernel over those candidates:

```
attr(x, y)  = gate · (mu·name_sim + 1 − mu) · value_satisfies(x, y).score     (categorical: compat_score)
a(q, c)     = mean over q's attributes of the best attr(x, ·)                 (1 without attributes)
L(q, c)     = Bhattacharyya(p(q), p(c))   or 0/1 with labels := 'hard'
score(q, c) = label_weight·L + (1 − label_weight)·L·a                         (default label_weight 0)
req(q)      = best score × min(1, Σ counts of compatible items with L ≥ count_gate / count(q))
coverage    = mean req(q);  rank_score = coverage + stage1_weight · stage1_score
```

`assignment := 'partition'` gives each candidate item to the requirement it agrees with most
(ties: the largest remaining count need, then the lowest node id). An item may then not stand
in for another function, and one item serves one requirement. `'best_match'` lets requirements
share items. `'auto'` (default) uses partition when both the query tree and the candidate tree
are reviewed.

**Candidate alternatives (`alternatives`).** A candidate's OR groups (variant groups: module
variants, product configurations) are alternatives of one product. With `'joint'` (default) the
whole request is held to **one alternative per group**: groups are decided outer first, each
taking the alternative with the best coverage given the earlier decisions. A station that needs a
quiet conveyor and a high stack therefore scores like the best single variant, not like a mix of
two variants nobody can build. `'per_requirement'` lets each requirement use its best variant
(the behaviour before AAS-2). The cost is one evaluation per alternative (the sum, not the
product, over groups).

**Named parameters**

| Parameter | Default | Description |
|---|---|---|
| `candidates` | NULL | explicit candidate tree ids; NULL → stage-1 top `k` (the query tree is never its own candidate) |
| `k` | 20 | stage-1 depth |
| `nodes_table`, `attrs_table` | `'tree_nodes'`, `'tree_attributes'` | the query trees; `attrs_table := NULL` for none. A `nodes_table` other than the default needs its `attrs_table` |
| `candidate_nodes_table`, `candidate_attrs_table` | the query tables | the candidate trees; a `candidate_nodes_table` other than the query's needs its `candidate_attrs_table` |
| `overrides_table`, `label_table` | NULL | class resolution (see [property trees](12-property-trees.md)) |
| `labels` | `'soft'` | `'soft'` uses the classifier's probabilities; `'hard'` uses one class |
| `assignment` | `'auto'` | `'partition'`, `'best_match'`, `'auto'` |
| `label_weight` | 0.0 | λ: label credit independent of the values |
| `mu` | 0.2 | weight of the attribute-name similarity (names only discount) |
| `partition_gate`, `count_gate` | 0.3, 0.5 | minimum label agreement for a block / for counting instances |
| `stage1_weight`, `tie_break` | 0.05, 0.01 | stage-1 score in the rank; Jaccard tie-break in stage 1 |
| `recall_mode` (`mode` in `coverage_recall`) | `'auto'` | `'containment'`, `'jaccard'`, `'auto'` |
| `default_bounds`, `templates_table` | NULL, `'catalogue_attributes'` | `'catalogue'`: an unqualified requirement takes the bound of its template with the same class and dimension (none when ambiguous) |
| `rules_table` | NULL | compatibility rules for categorical attributes (no rule: equal → 1) |
| `name_vectors`, `name_floor` | NULL, 0.85 | `(name, vec)`: attribute-name similarity is the cosine calibrated by the floor; without vectors, token overlap. Equal `attr_key`s override both (see below) |
| `alternatives` | `'joint'` | `'joint'`: one alternative per candidate OR group for the whole request; `'per_requirement'`: each requirement picks its own |
| `class_hierarchy`, `generalization_credit` | NULL, 0.0 | `(class_id, parent_class_id)`: a related class (ancestor or descendant at distance d) agrees by `generalization_credit^d` instead of 0 (see below); 0 = off |
| `candidate_multiset_table` | NULL | a precomputed index of the candidate class multisets (`tree_class_multiset` output: `entity_id, feature_id, weight[, reviewed]`). Stage 1 reads it instead of resolving every candidate tree; the results are the same |

**Gaps.** `is_gap` marks a requirement that has no counterpart, too few instances, or a
value below 1. It does not depend on how soft the labels are. A required attribute that the
matched item lacks gives a row with `gate = true`. `distance` is relative for the limit rules,
a share for within and point (also for temperature), and in K for the temperature limit rules.

**Dictionary keys (`attr_key`).** An optional `attr_key` column on the attributes tables
holds a dictionary identifier, such as an AAS semanticId (ECLASS or IEC CDD IRDI, or ECLASS IRI).
Keys are compared after [`semantic_key`](17-aas.md), so the IRDI and IRI forms and different
release suffixes (`#007`, `#008`) are the same key. When **both** attributes carry a key:

- equal keys give `name_sim = 1` (`name_rule = 'key'`);
- different keys are a hard gate, like a dimension mismatch: score 0 and `rule = 'key_mismatch'`
  in the trace, not a `mu` discount;
- if the candidate item has an attribute with the requirement's key, only keyed attributes can
  answer that requirement. An unkeyed attribute of the same dimension does not stand in.

When only one side has a key, or neither, names are compared as before (`name_rule` = `vector`,
`exact` or `tokens`). Without `attr_key`, IRDIs used as attribute names are **not** exact: two
different IRDIs share the tokens `0173`, `1` and `02` and get partial name credit.

```sql
-- the AAS reader fills attr_key; hand-made tables can add it
ALTER TABLE tree_attributes ADD COLUMN attr_key VARCHAR;
UPDATE tree_attributes SET attr_key = name WHERE name LIKE '0173-1#%';
SELECT attr_id, rule, name_rule, attr_score
FROM tree_coverage_trace('RFQ-117', candidates := ['OFF-B']);
```

**Class hierarchy (`class_hierarchy`, `generalization_credit`).** By default classes are flat: a
requirement for hot-plate sealing gets nothing from an offer of heat sealing. With a hierarchy
table `(class_id, parent_class_id)` (for AAS: [`aas_class_hierarchy`](17-aas.md) of an IDTA-02020
GeneralizedBy taxonomy) the hierarchy is an **is-a** relation:

```
rel(required k, provided l) = 1     same class, or l a subclass of k at any distance (specialized)
                            = c^d   l a superclass of k at distance d, up to 32 levels (generalized)
                            = 0     otherwise
L(q, c) = Σ_k,l √(p_k(q) · p_l(c)) · rel(k, l), capped at 1         (hard labels: rel(class, class))
```

A hot-plate sealer is a heat sealer, so it fully meets a heat-sealing requirement. A general
heat sealer for a hot-plate requirement gets `generalization_credit` c (default 0: nothing).
Without a table this is the Bhattacharyya coefficient, so results are unchanged. The trace column
`label_rule` says `exact`, `soft`, `specialized` or `generalized`. A generalized item counts
towards a required count relative to the match: fully when the best match is itself generalized
(its score carries the discount), by L / L(best) next to a closer match, so superclass items cannot
hide a count shortfall. A generalized item must still pass `partition_gate` (0.3, reviewed trees)
and `count_gate` (0.5): with c = 0.5 a parent (0.5) passes, a grandparent (0.25) does not.
Stage-1 recall does not use the hierarchy: pass `candidates :=`, or raise `k`, when the related
offers would not be recalled by class.

**Partition and repeated classes.** The partition first assigns items by class agreement, then
moves or swaps items between requirements of the same class while that raises the summed score.
A request for a 10 W and a 90 W motor against an offer of a 95 W and a 20 W motor is therefore
covered in full, whatever the item ids; one 95 W motor still cannot serve both.

**Recommended setup.** Calibrate the attribute-name floor once per corpus. It is the median
cosine between unrelated names:

```sql
SET VARIABLE name_floor = (SELECT median(list_cosine_similarity(a.vec, b.vec))
                           FROM name_vectors a, name_vectors b WHERE a.name < b.name);
```

**Example: the gap list of the best programme.** The trace takes the same parameters as the
ranking, so it explains the same score. The benchmark's request items have no `class_id`, so
they take their classes from a `label_table` (`request_labels`, the output of
[`classify_to_catalogue`](14-classification-and-review.md#classify_to_catalogue) over `request_item_vectors`).
A request item without a class matches nothing, and a request without any classified item
raises an error naming the fix (see [guide 04](../guides/04-coverage-matching.md)). The trace of
the best candidate for one benchmark request:

```sql
SET VARIABLE top = (SELECT candidate_id FROM tree_coverage('R07', k := 20,
       nodes_table := 'request_nodes', attrs_table := 'request_attributes', label_table := 'request_labels',
       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'catalogue_attributes', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'))
    WHERE rank = 1);

-- is_gap flags the requirement on all its rows; the failing attribute has value_score < 1
-- (or a count shortfall); attr_score also carries the name discount
SELECT req_node_id, attr_name, req_value, prov_value, round(value_score, 2) AS value_score,
       round(distance, 3) AS distance
FROM tree_coverage_trace('R07', candidates := [getvariable('top')],
       nodes_table := 'request_nodes', attrs_table := 'request_attributes', label_table := 'request_labels',
       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes',
       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
       templates_table := 'catalogue_attributes', rules_table := 'compatibility_rules',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'))
WHERE is_gap AND (value_score IS DISTINCT FROM 1 OR count_available < count_needed);
```

**Request alternatives.** A request may list alternatives (OR edges). The best alternative
counts. Every node gives (sum of requirement scores, number of requirements); an OR group
takes the alternative with the best mean; coverage = sum / number. Alternatives that were not
chosen do not appear in `n_requirements`, `n_gaps` or the trace.

**`coverage_kernel(query, candidate, options)`** is the building block behind the table
functions: a scalar over two packed node lists. It is public for custom pipelines, and its
input types are checked.

**Large archives.** Index the archive once and rank against the index:

```sql
CREATE TABLE programme_index AS
SELECT * FROM tree_class_multiset('programme_nodes', overrides_table := 'vocabulary_overrides');

SELECT * FROM tree_coverage('R07', k := 20,
                            nodes_table := 'request_nodes', attrs_table := 'request_attributes',
                            label_table := 'request_labels',
                            candidate_nodes_table := 'programme_nodes',
                            candidate_attrs_table := 'programme_attributes',
                            overrides_table := 'vocabulary_overrides',
                            candidate_multiset_table := 'programme_index');
```

One request against 100 200 trees (2.6 M nodes): stage 1 took 0.12 s and stage 2 0.58 s with the
index, and 1.9 s and 12.9 s without it; building the index took 1.9 s. These are indicative
timings measured once during development (DuckDB 1.5.6, multi-core workstation), not part of
the test suite; `test/sql/coverage/r3/uc_10_index_the_archive_once.test` checks that the index
gives the same results.
Rebuild the index when the archive or the vocabulary changes.

**Input checks.** The `coverage_kernel` scalar checks its argument types, so malformed input
gives an error and does not abort. An empty `candidates` or query list selects nothing.
Label rows are cleaned: duplicates add up, and NULL classes and probabilities ≤ 0 drop out.
Soft distributions are normalised to 1.

**Benchmark** (80 requests at once, reviewed vocabulary, catalogue bounds, name vectors):
recall@20 0.925, top-1 0.925, mean regret 0.009, MAE over the top 20 0.036, unmeetable
flagged 1.0, precision at 0.9 0.985, auto-accept share 82.5 %. Reproduce them with
`examples/coverage_matching/tree_coverage_benchmark.sql` and `examples/evaluation/evaluate_pipeline.sql`;
`test/sql/coverage/r3/uc_06_brd_kpis_on_the_benchmark.test` asserts the floors (recall@20 ≥ 0.90,
top-1 ≥ 0.85, MAE ≤ 0.05, precision at 0.9 ≥ 0.95).

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `tree_coverage`

*Table function*

Stage-2 coverage: ranks candidate trees by the share of the request they cover. Per request item q and candidate item c: score = label_weight*L + (1-label_weight)*L*a, with L the label agreement (Bhattacharyya of soft labels, or 0/1) and a the mean over q's attributes of the best (mu*name_sim + 1-mu)*value_satisfies; counts are summed over compatible items (never across OR alternatives). alternatives 'joint' (default) holds the request to one alternative per candidate OR group (one configuration), 'per_requirement' lets each requirement pick its own. class_hierarchy (class_id, parent_class_id) makes a subclass offer meet a superclass requirement; generalization_credit c gives a superclass offer c^distance. assignment 'partition' gives each candidate item one requirement block (no substitution), 'best_match' lets requirements share, 'auto' uses partition when both the query and the candidate trees are reviewed. Returns query_id, candidate_id, coverage, stage1_score, rank_score (coverage + stage1_weight x stage1_score), rank, n_requirements, n_gaps, reviewed, assignment_used. default_bounds := 'catalogue' gives unqualified values the bound of their template (templates_table). tol and soft_band (default 0.02, 0.05) are passed to value_satisfies; use 0 and 0 for hard limits. query_tree_id is a tree id or a LIST of ids. Trees come from nodes_table/attrs_table (default 'tree_nodes'/'tree_attributes'); candidates from candidate_nodes_table/candidate_attrs_table (default: the same tables). Naming a nodes table without its attributes table is an error (pass NULL for none). Classes resolve class_id -> overrides_table (reviewed vocabulary) -> label_table (classifier). Stage 1 ranks candidates by containment (+ tie_break x jaccard) when every candidate tree is reviewed, else by weighted jaccard (mode 'auto'). candidate_multiset_table: a precomputed index of the candidate class multisets (tree_class_multiset output: entity_id, feature_id, weight[, reviewed]) makes stage 1 skip resolving every candidate tree.

```sql
tree_coverage(query_tree_id, candidates, k, nodes_table, candidate_nodes_table, overrides_table, label_table, recall_mode, tie_break, candidate_multiset_table, attrs_table, candidate_attrs_table, labels, assignment, label_weight, mu, partition_gate, count_gate, stage1_weight, default_bounds, templates_table, rules_table, name_vectors, name_floor, tol, soft_band, class_hierarchy, generalization_credit, alternatives)
```

**Examples**

```sql
SELECT * FROM tree_coverage('R01', k := 20, candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes');
SELECT * FROM tree_coverage(['R01', 'R02'], candidates := ['P001', 'P002']);
```

### `tree_coverage_trace`

*Table function*

The explanation of tree_coverage: one row per request item and required attribute with the matched candidate item and path, label_sim, req_value, prov_value, gate, name_sim, value_score, distance, rule, attr_score, count_needed, count_available, req_score, is_gap (no counterpart, a count shortfall or a value below 1), name_rule (how the names were compared: key when both attributes carry the same attr_key, vector, exact or tokens), bound_source (where the requirement's bound came from: explicit, text, range, level_type, constraint or template) and label_rule (exact, soft, specialized: a subclass was offered, generalized: a superclass was offered, see class_hierarchy). rule is the value rule; gate for a dimension or unit mismatch; missing when the offer states the property without a value; key_mismatch when both attributes carry different attr_keys. Same parameters as tree_coverage. query_tree_id is a tree id or a LIST of ids. Trees come from nodes_table/attrs_table (default 'tree_nodes'/'tree_attributes'); candidates from candidate_nodes_table/candidate_attrs_table (default: the same tables). Naming a nodes table without its attributes table is an error (pass NULL for none). Classes resolve class_id -> overrides_table (reviewed vocabulary) -> label_table (classifier). Stage 1 ranks candidates by containment (+ tie_break x jaccard) when every candidate tree is reviewed, else by weighted jaccard (mode 'auto'). candidate_multiset_table: a precomputed index of the candidate class multisets (tree_class_multiset output: entity_id, feature_id, weight[, reviewed]) makes stage 1 skip resolving every candidate tree.

```sql
tree_coverage_trace(query_tree_id, candidates, k, nodes_table, candidate_nodes_table, overrides_table, label_table, recall_mode, tie_break, candidate_multiset_table, attrs_table, candidate_attrs_table, labels, assignment, label_weight, mu, partition_gate, count_gate, stage1_weight, default_bounds, templates_table, rules_table, name_vectors, name_floor, tol, soft_band, class_hierarchy, generalization_credit, alternatives)
```

**Examples**

```sql
SELECT * FROM tree_coverage_trace('R01', candidates := ['P001']) WHERE is_gap;
```

### `coverage_recall`

*Table function*

Stage-1 recall over class multisets: the top-k candidate trees per query with score, containment, jaccard, reviewed, mode_used and rank. mode 'auto' uses containment when every candidate tree is reviewed and weighted jaccard otherwise. query_tree_id is a tree id or a LIST of ids. Trees come from nodes_table/attrs_table (default 'tree_nodes'/'tree_attributes'); candidates from candidate_nodes_table/candidate_attrs_table (default: the same tables). Naming a nodes table without its attributes table is an error (pass NULL for none). Classes resolve class_id -> overrides_table (reviewed vocabulary) -> label_table (classifier). Stage 1 ranks candidates by containment (+ tie_break x jaccard) when every candidate tree is reviewed, else by weighted jaccard (mode 'auto'). candidate_multiset_table: a precomputed index of the candidate class multisets (tree_class_multiset output: entity_id, feature_id, weight[, reviewed]) makes stage 1 skip resolving every candidate tree.

```sql
coverage_recall(query_tree_id, candidates, k, nodes_table, candidate_nodes_table, overrides_table, label_table, mode, tie_break, candidate_multiset_table)
```

**Examples**

```sql
SELECT * FROM coverage_recall('R01', k := 20, candidate_nodes_table := 'programme_nodes');
```

### `coverage_kernel`

*Scalar function*

The stage-2 coverage kernel over two packed trees (LIST of STRUCT(node_id, parent_id, node_kind, edge_type, class_id, reviewed, count, probs MAP, attrs LIST(STRUCT(attr_id, name, dimension, unit, lo, hi, bound, category, default_bound, scale, name_vec)))) with an options STRUCT (labels, assignment, label_weight, mu, partition_gate, count_gate, name_floor, tol, soft_band, trace, rules). Returns STRUCT(coverage, n_requirements, n_gaps, reviewed, assignment_used, trace). tree_coverage and tree_coverage_trace build its inputs; use it directly for custom pipelines.

```sql
coverage_kernel(query ANY, candidate ANY, options ANY) → STRUCT(coverage DOUBLE, n_requirements BIGINT, n_gaps BIGINT, reviewed BOOLEAN, assignment_used VARCHAR, trace STRUCT(req_node_id VARCHAR, matched_node_id VARCHAR, matched_path VARCHAR, label_sim DOUBLE, attr_id VARCHAR, attr_name VARCHAR, req_value VARCHAR, prov_value VARCHAR, gate BOOLEAN, name_sim DOUBLE, value_score DOUBLE, distance DOUBLE, "rule" VARCHAR, attr_score DOUBLE, count_needed DOUBLE, count_available DOUBLE, req_score DOUBLE, is_gap BOOLEAN, name_rule VARCHAR, bound_source VARCHAR, label_rule VARCHAR)[])
```

**Examples**

```sql
SELECT coverage_kernel(q.nodes, c.nodes, struct_pack(labels := 'hard')).coverage FROM q, c;
```

<!-- generated:functions:end -->
