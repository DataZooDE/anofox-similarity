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

## Textual Embeddings

### Compute Textual Embeddings
Generates semantic embeddings from material descriptions.

**Signature:**
```sql
compute_textual_embeddings(
    makt_table := 'sap_makt',
    language := 'EN',
    provider := 'gemma-local',
    provider_config := NULL
) → TABLE (
    material_id VARCHAR,
    textual_embedding FLOAT[384]
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `makt_table` | VARCHAR | 'sap_makt' | Material text master table |
| `language` | VARCHAR | 'EN' | Language code |
| `provider` | VARCHAR | 'gemma-local' | Embedding provider |
| `provider_config` | VARCHAR | NULL | Provider-specific config JSON |

**Providers** (names the function accepts; none is wired to a real model in the released build):
- `gemma-local` - intended for a local OpenVINO model (384-D)
- `openai`, `anthropic` - intended for the vendors' APIs (config: `{"api_key": "..."}`)

> [!IMPORTANT]
> Unless the extension is built with a local OpenVINO model or a real API backend is wired in,
> **all providers currently return a deterministic hash-based PLACEHOLDER embedding** — useful for
> plumbing and tests, but not for semantic search. The `makt_table` must have `matnr`, `spras`,
> `maktx`, **and `maktg`** (both description columns are referenced; a MAKT lacking `maktg` errors).
> Multiple MAKT rows per `(matnr, spras)` are collapsed to one description. The default
> `language` is `'EN'`; SAP `MAKT.SPRAS` typically uses the single character `'E'`, so pass
> `language := 'E'` when working directly from SAP MAKT.

**Returns:**
- `material_id` - Material identifier
- `textual_embedding` - 384-D FLOAT array

**Example:**
```sql
SELECT
  material_id,
  textual_embedding
FROM compute_textual_embeddings(
  provider := 'gemma-local'
);
```

### Embed Text Lambda

User-friendly text embedding function.

**Signature:**
```sql
embed_text(description VARCHAR) → FLOAT[384]
```

**Example:**
```sql
SELECT embed_text('High-precision ball bearing assembly') AS embedding;
```

---

## Transactional Embeddings

### Compute Transactional Embeddings
Extracts 98+ time-series features from goods movement history.

**Signature:**
```sql
compute_transactional_embeddings(
    movements_table := 'goods_movements',
    time_window_days := 365,
    batch_size := NULL,
    batch_offset := 0
) → TABLE (
    material_id VARCHAR,
    transactional_embedding FLOAT[128]
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `movements_table` | VARCHAR | 'goods_movements' | Source movements table |
| `time_window_days` | INTEGER | 365 | Historical window (relative to the latest movement date in the data) |
| `batch_size` | INTEGER | NULL | Optional batch size for incremental processing |
| `batch_offset` | INTEGER | 0 | Offset for batch processing |

> [!IMPORTANT]
> **Requires the `anofox-forecast` extension** (`anofox_fcst_ts_features`). If it is not loaded,
> this function raises a clear error telling you to install it — it does not silently return NULL.
> The `movements_table` must have `material_id`, `movement_date`, `quantity`; a `movement_type`
> column is used for the ERP movement-type features when present and treated as NULL when absent.

**Features Extracted:**
- **Consumption Metrics** (0-9): mean, median, variance, std dev, trend, sum, length, extrema
- **Volatility Metrics** (10-19): coefficient of variation, skewness, kurtosis, entropy
- **Frequency Metrics** (20-29): FFT coefficients and aggregates
- **Temporal Metrics** (30-97): Autocorrelation, quantiles, seasonality indicators
- **Domain-Specific Features** (92-97): Movement types, lifecycle indicators

**Returns:**
- `material_id` - Material identifier
- `transactional_embedding` - 128-D normalized FLOAT array

**Example:**
```sql
SELECT
  material_id,
  transactional_embedding
FROM compute_transactional_embeddings(
  movements_table := 'goods_movements',
  time_window_days := 180
);
```

### Recompute Embedding Statistics
Computes z-score normalization statistics for transactional embeddings.

**Signature:**
```sql
recompute_embedding_statistics(
    time_window_days := 365,
    min_observations := 3,
    movements_table := 'goods_movements'
) → TABLE (
    feature_name VARCHAR,
    feature_index INTEGER,
    feature_category VARCHAR,
    mean_value DOUBLE,
    std_value DOUBLE,
    min_value DOUBLE,
    max_value DOUBLE,
    num_samples INTEGER
)
```
> [!NOTE]
> Requires the `anofox-forecast` extension; without it this fails at bind time with a catalog error
> naming `anofox_fcst_ts_features` (unlike `compute_transactional_embeddings`, which gives a friendlier message).

**Example:**
```sql
SELECT * FROM recompute_embedding_statistics();
```

---

## Multi-modal Fusion

### Compute Fused Embeddings
Combines structural, textual, and transactional embeddings.

**Signature:**
```sql
compute_fused_embeddings(
    weights_structural := 0.5,
    weights_textual := 0.5,
    weights_transactional := 0.0
) → TABLE (
    material_id VARCHAR,
    combined_embedding FLOAT[768],
    fusion_weights STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT)
)
```

> [!NOTE]
> The fused-vector column is named **`combined_embedding`** (not `fused_embedding`). `fusion_weights`
> reports the **normalized** weights actually applied (each input weight divided by their sum), typed
> `FLOAT` (not `DOUBLE`). Each modality's vector is scaled by `sqrt(weight)` before concatenation (so
> that combining two unit-norm vectors at weight 0.5 each yields a unit-norm result), not by the raw
> weight — this affects magnitude if you consume `combined_embedding` for anything beyond cosine
> similarity / nearest-neighbor search.

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `weights_structural` | DOUBLE | 0.5 | Structural embedding weight |
| `weights_textual` | DOUBLE | 0.5 | Textual embedding weight |
| `weights_transactional` | DOUBLE | 0.0 | Transactional embedding weight |

> [!NOTE]
> Reads from the fixed `material_embeddings` table (there is no `embedding_table` parameter).
> Weights need not sum to 1 — they are normalized. A modality with weight `0` is ignored even if
> its vector is NULL, so the default (structural + textual) works without transactional embeddings.

**Returns:**
- `material_id` - Material identifier
- `combined_embedding` - Combined 768-D FLOAT array (column name is `combined_embedding`)
- `fusion_weights` - STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT) of the normalized weights applied

**Example:**
```sql
SELECT
  material_id,
  combined_embedding
FROM compute_fused_embeddings(
  weights_structural := 0.4,
  weights_textual := 0.4,
  weights_transactional := 0.2
);
```

---

## Statistics & Normalization

### Check Statistics Freshness
Validates whether transactional embedding statistics are current and complete.

**Signature:**
```sql
check_statistics_freshness() → TABLE (
    stat_count BIGINT,
    max_samples INTEGER,
    current_version INTEGER,
    last_updated TIMESTAMP,
    is_fresh BOOLEAN
)
```

**Returns:**
- `stat_count` - Number of statistics records
- `max_samples` - Maximum samples used in computation
- `current_version` - Statistics version number
- `last_updated` - Last update timestamp
- `is_fresh` - TRUE if ≥30 stats and updated within 7 days

**Example:**
```sql
SELECT * FROM check_statistics_freshness();
-- Result: stat_count=98, is_fresh=true
```

### Compute Domain-Specific Statistics
Computes advanced domain-specific ERP features (indices 92-97).

**Signature:**
```sql
compute_domain_specific_statistics(
    movements_table := 'goods_movements',
    time_window_days := 365,
    min_observations := 3
) → TABLE (feature_name, feature_index, feature_category, mean_value, std_value, ...)
```

> [!NOTE]
> The movements table must use the canonical column names `material_id`, `movement_date`,
> `quantity`, `movement_type`. Requires the `anofox-forecast` extension (lifecycle features 96–97).

**Features Computed:**
- Feature 92: Movement type receipt ratio
- Feature 93: Movement type reversal ratio
- Feature 94: Weekday/weekend ratio
- Feature 95: Weekday concentration (entropy-based)
- Feature 96: Lifecycle trend strength
- Feature 97: Lifecycle growth indicator

---

## Embedding Generation

### Compute Jaccard Embeddings
Generates 128-dimensional min-hash embeddings for Jaccard similarity approximation.

**Signature:**
```sql
compute_jaccard_embeddings(
    bom_table := 'bom_items'
) → TABLE (
    material_id VARCHAR,
    seed INTEGER,
    minhash_value FLOAT,
    num_components INTEGER
)
```

**Returns:**
- `material_id` - Material identifier
- `seed` - Seed index [0-127] for 128-D embedding
- `minhash_value` - Min-hash value for this seed
- `num_components` - Component count for this material

**Usage Pattern:**
```sql
-- Generate embeddings and aggregate into FLOAT[128] arrays
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components)
SELECT
    material_id,
    array_agg(minhash_value ORDER BY seed)::FLOAT[128],
    MAX(num_components)
FROM compute_jaccard_embeddings(bom_table := 'bom_items')
GROUP BY material_id
ON CONFLICT (material_id) DO UPDATE SET
    jaccard_embedding = EXCLUDED.jaccard_embedding,
    num_components = EXCLUDED.num_components;
```

### Extract Time-Series Features
Shared macro for efficient time-series feature extraction (avoids redundant computation).

**Signature:**
```sql
extract_ts_features(
    movements_table := 'goods_movements',
    time_window_days := 365,
    min_observations := 3,
    batch_size := NULL,
    batch_offset := 0
) → TABLE (
    material_id VARCHAR,
    features STRUCT
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `movements_table` | VARCHAR | 'goods_movements' | Source movements table |
| `time_window_days` | INTEGER | 365 | Historical window |
| `min_observations` | INTEGER | 3 | Minimum data points |
| `batch_size` | INTEGER | NULL | Batch size for incremental processing |
| `batch_offset` | INTEGER | 0 | Offset for batch processing |

**Note:** Requires anofox-forecast extension for `anofox_fcst_ts_features()`.

---

## Vector Search & Indexing

### HNSW Indexes

HNSW indexes over the `material_embeddings` vector columns are **created automatically when the
extension loads** (see [HNSW Indexes](06-infrastructure.md#hnsw-indexes) for the index list). There is no
SQL-callable `CreateHNSWIndexes(...)` function — that entry point does not exist. Build vector
indexes directly with the `vss` extension's DDL if you need custom parameters, e.g.:

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

Generates a semantic embedding per material from its description text, returning (material_id, textual_embedding). Reads descriptions from 'makt_table' in the given 'language' and embeds them with 'provider'; 'provider_config' passes provider-specific settings through to the backend.

```sql
compute_textual_embeddings(makt_table, language, provider, provider_config)
```

**Examples**

```sql
SELECT * FROM compute_textual_embeddings(makt_table := 'sap_makt', language := 'EN');
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

Generates a 384-dimensional text embedding (FLOAT[384]) for the given text. provider selects the backend and provider_config is a JSON string of provider-specific options (api_key, endpoint). NOTE: unless the extension is built with a local OpenVINO model or a real API backend is configured, all providers return a deterministic, hash-based PLACEHOLDER embedding (useful for wiring and tests, not semantic search).

```sql
embedding_backend(text VARCHAR, provider VARCHAR, provider_config VARCHAR) → FLOAT[]
```

**Examples**

```sql
SELECT embedding_backend('diesel pump', 'gemma-local', '{}');
SELECT embedding_backend('valve seat', 'gemini-api', '{"api_key": "..."}');
```

### `compute_transactional_embeddings`

*Table function*

Computes 128-dimensional transactional embeddings from goods movement data and returns them (it does not write to material_embeddings). Features include demand velocity, seasonality, frequency, and quantity statistics normalized against transactional_embedding_statistics. Supports batched computation via batch_size and batch_offset.

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

Fuses structural (FLOAT[256]), textual (FLOAT[384]), and transactional (FLOAT[128]) embeddings into a single 768-dimensional combined embedding using weighted concatenation. Each component is scaled by sqrt(weight). Weights do not need to sum to 1.0.

```sql
fuse_embeddings(structural_embedding FLOAT[], textual_embedding FLOAT[], transactional_embedding FLOAT[], weights STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT)) → FLOAT[]
```

**Examples**

```sql
SELECT fuse_embeddings(se, te, xe, {'structural':0.5,'textual':0.5,'transactional':0.0}) FROM material_embeddings;
```

### `compute_jaccard_embeddings`

*Table function*

Computes and RETURNS 128-dimensional MinHash embeddings (one row per material/seed) from BOM component sets; it does not write to material_embeddings — aggregate and upsert the result yourself. Each material's embedding is derived from its set of direct child components. Requires bom_table with columns parent_id and child_id.

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

Shared time-series feature extraction over a goods-movement table, computed once as a CTE so callers do not each re-run the expensive per-material feature computation. 'batch_size' and 'batch_offset' page through large material sets; NULL batch_size means all of them.

```sql
extract_ts_features(movements_table, time_window_days, min_observations, batch_size, batch_offset)
```

**Examples**

```sql
SELECT * FROM extract_ts_features(movements_table := 'goods_movements', time_window_days := 365);
```

### `recompute_embedding_statistics`

*Table macro*

Recomputes the transactional embedding statistics from current movement data, returning the per-feature normalisation statistics the embedding pipeline uses. 'time_window_days' is the historical window and 'min_observations' the minimum data points a material needs to qualify.

```sql
recompute_embedding_statistics(time_window_days, min_observations, movements_table)
```

**Examples**

```sql
SELECT * FROM recompute_embedding_statistics(time_window_days := 365, min_observations := 3);
```

### `check_statistics_freshness`

*Table function*

Returns a single-row summary of the transactional_embedding_statistics table including stat_count, max_samples, current_version, last_updated, and is_fresh. is_fresh is TRUE when >= 30 statistics rows exist and last_updated is within 7 days.

```sql
check_statistics_freshness()
```

**Examples**

```sql
SELECT * FROM check_statistics_freshness();
```

### `compute_domain_specific_statistics`

*Table macro*

Computes the advanced domain-specific ERP features — movement type mix, day-of-week pattern and lifecycle position — used alongside the core time-series features.

```sql
compute_domain_specific_statistics(movements_table, time_window_days, min_observations)
```

**Examples**

```sql
SELECT * FROM compute_domain_specific_statistics(movements_table := 'goods_movements');
```

<!-- generated:functions:end -->
