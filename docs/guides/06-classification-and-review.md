# Classification and Review

> Map free-text items onto your catalogue with your own vectors, and review only what matters.

**Use this guide to:**
- Classify items with `classify_to_catalogue` using vectors you compute yourself
- Use centring, late interaction, the attribute bonus, the temperature and the margin
- Review the archive vocabulary with `vocabulary_review_queue` and `apply_vocabulary_review`
- Send uncertain items to a reviewer as JSON with `uncertain_decisions` and read the answers back
- Feed the labels into `tree_coverage`

Coverage matching compares items by class (see [Coverage matching](04-coverage-matching.md)).
Specifications and old archives rarely carry classes. This guide gives them classes.

## Bring Your Own Vectors

The extension ships no language model. Embed the item texts and the catalogue classes with any
model, store the vectors as `FLOAT[]`, and pass the table names.

| Table | Columns |
|---|---|
| items | `node_id, vec[, tree_id, source, label, tokens]` |
| catalogue | `class_id, vec[, exemplar_tokens]` |

A small example with three-dimensional vectors:

```sql
CREATE TABLE catalogue AS SELECT * FROM (VALUES
  ('pump',  [1.0, 0.0, 0.0]::FLOAT[]),
  ('motor', [0.0, 1.0, 0.0]::FLOAT[]),
  ('valve', [0.0, 0.0, 1.0]::FLOAT[])) t(class_id, vec);
CREATE TABLE items AS SELECT * FROM (VALUES
  ('I1', 'Kreiselpumpe 3 kW', [0.9, 0.3, 0.1]::FLOAT[]),
  ('I2', 'Drehstrommotor',    [0.2, 0.9, 0.1]::FLOAT[]),
  ('I3', 'Pumpe mit Motor',   [0.6, 0.6, 0.0]::FLOAT[])) t(node_id, label, vec);

SELECT node_id, class_id, round(p, 3) AS p, rank, round(margin, 3) AS margin
FROM classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue', top := 2)
ORDER BY node_id, rank;
```

| node_id | class_id | p     | rank | margin |
|---------|----------|------:|-----:|-------:|
| I1      | pump     | 0.997 | 1    | 0.994  |
| I1      | motor    | 0.003 | 2    | 0.994  |
| I2      | motor    | 0.998 | 1    | 0.997  |
| I2      | pump     | 0.001 | 2    | 0.997  |
| I3      | motor    | 0.5   | 1    | 0.0    |
| I3      | pump     | 0.5   | 2    | 0.0    |

The output is a distribution over classes per item. `margin` is p1 − p2. A margin near 0 means
"ask someone". The output columns `tree_id, node_id, class_id, p` make it a `label_table` for
`tree_coverage`.

## The Pipeline

Per item: centring → similarity → z-score across the catalogue (+ attribute bonus) → softmax with
the temperature → margin.

| Parameter | Default | Effect |
|---|---|---|
| `center_by_source` | true | Subtracts the mean vector of the item's source. Removes a shared sentence frame ("Der Lieferant liefert ..."). Sources with fewer than `min_source_items` (default 10) items use the batch mean; a small batch is not centred. |
| `mode` | `'centroid'` | `'late_interaction'` scores token vectors (`tokens` against `exemplar_tokens`) with `maxsim`. Better for wordings the exemplars have not seen. |
| `attr_bonus` | 1.0 | Adds evidence from the item's attributes: a "current draw" fits a heater better than a frame. Needs `attrs_table` and `templates_table`. |
| `temperature` | 0.3 | Lower values give sharper distributions. |
| `top` | NULL | Keep only the best `top` classes per item. |

On the benchmark, each step adds accuracy. Top-1 accuracy on the 1 520 request items:

```sql
CREATE TABLE seat_catalogue AS
SELECT class_id, vec::FLOAT[] AS vec FROM read_csv_auto('test/data/coverage/catalogue_vectors.csv.gz');
CREATE TABLE requests AS SELECT * FROM read_csv_auto('test/data/coverage/requests.csv.gz');
CREATE TABLE request_items AS
SELECT tree_id, node_id, source, vec::FLOAT[] AS vec
FROM read_csv_auto('test/data/coverage/request_item_vectors.csv.gz');
CREATE TABLE request_attributes AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv_auto('test/data/coverage/request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);
CREATE TABLE catalogue_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/catalogue_attributes.csv.gz');
CREATE TABLE name_vectors AS
SELECT name, vec::FLOAT[] AS vec FROM read_csv_auto('test/data/coverage/attribute_name_vectors.csv.gz');
SET VARIABLE name_floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec', sample := 20000));

CREATE TABLE request_labels AS
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'seat_catalogue',
       attrs_table := 'request_attributes', templates_table := 'catalogue_attributes',
       name_vectors := 'name_vectors', name_floor := getvariable('name_floor'));

-- check against the benchmark's gold classes
SELECT round(avg((l.class_id = g.class_id)::INT), 3) AS top1
FROM request_labels l JOIN read_csv_auto('test/data/coverage/gold_item_class.csv.gz') g USING (tree_id, node_id)
WHERE l.rank = 1;
```

| top1  |
|------:|
| 0.953 |

The same check for the other settings:

| Setting | Top-1 |
|---|---:|
| `center_by_source := false`, no attributes | 0.593 |
| centring (default), no attributes | 0.853 |
| centring + attribute bonus (above) | 0.953 |

`cosine_floor` estimates the cosine of unrelated names (here 0.853, from 14 365 pairs). Sentence
encoders give unrelated texts a high cosine. The floor calibrates it, so only real synonyms count.
Use the same floor in `tree_coverage(name_vectors := ..., name_floor := ...)`.

## Review the Archive Vocabulary

An archive often uses a closed internal vocabulary: the same few wordings in thousands of trees.
Classify each wording once and let a reviewer confirm the ones that matter.

```sql
CREATE TABLE programme_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/programme_nodes.csv.gz');
CREATE TABLE wordings AS
SELECT wording AS node_id, wording AS label, source, vec::FLOAT[] AS vec
FROM read_csv_auto('test/data/coverage/vocabulary_vectors.csv.gz');
CREATE TABLE wording_decisions AS
SELECT * FROM classify_to_catalogue(items_table := 'wordings', catalogue_table := 'seat_catalogue');

SELECT wording, top1, top2, round(margin, 3) AS margin, n_trees_affected, round(impact, 1) AS impact
FROM vocabulary_review_queue(trees_table := 'programme_nodes', decisions := 'wording_decisions') LIMIT 5;
```

| wording            | top1             | top2             | margin | n_trees_affected | impact |
|--------------------|------------------|------------------|-------:|-----------------:|-------:|
| Klimapaket Heizung | seat_heating     | seat_ventilation | 0.925  | 23               | 1.7    |
| KS-Antrieb         | headrest_powered | seat_motor       | 0.987  | 55               | 0.7    |
| LV-Struktur        | seat_track       | seat_frame       | 0.996  | 187              | 0.7    |
| Massagelordose     | lumbar_support   | massage          | 0.996  | 38               | 0.1    |
| Lüftermodul        | seat_ventilation | trim_cover       | 0.999  | 68               | 0.1    |

`impact` = trees affected × (1 − margin). The first four wordings mislead: "Klimapaket Heizung" is a
ventilation module. In each case the runner-up `top2` is the true class, so the reviewer confirms
the second choice. Together they touch more than 60 % of the programmes.

Store the answers. Table functions cannot write, so keep the merged result with
`CREATE OR REPLACE TABLE`:

```sql
CREATE TABLE review AS SELECT * FROM (VALUES
  ('Klimapaket Heizung', 'internal', 'seat_ventilation'),
  ('LV-Struktur',        'internal', 'seat_frame'),
  ('Massagelordose',     'internal', 'massage'),
  ('KS-Antrieb',         'internal', 'seat_motor')) t(wording, source, class_id);

CREATE OR REPLACE TABLE vocabulary_overrides AS SELECT * FROM apply_vocabulary_review('review');
```

The review table may also carry `reviewed_by` and `reviewed_at`. Without `overrides_table`, the
function merges into an existing `vocabulary_overrides` table, so a second review adds to the
first. For the same wording and source, the new review replaces the old entry. Pass `overrides_table := 'vocabulary_overrides'`
to `vocabulary_review_queue` and the reviewed wordings leave the queue.

## Send Uncertain Items to a Reviewer

Request items are unique sentences, not a closed vocabulary. Review them per item.
`uncertain_decisions` builds one JSON request per uncertain item, ready for a colleague or an LLM:

```sql
CREATE TABLE texts AS SELECT NULL::VARCHAR AS tree_id, node_id, label AS text FROM items;
CREATE TABLE decided AS SELECT * FROM classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue');

SELECT node_id, request FROM uncertain_decisions(decisions := 'decided', texts_table := 'texts', top := 2);
```

| node_id | request |
|---------|---------|
| I3 | `{"tree_id":null,"node_id":"I3","text":"Pumpe mit Motor","margin":0.0,"candidates":[{"class_id":"motor","p":0.499788,"label":null,"definition":null},{"class_id":"pump","p":0.499788,"label":null,"definition":null}]}` |

The default selects margins below 0.2. `share := 0.15` selects the 15 % lowest margins instead.
`catalogue_table` adds class labels and definitions from a table `(class_id, label_en, definition)`.

Write the answers into a table `(tree_id, node_id, class_id)` and classify again:

```sql
CREATE TABLE answers AS SELECT NULL::VARCHAR AS tree_id, 'I3' AS node_id, 'pump' AS class_id;

SELECT node_id, class_id, round(p, 3) AS p, overridden
FROM classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue',
                           answers_table := 'answers', top := 1)
ORDER BY node_id;
```

| node_id | class_id | p     | overridden |
|---------|----------|------:|------------|
| I1      | pump     | 0.997 | false      |
| I2      | motor    | 0.998 | false      |
| I3      | pump     | 1.0   | true       |

Answers beat vocabulary overrides. `overrides_table` works the same way per wording, matched on the
item's `label`. On the benchmark, answering the 15 % lowest-margin items lifts recall@20 from 0.86
to 0.91.

## Feed the Labels Into Coverage

One `label_table` serves both sides. Reviewed wordings win over it through `overrides_table`:

```sql
CREATE TABLE programme_labels AS
SELECT n.tree_id, n.node_id, d.class_id, d.p
FROM programme_nodes n JOIN wording_decisions d ON d.node_id = n.label AND d.source = n.source
WHERE n.node_kind = 'item';
CREATE TABLE request_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/request_nodes.csv.gz');
CREATE TABLE labels AS
SELECT tree_id, node_id, class_id, p FROM programme_labels
UNION ALL
SELECT tree_id, node_id, class_id, p FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'seat_catalogue');

SELECT candidate_id, round(coverage, 3) AS coverage, rank, n_gaps, reviewed, assignment_used
FROM tree_coverage('R12', k := 20, nodes_table := 'request_nodes', attrs_table := NULL,
                   candidate_nodes_table := 'programme_nodes', candidate_attrs_table := NULL,
                   overrides_table := 'vocabulary_overrides', label_table := 'labels') LIMIT 3;
```

| candidate_id | coverage | rank | n_gaps | reviewed | assignment_used |
|--------------|---------:|-----:|-------:|----------|-----------------|
| P061         | 0.914    | 1    | 0      | false    | best_match      |
| P047         | 0.914    | 2    | 0      | false    | best_match      |
| P093         | 0.914    | 3    | 0      | false    | best_match      |

Classifier labels are soft, so coverage stays below 1 without gaps. They are not reviewed, so
`auto` uses best match. To let a confirmed request use partition, write its classes into
`class_id` of the nodes table.

## Pitfalls

| Pitfall | Solution |
|---|---|
| A single item gets the wrong class | A batch below `min_source_items` is not centred. Classify items in batches. |
| Vector dimension mismatch | Items and catalogue must use the same model. The function checks this. |
| An override class is not in the catalogue | Error. Fix the review table. |
| The review seems lost | `apply_vocabulary_review` only returns rows. Store them with `CREATE OR REPLACE TABLE`. |
| High cosine between unrelated names | Calibrate with `cosine_floor` and pass `name_floor`. |

## Next Steps

- [Evaluation and tuning](07-evaluation-and-tuning.md): measure the effect of each step
- [Coverage matching](04-coverage-matching.md): class resolution and assignment
- API: [Classification and review](../api/14-classification-and-review.md), [Embeddings](../api/04-embeddings.md)
- [Examples](../../examples/classification_review)
