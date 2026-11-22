# Evaluation and Scale

> What the evaluation metrics measure, why intervals resample clusters, how the auto-accept
> threshold and the weight fit work, and how MinHash, LSH and clustering trade exactness for
> speed. For the calls, see [guide 07](../guides/07-evaluation-and-tuning.md),
> [guide 08](../guides/08-scale-and-dedup.md), [api/15](../api/15-evaluation.md) and
> [api/16](../api/16-lsh-and-clustering.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Two Questions About a Ranking

A coverage ranking answers two questions, and each needs its own measure:

| Question | Measured by | Uses |
|---|---|---|
| Is the right candidate near the top? | `eval_ranking`: top-k accuracy, recall@k, MRR, regret | the rank order only |
| Can the number itself be trusted? | `eval_calibration`: MAE, bias, Pearson, Spearman | the predicted coverage against the true coverage |

A ranking can be perfect while every number is 0.2 too high, and well calibrated numbers can still
rank badly. Check both before you show coverage to a user or act on it automatically.

Gold is a table of reviewed pairs `(query_id, candidate_id, true_coverage)`. The **best
candidates** of a query are all gold candidates within 1e-9 of its best true coverage; there can
be several. A ranked candidate without gold counts as true coverage 0, so missing gold makes the
pipeline look worse, never better.

## Ranking Metrics

- **Top-k accuracy**: the share of queries with a best candidate in the top k. This is the
  number a user feels: "was a right answer on the first page?"
- **Recall@k**: the share of each query's best candidates in the top k. It differs from top-k
  accuracy only when several candidates tie for best.
- **MRR**: the mean of 1 / rank of the first best candidate (0 beyond k). It rewards rank 1 over
  rank 2 more than top-k accuracy does.
- **Regret**: best true coverage minus the true coverage of the top-1 pick. A wrong pick that
  covers almost as much costs little; a pick that covers half costs a lot. `share_regret_le_005`
  counts the picks within 0.05 of the best.

Four requests. Q2 ranks the worse candidate first (0.6 instead of 0.95):

```sql
CREATE OR REPLACE TABLE results AS SELECT * FROM (VALUES
  ('Q1', 'A', 1, 0.97), ('Q1', 'B', 2, 0.95),
  ('Q2', 'C', 1, 0.80), ('Q2', 'D', 2, 0.70),
  ('Q3', 'E', 1, 0.60), ('Q3', 'F', 2, 0.40),
  ('Q4', 'G', 1, 0.92), ('Q4', 'H', 2, 0.30)) t(query_id, candidate_id, rank, coverage);
CREATE OR REPLACE TABLE gold AS SELECT * FROM (VALUES
  ('Q1', 'A', 1.0), ('Q1', 'B', 0.9),
  ('Q2', 'C', 0.6), ('Q2', 'D', 0.95),
  ('Q3', 'E', 0.5), ('Q3', 'F', 0.4),
  ('Q4', 'G', 0.95), ('Q4', 'H', 0.2)) t(query_id, candidate_id, true_coverage);

SELECT k, top_k_accuracy, mrr, round(mean_regret, 4) AS mean_regret
FROM eval_ranking('results', 'gold', k := [1, 2]);
-- => 1 | 0.75 | 0.75 | 0.0875
-- => 2 | 1.0 | 0.875 | 0.0875
```

Three of four picks are right. The one miss costs 0.35, so the mean regret is 0.0875. Regret
measures the top-1 pick, so it repeats on every k row.

## Calibration

`eval_calibration` compares the predicted coverage of every ranked pair with its true coverage.
**MAE** is the mean absolute difference, **bias** the mean of predicted − true (positive: the
scores are too optimistic), and **Spearman** says whether higher predictions go with higher true
coverage at all. On the example: MAE 0.095, bias 0.0175, Spearman 0.8623. A small bias with a
large MAE means the errors cancel; look at both.

## The Auto-Accept Threshold

Which top-1 picks can be accepted without review? A pick is **good** when its true coverage
reaches `good` (default 0.9). `threshold_at_precision` sorts the top-1 picks by predicted
coverage and returns the **lowest** threshold whose accepted picks reach the target precision,
which is the largest share you can accept automatically:

```sql
SELECT threshold, precision, accepted_share
FROM threshold_at_precision('results', 'gold', target_precision := 0.95);
-- => 0.92 | 1.0 | 0.5
```

Accepting from 0.92 takes Q1 and Q4, both good. Lowering it to 0.80 would add Q2, whose pick is
not good, and precision falls to 2/3. The threshold is a property of your data and your gold:
recompute it when either changes, and report the holdout value, not the one you tuned on.

## Intervals by Cluster

Requests from one customer share wording, templates and habits, so their outcomes are not
independent: when the matcher fails on a maker's phrasing, it fails on all of that maker's
requests. Resampling single requests then treats ten correlated failures as ten independent
pieces of evidence, and the interval comes out too narrow. `bootstrap_ci` resamples whole
**clusters** (maker, plant, customer) with replacement and reports the pooled mean with a
percentile interval:

```sql
-- 40 requests from 4 makers; maker M4's wording defeats the matcher, so its misses come together
CREATE OR REPLACE TABLE per_request AS
SELECT 'R' || i AS query_id, 'M' || (i % 4 + 1) AS maker,
       CASE WHEN i % 4 = 3 THEN (i % 3 = 0)::INT ELSE 1 END::DOUBLE AS hit
FROM range(40) t(i);

SELECT (SELECT hi - lo FROM bootstrap_ci('per_request', 'maker', metrics := ['hit']))
     > (SELECT hi - lo FROM bootstrap_ci('per_request', 'query_id', metrics := ['hit'])) AS wider_by_maker,
       (SELECT estimate FROM bootstrap_ci('per_request', 'maker', metrics := ['hit'])) AS estimate,
       (SELECT n_clusters FROM bootstrap_ci('per_request', 'maker', metrics := ['hit'])) AS n_clusters;
-- => true | 0.85 | 4
```

The same 0.85 hit rate gets a wider interval by maker (0.3) than by request (0.225), because
only four makers stand behind it. Few clusters mean an honest, wide interval; that is the point.
The draws use DuckDB's hash, so the bounds can differ slightly between DuckDB versions; the same
seed gives the same interval on one version.

## Fitting Weights

`fit_coverage_weights` evaluates the kernel over reviewed pairs for a grid of `label_weight` and
`mu` and ranks the points by MAE. A grid over a few hundred reviewed pairs fits the noise of those
pairs as well as the signal. Fit on part of the reviewed requests, check the best point on the
rest, and change your defaults only when the holdout improves too. `mu` acts only with name
vectors; without them every `mu` scores the same. The grid takes at most 20 values per axis.

## Scale: Estimate Instead of Join

The exact covering search joins the request's features against the feature table. It is the
reference and usually fast enough. MinHash and LSH replace the join with an estimate and a
lookup; they pay off only for large vocabularies (raw part numbers), where most entities share
nothing. With a pivot vocabulary of about 20 classes, the multisets overlap so much that LSH still
scores about half the archive.

**Weighted MinHash** (ICWS) maps a weighted set to a signature of `num_hashes` values. The share
of equal positions in two signatures estimates their weighted Jaccard similarity; more hashes give
a tighter estimate. The hashing is stable across versions, so stored signatures stay valid.

**Containment from Jaccard.** A request wants containment (is the request inside the offer?),
but MinHash estimates Jaccard, which a large offer drags down. With the set sizes known, one turns
into the other: containment = J (|q| + |c|) / ((1 + J) |q|), capped at 1.

```sql
SELECT c AS containment, round(j, 4) AS jaccard, round(j * (2 + 6) / ((1 + j) * 2), 4) AS containment_from_jaccard
FROM (SELECT containment(['a', 'b'], ['a', 'b', 'c', 'd', 'e', 'f']) AS c,
             jaccard_similarity(['a', 'b'], ['a', 'b', 'c', 'd', 'e', 'f']) AS j);
-- => 1.0 | 0.3333 | 1.0
```

**Size-partitioned LSH** (LSH-Ensemble style). Banding finds pairs above a Jaccard threshold, but
a containment threshold t is a different Jaccard threshold for every offer size. The index
therefore splits the entities into size partitions (`partitions`, default 4). For each query and
partition it converts t into the Jaccard threshold for the partition's largest set,
t|q| / (|q| + u − t|q|) with u that size, and picks the rows per band (1, 2, 4 or 8) so that the
LSH S-curve passes below it. In mode `'jaccard'` the threshold is used as a Jaccard threshold
directly. Either way the threshold only sets recall: candidates that share no band are never
scored, and the output is not filtered by it, so a query can get fewer than `k` rows.

## Clustering Is Transitive

`cluster_matches` links every pair at or above the threshold and returns the connected groups
(union-find). Links chain: A–B and B–C put A and C in one cluster even when A–C scores low.

```sql
CREATE OR REPLACE TABLE pairs AS SELECT * FROM (VALUES
  ('Müller GmbH Ulm', 'Mueller GmbH Ulm', 0.95),
  ('Mueller GmbH Ulm', 'Mueller AG Ulm', 0.86),
  ('Müller GmbH Ulm', 'Mueller AG Ulm', 0.40)) t(a, b, score);
SELECT entity_id, cluster_size FROM cluster_matches('pairs', threshold := 0.85) ORDER BY entity_id;
-- => Mueller AG Ulm | 3
-- => Mueller GmbH Ulm | 3
-- => Müller GmbH Ulm | 3
```

The GmbH and the AG are different legal entities; one borderline pair merged them. The guards
come before the clustering: block on hard identifiers (tax id, country) so unrelated records are
never paired, score with fields that separate legal entities, and raise the threshold rather than
post-filter clusters. At 0.9 the AG stays alone.

## Why These Defaults

| Default | Why |
|---|---|
| `good` 0.9 in `threshold_at_precision` | "Covers almost everything" is what an automatic accept must guarantee (the BRD's auto-accept level) |
| `target_precision` 0.95 | The BRD target (goal G2): at most one wrong automatic accept in twenty; set your own |
| `k` [1, 5, 10, 20] in `eval_ranking` | First answer, first screen, and the stage-1 depth of `tree_coverage` |
| bootstrap `n` 1000, `level` 0.95, `seed` 7 | Stable percentile bounds at low cost; a fixed seed makes reports repeatable |
| `num_hashes` 128 | A standard error of about 0.04 at J = 0.5, at 1 KB per signature |
| `partitions` 4 | Enough size classes that small queries in large sets are found, few enough to keep the index small |
| `threshold` 0.8 in `cluster_matches` | A merge is hard to undo; start strict and lower it with review |

The design history (requirements, rejected options) is in the
[HLD](../requirements-matching/HLD.md) (§4.7 to §4.9); it records how these rules were decided,
not the current contract.
