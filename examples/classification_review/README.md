# Classification and Review Examples

> **Every item needs a class before it can be matched, and a person should only see the doubtful ones.**

Runnable SQL examples for mapping items onto a catalogue of classes with your own vectors (the
pivot classifier) and for turning its uncertainty into a short review list: a vocabulary review
of the archive and an external review of single items.

## Functions

| Function | Description |
|----------|-------------|
| `classify_to_catalogue(...)` | Distribution over catalogue classes per item: `class_id, p, rank, margin, overridden` |
| `cosine_floor(table, column [, sample, seed])` | Median cosine of unrelated vectors, to calibrate name similarity |
| `vocabulary_review_queue(trees_table, decisions [, overrides_table])` | Wordings ranked by impact = trees affected × (1 − margin) |
| `apply_vocabulary_review(review_table [, overrides_table])` | Merged vocabulary overrides; the review wins per wording and source |
| `uncertain_decisions(decisions, share \| threshold, ...)` | Lowest-margin items as JSON requests for a reviewer |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`classify_to_catalogue.sql`](classify_to_catalogue.sql) | Raw vs. centred vs. attribute bonus; centroid vs. late interaction on unseen wording | `test/data/coverage/` vectors |
| [`vocabulary_review.sql`](vocabulary_review.sql) | Review queue, reviewer answers, queue after the review | `test/data/coverage/` vocabulary + programmes |
| [`uncertain_decisions.sql`](uncertain_decisions.sql) | Export doubtful items as JSON, read answers back with `answers_table` | `test/data/coverage/` unseen phrasings |

The benchmark vectors are deterministic (no model): 64-dimensional, L2-normalised, with a
strong per-maker sentence frame. `gold_*` files stand in for the reviewer and score the result.

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/classification_review/classify_to_catalogue.sql
./build/release/duckdb < examples/classification_review/vocabulary_review.sql
./build/release/duckdb < examples/classification_review/uncertain_decisions.sql
```

---

## Usage

```sql
-- Classify, keep the best class per item; the output is a label_table for tree_coverage
CREATE TABLE request_labels AS
SELECT tree_id, node_id, class_id, p
FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                           attrs_table := 'req_attrs', templates_table := 'templates');

-- Send the 15 % least certain items to a reviewer, then classify again with the answers
SELECT request FROM uncertain_decisions(decisions := 'decided', share := 0.15,
                                        texts_table := 'item_texts', catalogue_table := 'classes');
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                                    answers_table := 'answers');
```

## Input Tables

| Parameter | Columns |
|-----------|---------|
| `items_table` | `node_id, vec[, tree_id, source, label, tokens]` |
| `catalogue_table` | `class_id, vec[, exemplar_tokens]` |
| `attrs_table`, `templates_table` | `([tree_id,] node_id, name, dimension)` and `(class_id, dimension[, name, name_en, name_de])` |
| `overrides_table` | `wording, source, class_id` (matched on the item's `label`) |
| `answers_table` | `([tree_id,] node_id, class_id)`: per-item answers, they beat overrides |

## Key Concepts

- **Per-source centring.** A maker's sentence frame ("Der Lieferant liefert ...") pulls all its
  items towards a few classes. Subtracting the mean vector of each source removes it. A source
  needs at least `min_source_items` (default 10) items for its own mean; smaller sources are
  centred with the mean of the whole batch.
- **Attribute bonus.** Stated values ("max. 38 dB(A)") point at classes whose templates have
  that dimension; this lifts top-1 accuracy from 0.853 to 0.953 on the benchmark.
- **Late interaction.** `mode := 'late_interaction'` compares token vectors (`maxsim`) and
  copes with wordings the exemplars never used (0.75 → 0.94 on 200 unseen phrasings).
- **Margin = p1 − p2.** A small margin marks a doubtful decision. The review functions sort by
  it, so reviewers see the items where an answer changes the most.
- **Answers are not reviews of trees.** A `label_table` never makes a tree *reviewed*; only
  `class_id` or the reviewed vocabulary does (this controls `assignment := 'auto'` in
  `tree_coverage`).
