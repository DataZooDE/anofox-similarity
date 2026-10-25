# HLD — Coverage Matching for Hierarchical Business Data

| | |
|---|---|
| **Document** | High-Level Design |
| **Scope** | anofox-similarity, releases R1–R5 of the coverage-matching feature set |
| **Status** | Draft for review |
| **Date** | October 2026 |
| **Requirements** | [BRD.md](BRD.md) (IDs `BR-xx` referenced below) |

---

## 1. Design goals

1. **One kernel, many hops.** The same functions match request → catalogue, request →
   offer, and request → past solution. Only the trees passed in change.
2. **Cheap first, explainable second.** Stage 1 scans everything with set arithmetic.
   Stage 2 re-ranks the top-k with a full, traced coverage computation.
3. **Embeddings at the leaves, rules for numbers.** Vectors are used for short text
   (labels, attribute names). Units and values use explicit, direction-aware rules.
   Whole-record embeddings are not used for coverage.
4. **SQL-native and self-contained.** Every input and output is a table. No dependency on
   other extensions. Works on DuckDB 1.5.6 and 2.0.
5. **Bring your own vectors.** Functions take `FLOAT[]` columns. The existing
   `embed_text` may be used where it is configured, but it is never required.
6. **Additive.** Existing functions keep their names, semantics and defaults.

## 2. Architecture overview

```
                 upstream (out of scope): extraction, embedding model, LLM re-rank
                 ───────────────────────────────────────────────────────────────────
 request trees ─┐                                                      ┌─► trace / gaps
 offer trees   ─┼─► [A] Property-tree store ─► [B] Quantity layer ─┐   │
 catalogue     ─┘        (nodes, attributes)     (units, bounds)   │   │
                                                                   ▼   │
 leaf vectors ──────► [C] Pivot classifier ──► p(class | node) ──► [E] Coverage kernel ─► ranked
 (FLOAT[])              (centring, z, softmax,      ▲   │              (stage 2, top-k)     candidates
                         late interaction)          │   │                  ▲
                                                    │   ▼                  │ top-k
                         [D] Vocabulary review ─────┘  class multisets ─► [F] Recall
                             (queue, overrides)        (entity, class, w)    (stage 1: containment,
                                                                              Tversky, weighted
                                                                              Jaccard, MinHash/LSH)
                         [G] Evaluation & calibration      [H] Entity clustering (dedup)
```

| Block | Release | New module(s) in `src/modules/` | Main BRD items |
|---|---|---|---|
| F — Directional set measures and recall | R1, R5 | `set_measures.cpp`, `coverage_search.cpp` | BR-01..03, BR-40 |
| B — Quantity layer | R2 | `quantity.cpp` | BR-10..16 |
| A — Property-tree store | R3 | `property_tree.cpp` | BR-04, BR-05 |
| E — Coverage kernel | R3 | `coverage_kernel.cpp` | BR-06, BR-20..23, BR-41 |
| C — Pivot classifier | R4 | `pivot_classifier.cpp` | BR-30, BR-33 |
| D — Vocabulary review | R4 | `vocabulary_review.cpp` | BR-31, BR-32 |
| G — Evaluation and calibration | R5 | `evaluation.cpp` | BR-50..53 |
| H — Entity clustering | R5 | `entity_clustering.cpp` | BR-43 |

Implementation pattern (as in the existing code):

- **Hot numeric kernels** are C++ scalar functions: set measures, value comparison,
  Bhattacharyya, MaxSim.
- **Search-style table functions** use `bind_replace` to generate SQL, like
  `find_similar_materials_jaccard` in `similarity_search.cpp`. Identifiers are validated
  with `ValidateSQLIdentifierPath` and columns with `ValidateTableColumns`.
- **Stage-2 coverage** is a C++ table function. The tree recursion runs in memory over the
  top-k candidates only (small), not as recursive SQL.
- Every function gets a `ddoc::Doc()` description and example, plus a telemetry hook,
  following the existing modules.
- Named arguments are documented in their positional order (DuckDB 1.5.6 binds them by
  position).

## 3. Data model

### 3.1 Generic feature multiset (R1)

The set measures and recall stop assuming `parent_id/child_id`. Any table with the following
shape is accepted:

```sql
-- one row per (entity, feature); weight defaults to 1
CREATE TABLE feature_multiset (
    entity_id  VARCHAR,
    feature_id VARCHAR,   -- component id, class id, certification, skill, ...
    weight     DOUBLE     -- quantity, channel count, level; NULL → 1
);
```

The existing `bom_items(parent_id, child_id[, quantity_per])` maps onto it through a column
mapping (`entity_col :=`, `feature_col :=`, `weight_col :=`), so current tables can be used
without being copied.

### 3.2 Property trees (R3)

```sql
CREATE TABLE tree_nodes (
    tree_id      VARCHAR NOT NULL,   -- request, offer, design, catalogue entry
    node_id      VARCHAR NOT NULL,
    parent_id    VARCHAR,            -- NULL for the root
    node_kind    VARCHAR NOT NULL,   -- 'container' | 'item' (anchor) | 'variant_group'
    edge_type    VARCHAR DEFAULT 'AND',  -- edge to parent: 'AND' (composition) | 'OR' (alternative)
    label        VARCHAR,            -- free text as found in the source
    context      VARCHAR,            -- e.g. enclosing heading; optional
    class_id     VARCHAR,            -- reviewed or gold class; NULL → use pivot
    count        DOUBLE DEFAULT 1,   -- multiplicity (extensive)
    source       VARCHAR,            -- source system / document family (used for centring)
    PRIMARY KEY (tree_id, node_id)
);

CREATE TABLE tree_attributes (
    tree_id      VARCHAR NOT NULL,
    node_id      VARCHAR NOT NULL,   -- the item node it belongs to
    attr_id      VARCHAR NOT NULL,
    name         VARCHAR,            -- e.g. 'max. output current', 'Lieferzeit'
    dimension    VARCHAR,            -- normalised: 'current', 'length', 'currency', 'time', 'count', ...
    unit         VARCHAR,            -- base unit after normalisation
    lo           DOUBLE,             -- interval in base units; lo = hi for a point value
    hi           DOUBLE,
    role         VARCHAR,            -- 'required' | 'provided'
    bound        VARCHAR,            -- 'at_least' | 'at_most' | 'within' | 'equals' | NULL (unqualified)
    qualifier    VARCHAR,            -- optional: 'nominal','peak','continuous','abs_max',...
    category     VARCHAR,            -- for categorical/ordinal attributes instead of lo/hi
    PRIMARY KEY (tree_id, node_id, attr_id)
);
```

Semantics:

- **Containers are transparent.** Only `item` nodes (anchors) and their attributes are
  compared. An item may sit at any depth. This lets a four-level request be compared with a
  two-level offer (BR-04).
- **AND / OR.** Children joined by `OR` (under a `variant_group` or directly) are
  alternatives; the best child counts (BR-05). `AND` children are composed.
- **Counts are extensive.** `count` sums across AND-siblings of the same class and never
  across OR-alternatives.
- **Catalogue entries** are trees too: one `item` per class, with attribute *templates*
  (name and dimension, no values).
- A loader `tree_from_json(json, tree_id)` turns nested JSON from upstream extraction into
  the two tables. Its schema is published as JSON Schema in `docs/requirements-matching/`.

### 3.3 Curation tables (R2, R4)

```sql
CREATE TABLE vocabulary_overrides (       -- BR-32
    wording   VARCHAR, source VARCHAR, class_id VARCHAR,
    reviewed_by VARCHAR, reviewed_at TIMESTAMP
);
CREATE TABLE compatibility_rules (        -- BR-15
    dimension VARCHAR,       -- e.g. 'protection_class', 'cert_level', 'qualifier'
    required  VARCHAR,
    provided  VARCHAR,
    score     DOUBLE         -- 1 = satisfies, 0 = does not, in-between allowed
);
CREATE TABLE units (                       -- BR-10; seeded, user-extensible
    symbol VARCHAR, alias VARCHAR[], dimension VARCHAR,
    factor DOUBLE, offset DOUBLE           -- base = value * factor + offset
);
```

The extension ships a built-in, read-only seed for `units`: SI base and derived units with
prefixes, time (s … year), percent/ppm, count, bit/byte rates, and ISO-4217 currency
symbols. Currency has `factor = NULL`; it requires a user-supplied rate table and is never
converted silently. Users pass their own `unit_table :=` to extend or override the seed.

## 4. Components

### 4.1 [F] Directional set measures (R1)

| Function | Signature | Formula |
|---|---|---|
| `tversky_similarity` | `(q LIST, c LIST, alpha DOUBLE := 1, beta DOUBLE := 1) → DOUBLE` | `|q∩c| / (|q∩c| + α|q\c| + β|c\q|)` |
| `containment` | `(q LIST, c LIST) → DOUBLE` | `|q∩c| / |q|` (= Tversky α=1, β=0) |
| `weighted_jaccard` | `(q MAP(ANY,DOUBLE), c MAP(ANY,DOUBLE)) → DOUBLE` | `Σ min(q_k,c_k) / Σ max(q_k,c_k)` |
| `weighted_containment` | `(q MAP, c MAP) → DOUBLE` | `Σ min(q_k,c_k) / Σ q_k` |
| `weighted_tversky` | `(q MAP, c MAP, alpha, beta) → DOUBLE` | Tversky with `Σmin`, `Σ(q−c)⁺`, `Σ(c−q)⁺` |

- Empty-request conventions match the existing `jaccard_similarity`. NULL in → NULL out;
  `containment([], c)` = NULL, not 1.
- The existing `jaccard_similarity` stays unchanged. It is Tversky with α = β = 1.

**Search table function**

```sql
find_covering_entities(
    query_entity_id VARCHAR, k BIGINT,
    mode          := 'containment',   -- 'jaccard' | 'containment' | 'tversky'
    alpha := 1.0, beta := 0.0,        -- used when mode = 'tversky'
    weighted      := true,
    tie_break     := 0.01,            -- weight of Jaccard added as secondary key (BR-03)
    min_score     := 0.0,
    feature_table := 'feature_multiset',
    entity_col := 'entity_id', feature_col := 'feature_id', weight_col := 'weight'
) → TABLE(entity_id, score, containment, jaccard, shared_weight, query_weight,
          candidate_weight, surplus_weight)
```

There is also a query-by-value variant, `find_covering_entities_for(query MAP, k, ...)`. A
request (such as a new specification) is usually not in the table yet.

`find_similar_materials_jaccard` gains an optional `mode :=` parameter, which defaults to
`'jaccard'` so existing behaviour is unchanged.

**Execution.** The candidate set is retrieved through an inverted join on `feature_id` (only
candidates that share at least one feature), and the measure is computed per candidate.
With a catalogue pivot the vocabulary is small (10–200 classes), and this exact path covers
10⁵ candidates within the BR-40 budget. Sub-linear recall (§4.7) is only needed for large
raw vocabularies (e.g., BOM components).

### 4.2 [B] Quantity layer (R2)

| Function | Signature | Purpose |
|---|---|---|
| `parse_quantity` | `(text VARCHAR, unit_table := NULL, lang := ['en','de']) → STRUCT(lo, hi, unit, dimension, bound, raw)` | Parses `"≥ 0.8627 A"`, `"9379…43004 mV"`, `"max. 3 A"`, `"6–18 V"`, `"mindestens 2 Wochen"`. Bound words come from a per-language list (at least / min. / mindestens / ≥; at most / max. / höchstens / ≤; ranges with –, …, to, bis). |
| `normalize_quantity` | `(value DOUBLE, unit VARCHAR, unit_table := NULL) → STRUCT(value, unit, dimension)` | Converts to base units; unknown unit → NULL dimension (never guessed). |
| `value_satisfies` | `(req STRUCT, prov STRUCT, scale := 'log', tol := 0.02, soft_band := 0.05) → STRUCT(score, distance, rule)` | Direction-aware comparison (below). |
| `compat_score` | `(dimension, required, provided, rules_table := 'compatibility_rules') → DOUBLE` | Lookup for categorical/ordinal attributes; missing rule → 0 (or 1 if equal). |

**`value_satisfies` rules** (req = required interval or bound; prov = provided interval
`[p_lo, p_hi]`):

| Bound of requirement | Satisfied when | `distance` (relative, ≥ 0) |
|---|---|---|
| `at_least v` | `p_hi ≥ v` | `(v − p_hi) / |v|` |
| `at_most v` | `p_lo ≤ v` (lower-is-better limits: ripple, delay, lead time, price) | `(p_lo − v) / |v|` |
| `within [lo, hi]` | interval containment; on `scale='log'`: `|ℓ(R) ∩ ℓ(P)| / |ℓ(R)|` with `ℓ([a,b]) = [log₁₀a, log₁₀b]` | uncovered share of `ℓ(R)` |
| `equals v` | `|v − p| ≤ tol` (relative) | `|v − p| / |v|` |
| unqualified point `v` | treated as `within [v, v]`, widened by ±0.01 decades (log) | as `within` |

- `score = 1` if satisfied within `tol` (0.02 decades on log scale, 2 % on linear scale).
  Otherwise the score falls linearly to 0 across `soft_band`, so a miss beyond 5 % scores 0.
- `distance` is always returned, so the trace can show "misses by 3 %" (BR-13).
- The **dimension gate** comes first: different dimensions → `score = 0`, `rule = 'gate'`
  (BR-11).
- `scale` defaults per dimension (`log` for physical quantities, `linear` for currency,
  time and percent) and can be overridden (BR-14).
- Qualifier roles (`nominal`, `peak`, …) are matched **only** through `compatibility_rules`
  (dimension `'qualifier'`). If no rule exists, an unqualified side matches any qualifier.
  Embedding similarity of qualifier words is explicitly not used, because it was neutral to
  harmful in the study (BR-15).

### 4.3 [A] Property-tree store (R3)

| Function | Purpose |
|---|---|
| `tree_from_json(json, tree_id)` | Upstream JSON → `tree_nodes` + `tree_attributes` rows |
| `tree_anchors(tree_table, attr_table, tree_ids := NULL)` | Flattens containers: one row per item with `anchor_path`, `variant_path` (OR choices on the way to the root) and `effective_count` |
| `tree_class_multiset(tree_table, class_source := 'resolved')` | Builds `(entity_id=tree_id, feature_id=class_id, weight=Σcount)` for stage 1. Class resolution order: `class_id` column → `vocabulary_overrides` → pivot arg-max |
| `validate_trees(...)` | Checks for orphans, cycles, attributes on containers, unknown units and mixed dimensions per attribute name |

`tree_anchors` is a recursive CTE (`WITH RECURSIVE … USING KEY` where available) and
produces the flattened view that stages 1 and 2 consume.

### 4.4 [C] Pivot classifier (R4)

The pivot classifier maps each item node onto the catalogue as a distribution `p(v)` over K
classes (BR-30). Inputs are vectors supplied by the user:

```sql
classify_to_catalogue(
    items_table   := 'item_vectors',      -- (node_id, source, vec FLOAT[], tokens FLOAT[][] NULL)
    catalogue_table := 'catalogue_vectors', -- (class_id, vec FLOAT[], exemplar_tokens FLOAT[][] NULL)
    mode          := 'centroid',          -- 'centroid' | 'late_interaction'
    center_by_source := true,
    attr_bonus    := 1.0,                 -- β: dimension-gated attribute-name match vs class templates
    temperature   := 0.3,                 -- τ
    overrides_table := 'vocabulary_overrides'
) → TABLE(node_id, class_id, p, rank, margin, overridden BOOLEAN)
```

Pipeline per item:

1. **Per-source mean-centring.** Subtract the mean vector of the item's `source`, then
   re-normalise. This removes shared boilerplate such as "The system shall provide …",
   which otherwise turns a few catalogue entries into hubs. It was the largest single
   improvement in the study.
2. **Similarity.** In `centroid` mode this is the cosine to the class vector. In
   `late_interaction` mode it is `maxsim(tokens, exemplar_tokens)`: each item token takes its
   maximum cosine over the class's exemplar tokens, and the maxima are averaged. This handles
   compounds and jargon (74.5 % → 92.9 % top-1 on unseen wording).
3. **z-score across the catalogue** per item, plus `β ×` the dimension-gated attribute
   template match.
4. **Softmax** with temperature τ → `p(v)`; `margin = p₁ − p₂`.
5. **Overrides.** A reviewed wording gets a one-hot `p` and `overridden = true` (BR-32).

Supporting scalars (C++):

| Function | Purpose |
|---|---|
| `maxsim(q FLOAT[][], d FLOAT[][]) → DOUBLE` | Late-interaction score |
| `bhattacharyya(p MAP(VARCHAR,DOUBLE), q MAP) → DOUBLE` | `Σ √(p_k q_k)`; soft label similarity `L(q,c)` used in stage 2 |
| `calibrated_cosine(a, b, floor DOUBLE) → DOUBLE` | `clip((cos − b)/(1 − b), 0, 1)` |
| `cosine_floor(table, col, sample := 10000) → DOUBLE` | Median cosine of random pairs; estimates `b` for a corpus (typical 0.84–0.88 for small sentence encoders) |

Prompt asymmetry (query vs document prompts), translation and model choice stay upstream.
The design document recommends: query prompt for items, document prompt for catalogue
entries, label plus a one-line definition, and exemplars once they exist.

**External re-rank hook (BR-33).** `uncertain_decisions(threshold := <margin>, top := 3)`
returns the low-margin items with their top-3 candidates and texts as JSON, ready for an LLM
or human. Results are written back as `vocabulary_overrides` (per wording) or as a
`node_class_decisions` table (per item), which the classifier then treats like overrides.

### 4.5 [D] Vocabulary review (R4)

```sql
vocabulary_review_queue(
    trees_table := 'tree_nodes', decisions := <classify_to_catalogue result>,
    sources := NULL                       -- restrict to internal/offer-side sources
) → TABLE(wording, source, n_occurrences, n_trees_affected, top1, p1, top2, p2,
          margin, impact)                  -- impact = n_trees_affected × (1 − margin)
```

- The unit of review is a **distinct wording per source**, not a node. Offer-side wordings
  are a small, closed set, so one confirmation fixes every occurrence.
- Sorting by `impact` puts first the few wordings that touch most trees (BR-31).
- `apply_vocabulary_review(review_table)` upserts into `vocabulary_overrides`.
- A **reviewed** flag per tree (all wordings reviewed) is derived. The kernel needs it to
  enable partition mode safely (§4.6, risk R2).

### 4.6 [E] Coverage kernel — stage 2 (R3)

```sql
tree_coverage(
    query_tree_id VARCHAR,
    candidates    := NULL,              -- LIST of tree_ids, or NULL → stage 1 top-k
    k             := 20,
    nodes_table := 'tree_nodes', attrs_table := 'tree_attributes',
    labels        := 'soft',            -- 'soft' (p(v)) | 'hard' (arg-max / reviewed)
    assignment    := 'auto',            -- 'best_match' | 'partition' | 'auto'
    lambda := 0.3, mu := 0.2,           -- label weight; attribute-name discount
    partition_gate := 0.3, count_gate := 0.5,
    stage1_weight := 0.05,              -- stage-1 score added as tie-break
    explain := true
) → TABLE(candidate_id, coverage, stage1_score, rank, n_requirements, n_gaps,
          reviewed BOOLEAN, assignment_used VARCHAR)

tree_coverage_trace(...same args...)
  → TABLE(candidate_id, req_node_id, matched_node_id, matched_path, label_sim,
          attr_name, req_value, prov_value, gate, name_sim, value_score, distance,
          attr_score, count_needed, count_available, req_score, is_gap)
```

**Scoring** (q = request item, c = candidate item, A(·) = attributes):

```
attr(x, y)  = gate(x, y) · (μ·s_name(x, y) + (1 − μ)) · value_satisfies(x, y).score
a(q, c)     = mean_{x ∈ A(q)}  max_{y ∈ A(c)} attr(x, y)
L(q, c)     = bhattacharyya(p(q), p(c))                   -- 1/0 with hard labels
score(q, c) = λ·L(q, c) + (1 − λ)·a(q, c)
counts      : cov_count(q) = min(1, Σ_{c : L(q,c) ≥ count_gate} n_c / n_q)
OR node     : max over children        AND node : compose (attributes: best instance; counts: sum)
coverage    = mean over request items q of req_score(q)
```

- `s_name` is the calibrated cosine of attribute-name vectors. If no vectors are given, it is
  a normalised token overlap, so the kernel also runs without embeddings. Names only
  discount (μ = 0.2), and never decide a match (BR-16).
- **Best match.** Each request item takes its best-scoring candidate item independently.
  This is robust to label noise, but it allows substitution: a different function can stand
  in for a missing one.
- **Partition.** Each candidate item c is assigned to `q*(c) = argmax_q L(q, c)` if
  `L ≥ partition_gate`. A request item is then covered only by its own block, with
  attributes from the best instance and counts summed over the block. An empty block is a
  gap. This prevents substitution (BR-06).
- **`auto`** (default) uses partition when the candidate tree is `reviewed`, and best match
  otherwise. This reproduces the study's best configuration and avoids its failure mode
  (risk R2).
- **Variants.** OR-subtrees are evaluated per request item. For a module-like OR tree,
  the best variant per requirement is chosen; counts never add across OR siblings.
- **Execution.** The C++ table function loads the query tree and the k candidate trees
  (anchors and attributes) into memory, builds per-tree indices `class → items` and
  `dimension → attributes`, and evaluates the formula above. Complexity per candidate is
  O(|items_q| · |items_c| · |attrs|), roughly 2 ms per pair in an unoptimised prototype; the
  C++ path targets ≤ 0.2 ms. The trace is produced from the same pass.

The **three hops** use one kernel:

| Hop | Query | Candidates | Notes |
|---|---|---|---|
| request → catalogue | request item | catalogue entries (templates without values) | values ignored; this is the pivot score with an attribute-template bonus |
| request item → offer item | request item | items of offer trees (e.g., module variants) | `tree_coverage` with a single-item query; counts reported as "instances needed" |
| request → past solution | full request tree | designs/solutions (stage 1 top-k) | full kernel |

### 4.7 [F] Stage 1 recall and scale (R1, R5)

```sql
coverage_recall(query_tree_id, k := 20, mode := 'auto', tie_break := 0.01, ...)
```

- Builds the query class multiset (§4.3) and calls `find_covering_entities`.
- `mode = 'auto'` uses **containment** if the candidate side is reviewed and **weighted
  Jaccard** otherwise. In the study, containment on unreviewed labels admitted designs that
  only looked complete through a mislabelled class, and lost 5.5 points (risk R1).
- **Sub-linear options** for large raw vocabularies (R5), stored as embedding tables in the
  same way as the existing `jaccard_embedding` MinHash:
  - weighted MinHash (ICWS) signatures for weighted Jaccard, with banding-based LSH;
  - partitioned containment LSH (LSH-Ensemble style: partition by set size, tune bands per
    partition) for containment queries;
  - both are optional accelerators; exact recall stays the reference and the default.

### 4.8 [G] Evaluation and calibration (R5)

| Function | Output |
|---|---|
| `eval_ranking(results, gold, k := [1,5,10,20])` | top-k accuracy, recall@k, MRR, mean regret, share with regret ≤ 0.05 |
| `eval_calibration(results, gold)` | MAE, bias, Pearson/Spearman of predicted vs true coverage |
| `threshold_at_precision(results, gold, target := 0.95)` | auto-accept threshold, coverage share, precision curve |
| `bootstrap_ci(metric_table, cluster_col, n := 1000, seed := 7)` | cluster bootstrap intervals (BR-51) |
| `fit_coverage_weights(reviewed_matches)` | λ, μ, β, τ grid search / logistic fit (C, BR-52) |

`gold` tables hold `(query_id, candidate_id, true_coverage)`. Regret is always computed
against the gold-best candidate.

### 4.9 [H] Entity clustering (R5)

```sql
cluster_matches(pairs_table, threshold := 0.8, left_col := 'a', right_col := 'b',
                score_col := 'score') → TABLE(entity_id, cluster_id, cluster_size)
```

- The core is a C++ union-find over pairs above the threshold, with no dependency (BR-42).
- **Optional DuckPGQ recipe** (documented, not called from macros): `CREATE PROPERTY GRAPH`
  over the pairs, then `weakly_connected_component(...)`. On DuckDB 2.0 this requires
  `SET active_grammar_extensions = ['duckpgq']` per connection. Because extension macros are
  registered at load time with the default grammar, no built-in macro embeds PGQ syntax.
- For hierarchical customer/supplier data, the property-tree model applies with
  **symmetric** semantics: identifiers (tax ID, DUNS, …) act as hard gates like dimensions,
  and container levels (group → legal entity → site) are transparent.

## 5. Defaults

All defaults were fixed a priori in the study and kept unless `fit_coverage_weights` is run.

| Parameter | Default | Where |
|---|---|---|
| λ (label weight in coverage) | 0.3 | `tree_coverage` |
| μ (attribute-name discount) | 0.2 | `tree_coverage` |
| β (attribute-template bonus in pivot) | 1.0 | `classify_to_catalogue` |
| τ (softmax temperature) | 0.3 | `classify_to_catalogue` |
| count gate (same-class threshold for summing counts) | 0.5 | `tree_coverage` |
| partition gate | 0.3 | `tree_coverage` |
| k (stage-1 depth) | 20 | `coverage_recall`, `tree_coverage` |
| stage-1 tie-break into stage 2 | 0.05 | `tree_coverage` |
| Jaccard tie-break under containment | 0.01 | `find_covering_entities` |
| value tolerance | 0.02 decades / 2 % | `value_satisfies` |
| point widening | ±0.01 decades | `value_satisfies` |
| soft band beyond the limit | 5 % | `value_satisfies` |
| auto-accept threshold | 0.9 (target ≥ 95 % precision) | guidance |

## 6. Non-functional design

| Concern | Design |
|---|---|
| Performance | Stage 1: inverted join + vectorised set arithmetic; stage 2: in-memory C++ on top-k only. Leaf vectors are cached in tables, never recomputed inside functions. |
| Compatibility | All new functions build against both the DuckDB 1.5.6 and 2.0 targets through the existing `duckdb_compat` / `compat_anofox` layers. Named arguments are documented in positional order. |
| Read-only databases | Search functions inline their SQL (as `find_similar_materials_jaccard` does), so they work where helper macros cannot be registered. |
| Security | Table and column names are validated with the existing helpers; no dynamic SQL from data values. |
| Determinism | No randomness except seeded sampling (`cosine_floor`, bootstrap, MinHash seeds). |
| Telemetry | Function-call telemetry as in the existing modules (respecting `TELEMETRY.md` opt-out). |
| Explainability | Every score has a traceable decomposition (`tree_coverage_trace`); gaps are explicit rows. |

## 7. Testing strategy

- **Unit (sqllogictest)** per function under `test/sql/coverage/`. Covers edge cases: empty
  sets, NULLs, unknown units, mixed dimensions, `at_most` limits, OR trees with one fitting
  variant, extensive counts, and the partition vs best-match difference.
- **Golden examples.** Small hand-built trees with hand-computed expected coverage and
  traces (e.g., the "requires ≥ 27 V, provided 40–48 V" case, where `at_least` passes and
  plain containment fails).
- **Synthetic benchmark (BR-53).** A seeded generator (`scripts/`, Python via `uv`) for a
  domain-neutral catalogue (~20 classes with attribute templates), a variant module library,
  a few hundred solution trees and requests with wording noise, unit changes, coarse/detailed
  granularity, and ~12 % deliberately unmeetable requirements. Accuracy tests assert the BRD
  §6 KPIs with tolerance, like `test/sql/accuracy/` today.
- **Regression of existing behaviour.** All current tests unchanged; `mode` defaults keep
  old outputs identical.
- **Both DuckDB targets** in CI, as today.

## 8. Release plan and work breakdown

| Release | Deliverables | Rough effort |
|---|---|---|
| **R1** | `tversky_similarity`, `containment`, weighted variants; `find_covering_entities[_for]`; `mode :=` on `find_similar_materials_jaccard`; feature-multiset column mapping; docs + tests | S (≈ 1–2 weeks) |
| **R2** | `units` seed; `parse_quantity`, `normalize_quantity`, `value_satisfies`, `compat_score`; golden tests | M (≈ 2–3 weeks) |
| **R3** | Tree tables, `tree_from_json`, `tree_anchors`, `validate_trees`, `tree_class_multiset`; `tree_coverage` + `tree_coverage_trace` (best match + partition + auto); `coverage_recall` | L (≈ 4–6 weeks) |
| **R4** | `classify_to_catalogue` (centroid + late interaction), `maxsim`, `bhattacharyya`, calibration helpers; `vocabulary_review_queue`, `apply_vocabulary_review`, `uncertain_decisions` | M–L (≈ 3–5 weeks) |
| **R5** | Evaluation and calibration functions; synthetic benchmark; weighted MinHash / containment LSH; `cluster_matches` + DuckPGQ recipe | M (≈ 3–4 weeks) |

R1 is useful on its own with existing BOM data. R2 and R3 together deliver the
differentiator, explainable coverage. R4 makes it robust on free text.

## 9. Explicit non-goals and rejected options

| Option | Decision | Reason |
|---|---|---|
| Whole-record embeddings, WL signatures or slot tensors as recall for coverage | Rejected | Lost 40+ points top-1 to set overlap; they pool away counts and ranges. They stay for symmetric similarity and predecessor search. |
| Embedding similarity for qualifiers/roles | Rejected | Lexical ≠ technical compatibility; use `compatibility_rules`. |
| Partition on unreviewed labels | Guarded by `auto` | −21 to −27 points on noisy labels. |
| Building on DuckPGQ for the kernel | Rejected | The kernel is joins, folds and numeric rules, not traversal. DuckPGQ is used only as an optional clustering/traversal accelerator. |
| LLM calls inside the extension | Out of scope | Keeps the extension deterministic; the hook exports candidates and margins. |
| MILP-based capacity assignment | Deferred | Greedy / min-cost flow may follow once the partition semantics are validated on real data. |

## 10. Open questions

1. **Vector storage format for late interaction.** Is `FLOAT[][]` per row acceptable in
   size, or should token vectors live in a side table `(node_id, token_idx, vec)`?
2. **Currency.** Do we ship a rate-table convention (`fx_rates(date, from, to, rate)`), or
   leave currency comparison to same-currency only?
3. **Capacity semantics.** Can one offer item legitimately serve two requests (shared
   capacity)? This must be validated with experts on real examples before going beyond
   partition.
4. **Catalogue mining.** In the study, a mined catalogue (clustering of extracted
   statements, seeded with known classes) matched the hand-made one for item matching but
   not yet for full-solution matching. Should a `mine_catalogue` helper be part of R5 or
   a later release?
5. **Default for `labels`.** `soft` keeps uncertain alternatives alive; `hard` is simpler to
   explain. The proposal is `soft` for stage 2 and `hard` (arg-max or reviewed) for stage 1.

## 11. Amendments (2026-10-25)

Decided with the product owner during R0 (HANDOVER §8 escalation; see
[REVIEW_LOG.md](REVIEW_LOG.md) R0-2). They change the semantics of §4.2 and §4.6 and take
precedence over the text above.

1. **Unqualified values take a default bound.** `value_satisfies` gains
   `default_bound := NULL` (a bound used when the requirement has none). `tree_coverage`
   and `tree_coverage_trace` gain `default_bounds := NULL | 'catalogue'` and
   `templates_table := 'catalogue_attributes'`. With `'catalogue'`, an unqualified
   requirement value gets the bound of the catalogue attribute template with the same
   class and dimension. If the template is missing or ambiguous, the widened-point rule of
   §4.2 applies. Without the option, behaviour is as in §4.2. Reason: about 30 % of values
   in real specifications have no bound word. Treating them as points cost regret and MAE
   beyond the BRD targets on the benchmark.
2. **Label agreement gates the value score.** The item score is
   `score(q, c) = λ·L(q, c) + (1 − λ)·L(q, c)·a(q, c)` with **default λ = 0**, i.e.
   `L·a`. With hard labels a same-class item scores `a`; an item of another class scores
   0. With soft labels the score is the expected share satisfied. A λ > 0 adds label
   credit that does not depend on the values. Reason: the BRD MAE KPI covers all stage-2
   results. The additive `λ·L + (1 − λ)·a` gave a candidate 0.3 per item even when every
   value failed, so the MAE over top-k stayed at about 0.07–0.12.
3. **Affine units use an absolute tolerance.** For temperature (stored in K) the
   tolerance is 1 K and the soft band 2.5 K, and `distance` is absolute, in K. A relative
   tolerance on Kelvin made "max. 37 °C" vs 45 °C score 0.88.
4. **One offer item serves one request block** (HANDOVER §7.3) also for ties. When
   several request items tie for `argmax_q L(q, c)` (e.g. a driver-seat and a
   passenger-seat requirement of the same class), the candidate item goes to the tied
   request item with the largest remaining count deficit, then to the lowest node id.
   Assignment stays at class level; seats or other scopes are context only.
5. **Stage-1 tie-break stays additive** (decided 2026-10-25): `score = measure +
   tie_break × jaccard`, which can exceed 1. A nearly complete partial cover can outrank a
   full but much larger cover. This is documented as a known limitation and pinned by a
   test; `min_score` applies to the measure itself.
6. **Conventions of the R1 functions** (not a semantic change; recorded so the tests have a
   source). Scalar parameters are named `query` and `candidate`. `weighted_tversky` defaults
   to α = β = 1, while the `tversky` mode of `find_covering_entities` defaults to α = 1,
   β = 0, as in §4.1. A zero denominator gives NULL, except that two empty inputs give 0.0
   (`containment` with an empty query is always NULL). In maps, a NULL weight counts as 1,
   weights must be finite and non-negative, and a zero weight means "absent".
   `weight_col := NULL` means unit weights. Ties are broken by `entity_id`. A negative or
   non-finite weight anywhere in the feature table raises an error.
