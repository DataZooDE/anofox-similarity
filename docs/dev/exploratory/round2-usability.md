# Exploratory user testing, round 2: usability and developer experience

Shells: DuckDB 1.5.6 (`build/release/duckdb`, extension linked in) and DuckDB 2.0
(`anofox-similarity-v2/build/release/duckdb -unsigned -cmd "LOAD '…/anofox_similarity.duckdb_extension'"`),
both with `DATAZOO_DISABLE_TELEMETRY=1`, run from the repository root. Every repro below gives
the same output on both shells unless noted otherwise.

Personas played:

- (a) **Planner**: find analogs and a predecessor for a new product using README + guides 01–03 on
  my own data (8 families × 5 variants, two-level BOMs with quantities, ~12 000 irregular movements
  on about a third of the days, 676 returns with negative quantities, a 200-day stock-out gap,
  a phase-out/phase-in pair, then a clean cut-over pair).
- (b) **Bid engineer**: a hand-written German customer spec as JSON (containers, counts, a
  `variant_group` with two `OR` alternatives, German value texts), loaded with `tree_from_json`
  from a file via `read_text`, ranked against the mini archive with `tree_coverage`, explained with
  `tree_coverage_trace`; then 45 German value notations through `parse_quantity`.
- (c) **Scale** (short): 100 000 BOMs × 20 parts out of 200 000 part numbers (2 M rows).
- (d) **Reviewer** (short): every SQL block of README and guides 01–08 run on both shells and
  compared with the documented tables; all 17 examples (`scripts/run_examples.sh`) on both shells.

Severity: **high** = a realistic input gives a wrong result without any error; **medium** = a
surprising result or error that costs a user real time; **low** = polish.
Status: **FIXED-DOC** = fixed in this change, the changed SQL re-run on 1.5.6 and 2.0;
**CODE** = needs a C++ change (not touched).

No crash and no INTERNAL error. `SELECT 1` worked after every error.

---

## High

### R2U-1 `parse_quantity` ignores common German bound words: `mind.`, `ab`, `größer`, `kleiner gleich` — CODE + FIXED-DOC

`mind.` is the usual abbreviation of *mindestens* in German specifications. The value becomes a
point, so an offer that exceeds the minimum is a gap.

```sql
SELECT t, parse_quantity(t, 'de').bound AS bound
FROM (VALUES ('mind. 6 cm'), ('ab 6 cm'), ('größer 6 cm'), ('kleiner gleich 8 A'),
             ('min. 6 cm'), ('mindestens 6 cm'), ('≥ 6 cm')) v(t);
-- NULL, NULL, NULL, NULL, at_least, at_least, at_least

SELECT value_satisfies(parse_quantity('mind. 6 cm', 'de'), parse_quantity('7 cm')).score;  -- 0.0 (rule point)
```

In my spec (`{"name": "Höhenverstellweg", "text": "mind. 6 cm"}` through `tree_from_json`), the
attribute came out with `bound` NULL and no warning from `validate_trees`.

- Actual: no bound; 7 cm against "mind. 6 cm" scores 0.
- Expected: `at_least`, score 1.
- Fix: add `mind.`, `mind`, `ab`, `größer`, `größer als`, `größer gleich`, `über` (at_least) and
  `kleiner`, `kleiner als`, `kleiner gleich`, `unter` (at_most) to the word lists in
  `src/modules/quantity.cpp:766-773`. Consider a `validate_trees` warning `bound_word_not_parsed`
  when the text holds a known bound-like token and `bound` is NULL. Docs now list the gap and
  the workaround (guide 05, "Parse a Quantity" and Pitfalls).

### R2U-2 A space as the thousands separator turns `1 000 N` into the count 1 — CODE + FIXED-DOC

DIN 1333 and most German documents write `1 000` (often with a narrow no-break space).

```sql
SELECT q.lo, q.unit, q.dimension FROM (SELECT parse_quantity('1 000 N', 'de') AS q);
-- 1.0 | 1 | count
SELECT value_satisfies(parse_quantity('1 000 N', 'de'), parse_quantity('1000 N')).rule;  -- gate
```

- Actual: lo = 1, dimension `count`; every comparison is a gate.
- Expected: 1000 N, force.
- Fix: accept a space, U+00A0 or U+202F between digit groups of exactly three digits. Documented
  in guide 05 until then.

## Medium

### R2U-3 `infer_predecessors` never finds a clean cut-over — CODE/design + FIXED-DOC

The most common ERP succession is a cut-over: the old material stops one week, the new one starts
the next at the same demand. There is no ramp, so the correlation is noise.

```sql
CREATE TABLE cutover_bom AS SELECT * FROM (VALUES
  ('PUMP-V1', 'SEAL-001'), ('PUMP-V1', 'BEARING-X'), ('PUMP-V1', 'MOTOR-M1'), ('PUMP-V1', 'HOUSING-1'),
  ('PUMP-V2', 'SEAL-001'), ('PUMP-V2', 'BEARING-X'), ('PUMP-V2', 'MOTOR-M1'), ('PUMP-V2', 'HOUSING-2'))
  t(parent_id, child_id);
CREATE TABLE cutover_movements AS
SELECT 'PUMP-V1' AS material_id, DATE '2022-01-03' + 7 * w::INTEGER AS movement_date, 100.0 + (w * 7) % 11 - 5 AS quantity
FROM range(0, 78) t(w)
UNION ALL
SELECT 'PUMP-V2', DATE '2022-01-03' + 7 * w::INTEGER, 100.0 + (w * 5) % 11 - 5 FROM range(78, 130) t(w);
SELECT count(*) FROM infer_predecessors('PUMP-V2', bom_table := 'cutover_bom', movements_table := 'cutover_movements');  -- 0
SELECT confidence, correlation, overlapping_weeks
FROM infer_predecessors('PUMP-V2', bom_table := 'cutover_bom', movements_table := 'cutover_movements', min_confidence := 0.0);
-- 0.407 | -0.0601 | 8
```

With white-noise demand (my first try, `hash()`-based) even `min_confidence := 0.0` returned no
row, because a positive correlation is filtered out (`predecessor_inference.cpp:205`).

- Actual: no predecessor, although BOM similarity is 0.6 and the old material stopped 7 days
  before the new one started.
- Expected: a candidate, or at least a documented way to find one.
- Fix (code): add a cut-over signal: when the candidate's last usage lies within a few weeks before
  the successor's start and its level is comparable, score the pair without requiring a negative
  correlation (or return the row with `correlation` NULL and a `signal` column). Fix (doc, done):
  guide 03 has a "Clean cut-over" pitfall with the repro and a verified SQL recipe
  (`find_similar_materials_jaccard` + last movement just before the successor's first).

### R2U-4 A forgotten `attrs_table` or `candidate_attrs_table` silently changes every score — CODE + FIXED-DOC

`attrs_table` defaults to `tree_attributes` even when `nodes_table` is given, and
`candidate_attrs_table` defaults to `attrs_table`. A user who loads a spec from JSON into
`spec_nodes`/`spec_attrs` and forgets one parameter gets plausible numbers:

```sql
-- after guide 04 up to "CREATE TABLE spec_attrs"
CREATE TABLE programme_nodes AS SELECT * FROM tree_nodes WHERE tree_id LIKE 'S%';
SELECT candidate_id, coverage FROM tree_coverage('NEW-1', k := 3, nodes_table := 'spec_nodes',
  candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'tree_attributes', mu := 0.0);
-- S2 1.0 (correct: 0.5): the query's values were read from tree_attributes, which has no NEW-1 rows
SELECT candidate_id, coverage FROM tree_coverage('NEW-1', k := 3, nodes_table := 'spec_nodes', attrs_table := 'spec_attrs',
  candidate_nodes_table := 'programme_nodes', mu := 0.0);
-- S1 0.5 (correct: 1.0): the candidates' values were read from spec_attrs
```

- Fix (code): when `nodes_table` is given and `attrs_table` is not, do not fall back to
  `tree_attributes` silently: either require it or treat it as `NULL` (no attributes) and say so;
  error when the query trees have zero attribute rows in an explicitly defaulted table while the
  candidates have many. Default `candidate_attrs_table` to the companion of `candidate_nodes_table`
  only when that is the default too. Fix (doc, done): guide 04 says "Name all four tables",
  shows both wrong numbers, and has a Pitfalls row.

### R2U-5 A value inside a `von … bis` range scores 0.51 — design + FIXED-DOC

```sql
SELECT value_satisfies(parse_quantity('von 22 bis 26 cm', 'de'), parse_quantity('25 cm')).score;  -- 0.51
```

`within` means "the offer covers the whole required range" (adjustment travel). For an acceptable
band ("Gewicht zwischen 4 und 5 kg") users expect 1. The trace shows `is_gap = true` for a
compliant offer, which the bid engineer reads as a failure.

- Fix (code, optional): a `range_mode := 'cover' | 'inside'` option or per-template default in
  `catalogue_attributes`. Fix (doc, done): guide 05 explains `within` and shows the two-limit
  workaround (as_limits = 1.0 for 25 cm, verified).

### R2U-6 `tree_from_json` silently ignores unknown keys; a typo drops a subtree — CODE + FIXED-DOC

```sql
SELECT len(tree_from_json('{"root": {"node_id": "r", "childs": [{"node_id": "a"}]}}'));  -- 1, no error
```

Hand-written JSON is exactly where `"childs"`, `"attribute"`, `"value"` or `"Count"` happen.

- Fix (code): error (or a warning row via `validate_trees`) for unknown keys on node and attribute
  objects, naming the key and the node; at least for `childs`/`children`-like and
  `attribute`/`attributes`-like near misses. Fix (doc, done): guide 04 "From JSON" says so and
  tells the user to compare node counts.

### R2U-7 A text `movement_date` gives an internal `date_part` binder error — CODE + FIXED-DOC

```sql
CREATE TABLE bom_items AS SELECT * FROM (VALUES ('A', 'x'), ('B', 'x')) t(parent_id, child_id);
CREATE TABLE mv AS SELECT * FROM (VALUES ('B', '20230115', 5.0)) t(material_id, movement_date, quantity);
SELECT * FROM cold_start_analogs('A', 3, movements_table := 'mv');
-- Binder Error: No function matches ... 'date_part(STRING_LITERAL, VARCHAR)' ... (14 candidate lines)
```

SAP extracts deliver `BUDAT` as `YYYYMMDD` text. The error names neither the parameter nor the
column. The missing-column check (round 1) is good: `movements_table 'mseg' is missing required
column(s): material_id, movement_date, quantity`.

- Fix (code): extend `ValidateTableColumns` to check the type: "movements_table 'mv': column
  movement_date is VARCHAR, expected DATE or TIMESTAMP (convert with strptime(movement_date,
  '%Y%m%d')::DATE)". Same for `infer_predecessors`. Fix (doc, done): guide 03 shows a mapping view
  (verified: `cold_start_analogs` through the view returns 3 rows) and a Pitfalls row.

## Low

### R2U-8 `history_months` is a calendar span, not months with data — FIXED-DOC

```sql
CREATE TABLE bom_items AS SELECT * FROM (VALUES ('A', 'x'), ('B', 'x')) t(parent_id, child_id);
CREATE TABLE two AS SELECT * FROM (VALUES ('B', DATE '2021-01-15', 5.0), ('B', DATE '2023-01-10', 5.0)) t(material_id, movement_date, quantity);
SELECT history_months FROM cold_start_analogs('A', 1, movements_table := 'two');  -- 24
```

Two movements count as 24 months, and pass `min_history_months := 12`. The guide advised
"prefer a long `history_months` when you need seasonality". Also a material with only returns
(600 days of `-5.0`) qualifies as an analog with 19 months of history.
Fix (doc, done): guide 03 and `docs/api/02-material-search.md` define the column. Suggested code
change: add `months_with_movements` and ignore materials whose net consumption is ≤ 0.

### R2U-9 `n_gaps` counts items, the trace lists attributes — FIXED-DOC

README quick start: S2 has `n_gaps = 2` and the next query lists 3 gap rows. Fixed in the README
and guide 04 ("`n_gaps` counts requirements (items), not attributes").

### R2U-10 Missing German/imperial units — CODE

`3 Sek.` (unit `Sek`, dimension NULL), `1200 U/min` (NULL), `3 in`, `3 inch`, `3 Zoll` (NULL),
and `3 "` parses as the count 3. Each becomes a gate. Suggested: add `Sek`, `Sek.`, `U/min`,
`1/min`, `rpm`, `in`, `inch`, `Zoll`, `"` to the unit seed.

### R2U-11 DuckPGQ recipe does not run on DuckDB 2.0 — FIXED-DOC

`INSTALL duckpgq FROM community` returns HTTP 404 on the 2.0 shell; on 1.5.6 the recipe runs. Guide
08 now says so.

### R2U-12 On DuckDB 2.0 a binder error hides the named parameters — CODE/upstream

`SELECT * FROM cold_start_analogs('X', k := 3)` lists the candidate as
`cold_start_analogs(material_id VARCHAR, k BIGINT, /, **"options" ANY)` on 2.0, while 1.5.6 lists
`min_history_months`, `min_similarity`, `bom_table`, `movements_table`. The user cannot discover the
options from the error on 2.0. Suggested: a hint in the binder error, or check whether the 2.0
`TableFunction` registration can keep the typed named parameters.

### R2U-13 One unknown id fails the whole batch — design

`tree_coverage(['Q1', 'NOPE'], …)` errors for the batch. For 80 requests, one stale id stops all.
Suggested: an `on_missing := 'error' | 'skip'` option, or name all missing ids in the error.

### R2U-14 `infer_predecessors` only looks at the top 100 Jaccard neighbours — FIXED-DOC

Hard-coded `find_similar_materials_jaccard(…, 100, …)` in `predecessor_inference.cpp:113`. With
many near-identical variants the true predecessor can fall outside. Documented in guide 03.

---

## Round-1 fixes checked (not re-reported)

| Round-1 item | Check | Result |
|---|---|---|
| Unknown query id (F1/F4) | `tree_coverage`, `tree_coverage_trace`, `coverage_recall` with `'NOPE'` and with `['Q1','NOPE']` | error naming the id and table, on both shells |
| No classified items | guide 04 repro | error with the three remedies |
| Missing column | `bom_table := 'stpo'`, `movements_table := 'mseg'` | clear error listing the expected columns |
| Empty MAP / NULL weights | `weighted_jaccard(MAP {}, …)`, `weighted_containment(NULL, …)`, `(…, MAP {})` | 0.0, NULL, 0.0, no error |
| Tie order | Jaccard ties FG-03-0/FG-03-1, WL ties Y/Z/W, covering-entity ties P021/P074 | ordered by id, stable on both shells |
| E2 WL unknown id | `find_similar_materials_wl_kernel('NOPE', 3)` | 0 rows |
| F9 dimension mismatch | 2-dim items vs 3-dim catalogue | error naming both dimensions |
| F10 `IP 54`, `KG`, `STK` | `parse_quantity` | normalised (but see R2U-1 for `mind. IP 54`: category IP54, bound NULL) |
| WL kernel at scale (known issue) | WL search over 100 000 parents | 1.0 s, no crash |

## Scale measurements (1.5.6, 32 threads)

100 000 BOMs × 20 parts from 200 000 part numbers (1 999 964 rows):

| Step | Time |
|---|---:|
| `minhash_index` | 2.98 s |
| `minhash_bands` | 1.69 s |
| `find_covering_entities_lsh`, 200 queries, k 3, threshold 0.8 | 0.60 s (199/200 sources at rank 1, 1.44 candidates per query) |
| `find_covering_entities_for`, one exact query | 0.09 s |
| `find_similar_materials_jaccard` / `_wl_kernel` over 100 000 parents | 0.55 s / 1.0 s |
| `cluster_matches`, 200 000 pairs | 0.59 s |

Peak RSS of the whole script: 5.4 GB. The guide's numbers (20 000 BOMs) scale as stated; the LSH
recall dropped from 100/100 to 199/200 at five times the archive, as expected for an estimate.

## Documented vs actual output

All SQL blocks of README and guides 01–08 give the documented tables on 1.5.6 and 2.0. The only
differences between the shells are the documented scalar binding (`tversky_similarity` with
swapped named arguments: 0.5 on 1.5.6, 1.0 on 2.0) and the DuckPGQ download (R2U-11). The 17
examples pass on both shells, and the `-- Expected:` comments I compared (coverage_matching,
classification_review) match.

## Verdict

- **Planner (a): reached the goal on dense data.** Analogs and the predecessor were found on
  irregular movements with returns and gaps (FG-03-1, confidence 0.66). Blocked on the realistic
  clean cut-over (R2U-3) until the recipe in guide 03; slowed by the `date_part` error for SAP text
  dates (R2U-7). `history_months` would have misled the forecast choice (R2U-8).
- **Bid engineer (b): reached the goal, but with a wrong gap list.** The JSON loaded, alternatives
  and counts worked, the ranking and trace were readable. Two of six values were judged wrongly
  without any signal: `mind. 6 cm` (R2U-1) and the `von 22 bis 26 cm` range (R2U-5). R2U-2 would
  have hit any force or cycle count over 999. The silent table defaults (R2U-4) are the easiest
  way to get a believable but wrong ranking.
- **Data scientist (c): yes.** Everything at 100 000 entities ran in seconds.
- **Reviewer (d): yes.** Docs match the shells.

The biggest remaining risk is silent: German value texts that parse without error into the wrong
direction or magnitude. A `validate_trees` warning for unparsed bound words and suspicious numbers
would catch R2U-1 and R2U-2 in one place.
