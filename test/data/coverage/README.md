# Coverage-matching benchmark: automotive seating systems

A deterministic, synthetic test dataset for the coverage-matching functions
(`docs/requirements-matching/`). Everything in it is fictional. *Sitzwerk Demo GmbH*, a
seating supplier, receives seat-system specifications from eight fictional vehicle makers.
It answers them with modules from its library and reuses past seat programmes. Its master
data also holds the makers' corporate hierarchies, with duplicates.

```bash
cd scripts
uv run generate-coverage-data --seed 7 --output ../test/data/coverage/   # byte-identical on every run
uv run pytest                                                           # properties of the dataset
```

The generator is in `scripts/src/scripts/coverage/` and is pinned by
`scripts/tests/test_coverage_generator.py`. `mini/` is a small fixture made by hand; the
generator does not produce it.

## What a matcher may read, and what is gold

Files whose names start with `gold_` hold the ground truth: classes, exact values, true
bounds, source programmes and duplicate clusters. Matchers and the extension's functions
must never read them. Only tests read them, to score the results. The `class_id` column in
every `*_nodes` file is empty on purpose. Classes come from review (`gold_vocabulary`
stands in for a finished vocabulary review) or from the pivot classifier.

## Files

All files are gzipped CSV with a header row and UTF-8 text. Vectors are JSON arrays, so
`vec::FLOAT[]` and `tokens::FLOAT[][]` cast them in DuckDB. Values in `*_attributes` are in
base units: m, m/s, N, s, dB(A), kg, A, K, W, deg, N*m, m3/s, 1 (count), EUR.

### Catalogue (the pivot vocabulary)

| File | Rows | Columns |
|---|---|---|
| `catalogue_classes` | 23 | `class_id, label_en, label_de, definition` |
| `catalogue_attributes` | 62 | `class_id, attr_key, name_en, name_de, dimension, base_unit, bound, scale`: attribute templates. `bound` is the direction of the limit (`at_least`, `at_most`, `within`, `equals`) and `scale` is `log` or `linear` |
| `compatibility_rules` | 32 | `dimension, required, provided, score` for the ordinal dimensions `flammability_class` (F3 < F2 < F1 < F0) and `protection_class` (IP40 < IP54 < IP65 < IP67). A better class scores 1, one step worse 0.5, two or more steps worse 0 |

Exactly one class (`headrest_powered`) has two attributes with the same dimension, height
travel and tilt travel, so the attribute name has to tell them apart.

### Module library (offer side, OR trees)

`module_nodes` (323 rows) and `module_attributes` (516 rows) use the HLD §3.2 tree shape.
`module_nodes` adds an `application_id` column.

- 40 base modules (tree roots, `variant_group`) → 90 configurations (`variant_group`, `OR`)
  → 150 applications (`OR`). An application is either an `item` or, for the five **combo
  modules** (e.g., *Komfortpaket Premium*: heating + ventilation + massage), a `container`
  of `AND` items, one per class.
- Item labels come from the closed internal vocabulary. `count` is the extensive
  multiplicity, such as heated zones, massage zones or motors.
- `module_attributes` holds the provided values (`role = provided`). Point values have
  `lo = hi`; ranges have `lo < hi`.

### Programme archive (past solutions)

`programme_nodes` (7 970 rows), `programme_attributes` (17 323 rows), 300 programmes
(`P001`…`P300`), source `internal`.

- Front-row programmes list a driver seat (`.D`) and a passenger seat (`.B`) as containers.
  The other rows are one bench.
- 60 % are built from modules. `module_ref` names the module item they copy (class, count
  and provided values). 40 % are **capability-only** and carry no module references, as old
  archives often do.
- `programme_class_multiset` holds `(entity_id, feature_id, weight)` with reviewed classes
  and `weight` = Σ count. `programme_class_multiset_unreviewed` is the same table with the
  classes that a reading of the surface wording gives. It differs only on the misleading
  wordings.

### Internal vocabulary

`internal_vocabulary` (`wording, source`) lists 50 wordings. `gold_vocabulary` adds
`class_id, apparent_class_id, is_misleading`. Four wordings mislead: their surface form
points at the wrong class.

| Wording | True class | Apparent class |
|---|---|---|
| Klimapaket Heizung | seat_ventilation | seat_heating |
| LV-Struktur | seat_frame | seat_track |
| Massagelordose | massage | lumbar_support |
| KS-Antrieb | seat_motor | headrest_powered |

Together they occur in more than 60 % of the programmes. The vocabulary review queue has
to rank them first.

### Requests (customer specifications)

`request_nodes` (3 196 rows) and `request_attributes` (2 550 rows) cover 80 requests
(`R01`…`R80`) with 1 627 requirement items. `requests_json/<request_id>.json` holds the
same trees as nested JSON, the way an upstream extraction delivers them.

- Each request is derived from a hidden source programme (`gold_requests`). The wording is
  German or English (50/50), and the units are customer units (mm or cm, A or mA, s or min,
  kN or N, kg or g, cycles or Zyklen, EUR or €). Numbers use a decimal comma in German, and
  some counts use thousands separators.
- The hierarchy is spec → chapter → section (→ subsection) → requirement, with notes as
  leaf containers. A requirement sits at depth 2, 3 or 4. `context` is the enclosing
  heading.
- The label is the full requirement sentence, in the maker's sentence frame
  ("Der Lieferant liefert …"), and may state several values.
- `request_attributes` holds what extraction found: `name` and `value_text`
  (e.g. `max. 33 dB(A)`, `mindestens 24 cm`, `15–25 mm/s`, `IP54 or better`). Parse it
  with `parse_quantity`. In JSON the same attribute also has separate fields (`lo`, `hi`,
  `unit` in customer units, and `bound`, which is `null` when unqualified).
- About 30 % of the numeric values are **unqualified**, with no bound word (`noise 38 dB(A)`).
  About 11 % of the requirements are **stretched** beyond their source programme. In 10
  requests the stretch goes beyond every module and programme, so these requests are
  **unmeetable**.
- Detailed requests state most attribute values; coarse requests omit most of them.

### Phrasing pool and unseen requirements

- `phrasings` (`phrasing_id, class_id, lang, split, text`) has 18 phrasings per class, 9
  `dev` and 9 `test`. Request items use only `dev` phrasings of their class, and the
  catalogue exemplars come only from `dev` phrasings. `test` phrasings occur only in
  `unseen_requirements`.
- `unseen_requirements` (`item_id, source, lang, text`) holds 200 requirement sentences
  built from held-out phrasings. `gold_unseen` gives their class and phrasing.

### Vectors (deterministic, no model)

| File | Columns |
|---|---|
| `catalogue_vectors` | `class_id, vec, exemplar_tokens, exemplar_phrasing_ids` |
| `request_item_vectors` | `tree_id, node_id, source, vec, tokens` |
| `unseen_vectors` | `item_id, source, vec, tokens` |
| `vocabulary_vectors` | `wording, source, vec, tokens` |
| `attribute_name_vectors` | `name, attr_key, vec` |

`vec` is 64-dimensional and L2-normalised. `tokens` holds 3–12 token vectors. A vector is
`normalise(class centroid + phrasing offset + source boilerplate + noise)`. The boilerplate
is strong and leans towards two hub classes per source. Measured with the reference
classifier, top-1 accuracy is:

| Setting | Top-1 |
|---|---|
| raw cosine, request items | ≈ 0.58 |
| per-source centring (+ z-score), request items | ≈ 0.86 |
| centring, held-out phrasings | ≈ 0.75 |
| MaxSim over token vectors, held-out phrasings | ≈ 0.99 |

Misleading wordings sit closest to their apparent class. Unrelated attribute names have a
raw cosine of about 0.85, and synonyms of the same attribute about 0.98.

### Customer master (deduplication)

`customers` (1 910 rows) has 8 groups → legal entities → plants → purchasing contacts.
`gold_customer_clusters` (`record_id, cluster_id, dup_kind, lookalike_of`) marks 152
planted duplicates of these kinds: `spelling`, `umlaut` (ü/ue), `legal_form`
(GmbH/G.m.b.H.), `moved_plant` and `shared_tax_id`. It also marks 12 `near_miss` records,
which are different companies at the street address of an existing plant. They must not be
merged.

## Ground truth and oracle semantics

The oracle sees gold classes and exact values. It uses the **true** bound, even when the
text is unqualified.

| Bound | Satisfied when (provided interval `[p_lo, p_hi]`) |
|---|---|
| `at_least v` | `p_hi ≥ v` |
| `at_most v` | `p_lo ≤ v` (lower-is-better limits: noise, mass, current, time, price) |
| `within [lo, hi]` | `p_lo ≤ lo` and `p_hi ≥ hi` (the provided range covers the required one) |
| `equals v` | `|p − v| ≤ 2 % of |v|` |
| ordinal | provided class at least as good as the required class |

For a request item *q* (class *k*, count *n_q*) against a programme, with *C* the programme
items of class *k*:

- item score = `max_{c∈C} (share of q's attributes c satisfies)` × `min(1, Σ_{c∈C} n_c / n_q)`,
  and 1 for the attribute part when *q* states no values; 0 if *C* is empty
- coverage = mean item score over the request's items

Gold files:

| File | Content |
|---|---|
| `gold_item_class` | `tree_id, node_id, class_id, phrasing_id`: class of every module, programme and request item; phrasing of request items |
| `gold_request_attributes` | normalised requirement (`attr_key, dimension, unit, lo, hi, bound, category`), the `stretched` flag and the source value (`src_lo, src_hi, src_category`) |
| `gold_requests` | `request_id, source_programme_id, maker, lang, granularity, is_unmeetable, best_programme_id, best_coverage`. `is_unmeetable` ⇔ `best_coverage < 0.95` (the BRD flag threshold) |
| `gold_coverage` | `query_id, candidate_id, true_coverage` for all 80 × 300 pairs |
| `gold_best_applications` | `tree_id, node_id, application_id, oracle_score, is_best` per requirement and same-class application (counts ignored) |
| `gold_unseen`, `gold_vocabulary`, `gold_customer_clusters` | see above |

Ties are common: several programmes can reach the best coverage. A top-1 pick is correct
when its true coverage equals `best_coverage`.

## Mini fixture (`mini/`, hand-made)

Plain CSV in the HLD shape with `class_id` filled in (reviewed). Unit tests compute their
expected values by hand from it.

| File | Content |
|---|---|
| `tree_nodes.csv`, `tree_attributes.csv` | Requests `Q1` (2 × heating, 4 × massage, powered headrest, memory, with limits), `Q2` (seat track: ≥ 0.24 m travel, ≤ 38 dB(A)), `Q3` (2 heated + 2 ventilated zones, coarse); programmes `S1`…`S6`; modules `M1`…`M5` with 10 applications (`M1`: exactly one variant meets `Q2`; `M3`: combo) |
| `feature_multiset.csv` | Reviewed class multiset of `S1`…`S6` |
| `vocabulary.csv` | Wordings with true and apparent class (`Klimapaket Heizung` misleads) |
