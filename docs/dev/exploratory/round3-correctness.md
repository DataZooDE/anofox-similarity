# Round 3 exploratory findings: correctness (property-based)

Tested 2026-10-27 on DuckDB 1.5.6 (linked-in shell) and 2.0 (explicit LOAD), from the
repository root. Every finding below reproduces identically on both shells. `SELECT 1` worked
after every error. Inputs were generated in SQL (`setseed` + `random()`, `range()`); the harness
scripts are in `build/tmp/crew3/correctness/` (`p1.sql` … `p15.sql`).

Findings already recorded in UR-1, UR-2 and D-1..D-5 are not repeated here. In particular, the
empty-set convention (D-4) also explains `tversky_similarity([], [], 1.0, 0.0) = 0.0` versus
`containment([], []) = NULL`.

## Properties that hold (thousands of generated cases each)

| Area | Property | Cases | Result |
|---|---|---|---|
| set scalars | `jaccard_similarity` symmetric; equals \|∩\|/\|∪\| (lists with duplicates, empty lists) | 4 000 | 0 violations |
| set scalars | `tversky(q,c)` = `tversky(q,c,1,1)` = Jaccard; `tversky(q,c,1,0)` = `containment` (non-empty q); `tversky(q,c,a,b)` = `tversky(c,q,b,a)`; matches the formula for random α, β ∈ [0,3]; all in [0,1] | 4 000 | 0 |
| weighted scalars | `weighted_jaccard` symmetric and scale-invariant (×7); matches Σmin/Σmax; `weighted_tversky` defaults = weighted Jaccard, (1,0) = `weighted_containment`, swap symmetry, formula for random α, β; zero weights | 4 000 | 0 |
| exact search | `find_covering_entities_for` containment/jaccard/tversky/`weighted := false` equal the scalars on the aggregated table (duplicate rows add up); no overlapping entity is missing; score = containment + 0.01·jaccard | 39 entities × 4 modes | 0 |
| `value_satisfies` | at_least non-decreasing, at_most non-increasing in the provided value (log and linear, 601-point grid); at_least/at_most mirror images on the linear scale (max diff 3e-15); equals/point unimodal | 4 800 | 0 |
| `value_satisfies` | unit invariance mm / cm / m / mixed and mA / A for all bounds and both scales | 5 000 | max diff 8e-15 (but see F4 at the exact boundary) |
| `value_satisfies` | within: widening the provided range never lowers the score; at_least with a higher top end, at_most with a lower bottom end never lower; scores in [0,1]; a fully covering range scores 1 (NULL dimension gives a gate, as documented) | 5 000 | 0 |
| `tree_coverage` | coverage in [0,1]; partition ≤ best_match; adding a matching leaf to every candidate never lowers coverage (both assignments); shuffled physical row order gives identical coverage, n_gaps and rank | 1 800 pairs × 2 | 0 |
| trace | `req_score` and `is_gap` constant within a requirement; mean `req_score` = coverage; gap count = `n_gaps`; requirement count = `n_requirements`; every scored pair has a trace | 1 800 pairs × 2 | 0 |
| evaluation | `eval_ranking` top_k, recall@k, MRR (cut at k), regret against a hand-computed 4-query case with ties, a query without results and a result without gold; `eval_calibration` MAE, bias, Pearson, Spearman (average ranks) | hand case | exact match |
| evaluation | `bootstrap_ci`: same seed gives the same bounds; another seed changes them; estimate = pooled mean; one cluster gives lo = hi; n and level are range-checked; a NULL cluster is an error | — | holds |
| `cluster_matches` | identical to a Python union-find on 403 random pairs (threshold, self pair, NULL partner, singleton below threshold); a/b swap gives the same result; clusters at 0.8 nest inside clusters at 0.6 | 279 entities | 0 mismatches |

UR-2 fixes checked beyond their regression inputs:

- **G1 (German bound words): holds for the listed words** (`mind.` in any case, `mind.24`, `ab`,
  `über`, `größer (als)`, `kleiner (als)`, `bis (zu)`, `höchstens`, `unter`, `weniger als`,
  `mehr als`, `≥24`, suffix `min.` / `oder mehr` / `oder weniger`). **Fails for negations other
  than the two pinned ones: F1.**
- **G2 (thousands separator): holds** for `1 000 000 N`, `12 000,5 N`, `1 000 N bis 2 000 N`,
  `mind. 1 000 N`, `10 000 U/min`. Malformed groupings are misread silently (F6).
- **G7 (tree_from_json): holds** for `count` 0, -1, "0", "-2", "abc", true, "1,5" and for the near
  misses `Count`, `counts`, `count `, `lable`, `Label`, `Children`, `child`, `chlidren`,
  `atributes`, `attrs`, `kind2`, `nodeid`. The abbreviation `"cnt": 2` is silently ignored
  (count 1); `"count": 1e-300` is accepted (low, not reported further).

## Findings

### F1 [high] Negated bound words reverse the direction: `nicht über 38 dB(A)` accepts 45 dB(A)

`parse_quantity` knows only four negated forms (`nicht mehr als`, `nicht weniger als`,
`not more than`, `not less than`, `src/modules/quantity.cpp:812-822`). Every other negation
matches the bare word and loses the `nicht`/`no`, so the bound is reversed. A value that breaks
the requirement then scores 1, a false accept with nothing in the output to show it.

```sql
SELECT t, parse_quantity(t).bound AS bound FROM (VALUES
  ('nicht über 38 dB(A)'), ('nicht unter 24 cm'), ('nicht größer als 24 cm'), ('nicht kleiner als 24 cm'),
  ('no more than 24 cm'), ('no less than 24 cm')) v(t);
-- at_least, at_most, at_least, at_most, at_least, at_most   (all six reversed)

SELECT value_satisfies(parse_quantity('nicht über 38 dB(A)'), parse_quantity('45 dB(A)')).score AS false_accept,  -- 1.0
       value_satisfies(parse_quantity('no more than 38 dB(A)'), parse_quantity('45 dB(A)')).score AS false_accept2, -- 1.0
       value_satisfies(parse_quantity('nicht unter 24 cm'), parse_quantity('30 cm')).score AS false_gap;          -- 0.0
SELECT 1;
```

Expected: `nicht über` / `nicht größer (als)` / `nicht höher als` / `no more than` / `no greater
than` → at_most; `nicht unter` / `nicht kleiner (als)` / `no less than` / `no smaller than` →
at_least. The same parser feeds `tree_from_json` text values, so the error reaches
`tree_coverage`. Related gaps (no bound, so the value is compared as a ±1 % point and a met
requirement scores 0): `greater than`, `over`, `under`, `higher than`, `lower than`,
`exceeding`, `not exceeding`, `must not exceed`, `or greater`, `or higher`, `und mehr`,
`oder größer`, `ueber`, `groesser`, `nicht höher als`; e.g.
`value_satisfies(parse_quantity('greater than 24 cm'), parse_quantity('30 cm')).score` = 0.0.
Prefix words in a label also set a bound: `Länge über alles 24 cm` → at_least.

Suggested fix (code): detect a negator (`nicht`, `kein`, `no`, `not`, `never`) directly before
a bound word and flip the bound; add the missing English comparatives; one test per phrase,
including the six above. Doc mitigation applied in `docs/guides/05-quantities-and-specs.md`.

### F2 [high] Partition assignment ignores values, so coverage depends on node ids and reports false gaps

`PartitionBlocks` (`src/modules/coverage_kernel.cpp:763-801`) gives each candidate item to the
requirement with the highest *label* agreement. Requirements of the same class always tie,
and the tie goes to the largest remaining count need and then to the lowest node id. Values
are not considered. With two same-class requirements of different values, the strong item
can land on the weak requirement. `auto` picks partition for reviewed trees, which is the
recommended setup.

```sql
CREATE TABLE rq AS SELECT * FROM (VALUES
 ('RQ','RQ',NULL,'container','AND','spec',NULL,1.0),
 ('RQ','RQ.a','RQ','item','AND','motor small','motor',1.0),
 ('RQ','RQ.b','RQ','item','AND','motor big','motor',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE rqa AS SELECT * FROM (VALUES
 ('RQ','RQ.a','p','power','power','W',10.0,10.0,'at_least',NULL),
 ('RQ','RQ.b','p','power','power','W',90.0,90.0,'at_least',NULL)) t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
CREATE TABLE pg AS SELECT * FROM (VALUES
 ('PG','PG',NULL,'container','AND','prog',NULL,1.0),
 ('PG','PG.x','PG','item','AND','motor','motor',1.0),
 ('PG','PG.y','PG','item','AND','motor','motor',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE pga AS SELECT * FROM (VALUES
 ('PG','PG.x','p','power','power','W',95.0,95.0,NULL,NULL),
 ('PG','PG.y','p','power','power','W',20.0,20.0,NULL,NULL)) t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
SELECT assignment_used, coverage, n_gaps FROM tree_coverage('RQ', candidates := ['PG'], nodes_table := 'rq',
       attrs_table := 'rqa', candidate_nodes_table := 'pg', candidate_attrs_table := 'pga');
-- partition | 0.5 | 1      (best_match: 1.0 | 0)
SELECT req_node_id, matched_node_id, value_score, is_gap FROM tree_coverage_trace('RQ', candidates := ['PG'],
       nodes_table := 'rq', attrs_table := 'rqa', candidate_nodes_table := 'pg', candidate_attrs_table := 'pga');
-- RQ.a PG.x 1.0 false / RQ.b PG.y 0.0 true   <- "the 90 W motor is missing", but PG.x has 95 W
UPDATE pga SET lo = 115 - lo, hi = 115 - hi;   -- same offer, the values swap item ids
SELECT coverage FROM tree_coverage('RQ', candidates := ['PG'], nodes_table := 'rq', attrs_table := 'rqa',
       candidate_nodes_table := 'pg', candidate_attrs_table := 'pga');
-- 1.0
```

Property run: 30 random requests × 60 random programmes (4 classes, 1–5 items, one `at_least`
power attribute). Renaming the item ids (`.0,.1,…` → `.r9,.r8,…`) leaves best_match unchanged.
Under partition it changes coverage for 65 of 1 800 pairs (up to 0.75) and n_gaps for 49 pairs,
and the rank-1 programme changes for 5 of the 30 requests (on 2.0, with its different random
stream: 119 pairs, 7 requests).

Expected: an id-independent result. REVIEW_LOG UR-0 F1 says the oracle uses the best one-to-one
assignment, and the kernel should not report a gap when a feasible assignment covers it.
Suggested fix (code): among tied requirements, use the attribute score of the candidate item
before count need and node id. Better, solve the assignment inside each same-class block (a
small Hungarian or max-weight matching on req score; blocks are tiny). Test: the repro above,
plus the rename property. Doc mitigation applied in `docs/api/13-coverage-kernel.md`.

### F3 [medium] `jaccard_similarity` and `tversky_similarity`/`containment` disagree on mixed-type lists, and integer vs decimal ids never match

`jaccard_similarity` compares typed `Value`s (`src/modules/jaccard_similarity.cpp:24-36`). The
Tversky/containment family compares the text form (`src/modules/set_measures.cpp:79-90`). The
documented identity "α = β = 1 is Jaccard" therefore fails, and neither function warns about
mismatched element types.

```sql
SELECT jaccard_similarity([1, 2], ['1', '2']) AS j,          -- 0.0
       tversky_similarity([1, 2], ['1', '2']) AS t,          -- 1.0  (documented to equal j)
       containment([1, 2], [1.0, 2.0]) AS c,                 -- 0.0  ('1' vs '1.0')
       jaccard_similarity([1, 2]::INT[], [1, 2]::DOUBLE[]) AS jd,          -- 0.0
       weighted_containment(MAP {1: 1}, MAP {1::DOUBLE: 1}) AS wc;         -- 0.0
SELECT 1;
```

The realistic case: component ids typed BIGINT in one source and DOUBLE in another (an Excel
export) silently score 0. Expected: one comparison rule for all set scalars, and an error or a
cast to a common type when the element types differ. Suggested fix (code): in bind, cast both
list/map key types to their max logical type (error if none); use the same key function in
jaccard and set_measures. Doc mitigation applied in `docs/api/01-similarity-scalars.md`.

### F4 [low] A value exactly at the tolerance scores just below 1, depending on the unit

```sql
SELECT value_satisfies(parse_quantity('at least 100 mm'), parse_quantity('98 mm'), 'linear').score AS mm, -- 0.9999999999999997
       value_satisfies(parse_quantity('at least 100 m'),  parse_quantity('98 m'),  'linear').score AS m;  -- 1.0
```

Over `r` = 50, 100, … 5 000 with `p = 0.98·r` (exactly at tol 0.02): in m every case scores
1.0; in cm 37 of 100 and in mm 68 of 100 score < 1. The documented filter `HAVING min(v.score) = 1`
(docs/api/11-quantities.md) rejects them, so the result of `= 1` depends on the unit the data
was entered in. Suggested fix (code): compare `distance <= tol + 1e-12` (and clamp the soft
score to 1 within that epsilon). Doc mitigation applied: the example now filters with
`>= 1 - 1e-9`, with a note.

### F5 [low] `weighted_jaccard` returns NaN for large finite weights

```sql
SELECT weighted_jaccard(MAP {'a': 1e308, 'b': 1e308}, MAP {'a': 1e308, 'b': 1e308});  -- -nan (expected 1.0)
```

The sums overflow to inf and inf/inf is NaN. NaN sorts above every number in DuckDB, so such a
row would rank first. The weights are unrealistic, but the scalars promise [0, 1]. Suggested fix:
scale by the maximum weight before summing, or reject non-finite sums.

### F6 [low] Malformed digit groups are read as a count without warning

```sql
SELECT t, parse_quantity(t).lo, parse_quantity(t).unit FROM (VALUES ('1 00 N'), ('24 1000 N'), ('3 x 1 000 N')) v(t);
-- 1 00 N    -> 1.0, unit '1'   (the N is lost)
-- 24 1000 N -> 24.0, unit '1'
-- 3 x 1 000 N -> 3.0, unit 'x' (dimension NULL, so at least it gates)
```

Expected: an unparsed tail keeps the value from becoming a count, for example `dimension` NULL
as for unknown units. Then the comparison gates, and the value is not compared as the
dimensionless number 1. Suggested fix: when digits or a unit follow the first number and are
not consumed, return the unknown-unit form.

### F7 [low, doc] `distance` for temperature within/point is a share, not kelvin; `mean_regret` does not depend on k

- `value_satisfies({'lo':-10,'hi':40,'dimension':'temperature','unit':'°C','bound':'within'},
  {'lo':0,'hi':40,'dimension':'temperature','unit':'°C'})` gives distance 0.18 (= (10 K − 1 K
  tol) / 50 K), a share, while the docs said "in K for temperature". Fixed in
  `docs/api/11-quantities.md` and `docs/api/13-coverage-kernel.md`.
- `eval_ranking` repeats the same `mean_regret` / `share_regret_le_005` on every k row (they
  measure the top-1 pick; the generated text says so, the hand-written list did not). Fixed in
  `docs/api/15-evaluation.md`.

## Doc changes in this round (all SQL verified on 1.5.6 and 2.0)

- `docs/guides/05-quantities-and-specs.md`: negations that are reversed or not recognised (F1), with
  a runnable check.
- `docs/api/13-coverage-kernel.md`: partition with repeated classes (F2); temperature distance (F7).
- `docs/api/01-similarity-scalars.md`: mixed element types (F3).
- `docs/api/11-quantities.md`: boundary rounding note, the example filter changed to `>= 1 - 1e-9`
  (F4; re-run with sample tables on both shells), temperature distance (F7).
- `docs/api/15-evaluation.md`: regret columns are top-1 (F7).

## Verdict

The math largely holds up. The set and weighted measures, the exact search, the monotonicity
and unit invariance of `value_satisfies`, the coverage/trace bookkeeping, the evaluation metrics
and the clustering all pass thousands of generated cases on both shells with no violations
beyond floating-point noise. Two problems give wrong answers with no error. **F1** reverses the
direction of common negated requirement phrasing, so non-compliant offers pass. **F2**, under the
default `auto` assignment for reviewed trees, reports false gaps and lets node-id naming change
the top-ranked programme. A bid engineer would trust both outputs, so fix both before the
next release. F3 is a silent zero for a realistic type mismatch. F4–F7 are minor.
