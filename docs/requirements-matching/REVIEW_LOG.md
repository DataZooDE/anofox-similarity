# Review Log — Coverage Matching (R0–R5)

Agent-crew reviews at the checkpoints of [HANDOVER.md](HANDOVER.md) §6. Each entry: date,
checkpoint, crew, findings, resolution. Raw reports live under `.crew/runs/` (not committed).

Crew backends: codex, claude, muse. agy is not usable headless on this machine. Muse ran out
of quota on 2026-10-25, so later runs that day use codex and claude only.

---

## R0-1 — 2026-10-25 — after R0 red batch (generator tests)

- **Crew:** testing (muse, failed with 429), correctness (claude), design (codex), pragmatist
  (muse, failed with 429). Verdict: REQUEST_CHANGES.
- **Unit:** `scripts/tests/test_coverage_generator.py` at 92276c4.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | The oracle test uses only point ranges, so it cannot catch a p_lo/p_hi mix-up for at_most/at_least | Fixed in e59f126: non-degenerate ranges, limits, equals boundary, partial-overlap within, ordinal |
| F2 | high | The feasibility guard uses template bounds instead of the HLD rule for unqualified values, and recall@20 and precision@0.9 are missing | Fixed in e59f126: the guard follows the HLD (unqualified → point), uses reviewed labels and asserts all six BRD §6 KPIs. If it fails → HANDOVER §8 |
| F3 | high | gold_coverage is circular; duplicate or missing pairs go unnoticed | Fixed in e59f126: pairs unique and complete; 300 sampled pairs plus every source pair recomputed by an oracle written in the test |
| F4 | high | The determinism check runs in one process, so it misses hash-seed effects | Fixed in e59f126: two subprocess runs with PYTHONHASHSEED 1 and 2; gzip mtime asserted 0 |
| F5 | high | Nothing shows that a stretched value is harder for its source | Fixed in e59f126: gold carries the source range; stretched ⇔ the source fails |
| F6 | med | The unmeetable cutoff of 0.90 conflicts with the BRD flag threshold (< 0.95) | Fixed in e59f126: unmeetable ⇔ best coverage < 0.95. The crew proposed "< 1", which would require flagging requests whose true coverage is above the flag threshold, so I used the BRD threshold instead (see Disagreements) |
| F7 | med | The leakage check is a substring regex and ignores exemplars | Fixed in e59f126: checks by phrasing id (request items dev and own class; exemplars dev only; unseen items test and own class), plus a verbatim check over all labels |
| F8 | med | Combo module refs vs one class per programme item | Fixed in e59f126: a combo contributes one programme item per class; refs copy class, count and values |
| F9 | med | The centring test skips the z-score step and its upper bound is tight | Fixed in e59f126: centring + z-score, bound widened to 0.95, lift ≥ 0.10. Note: a per-item z-score across the catalogue does not change the arg-max, so top-1 equals centring alone |
| F10 | med | No test covers the unreviewed multiset or the failure mode | Fixed in e59f126: unreviewed multiset recomputed from apparent classes; containment < Jaccard on unreviewed, ≥ on reviewed |
| F11 | med | Committed data and mini/README are unguarded; pytest is not in CI | Partly fixed in e59f126 (committed = regenerated, README and mini exist). **Deferred:** no CI step for `uv run pytest`, because the user asked to keep CI time minimal. It runs locally before each release |
| F12 | low | A committed .pyc file | Fixed in e59f126 (own file removed, `__pycache__/` ignored). The older `test/scripts/parsers/__pycache__` files predate this work and are left alone |
| F13–F15 | low | Loose unit regexes, vacuous multi-value check, BR-16 names never matter, sampling, near-miss, neutrality patterns | Fixed in e59f126: per-unit regexes, an independent bound-word regex, ≥ 2 numbers per label, one class with two same-dimension attributes, full loops, near-miss assertion, diacritics, and a scan of the generator source |

**Disagreements.** (1) Unmeetable definition: the crew suggested `best < 1`. I followed the BRD
§6 semantics instead ("flagged (score < 0.95)" only makes sense for requests whose true
coverage is below 0.95). This does not change any HLD semantics. (2) Centred-accuracy upper
bound: widened, not removed. The raw-accuracy cap (≤ 0.70) stays so that hubness remains
measurable.

**Open questions from the crew.** The oracle is a strict boolean that uses the true bound
(and the 2 % equals tolerance). The kernel's tolerance bands are what the MAE KPI measures
against. The hidden source programmes are part of the archive. If the HLD unqualified rule
makes the KPIs unreachable, I escalate (HANDOVER §8) and do not deviate silently.

---

## R0-2 — 2026-10-25 — after R0 dataset (generator, data README, sample rows)

- **Crew:** correctness (claude), design (codex), testing (claude), usability (codex).
  Verdict: REQUEST_CHANGES.
- **Unit:** generator at f97df0a, README, sample rows.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | The partition puts one offer item into every tied same-class request block | Fixed: reference kernel gives each candidate one block (largest remaining deficit, then lowest node id); the oracle uses the best one-to-one assignment; test computes it by brute force (ad41ae4, this commit). Recorded as HLD §11.4 |
| F2 | high | Unqualified values are copies of the source, so the HLD point rule looked feasible only because of the rendering | Fixed: same slack as qualified values. The KPIs then failed under the HLD rule → **escalated (HANDOVER §8)** |
| F3 | high | MAE measured on top-1 only narrows the BRD KPI | **Escalated.** Decision: MAE over all top-k pairs; label credit no longer added when values fail (HLD §11.2). Guard asserts `mae_top_k` |
| F4 | high | Provided at_least/at_most values are always points, so a p_lo/p_hi swap is invisible | Fixed: 14 % of directional provided values are ranges; test counts requirement pairs where the swap changes the answer (≥ 50). Mini ranges follow with the R2 tests |
| F5 | high | Temperature tolerance is relative to Kelvin | **Escalated.** Decision: absolute tolerance 1 K, band 2.5 K, distance in K (HLD §11.3) |
| F6 | med | Concept vectors pulled towards their class through held-out labels | Fixed: development phrasings only; held-out MaxSim 0.94 (bounded ≤ 0.95) |
| F7 | med | Flag margin too narrow, tautological unmeetable test | Fixed: meetable ≥ 0.97, unmeetable 0.5–0.90 asserted |
| F8 | med | R50 has coverage 0 against every programme | Fixed: level-2 stretch on one attribute per item, best ≥ 0.5; no request ties ≥ 10 % of the archive (coarse specs only for seats with ≥ 14 items) |
| F9 | med | Seat scope: counts pooled across seats | **Escalated.** Decision: class-level one-to-one assignment, seats are context only (HLD §11.4) |
| F10 | med | Request language only in gold | Fixed: public `requests` table with `lang` |
| F11 | med | The guard is an oracle-parsed ceiling | Documented in reference.py and README. R2 adds a parse accuracy test, R3 adds variant-aware and partition-vs-best-match tests |
| F12 | low | Gold uses the true bound for unqualified text | Documented in README |
| F13–F16 | low | "max. current" name, identical typo swap, gzip bytes in the committed check, equals always unqualified, headings give classes away | Fixed: name changed, typo never returns the input, committed data compared on decompressed content, 40 % of equals qualified, 30 % generic headings |

**Escalation (HANDOVER §8) and decisions, 2026-10-25.** With the source-copy leak removed,
the HLD reference kernel missed the regret and MAE targets (regret 0.024, MAE over top-20
0.075, top-1 0.838). The product owner decided:
1. unqualified values take a default bound from the catalogue through a kernel option
   (`default_bounds := 'catalogue'`);
2. the MAE KPI covers all top-k pairs;
3. affine units get an absolute tolerance;
4. assignment stays class-level and one-to-one.

All four are recorded in HLD §11. With them, every BRD KPI passes on the reference kernel:
recall@20 0.925, top-1 0.925, regret 0.009, MAE over top-20 0.037, unmeetable flagged 1.0,
precision@0.9 0.985.

**Other open questions from the crew.** at_most is judged on `p_lo` ("configurable down
to"), as HLD §4.2 states; no change. Source programmes stay in the archive. Precision at
0.9 counts a top-1 pick whose true coverage is ≥ 0.9.

---

## R1-1 — 2026-10-25 — after R1 red batch

- **Crew:** correctness (claude), testing (codex), design (claude), usability (codex).
  Verdict: REQUEST_CHANGES.
- **Unit:** `test/sql/coverage/r1/` at its first red commit.

Every member recomputed every expected value: all correct (one comment typo).

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | The additive tie-break can rank a partial cover above a full cover (BR-03), and the score exceeds 1 | **Escalated (HANDOVER §8).** Decision: keep the additive form as written in the HLD; document it as a known limitation and pin it with a regression test (HLD §11.5) |
| F2 | med | min_score boundary untested | Fixed: `min_score := 0.63` excludes S3 (measure 0.625, score 0.631) |
| F3 | med | uc_04 breaks on a repeated class | Fixed: GROUP BY class with sum(count); front-row spec with driver/passenger items |
| F4 | med | Scalar parameter names | Decided: `query`/`candidate`; 1.5.6 skip trap documented (API reference, HLD §11.6) |
| F5 | med | Zero-denominator and empty conventions | Fixed: one rule (HLD §11.6) plus a test row per case |
| F6 | med | function_documentation pin would change in green | Fixed in a separate `test:` commit (9964ff5) |
| F7 | med | NULL map weight semantics, weighted_tversky defaults | Decided: NULL → 1, α = β = 1 for the scalar (HLD §11.6) |
| F8 | med | No test for a table without a weight column | Fixed: `weight_col := NULL` means unit weights |
| F9 | med | MAP keys inlined into SQL | Fixed/tested: hostile key, table still intact; keys go through `QuoteSQLStringLiteral` |
| F10 | low | Hidden tie order, weak uc_01 cross-check | Fixed: symmetric cross-check, distinct count, surplus identity; ties by entity_id documented |
| F11 | low | α/β swap undetectable | Fixed: asymmetric example |
| F12 | low | Old vs new search differences | Fixed: uc_06 shows presence-only counting next to the weighted call; API reference explains the difference |
| F13 | low | Missing edge cases | Fixed (76c5a32) |
| F14 | low | getvariable() binding | Verified on 1.5.6 and 2.0 |
| F15 | low | Comment typo | Fixed |

## R1-2 — 2026-10-25 — after R1 green step

- **Crew:** correctness (claude), security (codex), performance (claude), design (codex).
  Verdict: REQUEST_CHANGES.
- **Unit:** diff of 0eab733.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | The Value-based scalars take 4–6 s per 10⁵ rows | Fixed: vectorised element access, VARCHAR keys read from string data, map weights cast once per chunk, constant query read once. Now 0.14 s per 10⁵ rows for the weighted scalars and 0.17 s for containment (jaccard_similarity: 0.71 s) |
| F2 | med | Unweighted mode returns DECIMAL weight columns | Fixed: `1.0::DOUBLE`; typeof test |
| F3 | med | Candidates sharing only zero-weight features come back | Fixed: shared features need weight > 0 on both sides; zero-weight request keys are skipped; repro test |
| F4 | med | Non-finite weights and parameters accepted | Fixed: finite checks in table functions, MAP loop, feature SQL and scalars; tests |
| F5 | low | Reversed named arguments swap α/β on 1.5.6 | Documented (API reference); 2.0 binds by name |
| F6 | low | find_similar_materials_jaccard containment cross-joins every material | Deferred: kept the existing cross-join pattern of that function (the default path must stay unchanged); with the faster scalars it is no longer bound by them. Revisit with R5 sub-linear recall |
| F7 | low | The whole feature table is materialised per call | Deferred to R5 (scale): meets BR-40 (0.15–0.2 s at 10⁵ entities / 1.5 M rows on 1.5.6 and 2.0) |
| F8 | low | Table path inlined unquoted | Fixed: every path segment quoted; test with a table called `order` |
| F9 | low | Type-sensitive key matching | Now compared by text form (1 and '1' match); documented |
| F10 | low | weight_col := NULL overrides weighted := true silently | Fixed: error |

The fixes were implemented together with their regression tests in one commit and were not
written test-first. Logged here as a deviation from HANDOVER §4.

## R1-3 — 2026-10-25 — before PR (end of R1)

- **Crew:** design (codex), usability (codex), testing (claude), maintainability (claude).
  Verdict: REQUEST_CHANGES (no correctness blocker).

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | med | Docs claimed that the table functions' named arguments bind by position on 1.5.6 | Fixed: only scalars bind by position; NAMED_DOC and API reference corrected |
| F2 | med | Wrong trap example for tversky | Fixed: reversed α/β is the silent trap; passing only one does not bind |
| F3 | med | Weights on rows with a NULL key skip validation | Fixed: an uncorrelated whole-table check (the filter had been pushed below the aggregate); the error names the row; regression test |
| F4 | med | Only 4 of 7 scenarios load the dataset | Fixed: uc_08 ranks all 80 benchmark requests at once (stage-1 recall@20 ≥ 0.90; 0.925 measured) |
| F5 | med | Text-form key matching logged as documented but not | Fixed in the API reference |
| F6 | med | NULL query semantics differ between the two functions | Documented per function (behaviour unchanged; both pinned) |
| F7 | med | No documented batch pattern | Fixed: "Many requests at once" in the API reference, used by uc_08 |
| F8 | low | Error messages not actionable | Fixed: hint for a missing weight column; the bad-weight error names the entity, feature and table |
| F9 | low | Different tie order of the two containment searches | Documented |
| F10 | low | Read-only list | Fixed |
| F11 | low | Counts, naming convention, signature | Fixed |
| F12 | low | alpha/beta/tie_break ignored outside their modes | Documented (no error, so scripts can reuse one parameter set) |
| F13 | low | Hygiene | QuotedColumn renamed to ValidateColumnName; α = β = 1 default commented. **Deferred:** sharing ParseSubquery across modules (pre-existing duplication) |
| F14 | low | Scalar input errors come at execution time with the report-a-bug banner | Message improved (names the functions, suggests the list variants). **Deferred:** bind-time validation (scalar bind signatures differ between 1.5.6 and 2.0) and the INVALID_INPUT banner policy (datazoo-banner) |

R1 status: all DoD items met. 8 uc_ scenarios, 5 of them on the dataset; edge cases;
1.5.6 and 2.0 suites green; docs updated.

---

## R2-1 — 2026-10-25 — after R2 red batch

- **Crew:** correctness (claude), testing (codex), design (claude), usability (codex).
  Verdict: REQUEST_CHANGES.

All hand-derived values were recomputed and found correct, except uc_05 W3. In my own check
before the review, I also corrected the log tolerance (1 − 10^−0.02 = 4.5007 %, not 4.5501 %)
in three expectations before the red commit.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | uc_05 W3 expected 0, but IP40 is one step below IP54 (0.5) | Fixed in a separate `test:` commit (8eedb12); a 0 case added (IP65 vs IP40) |
| F2 | high | uc_06 cannot catch a dropped bound | Fixed: stated vs unstated texts checked NULL-safely; row completeness |
| F3 | high | Lookup tables passed two ways | Decided (HLD §11.7): unit_table stays a LIST (an optional user table cannot be read by a scalar), compat_score keeps the §4.2 table name. Both safety cases are tested |
| F4 | med | Equals tolerance on log scale (4.5 %) vs oracle 2 % | Decided without escalation: the §4.2 table itself says "(relative)", so equals is 2 % relative on every scale; the reference kernel follows. The KPI guard still passes |
| F5 | med | `distance` has three meanings | Documented per rule (HLD §11.7, API reference); uc_04 phrases "misses by" only for limit rules |
| F6 | med | Unlogged deviations from §4.2 | HLD §11.7 |
| F7 | med | v = 0 undefined | Absolute distance, strict score; log falls back to linear for non-positive values; tests |
| F8 | med | Unit mismatch within a dimension passes silently | Struct values in a non-base unit are normalised; test |
| F9 | med | Categorical structs passed to value_satisfies | Error pointing to compat_score; test |
| F10 | med | Category rule undefined | Defined (single upper-case code with digits, no number in front); tests '1 F' vs 'F1', 'M8' |
| F11 | med | User unit table override and round trip | Tests: override of 't', seed passed back in |
| F12 | med | Named arguments incomplete | Full named calls for parse_quantity and value_satisfies |
| F13 | med | Temperature only partly pinned | at_least, point and fixed tolerances tested |
| F14 | low | Small edge cases | Added (NULL dimensions, soft_band 0, range at_most failure, unknown unit fields, cross-language words, confusable symbols) |
| F15 | low | Base-unit check could not detect a missing base | Rewritten; currency checked separately |
| F16 | low | uc_02 does not select | HAVING min(score) = 1 → M1.C2.A1 |

Deferred: carrying a stated "± x %" tolerance from parse_quantity into value_satisfies.

## R2-2 — 2026-10-25 — after R2 green step

- **Crew:** correctness (claude), security (codex), performance (claude), maintainability
  (codex). Verdict: REQUEST_CHANGES.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | `>=`/`<=` parsed as equals | Fixed: operators before words; `>`/`<` map to at_least/at_most (strictness dropped, documented) |
| F2 | high | Bound words matched as substrings | Fixed: word-boundary matching in prefix and suffix |
| F3 | high | Leading-zero decimals read as thousands | Fixed: a leading 0 is always a decimal |
| F4 | high | Mixed-unit ranges convert both ends with one unit | Fixed: per-end units; different dimensions give dimension NULL |
| F5 | high | Use-after-free in normalize_quantity with per-row unit tables | Fixed: one table reference per row |
| F6 | high | Struct units unchecked | Fixed: struct units resolve against the seed; unknown or mismatched units gate; currency symbols canonical; currency without a unit gates |
| F7 | med | Signed range ends | Fixed: signs after connectors and operators, `+`, spaced dash |
| F8 | med | `220 ± 5 N`, suffix bounds | Fixed |
| F9 | med | Unit table rebuilt per row | Fixed: reused while the units repeat; Seed() for NULL |
| F10 | med | User unit factors unchecked; malformed numbers | Fixed: factor positive and finite; malformed digit groups are not numbers |
| F11 | med | Per-row data errors abort bulk queries | **Kept** (fail loudly): categorical values belong to compat_score, and the R3 kernel routes them there itself. Logged as a decision |
| F12 | low | lo > hi, non-finite values | Fixed |
| F13 | low | One-letter aliases, μ/Ω, EUR prefix, umlauts | Fixed |
| F14 | low | Value-based I/O about 3–4 µs per row | Accepted for the SQL surface; the R3 kernel calls the C++ functions directly |

All regressions are pinned in `test/sql/coverage/r2/parser_regressions.test`.

---

## R3-1 — 2026-10-25 — after R3 red batch A (property-tree store)

- **Crew:** correctness (claude), testing (codex), design (claude), usability (codex).
  Verdict: REQUEST_CHANGES.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | The R01 check expected 13 numeric attributes; only 11 exist | Fixed in a `test:` commit: a symmetric check over three requests (55 attributes, 52 numeric, 3 categorical) |
| F2 | high | The test depended on overrides_table defaulting to NULL | Decided: opt-in, default NULL (HLD §11.8), stated in the test |
| F3 | high | label_table shape vs the R4 producer; arg-max untested | Decided: `(tree_id, node_id, class_id, p)`, arg-max, ties to the lowest class_id; classify_to_catalogue returns tree_id (HLD §11.8); multi-row and tie tests |
| F4 | med | Precedence class_id > override untested | Contradicting override added |
| F5 | med | OR max only tested with same-class alternatives | **Escalated:** product owner chose elementwise max as an optimistic stage-1 bound; fixture with different classes |
| F6 | med | Count rules untested in the multiset | **Escalated:** product owner chose the product along the path; bench and AND-sum fixtures |
| F7 | med | Undocumented HLD deviations | HLD §11.8 |
| F8 | med | Attribute structs lack keys; no multi-document test | Keys added; three requests in one read_text query |
| F9 | med | validate_trees gaps; recursion on broken input | duplicate_node, zero count, orphan_attribute, mixed_edge_types, bad_interval, bad_bound; tree_anchors termination test |
| F10 | med | No lang/unit_table on tree_from_json | Added |
| F11 | med | mixed_dimensions too broad; no severity | Scoped per source and name; severity column |
| F12 | low | One-way node comparison on 4 columns | Symmetric over 10 columns |
| F13 | low | tree_from_json edge and error cases | Added, errors name the node |
| F14 | low | Path conventions, n_attributes | Documented; n_attributes 0 without an attribute table |
| F15 | low | Misleading comment, weak identifier tests | Fixed; identifier tests for every table parameter |

## R3-2 — 2026-10-25 — after R3 green step, batch A (property-tree store)

- **Crew:** correctness (claude), security (codex), performance (claude), maintainability
  (codex). Verdict: REQUEST_CHANGES (two crashes).

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | JSON nesting has no limit: segfault | Fixed: nesting limit, clean error; test with 500 000 levels |
| F2 | high | tree_multiset recursion has no limit: segfault on long chains | Fixed: iterative traversal; trees deeper than 1000 levels are a clean error; test with a 100 000-node chain |
| F3 | med | Depth caps are inconsistent; tree_anchors truncates silently | Fixed: one MAX_TREE_DEPTH (1000) everywhere; validate_trees reports too_deep |
| F4 | med | Stage 1 vs stage 2 disagree on orphans and duplicates | Fixed: tree_multiset counts reachable nodes and the first row of a duplicated node only |
| F5 | med | validate_trees ignores tree_ids for attributes | Fixed; test |
| F6 | med | Resolver not ready for stage 2 | Fixed: labels filtered by tree, deterministic dedupe, class_origin, materialised CTEs |
| F7 | med | An explicit null bound suppresses the text's bound | Fixed: the text's bound fills a missing or null bound |
| F8 | med | Surrogates | Fixed: lone surrogates and control characters rejected |
| F9 | med | node_kind not validated | Fixed: validated and lower-cased |
| F10 | low | Loose numbers | Fixed: strict JSON number grammar, finite checks, numbers kept as written |
| F11 | med | TableHasColumn always true for views | Fixed: view column info; test with a view lacking optional columns |
| F12 | low | Cycle detection O(N·depth²), misses nodes below cycles | Fixed: top-down reachability; cycles detected only among unreachable nodes; `unreachable` issue |
| F13 | low | attr_id collisions | Fixed: unique given ids, generated ids skip used ones |
| F14 | low | tree.schema.json missing | Added |
| F15 | low | Per-row unit table rebuild in tree_from_json | Deferred: user unit tables in JSON loading are rare; the seed path does not rebuild |

## R3-3 — 2026-10-25 — after R3 red batch B (coverage kernel)

- **Crew:** correctness (claude), testing (codex), design (claude), usability (codex).
  Verdict: REQUEST_CHANGES. All mini values recomputed by three members: all correct.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | `lambda` is a reserved word in DuckDB | Renamed to `label_weight` (HLD §11.9) |
| F2 | high | parse_quantity gives categorical requirements a NULL dimension, so they gate | Decided: a categorical requirement without a dimension matches categorical offers of any dimension through the rules (HLD §11.9). The KPI test passes with the parsed path |
| F3 | high | k-only tests assume the query is excluded | Documented and implemented: never its own candidate |
| F4 | high | Partition/counts and OR: the reference is OR-unaware | Decided: OR alternatives are options under partition (best variant per requirement, §4.6); counts only over compatible items. Tests added (two seats vs M1 = 1.0; T2 = 0.5). The benchmark has no OR edges, so its KPIs are unaffected |
| F5 | med | Kernel surface not in the HLD | HLD §11.9 |
| F6 | med | Partition order underspecified | Node-id order, documented |
| F7 | med | Benchmark tests feed gold request classes into the matcher | Documented as a stand-in for upstream labelling until R4 (classifier); R4 scenarios use the classifier |
| F8 | med | Trace rows for missing attributes or counterparts undefined | Defined (HLD §11.9) |
| F9 | med | 'Ambiguous template' undefined | Defined (HLD §11.9) |
| F10 | med | Amendments without focused tests | label_weight, count_gate, partition_gate, OR-option tests |
| F11 | low | One-way EXCEPT | Symmetric in uc_05 and uc_08 |
| F12 | low | Error message vs the templates default | Consistent: a missing default table is an error naming templates_table |
| F13 | low | Negative assertions that depend on the generator | Kept: they are pinned dataset properties (also in pytest) |
| F14 | low | uc_04 description | Narrowed |

**Measured with the C++ kernel** (test uc_06): recall@20 0.925, top-1 0.925, regret 0.0087,
MAE over the top 20 0.036 (top-1 0.014), unmeetable flagged 1.0, precision at 0.9 0.985,
auto-accept share 82.5 %. These are identical to the reference kernel. Runtime: 0.65 s for
80 × 20 pairs.

The partition-versus-best-match failure mode on unreviewed labels needs soft labels from a
classifier. It moves to R4.

## R3-4 — 2026-10-25 — after R3 green step, batch B (coverage kernel)

- **Crew:** correctness, security, performance, design (codex/claude). Verdict:
  REQUEST_CHANGES. SQL injection: none found (every identifier validated and quoted,
  literals quoted, doubles checked for finiteness).

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | Malformed scalar input gives an INTERNAL error that invalidates the database | Fixed: per-chunk type checks in coverage_kernel and tree_multiset (InvalidInputException); NULL options = defaults; tests followed by `SELECT 1` |
| F2 | high | An empty candidates/query list disables the filter | Fixed: an empty list selects nothing (`has_tree_ids`); tests for all three functions |
| F3 | med | Partition shares AND siblings inside one OR variant | Pinned and documented (HLD §11.10): variants are per requirement, as §4.6 prescribes; test |
| F4 | med | NaN can reach a score | Fixed: Num() assigns only finite values; counts must be positive and finite (error names the node) |
| F5 | med | Duplicate/NULL label rows abort map_from_entries | Fixed in the soft-label maps and the resolver's arg-max: duplicates add up, NULL classes and p ≤ 0 drop out |
| F6 | med | Soft labels are not normalised | Fixed: normalised to 1 |
| F7 | med | Scope of the auto recall mode | Documented (HLD §11.10): the candidate pool of the call |
| F8 | med | Stage 1 cross join; scale not verified | Fixed: inverted join; new `candidate_multiset_table` index (uc_10 proves the results are identical). 100 200 trees: stage 1 0.12 s, stage 2 0.58 s per request (BR-40 met); without the index 1.9 s / 12.9 s |
| F9 | med | Per-pair cost | Partly fixed: no attribute scoring without label agreement; one-hot distributions built once. Loading the query once per query: deferred (0.58 s per request is within budget) |
| F10 | med | The committed code did not build on 2.0 | Fixed (overloads instead of varargs, compat parameter map); full suite green on 2.0 |
| F11 | low | Options STRUCT not validated | Fixed |
| F12 | low | Mixed name-vector lengths fall back silently | Documented fallback (token overlap); deferred |
| F13 | low | Self-exclusion across different tables | Fixed: only within one table; test |
| F14 | low | One bad candidate tree aborts stage 1 for all | Deferred: validate_trees is the tool for that, and errors name the tree in tree_multiset |
| F15 | low | TableExists fails open; duplicated helpers | Kept: a failure falls through to the column check, which errors; consolidation deferred to the R5 refactor |
| F16 | low | Supplier class_id counts as reviewed | Documented (HLD §11.10) |

## R3-5 — 2026-10-25 — before the R3 milestone (end-of-release review)

- **Crew:** correctness, usability, testing, design (codex/claude). Verdict:
  REQUEST_CHANGES.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | The kernel ignores request-side OR alternatives | Fixed (red test first): best alternative by mean, coverage = sum / number (HLD §11.11). The benchmark has no OR edges, so its KPIs are unchanged |
| F2 | high | No R3 read-only test | Added r3/readonly.test (three-part paths, all functions) |
| F3 | high | API examples not runnable; heading bug | Gap-list example written out in full, name_floor recipe, tree-function examples, blank line before `---` |
| F4 | med | Partition-vs-best-match failure mode not tracked | Tracked here as an open R4 DoD item; it needs the classifier's soft labels |
| F5 | med | No named-argument test for tree_from_json | Added (documented order) |
| F6 | med | KPI joins can drop pairs silently | Guard added: 80 requests, 1 600 pairs with ground truth |
| F7 | med | Missing ids are dropped silently | Documented behaviour (as for find_covering_entities); a strict mode is deferred |
| F8 | med | Errors name the wrong parameter | Fixed: candidate_* names; a missing table names its parameter |
| F9 | med | coverage_kernel status undocumented | Documented as the public building block |
| F10 | low | Non-finite counts pass validate_trees / tree_multiset | Fixed |
| F11 | low | Doc drift | Test README updated; function count stays an approximation ("about 65") |
| F12 | low | tree_anchors truncates at depth 1000 | Kept: documented, and validate_trees reports too_deep |
| F13 | low | Helpers copied again | TableExists shared; ParseSubquery/Quoted consolidation deferred to the R5 refactor |
| F14 | low | recall_mode vs mode | Kept: `mode` in coverage_recall matches find_covering_entities; `recall_mode` in tree_coverage avoids a clash with stage 2. Documented |

**R3 status:** done on DuckDB 1.5.6 and 2.0. Remaining DoD item for R4: the
partition-on-unreviewed-labels failure mode.

## R4-1 — 2026-10-25 — after R4 red batch (pivot classifier, vocabulary review)

- **Crew:** correctness, testing, design, usability (codex/claude). Verdict:
  REQUEST_CHANGES. All hand values recomputed: correct.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | uc_07 KPIs unmeasured with classifier labels | Measured. With soft classifier labels and `auto` = partition (candidate reviewed): MAE 0.19, so **escalated** (HANDOVER §8). User decisions: (1) `auto` partitions only when both trees are reviewed (HLD §11.12); (2) treat the remaining recall/regret miss (0.8625 / 0.022 with best match) as a KPI miss and investigate. Finding: 63 of the 71 misread items sit in the 15 % lowest margins; after a reviewer answers those (BR-33), all six BRD KPIs are met (recall 0.9125, top-1 0.9125, regret 0.0099, MAE 0.035, precision 0.985). uc_07 pins both |
| F2 | high | uc_04 class-only, value-blind | Reworked with parsed attributes: 34 ventilation requests, 14 → 23 best-first; R12 flips P061 → P047; the queue's top 3 include 'Klimapaket Heizung' |
| F3 | high | overrides_table default contradicts §4.4 | HLD §11.12: default NULL (no implicit table lookup); test |
| F4 | med | Untyped fixtures | Typed where it matters; DECIMAL input covered by the literal fixtures |
| F5 | med | cosine_floor default samples 14 365 pairs | `sample := 20000` for the exact floor; n_pairs asserted (14 365, 2 000) |
| F6 | med | Degenerate classifier inputs | Tests: single-item source (uniform), one-class catalogue, NULL vec (skipped), dimension mismatch |
| F7 | med | Bonus and centring under-constrained | Tests: two sources; name-vector bonus by hand (mean over all attributes, best template, NULL dimension counts 0) |
| F8 | med | apply merge semantics | Tests: untouched entries survive, NULL source key, reviewed_at kept |
| F9 | med | Node-averaging denominator | Test: missing class rows count 0 |
| F10 | med | Key/precedence rules implicit | Tests: decision beats override, NULL tree_id keys |
| F11 | low | R4 surface not in the HLD | HLD §11.12 |
| F12 | low | Weak assertions; read-only | Ordered queue positions, defaults test, r4/readonly.test |

The documented failure mode "partition on unreviewed labels is worse than best match"
(HANDOVER §5.3, open since R3) now shows on the request side (MAE 0.19 vs 0.038). On the
unreviewed archive it does not reproduce: there the label noise is systematic (four
misleading wordings) and hurts both modes alike (regret 0.102 vs 0.100 with hard labels).

## R4-2 — 2026-10-25 — after R4 green step

- **Crew:** correctness, security, performance, design (codex/claude). Verdict:
  REQUEST_CHANGES. SQL injection: none found; the `auto` change is correct.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | high | An override/decision class outside the catalogue gives p = 0 everywhere with overridden = true | Fixed: guard error (red test first) |
| F2 | high | Duplicate catalogue class ids | Fixed: guard error |
| F3 | med | Duplicate items | Fixed: guard error |
| F4 | med | NaN/inf/NULL vector elements | Fixed: guard errors in SQL and in maxsim/calibrated_cosine |
| F5 | med | Queue stacks node-level and wording-level distributions | Fixed: one distribution per wording (node > exact source > NULL source); p validated |
| F6 | med | apply keeps duplicate old keys, accepts NULL classes | Fixed; extra columns are dropped (documented) |
| F7 | med | Performance unmeasured | Measured: centroid 10⁵ × 200 in 8.7 s; maxsim rewritten without Value boxing (late interaction 10⁴ × 200: 30 s → 1.6 s); cosine_floor 10⁵ vectors 2.6 s |
| F8 | low | cosine_floor cross-version determinism; exact-path pruning | Pruning confirmed (10⁵ vectors in 2.6 s). The sample depends on DuckDB's hash; tests pin properties (same call twice, n_pairs, |sample − exact| < 0.01), not a value |
| F9 | low | `reviewed` output is the candidate side | Documented (API reference); `assignment_used` shows the decision |
| F10 | low | NULL node_id, empty catalogue, NULL exemplar tokens | Fixed: guard errors |
| F11 | low | attrs/decisions without tree_id leak across trees | Fixed: error when the items have tree_id |
| F12 | low | any_value, control characters, p validation | Fixed: min(); \b \f escaped, other control characters stripped (documented); p validated in the queue |
| F13 | low | Duplicated helpers | Deferred to the R5 refactor |
