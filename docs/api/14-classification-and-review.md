# Classification and vocabulary review

> Map items onto a catalogue and review the wordings that matter.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Pivot classifier and vocabulary review

The pivot classifier maps items onto the catalogue as a distribution over classes. The review
functions turn its uncertainty into a short work list. The extension ships no
models: bring your own vectors.

| Function | Purpose |
|---|---|
| `maxsim(query_tokens, document_tokens)` | Late-interaction score: mean over query tokens of the best cosine over the document tokens |
| `bhattacharyya(p MAP, q MAP)` | Σ √(p_k q_k): the soft label agreement used by the kernel |
| `calibrated_cosine(a, b, floor)` | clip((cos − floor) / (1 − floor), 0, 1) |
| `cosine_floor(table, column [, sample := 10000, seed := 7])` | `(floor, n_pairs)`: median cosine of distinct pairs. Exact when all pairs fit into `sample`, otherwise a seeded sample |
| `classify_to_catalogue(...)` | `(tree_id, node_id, source, class_id, p, rank, margin, overridden)`, one row per item and class |
| `vocabulary_review_queue(trees_table, decisions [, sources, overrides_table])` | `(wording, source, n_occurrences, n_trees_affected, top1, p1, top2, p2, margin, impact)`, highest impact first |
| `apply_vocabulary_review(review_table [, overrides_table])` | The merged overrides; the review wins per wording and source |
| `uncertain_decisions(decisions [, threshold \| share, top, texts_table, catalogue_table])` | `(tree_id, node_id, margin, request)` with `request` as JSON text |

**`classify_to_catalogue` parameters** (all named)

| Parameter | Default | Description |
|---|---|---|
| `items_table` | `'item_vectors'` | `node_id, vec[, tree_id, source, label, tokens]`; items without a vector are skipped |
| `catalogue_table` | `'catalogue_vectors'` | `class_id, vec[, exemplar_tokens]` |
| `mode` | `'centroid'` | `'centroid'`: cosine to the class vector; `'late_interaction'`: `maxsim(tokens, exemplar_tokens)` |
| `center_by_source` | true | subtract the mean vector of the item's source (centroid mode) |
| `attr_bonus` | 1.0 | β × the attribute-template match: mean over the item's attributes of the best same-dimension template |
| `temperature` | 0.3 | softmax temperature τ |
| `overrides_table` | NULL | reviewed wordings `(wording, source, class_id)`, matched on the item's `label` |
| `answers_table` | NULL | per-item answers `([tree_id,] node_id, class_id)`; they beat overrides |
| `attrs_table`, `templates_table` | NULL | `([tree_id,] node_id, name, dimension)` and `(class_id, dimension[, name, name_en, name_de])` for the bonus |
| `name_vectors`, `name_floor` | NULL, 0.85 | name similarity for the bonus (calibrated cosine); without vectors a dimension match counts 1 |
| `top` | NULL | keep the best `top` classes per item |

Pipeline per item: centring → similarity → z-score across the catalogue (+ β × bonus) →
softmax(score / τ) → `margin = p1 − p2`. Overridden items get a one-hot `p` and
`overridden = true`. The output is a `label_table` for `tree_coverage`.

**Workflow: review the archive's vocabulary** (runs on the benchmark files)

```sql
CREATE TABLE catalogue AS SELECT class_id, vec::FLOAT[] AS vec, exemplar_tokens::FLOAT[][] AS exemplar_tokens
FROM read_csv_auto('catalogue_vectors.csv.gz');
CREATE TABLE programme_nodes AS SELECT * FROM read_csv_auto('programme_nodes.csv.gz');
CREATE TABLE wordings AS SELECT wording AS node_id, wording AS label, source, vec::FLOAT[] AS vec
FROM read_csv_auto('vocabulary_vectors.csv.gz');

-- classify every wording once; the archive's items take the distribution of their wording
CREATE TABLE wording_decisions AS SELECT * FROM classify_to_catalogue(items_table := 'wordings', catalogue_table := 'catalogue');
CREATE TABLE programme_labels AS
SELECT n.tree_id, n.node_id, d.class_id, d.p
FROM programme_nodes n JOIN wording_decisions d ON d.node_id = n.label AND d.source = n.source
WHERE n.node_kind = 'item';

-- the few wordings that touch most trees, least certain first
SELECT wording, top1, top2, round(margin, 3) AS margin, n_trees_affected, round(impact, 1) AS impact
FROM vocabulary_review_queue(trees_table := 'programme_nodes', decisions := 'wording_decisions') LIMIT 10;

-- the reviewer's answers (wording, source, class_id[, reviewed_by, reviewed_at]); store the merged
-- overrides (table functions cannot write). Without overrides_table, the review merges into an
-- existing vocabulary_overrides table, or starts one.
CREATE TABLE review AS SELECT * FROM (VALUES ('Klimapaket Heizung', 'internal', 'seat_ventilation'))
    t(wording, source, class_id);
CREATE OR REPLACE TABLE vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review');

-- rank with the reviewed wordings; the rest keep the classifier's soft labels. The request
-- items need classes too: one label_table serves both sides
CREATE TABLE request_nodes AS SELECT * FROM read_csv_auto('request_nodes.csv.gz');
CREATE TABLE request_items AS SELECT tree_id, node_id, source, vec::FLOAT[] AS vec
FROM read_csv_auto('request_item_vectors.csv.gz');
CREATE TABLE labels AS
SELECT tree_id, node_id, class_id, p FROM programme_labels
UNION ALL
SELECT tree_id, node_id, class_id, p
FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue');
SELECT * FROM tree_coverage('R12', k := 20, nodes_table := 'request_nodes', attrs_table := NULL,
       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := NULL,
       overrides_table := 'vocabulary_overrides', label_table := 'labels');
```

**Workflow: send uncertain items to an external reviewer**

```sql
CREATE TABLE decided AS SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
       attrs_table := 'request_attributes', templates_table := 'catalogue_attributes',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'));
-- the item texts the reviewer sees: (tree_id, node_id, text)
CREATE TABLE item_texts AS SELECT tree_id, node_id, label AS text FROM request_nodes WHERE node_kind = 'item';
-- one JSON object per row, ready for an LLM or a colleague
SELECT request FROM uncertain_decisions(decisions := 'decided', share := 0.15,
       texts_table := 'item_texts', catalogue_table := 'catalogue_classes');
-- write the answers into a table answers(tree_id, node_id, class_id) and classify again
CREATE TABLE request_labels AS
SELECT tree_id, node_id, class_id, p FROM classify_to_catalogue(items_table := 'request_items',
       catalogue_table := 'catalogue', attrs_table := 'request_attributes', templates_table := 'catalogue_attributes',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'), answers_table := 'answers');
SELECT * FROM tree_coverage('R07', k := 20, nodes_table := 'request_nodes', attrs_table := 'request_attributes',
       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes',
       overrides_table := 'vocabulary_overrides', label_table := 'request_labels');
```

`share` selects the lowest-margin share of the items that are not overridden (ceil(share × n)).
A tree becomes *reviewed* only through `class_id` or the reviewed vocabulary, not through a
`label_table`. Once every item of a request is confirmed, write the classes into its nodes to
let `auto` partition:

```sql
CREATE TABLE request_nodes_reviewed AS
SELECT n.* REPLACE (COALESCE(n.class_id, l.class_id) AS class_id)
FROM request_nodes n LEFT JOIN (SELECT * FROM request_labels WHERE p = 1.0) l USING (tree_id, node_id);
```

**Benchmark.** On request items, top-1 class accuracy is 0.59 with raw cosine, 0.85 after
per-source centring and 0.95 with the attribute bonus. On 200 unseen phrasings, late
interaction beats the centroid by more than 10 points. The review queue puts the four
misleading archive wordings first. With classifier labels on the request side, the pipeline
reaches recall@20 0.86 and regret 0.022. After a reviewer answers the 15 % lowest-margin items,
it reaches recall@20 0.91, top-1 0.91, regret 0.0099 and MAE 0.035, which meets all BRD
targets.

**`assignment := 'auto'`** partitions only when both the query tree and the candidate tree
are reviewed. Classifier labels are not reviewed, so `auto` uses best match. The
`reviewed` output column of `tree_coverage` describes the candidate tree; `assignment_used`
shows the decision.

**Input checks.** `classify_to_catalogue` rejects an override or decision whose class is not in
the catalogue, duplicate class ids or items, an empty catalogue, NULL node ids and non-finite
vector elements. `apply_vocabulary_review` keeps the columns wording, source, class_id,
reviewed_by and reviewed_at; JSON text escapes quotes, backslashes, \n, \r, \t, \b and \f and
drops other control characters.

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `classify_to_catalogue`

*Table function*

Pivot classifier: maps every item onto the catalogue as a distribution p(class). Steps: per-source mean-centring of the item vectors (center_by_source, centroid mode; sources with fewer than min_source_items items, default 10, use the batch mean); cosine to the class vector, or maxsim(tokens, exemplar_tokens) in mode 'late_interaction'; z-score across the catalogue; plus attr_bonus x the attribute-template match (mean over the item's attributes of the best same-dimension template, name similarity by calibrated cosine of name_vectors, or 1 without vectors); softmax with temperature; margin = p1 - p2. Reviewed wordings (overrides_table: wording, source, class_id; matched on the item's label) and per-item decisions (answers_table: tree_id, node_id, class_id) give a one-hot p and overridden = true. items_table: node_id, vec[, tree_id, source, label, tokens]; catalogue_table: class_id, vec[, exemplar_tokens]; attrs_table: [tree_id,] node_id, name, dimension; templates_table: class_id, dimension[, name, name_en, name_de]. Returns tree_id, node_id, source, class_id, p, rank, margin, overridden; tree_id, node_id, class_id and p make it a label_table for tree_coverage.

```sql
classify_to_catalogue(items_table, catalogue_table, mode, center_by_source, attr_bonus, temperature, overrides_table, answers_table, attrs_table, templates_table, name_vectors, name_floor, top, min_source_items)
```

**Examples**

```sql
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue', top := 3);
SELECT * FROM classify_to_catalogue(items_table := 'unseen', catalogue_table := 'catalogue', mode := 'late_interaction', top := 1);
```

### `maxsim`

*Scalar function*

Late-interaction score (ColBERT-style MaxSim): each query token takes its maximum cosine over the document tokens, and the maxima are averaged. Asymmetric. Token vectors must share one dimension; a zero token has cosine 0. NULL or an empty list gives NULL.

```sql
maxsim(query_tokens DOUBLE[][], document_tokens DOUBLE[][]) → DOUBLE
```

**Examples**

```sql
SELECT maxsim([[1.0, 0.0], [0.0, 1.0]], [[1.0, 0.0], [0.6, 0.8]]); -- 0.9
```

### `bhattacharyya`

*Scalar function*

Bhattacharyya coefficient of two class distributions MAP(class, probability): Σ √(p_k q_k). 1 for identical distributions, 0 for disjoint ones; the soft label agreement L(q, c) of the coverage kernel. Distributions are used as given (tree_coverage normalises them). Negative values are an error; NULL gives NULL.

```sql
bhattacharyya(p MAP(VARCHAR, DOUBLE), q MAP(VARCHAR, DOUBLE)) → DOUBLE
```

**Examples**

```sql
SELECT bhattacharyya(MAP {'heating': 0.6, 'ventilation': 0.4}, MAP {'heating': 0.5, 'ventilation': 0.5}); -- 0.9949
```

### `calibrated_cosine`

*Scalar function*

Cosine rescaled above a corpus floor: clip((cos - floor) / (1 - floor), 0, 1). Small sentence encoders give unrelated texts a cosine around 0.85; estimate the floor with cosine_floor. floor must be in [0, 1); NULL gives NULL.

```sql
calibrated_cosine(a DOUBLE[], b DOUBLE[], floor DOUBLE) → DOUBLE
```

**Examples**

```sql
SELECT calibrated_cosine([1.0, 0.0], [0.8, 0.6], 0.5); -- 0.6
```

### `cosine_floor`

*Table function*

Estimates the cosine floor b of a vector corpus: the median cosine of pairs of distinct vectors. With at most `sample` pairs (default 10 000) all pairs count; otherwise `sample` pairs are drawn deterministically from `seed` (default 7). Returns floor and n_pairs. Use the floor with calibrated_cosine or as name_floor.

```sql
cosine_floor(table, column, sample, seed)
```

**Examples**

```sql
SET VARIABLE name_floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec'));
```

### `vocabulary_review_queue`

*Table function*

The vocabulary review queue: one row per distinct wording and source of the item nodes in trees_table (default 'tree_nodes'), with n_occurrences, n_trees_affected, the classifier's top1/p1 and top2/p2, margin and impact = n_trees_affected x (1 - margin), highest impact first. decisions is a classify_to_catalogue result: per wording (tree_id NULL, node_id = wording) or per node (averaged over the wording's occurrences). Wordings in overrides_table are reviewed and leave the queue; sources restricts the document families.

```sql
vocabulary_review_queue(trees_table, decisions, sources, overrides_table)
```

**Examples**

```sql
SELECT * FROM vocabulary_review_queue(trees_table := 'programme_nodes', decisions := 'wording_decisions', overrides_table := 'vocabulary_overrides') LIMIT 10;
```

### `apply_vocabulary_review`

*Table function*

Applies a review (wording, class_id[, source, reviewed_by, reviewed_at]) to the overrides (default 'vocabulary_overrides' when that table exists; NULL for none): one entry per wording and source, the review wins. Returns the new overrides table; table functions cannot write, so store it: CREATE OR REPLACE TABLE vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review').

```sql
apply_vocabulary_review(review_table, overrides_table)
```

**Examples**

```sql
CREATE OR REPLACE TABLE vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review');
```

### `uncertain_decisions`

*Table function*

The external re-rank hook: the low-margin items of a classify_to_catalogue result (margin < threshold, default 0.2, or the lowest-margin share of the items), overridden items excluded, lowest margin first. request is JSON text for an LLM or a human: tree_id, node_id, text (texts_table: [tree_id,] node_id, text), margin and the top candidates with p, label and definition (catalogue_table: class_id[, label_en|label, definition]). Write the answers back as an answers_table for classify_to_catalogue, or as vocabulary_overrides per wording.

```sql
uncertain_decisions(decisions, threshold, share, top, texts_table, catalogue_table)
```

**Examples**

```sql
SELECT request FROM uncertain_decisions(decisions := 'decided', share := 0.15, texts_table := 'texts', catalogue_table := 'catalogue_classes');
```

<!-- generated:functions:end -->
