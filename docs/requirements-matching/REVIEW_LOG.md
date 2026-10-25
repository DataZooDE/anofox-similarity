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
