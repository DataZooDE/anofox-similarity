# Classification and Calibration

> How items get classes from your vectors, what the probabilities and the margin mean, why raw
> cosines need a floor, and why a review of a few wordings fixes most of an archive. For the
> calls, see [guide 06](../guides/06-classification-and-review.md) and
> [api/14](../api/14-classification-and-review.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Classes Are a Distribution

Coverage compares items by class (see [coverage semantics](coverage-semantics.md)). Free-text
specifications and old archives rarely carry classes, so `classify_to_catalogue` maps each item
onto the catalogue as a **distribution** `p` over the classes, not as one answer. The kernel uses
the distribution directly: the label agreement of two items is the Bhattacharyya coefficient
`Σ √(p_k(q) · p_k(c))`, so an item that is 50 % pump, 50 % motor half-matches both. The extension
ships no language model: the item and catalogue vectors come from your own model.

## The Pipeline

Per item:

1. **Centring.** Subtract the mean vector of the item's source (`center_by_source`). Every
   sentence of a specification shares a frame ("Der Lieferant liefert …"); without centring that
   frame dominates the cosine and pulls items towards a few hub classes. A source with fewer than
   `min_source_items` (10) items uses the batch mean instead.
2. **Similarity.** `'centroid'`: the cosine to each class vector. `'late_interaction'`: `maxsim`,
   each item token takes its best cosine over the class's exemplar tokens, and the maxima are
   averaged. Token-level matching handles compounds and jargon the class vectors have not seen.
3. **z-score** of the similarities across the catalogue, plus `attr_bonus` (β) times the
   attribute-template match: a "current draw" fits a heater better than a frame.
4. **Softmax** of z / `temperature` (τ) gives `p`; `margin = p1 − p2`.
5. **Overrides and answers.** A reviewed wording (`overrides_table`) or a per-item answer
   (`answers_table`) replaces the distribution by a one-hot `p` with `overridden = true`.

A worked example: three classes, two items, no centring (a batch below 10 items is not centred).
Steps 2 to 4 by hand give exactly the function's `p`:

```sql
CREATE OR REPLACE TABLE catalogue AS SELECT * FROM (VALUES
  ('pump',  [1.0, 0.0, 0.0]::FLOAT[3]),
  ('motor', [0.0, 1.0, 0.0]::FLOAT[3]),
  ('valve', [0.0, 0.0, 1.0]::FLOAT[3])) t(class_id, vec);
CREATE OR REPLACE TABLE items AS SELECT * FROM (VALUES
  ('I1', 'Kreiselpumpe 3 kW', [0.9, 0.3, 0.1]::FLOAT[3]),
  ('I3', 'Pumpe mit Motor',   [0.6, 0.6, 0.0]::FLOAT[3])) t(node_id, label, vec);

WITH s AS (SELECT i.node_id, c.class_id, array_cosine_similarity(i.vec, c.vec) AS cos FROM items i, catalogue c),
z AS (SELECT *, (cos - avg(cos) OVER w) / stddev_pop(cos) OVER w AS z FROM s WINDOW w AS (PARTITION BY node_id)),
p AS (SELECT *, exp(z / 0.3) / sum(exp(z / 0.3)) OVER (PARTITION BY node_id) AS p FROM z)
SELECT p.node_id, p.class_id, round(p.cos, 3) AS cos, round(p.z, 3) AS z, round(p.p, 3) AS by_hand,
       round(k.p, 3) AS classify
FROM p JOIN classify_to_catalogue(items_table := 'items', catalogue_table := 'catalogue') k USING (node_id, class_id)
ORDER BY node_id, k.rank;
-- => I1 | pump | 0.943 | 1.373 | 0.997 | 0.997
-- => I1 | motor | 0.314 | -0.392 | 0.003 | 0.003
-- => I1 | valve | 0.105 | -0.981 | 0.0 | 0.0
-- => I3 | motor | 0.707 | 0.707 | 0.5 | 0.5
-- => I3 | pump | 0.707 | 0.707 | 0.5 | 0.5
-- => I3 | valve | 0.0 | -1.414 | 0.0 | 0.0
```

The z-score makes the scores of different items comparable (a model that gives everything a cosine
above 0.8 still spreads them), and τ = 0.3 turns a clear lead into near-certainty: I1's z of 1.37
against −0.39 becomes 0.997. I3 is a "pump with motor": two classes tie and the margin is 0, which
is the signal to ask someone.

## Calibrated Cosine and the Floor

Sentence encoders give unrelated texts a high cosine, often 0.8 and more. A raw cosine of 0.82 is
then no evidence of a synonym. `calibrated_cosine` rescales against a **floor**, the cosine of
unrelated pairs:

```
calibrated(a, b) = clip((cos(a, b) − floor) / (1 − floor), 0, 1)
```

`cosine_floor` estimates the floor as the median cosine of distinct pairs in your corpus: most
pairs are unrelated, so their median is the level "unrelated" sits at for this model. With four
attribute names:

```sql
CREATE OR REPLACE TABLE names AS SELECT * FROM (VALUES
  ('current draw', [0.9, 0.4, 0.1]::FLOAT[3]), ('Stromaufnahme', [0.85, 0.45, 0.15]::FLOAT[3]),
  ('noise', [0.5, 0.8, 0.3]::FLOAT[3]), ('mass', [0.6, 0.5, 0.6]::FLOAT[3])) t(name, vec);
SELECT b.name, round(array_cosine_similarity(a.vec, b.vec), 3) AS raw,
       round(calibrated_cosine(a.vec, b.vec, (SELECT floor FROM cosine_floor('names', 'vec'))), 3) AS calibrated
FROM names a, names b WHERE a.name = 'current draw' AND b.name IN ('Stromaufnahme', 'noise') ORDER BY raw DESC;
-- => Stromaufnahme | 0.996 | 0.973
-- => noise | 0.816 | 0.0
```

The floor here is 0.861 (6 pairs). The translation keeps 0.973; the unrelated name, raw 0.816,
drops to 0. The coverage kernel uses the same calibration for attribute names
(`name_vectors`, `name_floor`), and the classifier for its attribute bonus. Estimate the floor once
per corpus and model, and pass the same value everywhere.

## Soft Labels and Reviewed Trees

A classifier label is evidence, not a fact. A tree is **reviewed** only when every item got its
class from `class_id` or from the reviewed vocabulary (`overrides_table`); a `label_table` of
classifier output never makes it reviewed. This matters for coverage: `assignment := 'auto'` uses
the strict partition only on reviewed trees, because a wrong class there would make an item
unavailable to the requirement it really serves. Soft labels also keep coverage below 1 even
without gaps: two classifier distributions rarely agree exactly, so their agreement stays below 1.

## Review Where It Matters

Archives and offer catalogues use a closed internal vocabulary: the same few wordings in thousands
of trees. The unit of review is therefore the **distinct wording per source**, not the item: one
confirmation fixes every occurrence. `vocabulary_review_queue` ranks the wordings by

```
impact = trees affected × (1 − margin)
```

so a wording that is both uncertain and frequent comes first. On the seating benchmark the four
misleading wordings come first, together touching more than 60 % of the programmes
([guide 06](../guides/06-classification-and-review.md#review-the-archive-vocabulary)). For single
items of a request, `uncertain_decisions` selects the lowest margins (`threshold`, or a `share`)
and packs their texts and top classes as JSON for a reviewer or an LLM; the answers come back as
`answers_table`.

## Why These Defaults

| Default | Why |
|---|---|
| `center_by_source` true | Removes the shared sentence frame; on the benchmark top-1 goes from 0.593 to 0.853 |
| `min_source_items` 10 | The mean of a few items is mostly those items: centring would erase what tells them apart |
| `attr_bonus` 1.0 | Attributes add independent evidence; with it top-1 reaches 0.953 |
| `temperature` 0.3 | Sharp enough that a clear lead reads as near-certain, soft enough that ties stay visible in the margin |
| `mode` `'centroid'` | Needs only one vector per item; use `'late_interaction'` for wordings the exemplars have not seen (more than 10 points better on 200 unseen phrasings) |
| `name_floor` 0.85 | A typical floor of small sentence encoders; estimate yours with `cosine_floor` |

The design history (study results, rejected options) is in the
[HLD](../requirements-matching/HLD.md) §4.4 and §4.5; it records how the classifier was decided,
not the current contract.
