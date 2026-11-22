# Embeddings and Fusion

> What the embedding vectors contain, which of them carry meaning, how fusion weighs them, and
> which distance fits which vector. For the calls, see [api/04](../api/04-embeddings.md) and
> [api/06](../api/06-infrastructure.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Four Vectors per Material

`material_embeddings` holds up to four vectors per material, each a fixed-size `FLOAT[n]` array:

| Vector | Size | Built from | Carries meaning | Compare with |
|---|---|---|---|---|
| `structural_embedding` | 256 | your BOM-structure pipeline (no function here computes it) | as good as your model | L2 (`array_distance`), HNSW `l2sq` |
| `textual_embedding` | 384 | `compute_textual_embeddings`, `embed_text` | **no** in the released build: a hash placeholder | cosine, HNSW `cosine` |
| `transactional_embedding` | 128 | `compute_transactional_embeddings` (goods movements) | yes | cosine |
| `jaccard_embedding` | 128 | `compute_jaccard_embeddings` (BOM component sets) | yes, as a MinHash signature | `minhash_similarity` only |
| `combined_embedding` | 768 | `compute_fused_embeddings`, `fuse_embeddings` | as much as its parts | cosine, HNSW `cosine` |

`FLOAT[n]` is the type DuckDB `vss` indexes and `array_cosine_distance` take. A list of the right
length casts in; a list of another length is a cast error, not NULL.

## Textual Embeddings Are a Placeholder

Without a real backend compiled in, every textual provider (`gemma-local`, `openai`, `anthropic`,
`cohere`) returns a deterministic 64-bit hash per element. The same text always gets the same
vector, on every platform; different texts get unrelated vectors, however close their meaning:

```sql
SELECT round(array_cosine_similarity(embed_text('ball bearing 6204'), embed_text('ball bearing 6204-2RS')), 3) AS near_text,
       round(array_cosine_similarity(embed_text('ball bearing 6204'), embed_text('ball bearing 6204')), 3) AS same_text;
-- => -0.065 | 1.0
```

Use it for plumbing and tests. For meaning, embed texts with your own model, store the vectors as
a table and pass them where a function takes vectors (`classify_to_catalogue`, `name_vectors`), or
write them into `textual_embedding` and fuse.

## Transactional Embeddings

A material's goods movements become a time series. Thirty features are taken from
`anofox_fcst_ts_features` (the `anofox-forecast` extension): level and spread (mean, median,
variance, standard deviation, sum, length, trend slope and intercept, position of the maximum),
shape and volatility (coefficient of variation, skewness, kurtosis, entropies, Benford
correlation), three FFT coefficients and two FFT aggregates, and autocorrelation at lags 1, 7,
14 and 28 days. Six ERP features follow at positions 92 to 97: the shares of movement types
261 and 262, the weekend share, the weekday concentration, and two lifecycle indicators from the
trend relative to the mean.

Each feature is turned into a z-score with the statistics in `transactional_embedding_statistics`
(`recompute_embedding_statistics` returns them; store them in that table yourself; a feature without a
statistics row comes out as 0), so a feature counts by how unusual it is across your materials, not
by its unit. The 36 values sit at positions 0 to 29 and 92 to 97; the other 92
positions are 0, kept free for further features. The vector is then scaled to length 1, so cosine
compares the profile of demand, not its volume.

## Jaccard Embeddings: MinHash, Not a Geometry

`compute_jaccard_embeddings` hashes each material's direct component set with 128 seeds and keeps
the smallest hash per seed, scaled to `[0, 1)`. Two sets agree at one position with probability
equal to their Jaccard similarity, so the share of equal positions (`minhash_similarity`) estimates
it. Distances over the values do not:

```sql
CREATE OR REPLACE TABLE bom_items AS SELECT * FROM (VALUES
  ('PUMP-A', 'MOTOR', 1), ('PUMP-A', 'SEAL', 1), ('PUMP-A', 'HOUSING', 1), ('PUMP-A', 'IMPELLER', 1),
  ('PUMP-B', 'MOTOR', 1), ('PUMP-B', 'SEAL', 1), ('PUMP-B', 'HOUSING', 1), ('PUMP-B', 'SENSOR', 1)
) t(parent_id, child_id, quantity);
CREATE OR REPLACE TABLE mh AS
SELECT material_id, array_agg(minhash_value ORDER BY seed)::FLOAT[128] AS jaccard_embedding
FROM compute_jaccard_embeddings(bom_table := 'bom_items') GROUP BY material_id;

SELECT round(minhash_similarity(a.jaccard_embedding, b.jaccard_embedding), 3) AS minhash_estimate,
       round(jaccard_similarity(['MOTOR', 'SEAL', 'HOUSING', 'IMPELLER'],
                                ['MOTOR', 'SEAL', 'HOUSING', 'SENSOR']), 3) AS exact_jaccard,
       round(array_cosine_similarity(a.jaccard_embedding, b.jaccard_embedding), 3) AS cosine
FROM mh a, mh b WHERE a.material_id = 'PUMP-A' AND b.material_id = 'PUMP-B';
-- => 0.625 | 0.6 | 0.846
```

Three of five components are shared (0.6); 128 positions estimate 0.625. The cosine of 0.846
means nothing: two sets with no shared component (`x1, x2, x3` against `y1, y2, y3`) get
`minhash_similarity` 0.0 but a cosine of 0.573, because all values lie in `[0, 1)`. That is why no HNSW index is built on `jaccard_embedding`. MinHash signatures for LSH
(`weighted_minhash`, `minhash_index`) are `UBIGINT[]` for the same reason: they are compared for
equality, never by distance.

## Fusion

`fuse_embeddings` concatenates the three parts (256 + 384 + 128 = 768). The weights are first
normalised to sum to 1. Each part is then multiplied by √weight, not by the weight:

```
combined = [ √w_s · structural , √w_t · textual , √w_x · transactional ],   w normalised to Σ w = 1
cos(combined_a, combined_b) = Σ_m w_m · cos_m(a, b)          (each part of length 1)
```

With unit-length parts the fused vector has length 1, and its cosine is the weighted mean of the
per-modality cosines: the weights mean what they say. With weights 2 : 1 : 1:

```sql
SELECT round(sqrt(list_sum(list_transform(f::FLOAT[], lambda v: v * v))), 4) AS norm,
       round(f[1], 4) AS structural_part, round(f[258], 4) AS textual_part, round(f[641], 4) AS transactional_part
FROM (SELECT fuse_embeddings(list_resize([1.0], 256, 0.0), list_resize([0.0, 1.0], 384, 0.0),
                             list_resize([1.0], 128, 0.0),
                             {'structural': 2.0, 'textual': 1.0, 'transactional': 1.0}) AS f);
-- => 1.0 | 0.7071 | 0.5 | 0.5
```

The normalised weights are 0.5, 0.25 and 0.25; the parts are √0.5 = 0.7071 and √0.25 = 0.5. A
modality with weight 0 contributes zeros and may be NULL; a positive weight on a NULL vector makes
the result NULL. The fused vector inherits the placeholder problem: with a textual weight above 0
and placeholder text vectors, that share of the cosine is noise.

## Which Distance for Which Vector

| Vector | Use | Do not use |
|---|---|---|
| structural | `array_distance` (the `l2sq` index) | — |
| textual, combined | `array_cosine_distance` (the `cosine` index), only with real text vectors | the placeholder for anything semantic |
| transactional | `array_cosine_distance` (length 1: same order as L2) | raw volume comparisons (the vector is normalised) |
| jaccard | `minhash_similarity` | cosine or L2: no meaning |

An HNSW index is used only for `ORDER BY array_cosine_distance(col, <constant>) LIMIT k` (check
with `EXPLAIN`: `HNSW_INDEX_SCAN`); a join or correlated subquery scans everything.

## Why These Defaults

| Default | Why |
|---|---|
| Fusion weights 0.5 / 0.5 / 0.0 | A weight-0 modality may be NULL, so the default works for materials without movement history (new parts) |
| √weight scaling | The fused cosine is the weighted mean of the parts' cosines |
| z-scores, then length 1 | Features in different units count equally; cosine compares demand profiles, not volumes |
| 128 MinHash seeds | A standard error of about 0.04 on the Jaccard estimate at 0.5, at 128 floats per material |
| No index on `jaccard_embedding` | No vss metric approximates Jaccard on MinHash values |
| Placeholder instead of an error | Pipelines and tests run end to end before a real model is wired in |

The design history is in [background/CONCEPT.md](../background/CONCEPT.md); it records how the
embedding layer was planned, not the current contract.
