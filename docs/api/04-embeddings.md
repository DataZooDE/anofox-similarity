# Embeddings and vector search

> Textual, transactional and fused embeddings, statistics and HNSW indexes.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

> [!WARNING]
> **Textual embeddings are a placeholder in the released build.** Without a real embedding backend
> compiled in, `compute_textual_embeddings`, `embed_text` and `embedding_backend` return a
> deterministic hash vector, not a semantic embedding: similar texts do not get similar vectors.
> For semantic similarity, compute vectors with your own model and bring them in as a table (the
> pivot classifier, [`classify_to_catalogue`](14-classification-and-review.md), works that way), or
> combine your vectors with `fuse_embeddings`. Transactional and Jaccard embeddings are real.

Every embedding here is a fixed-size `FLOAT[n]` array (`FLOAT[384]` textual, `FLOAT[128]`
transactional and Jaccard, `FLOAT[256]` structural, `FLOAT[768]` fused): the type DuckDB `vss` HNSW
indexes and the `array_distance` / `array_cosine_distance` / `array_negative_inner_product`
functions take. A list of the right length casts in implicitly. See
[Embedding vs signature](00-conventions.md#embedding-vs-signature).

## Status

| Function | Returns | Real or placeholder | Needs |
|---|---|---|---|
| `compute_transactional_embeddings` | `material_id, transactional_embedding FLOAT[128]` | real | `anofox_forecast` from community (with `anofox_fcst_ts_features_agg`); `transactional_embedding_statistics` filled once (writable database) |
| `compute_jaccard_embeddings` | `material_id, seed, minhash_value FLOAT, num_components` (one row per seed) | real (MinHash) | a BOM table (`parent_id, child_id`) |
| `fuse_embeddings` | `FLOAT[768]` | real (as good as its inputs) | — |
| `compute_fused_embeddings` | `material_id, combined_embedding FLOAT[768], fusion_weights STRUCT(structural, textual, transactional FLOAT)` | real (as good as its inputs) | `material_embeddings` (writable database once) |
| `compute_textual_embeddings`, `embed_text`, `embedding_backend` | `FLOAT[384]` | **placeholder** (hash) | MAKT with `matnr, spras, maktx, maktg` for the table macro |
| `recompute_embedding_statistics`, `extract_ts_features` | the 36 statistics rows the embedding reads; weekly features per material | real | `anofox_forecast` |
| `compute_domain_specific_statistics` | the six ERP rows (92-97), also part of `recompute_embedding_statistics` | real | `anofox_forecast` |
| `check_statistics_freshness` | `stat_count, max_samples, current_version, last_updated, is_fresh` | real | — (a read-only database gives `0`, NULL, `false`) |

What the vectors contain, how fusion weighs them and which distance fits which vector:
[embeddings and fusion](../concepts/embeddings-and-fusion.md).

## Signatures and Defaults

```text
compute_textual_embeddings(makt_table := 'sap_makt', language := 'E', provider := 'gemma-local', provider_config := '')
embed_text(description VARCHAR) → FLOAT[384]          -- embedding_backend(description, 'gemma-local', '')
compute_transactional_embeddings(movements_table := 'goods_movements', time_window_days := 365,
                                 batch_size := NULL, batch_offset := 0)
recompute_embedding_statistics(time_window_days := 365, min_observations := 3, movements_table := 'goods_movements')
  → TABLE(feature_name, feature_index, feature_category, mean_value, std_value, min_value, max_value, num_samples)
compute_domain_specific_statistics(movements_table := 'goods_movements', time_window_days := 365, min_observations := 3)
extract_ts_features(movements_table := 'goods_movements', time_window_days := 365, min_observations := 3,
                    batch_size := NULL, batch_offset := 0) → TABLE(material_id, features STRUCT)
compute_fused_embeddings(weights_structural := 0.5, weights_textual := 0.5, weights_transactional := 0.0)
compute_jaccard_embeddings(bom_table := 'bom_items')
check_statistics_freshness()
```

## Rules and Pitfalls

- **Textual providers** are `gemma-local`, `openai`, `anthropic` and `cohere` (case-insensitive);
  any other name is an error (`embedding_backend: unknown provider ...`). None is wired to a model
  in the released build. `provider_config` is not read yet; do not put API keys in SQL.
- **`language`** takes SAP's one-character key (`'E'`, default) or the ISO code (`'EN'`); MAKT rows
  match in either form. Several MAKT rows per `(matnr, spras)` collapse to one description; a MAKT
  without `maktg` is an error.
- **`anofox_forecast`**: `INSTALL anofox_forecast FROM community; LOAD anofox_forecast;` first.
  Without it `compute_transactional_embeddings` says to install and load it; with a build that has
  no `anofox_fcst_ts_features_agg` it says to update it. `recompute_embedding_statistics` and
  `extract_ts_features` fail at bind time with a catalog error naming that aggregate.
- **Movements** need `material_id, movement_date, quantity`; `movement_type` (SAP `bwart`) is
  optional, without it positions 92 and 93 are 0. The time-series features (0-29) read the
  positive quantities as a weekly series, zero-filled between a material's first and last week;
  the ERP features (92-95) count movements of either sign, so a reversal signed negative counts.
  A material needs `min_observations` (default 3) movements. `time_window_days` counts back from
  the latest movement date in the data.
- **Statistics**: `recompute_embedding_statistics` returns all 36 rows the embedding reads
  (positions 0-29 and 92-97); store them yourself, once per refresh:
  `INSERT OR REPLACE INTO transactional_embedding_statistics SELECT *, now(), 1 FROM recompute_embedding_statistics();`
  A feature a short history cannot give (NaN, infinite) is left out of the statistics and is 0 in
  that material's vector. `check_statistics_freshness` says `is_fresh` with
  at least 30 rows, the newest updated within 7 days; `last_updated` is NULL without statistics.
- **`compute_fused_embeddings`** reads the fixed `material_embeddings` table (no table parameter);
  the column is `combined_embedding`, and `fusion_weights` reports the normalised weights as `FLOAT`.
  Weights need not sum to 1. A modality with weight 0 may be NULL; a positive weight on a NULL
  vector gives NULL.
- **`fuse_embeddings`** takes `FLOAT[256]`, `FLOAT[384]`, `FLOAT[128]` and a weights STRUCT; a list
  of another length is a cast error (`Cannot cast list with length 2 to array with length 256`),
  not NULL. All weights 0 gives NULL.
- **`compute_jaccard_embeddings`** returns rows; aggregate them in seed order into the `FLOAT[128]`
  `jaccard_embedding` and compare with `minhash_similarity`, never with cosine or L2:

```sql
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components)
SELECT material_id, array_agg(minhash_value ORDER BY seed)::FLOAT[128], max(num_components)
FROM compute_jaccard_embeddings(bom_table := 'bom_items')
GROUP BY material_id
ON CONFLICT (material_id) DO UPDATE SET
    jaccard_embedding = EXCLUDED.jaccard_embedding, num_components = EXCLUDED.num_components;
```

## Vector Search & Indexing

### HNSW Indexes

HNSW indexes over the `material_embeddings` vector columns are **created automatically when the
extension loads** (see [HNSW Indexes](06-infrastructure.md#hnsw-indexes) for the index list). There is no
SQL-callable `CreateHNSWIndexes(...)` function — that entry point does not exist. Build vector
indexes directly with the `vss` extension's DDL if you need custom parameters. HNSW indexes only
`FLOAT[n]` ARRAY columns, which is why every embedding here is a `FLOAT[n]` array, e.g.:

```sql
INSTALL vss; LOAD vss;
SET hnsw_enable_experimental_persistence = true;
CREATE INDEX my_idx ON material_embeddings USING HNSW (combined_embedding) WITH (metric = 'cosine');
```

---

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `compute_textual_embeddings`

*Table macro*

Generates an embedding per material from its description text, returning (material_id, textual_embedding). In the released build every provider returns a deterministic hash PLACEHOLDER, not a semantic embedding. Reads descriptions from 'makt_table' in the given 'language' (SAP key 'E' or ISO 'EN', either form matches; default 'E'; clients are merged, so filter MAKT to one client first) and embeds them with 'provider'; 'provider_config' is accepted but not read yet (do not put API keys in it).

```sql
compute_textual_embeddings(makt_table, language, provider, provider_config)
```

**Examples**

```sql
SELECT * FROM compute_textual_embeddings(makt_table := 'sap_makt', language := 'E');
```

### `embed_text`

*Scalar macro*

Embeds a single piece of text with the default local backend — shorthand for embedding_backend(description, 'gemma-local', '').

```sql
embed_text(description)
```

**Examples**

```sql
SELECT embed_text('centrifugal pump seal kit');
```

### `embedding_backend`

*Scalar function*

Generates a 384-dimensional text embedding (FLOAT[384] array) for the given text. provider (gemma-local, openai, anthropic or cohere, case-insensitive; anything else is an error) selects the backend. provider_config is accepted but not read yet; do not put API keys in it. NOTE: unless the extension is built with a local OpenVINO model or a real API backend is configured, all providers return a deterministic, hash-based PLACEHOLDER embedding (useful for wiring and tests, not semantic search).

```sql
embedding_backend(text VARCHAR, provider VARCHAR, provider_config VARCHAR) → FLOAT[384]
```

**Examples**

```sql
SELECT embedding_backend('diesel pump', 'gemma-local', '{}');
SELECT embedding_backend('valve seat', 'openai', '');
```

### `compute_transactional_embeddings`

*Table function*

Computes 128-dimensional transactional embeddings from goods movements (material_id, movement_date, quantity[, movement_type]) and returns them (it does not write to material_embeddings): 30 time-series features of the weekly series (positions 0-29, from anofox_forecast) and six ERP features (92-97), each a z-score against transactional_embedding_statistics (fill it with recompute_embedding_statistics), then scaled to length 1; the other positions are 0. Needs anofox_forecast (INSTALL/LOAD anofox_forecast FROM community). Volume features count: the same pattern at another volume is not close. Supports batched computation via batch_size and batch_offset.

```sql
compute_transactional_embeddings(movements_table, time_window_days, batch_size, batch_offset)
```

**Examples**

```sql
SELECT * FROM compute_transactional_embeddings();
SELECT * FROM compute_transactional_embeddings(movements_table := 'sap_mseg', time_window_days := 365);
SELECT * FROM compute_transactional_embeddings(batch_size := 1000, batch_offset := 0);
```

### `compute_fused_embeddings`

*Table macro*

Combines the structural, textual and transactional embedding of every material in material_embeddings into one fused vector, returning (material_id, combined_embedding, fusion_weights). Weights are normalised to sum to 1, matching what fuse_embeddings applies internally, and a modality's vector is only required when its weight is above zero.

```sql
compute_fused_embeddings(weights_structural, weights_textual, weights_transactional)
```

**Examples**

```sql
SELECT * FROM compute_fused_embeddings(weights_structural := 0.6, weights_textual := 0.4);
```

### `fuse_embeddings`

*Scalar function*

Fuses structural (FLOAT[256]), textual (FLOAT[384]) and transactional (FLOAT[128]) embeddings into one FLOAT[768] array by weighted concatenation, each part scaled by sqrt(weight). Weights are normalised to sum to 1; a modality with weight 0 may be NULL. Lists cast to the array types (a list of another length is an error).

```sql
fuse_embeddings(structural_embedding FLOAT[256], textual_embedding FLOAT[384], transactional_embedding FLOAT[128], weights STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT)) → FLOAT[768]
```

**Examples**

```sql
SELECT fuse_embeddings(structural_embedding, textual_embedding, transactional_embedding, {'structural':0.5,'textual':0.5,'transactional':0.0}) FROM material_embeddings;
```

### `compute_jaccard_embeddings`

*Table function*

Computes and RETURNS 128-dimensional MinHash embeddings (one row per material/seed, minhash_value FLOAT in [0, 1)) from BOM component sets; it does not write to material_embeddings: aggregate them in seed order into the FLOAT[128] jaccard_embedding yourself. minhash_similarity of two such arrays estimates the Jaccard index. Each material's embedding is derived from its set of direct child components. Requires bom_table with columns parent_id and child_id.

```sql
compute_jaccard_embeddings(bom_table)
```

**Examples**

```sql
SELECT * FROM compute_jaccard_embeddings();
SELECT * FROM compute_jaccard_embeddings(bom_table := 'sap_stpo');
```

### `extract_ts_features`

*Table macro*

The 30 time-series features of the transactional embedding per material, from anofox_forecast's anofox_fcst_ts_features_agg over the weekly series of positive quantities (zero-filled between the first and last week in the window). Returns (material_id, features STRUCT) with stable field names; NaN and infinite values (short histories) are NULL. A material needs min_observations movements. 'batch_size' and 'batch_offset' page through large material sets; NULL batch_size means all of them. Needs anofox_forecast (INSTALL/LOAD anofox_forecast FROM community).

```sql
extract_ts_features(movements_table, time_window_days, min_observations, batch_size, batch_offset)
```

**Examples**

```sql
SELECT * FROM extract_ts_features(movements_table := 'goods_movements', time_window_days := 365);
```

### `recompute_embedding_statistics`

*Table macro*

Recomputes the statistics compute_transactional_embeddings normalises with: one row per feature it reads (positions 0-29 from extract_ts_features, 92-97 from compute_domain_specific_statistics), mean and standard deviation over the finite values. Store them: INSERT OR REPLACE INTO transactional_embedding_statistics SELECT *, now(), 1 FROM recompute_embedding_statistics(). 'time_window_days' is the historical window and 'min_observations' the minimum movements a material needs. Needs anofox_forecast.

```sql
recompute_embedding_statistics(time_window_days, min_observations, movements_table)
```

**Examples**

```sql
SELECT * FROM recompute_embedding_statistics(time_window_days := 365, min_observations := 3);
```

### `check_statistics_freshness`

*Table function*

Returns a single-row summary of the transactional_embedding_statistics table including stat_count, max_samples, current_version, last_updated, and is_fresh. is_fresh is TRUE when >= 30 statistics rows exist and the newest was updated within 7 days. last_updated is NULL when there are no statistics (also on a read-only database, where the table is not created).

```sql
check_statistics_freshness()
```

**Examples**

```sql
SELECT * FROM check_statistics_freshness();
```

### `compute_domain_specific_statistics`

*Table macro*

Statistics of the six ERP features of the transactional embedding (positions 92-97): share of movement type 261, share of reversals (262, 602), weekend share, weekday concentration, lifecycle trend strength and growth. Counts movements of either sign; movement_type is optional (shares 0 without it). Part of recompute_embedding_statistics. Needs anofox_forecast for the lifecycle features.

```sql
compute_domain_specific_statistics(movements_table, time_window_days, min_observations)
```

**Examples**

```sql
SELECT * FROM compute_domain_specific_statistics(movements_table := 'goods_movements');
```

<!-- generated:functions:end -->
