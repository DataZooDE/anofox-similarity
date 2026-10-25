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
