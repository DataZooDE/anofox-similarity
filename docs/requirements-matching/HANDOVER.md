# Handover — Implementing Coverage Matching (R0–R5)

This is the working brief for the coding agent that implements the feature set described in
[BRD.md](BRD.md) and [HLD.md](HLD.md). Read both documents fully before starting. This file
adds **how** to work: test dataset, TDD discipline, definition of done, review cadence, and
the decisions already taken on the HLD's open questions.

---

## 1. Mission

Implement releases **R0 → R5** in order:

- **R0** — test dataset and generator (this file, §3);
- **R1–R5** — as in HLD §8.

Each release is one pull request against `main`, from a branch `feat/coverage-r<N>-<slug>`.
Do not start a release before the previous PR is green in CI.

Work **strictly red → green → refactor** (§4). The **definition of done** is a suite of
realistic, SQL-only tests that read like what a user of the extension would write (§5).

## 2. Ground rules

1. **Domain neutrality.** Code, function names, docs, comments, test names and data must stay
   generic: "request", "offer", "catalogue", "class", "item". The test dataset uses
   **automotive seating systems** as its example domain (§3). Use only this domain for
   examples, and do not name real companies or real customer programmes. All company and programme names in the dataset are fictional.
2. **No breaking changes.** Existing functions keep their names, signatures, defaults and
   outputs. All existing tests stay green.
3. **Both DuckDB targets.** Everything builds and passes on DuckDB **1.5.6** and **2.0**
   (see the CI matrix in `.github/workflows`). On 1.5.6, named arguments of scalar functions
   bind by position: document and test them in the documented order.
4. **Follow existing patterns.**
   - Search functions use `bind_replace` + inlined SQL (see `src/modules/similarity_search.cpp`),
     so they work on read-only databases.
   - Validate identifiers and columns with `ValidateSQLIdentifierPath` / `ValidateTableColumns`.
   - Every function gets a `ddoc::Doc()` description + example and a telemetry hook.
   - Hot kernels are C++ scalars; stage 2 (`tree_coverage`) is a C++ table function working
     in memory on the top-k.
5. **Python only via `uv`** (`scripts/pyproject.toml` already exists): `uv add`, `uv run`.
   No bare `pip`.
6. **No model downloads in tests.** Tests use the deterministic synthetic vectors from the
   dataset (§3.6), never a live embedding model or an LLM.
7. **Run tests with telemetry off:** `DATAZOO_DISABLE_TELEMETRY=1 make test` (see
   `Makefile`).

## 3. R0 — Test dataset: automotive seating systems

### 3.1 Story

A fictional seating supplier, *Sitzwerk Demo GmbH*, receives seat-system specifications
from fictional vehicle makers. It answers with modules from its library and reuses past seat
programmes. Its master data also holds the vehicle makers' corporate hierarchies (group →
legal entity → plant), with duplicates.

The dataset exercises every BRD requirement:

| Exercised feature | How the data does it |
|---|---|
| Pivot / catalogue | ~22 seat functions with definitions and attribute templates |
| AND / OR trees | Base modules → configurations → applications (variants) |
| Extensive counts | E.g., "4 × massage zones", "2 × heated zones", "6-way vs 10-way adjust" |
| Direction-aware values | Lower-is-better limits (noise, mass, current draw, cost, time-to-temperature) next to at-least limits (load, durability cycles, travel) |
| Units | N/kN, mm/cm, W, s/min, dB(A), kg/g, A/mA, °C, cycles, EUR |
| Ordinal compatibility | Flammability class, abrasion class (Martindale cycles), protection class (IPxx) |
| Wording noise | EN/DE, supplier jargon, coarse vs detailed specs, section headings |
| Internal vocabulary | ~50 internal wordings, of which 3–4 are systematically misleading (for the review queue) |
| Unmeetable requests | ~12 % of requirements stretched beyond any module |
| Dedup | Customer master with planted duplicates across the hierarchy |

### 3.2 Catalogue (≈ 22 classes)

Suggested classes, each with a one-line definition and 2–4 attribute templates
(name + dimension):

seat track / longitudinal adjuster · height adjuster · recliner · lumbar support ·
side bolster adjust · cushion length extension · headrest (manual / powered) ·
seat heating · seat ventilation · massage · seat memory module · seat motor drive ·
easy-entry mechanism · fold-flat / 2nd-row tumble · seat-belt buckle presenter ·
occupant detection mat · side airbag integration · armrest · trim cover · foam pad ·
seat frame / structure · wiring and connectors.

Typical attribute templates:

- track: travel (mm, `at_least`), adjust speed (mm/s, `within`), noise (dB(A), `at_most`),
  holding load (kN, `at_least`);
- heating: power (W, `within`), time to 37 °C (s, `at_most`), current draw (A, `at_most`),
  zones (count);
- frame: mass (kg, `at_most`), crash load (kN or g, `at_least`);
- trim: abrasion (Martindale cycles, `at_least`), flammability (ordinal class).

### 3.3 Module library (OR-trees)

- ≈ 40 base modules → ≈ 90 configurations → ≈ 150 applications. An application fixes the
  provided ranges.
- Include 4–5 **combo modules** that provide several classes at once (e.g., a comfort module
  with heating + ventilation + massage). Their counts must add up across classes.
- Module-side wordings come from a **closed internal vocabulary** (§3.5).

### 3.4 Past seat programmes ("solutions") and requests

- **≈ 300 past programmes** (front row / 2nd row / 3rd row, economy / mid / premium, model
  years 2012–2026, 8 fictional vehicle makers). Each is a set of module applications (AND).
  About 40 % are **capability-only** (no module list, only classes and values), as old
  archives often are.
- **≈ 80 request specifications** (~1 500 requirements in total), each derived from a hidden
  source programme:
  - wording EN or DE (≈ 50/50), customer units (mm vs cm, mA vs A, min vs s);
  - coarse (most values omitted) vs detailed;
  - hierarchy spec → chapter → section → requirement, with section headings, intro prose and
    notes; requirement depth varies;
  - ≈ 12 % of requirements stretched beyond the source (some unmeetable by any module);
  - ≈ 30 % of values **unqualified** ("noise 38 dB(A)" with no bound word);
  - several values per sentence in some requirements.
- **Held-out phrasing pool.** For each class, 18 phrasings (EN + DE, jargon, compounds,
  short/long forms), split 9 development / 9 test. Test phrasings must never appear in
  development data. This is what makes the pivot accuracy test honest.

### 3.5 Internal vocabulary

- ≈ 50 distinct internal wordings for module and programme items (e.g., "Längsversteller",
  "LV-Schiene", "Komfortpaket Klima", "Sitzklima-Modul", "Beifahrer-Belegungsmatte").
- Plant **3–4 misleading wordings** whose surface form points to the wrong class (e.g., a
  ventilation fan module named "Klimapaket Heizung" that is read as heating). They must
  occur often enough to affect **> 60 %** of past programmes. The vocabulary review queue
  must rank them at the top.

### 3.6 Deterministic synthetic vectors (no model)

Generate vectors so that every embedding-dependent algorithm has a measurable effect:

- dimension 64, L2-normalised, `FLOAT[]`;
- `vec = normalise(class_centroid + phrasing_offset + source_boilerplate + noise)`:
  - `source_boilerplate` is a strong vector shared by all items of the same source
    ("The seat shall provide …"). Without per-source mean-centring it creates hubs; with
    centring, accuracy must rise measurably. This is a test.
  - Misleading internal wordings sit closer to the wrong class centroid.
  - Calibrate the noise so that raw-cosine top-1 is ≈ 0.6 and centring + z-score gets
    ≈ 0.85 on dev phrasings.
- **Token vectors** (`FLOAT[][]`, 3–12 tokens per text) for late interaction. Compounds
  share tokens with several exemplars of the right class, so `maxsim` beats single-vector
  matching on held-out phrasings.
- Attribute-name vectors with a high raw-cosine floor (≈ 0.85) between unrelated names, so
  `cosine_floor` / `calibrated_cosine` are needed.

### 3.7 Customer master (dedup)

- ≈ 2 000 records: 8 fictional vehicle-maker groups → legal entities → plants and
  purchasing contacts.
- ≈ 8 % planted duplicates: spelling variants, umlaut transliteration (ü/ue), legal-form
  variants (GmbH / G.m.b.H.), moved plants, shared tax IDs, and a few **near-miss
  non-duplicates** (same street, different company).

### 3.8 Ground truth and files

- Ground truth comes from an **oracle** that sees class ids and exact values. The matchers
  never see them. Provide:
  - gold class per item;
  - gold best application(s) per requirement;
  - oracle coverage for every (request, programme) pair needed for top-k / regret;
  - gold duplicate clusters.
  Oracle semantics are **direction-aware** (`at_least` / `at_most` / `within`).
- Generator: `scripts/src/.../generate_coverage_data.py`, run with
  `uv run generate-coverage-data --seed 7 --output test/data/coverage/`. It must be
  deterministic: re-running produces byte-identical files.
- Output: gzipped CSV (vectors as JSON arrays), plus one JSON file per request tree for
  testing `tree_from_json`. Keep the total size **≤ 8 MB**. Add a `README.md` in
  `test/data/coverage/` with the schema of every file and the oracle semantics.
- Also add a **tiny hand-made fixture** (`test/data/coverage/mini/`, ~3 requests,
  ~10 modules, ~6 programmes) whose expected scores are computed by hand in the test files.
  Unit-level tests use the mini fixture; accuracy tests use the full set.

## 4. TDD discipline (red → green → refactor)

For every function and behaviour:

1. **Red.** Write the sqllogictest(s) first, under `test/sql/coverage/<release>/`. Run them,
   see them fail for the right reason (missing function or wrong value, not a typo). Commit
   with the message prefix `test(red):`.
2. **Green.** Write the minimal implementation that passes. Commit with `feat(green):`.
3. **Refactor.** Clean up with all tests green. Commit with `refactor:`.

Rules:

- Never write implementation code without a failing test that demands it.
- Never weaken or delete a test to get green. If a test is wrong, fix it in a separate,
  explained `test:` commit, and mention it in the review log (§6).
- Expected values in unit tests are **hand-computed** in a comment above the query (e.g.,
  `# containment = (min(3,2)+min(1,1)) / (3+1) = 0.75`), never copied from the output of the
  implementation.
- Accuracy tests assert KPI floors with a tolerance (as `test/sql/accuracy/` does today).
  They do not assert exact numbers.

## 5. Definition of done

A release is done when **all** of the following hold:

1. **Realistic user-workflow tests** exist and pass. They are SQL scripts that load the
   dataset and do what a user would do, end to end. They are not just function probes. Each
   release ships at least 5 such scenario tests, named `uc_<nn>_<story>.test`. Examples
   follow below.
2. **Edge-case tests** pass: NULLs, empty sets, unknown units, mixed dimensions, zero
   counts, OR trees with exactly one fitting variant, `at_most` limits, read-only database,
   invalid identifiers (error message checked), named arguments in documented order on
   1.5.6.
3. **KPI tests** (from R3 on) pass at the BRD §6 targets on the full dataset with reviewed
   vocabulary. They also show the documented failure modes: containment on unreviewed
   labels is worse than Jaccard; forcing partition on unreviewed labels is worse than best
   match.
4. **The whole existing suite** is green on DuckDB 1.5.6 and 2.0.
5. **Docs updated:** `docs/API_REFERENCE.md` (signature, parameters, example), `README.md`
   feature table, and the test README navigation table.
6. **Agent-crew review passed** (§6), with findings resolved or explicitly deferred in the
   review log.
7. CI green on the PR.

### Example scenario tests (tone and depth expected)

```sql
# name: test/sql/coverage/r1/uc_01_which_programmes_contain_request.test
# description: A sales engineer looks for past seat programmes that provide at least
#              everything a new request asks for (counts included), leanest first.
require anofox_similarity

statement ok
CREATE TABLE prog_classes AS
SELECT * FROM read_csv_auto('test/data/coverage/programme_class_multiset.csv.gz');

# Request: 2 heated zones, 4 massage zones, 1 powered headrest, 1 10-way memory module
query TRR
SELECT entity_id, round(score, 3), round(jaccard, 3)
FROM find_covering_entities_for(
       MAP {'heating': 2, 'massage': 4, 'headrest_powered': 1, 'memory': 1}, 3,
       mode := 'containment', feature_table := 'prog_classes')
ORDER BY score DESC, jaccard DESC;
----
<hand-checked rows>
```

```sql
# name: test/sql/coverage/r2/uc_03_noise_limit_is_at_most.test
# description: A request says "adjust noise max. 38 dB(A)". A track at 35 dB(A) passes,
#              one at 41 dB(A) fails and reports how far off it is.
query TRR
SELECT application_id, v.score, round(v.distance, 3)
FROM applications a,
     LATERAL (SELECT value_satisfies(parse_quantity('max. 38 dB(A)'),
                                     {'lo': a.noise_lo, 'hi': a.noise_hi, ...}) AS v)
WHERE a.class = 'seat_track'
ORDER BY application_id;
----
...
```

Other R2–R5 stories to cover, at minimum:

- "Spec in German with cm and mA; find the application that meets every limit."
- "Coarse request without values: coverage counts classes only."
- "Show me the gap list for the top programme." (`tree_coverage_trace`, `is_gap = true`
  rows, with failing attribute and distance)
- "Ventilation module mislabelled as heating: before review the wrong programme wins;
  after confirming the top 3 queue entries the right one wins."
- "Classify 200 unseen requirement phrasings; send the 15 % lowest-margin ones to an
  external reviewer (JSON export), write decisions back, re-rank."
- "Evaluate my pipeline: recall@20, MRR, regret, threshold for 95 % precision."
- "Merge duplicate customer plants: cluster pairs above 0.85, check the planted
  duplicates are found and the near-miss is not merged."

## 6. Review cadence with the agent crew

Use the **agent crew** (the local multi-agent review setup) regularly, not only at the end:

| Checkpoint | What the crew reviews | Focus |
|---|---|---|
| After R0 dataset | Generator + data README + sample rows | Realism, determinism, domain neutrality, oracle correctness (esp. `at_most`), leakage between dev and test phrasings |
| After each **red** batch | New tests only | Do they encode the BRD/HLD? Are expected values hand-derived? Realistic user stories? Missing edge cases? |
| After each **green** step of a non-trivial function | Diff | Correctness, existing-pattern conformity, SQL-injection safety, 1.5.6/2.0 compat, performance of hot paths |
| Before each PR | Whole release diff + docs | DoD checklist (§5), API consistency across functions, naming, docs accuracy |

- Record each review in `docs/requirements-matching/REVIEW_LOG.md`: date, checkpoint,
  findings, resolution (fixed / deferred with reason).
- Act on every blocking finding before moving on.
- If the crew and the HLD disagree, follow the HLD and log the disagreement. Escalate to
  the human if it affects semantics.

## 7. Decisions on HLD open questions

These are taken, so they do not block you:

1. **Token vectors.** `FLOAT[][]` column on the item row (simple; the dataset is small). Keep
   `maxsim` agnostic, so a side table can be added later.
2. **Currency.** Same-currency comparison only. A different currency gives `rule = 'gate'`.
   An optional `fx_table :=` parameter may be added in R2 if cheap.
3. **Capacity.** Partition semantics only (one offer item serves one request block).
   No capacity-constrained assignment in R1–R5.
4. **Catalogue mining.** Out of scope.
5. **Labels.** `soft` in stage 2, `hard` (arg-max or reviewed) in stage 1.

## 8. When to stop and ask the human

- A BRD KPI cannot be reached on the dataset without changing scoring semantics.
- A change would alter the output of an existing function.
- A DuckDB 1.5.6 vs 2.0 difference forces divergent behaviour.
- The dataset size budget or CI runtime (> +5 min) would be exceeded.

## 9. Order of work (checklist)

- [ ] R0: generator + full dataset + mini fixture + data README → crew review → PR
- [ ] R1: set measures, `find_covering_entities[_for]`, `mode :=` on the existing search → PR
- [ ] R2: units seed, `parse_quantity`, `normalize_quantity`, `value_satisfies`, `compat_score` → PR
- [ ] R3: tree tables, `tree_from_json`, `tree_anchors`, `validate_trees`, `tree_class_multiset`,
      `tree_coverage`, `tree_coverage_trace`, `coverage_recall` → PR
- [ ] R4: `classify_to_catalogue`, `maxsim`, `bhattacharyya`, `calibrated_cosine`, `cosine_floor`,
      `vocabulary_review_queue`, `apply_vocabulary_review`, `uncertain_decisions` → PR
- [ ] R5: evaluation and calibration functions, weighted MinHash / containment LSH,
      `cluster_matches` + DuckPGQ recipe doc → PR
