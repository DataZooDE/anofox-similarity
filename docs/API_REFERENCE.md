# API Reference

**Version:** 2.0.0
**DuckDB Compatibility:** v1.5.6 and v2.0
**License:** Business Source License 1.1 (BSL 1.1)

---

## Table of Contents

1. [Overview](#overview)
2. [Core Similarity Algorithms](#core-similarity-algorithms)
3. [Similarity Search](#similarity-search)
4. [BOM Traversal & Navigation](#bom-traversal--navigation)
5. [Textual Embeddings](#textual-embeddings)
6. [Transactional Embeddings](#transactional-embeddings)
7. [Multi-modal Fusion](#multi-modal-fusion)
8. [ERP Transformations](#erp-transformations)
9. [Statistics & Normalization](#statistics--normalization)
10. [Embedding Generation](#embedding-generation)
11. [Helper Macros](#helper-macros)
12. [Incremental Updates](#incremental-updates)
13. [Dependency Checks](#dependency-checks)
14. [Vector Search & Indexing](#vector-search--indexing)
15. [Property Graph Integration](#property-graph-integration)
16. [Database Infrastructure](#database-infrastructure)
17. [Coverage Matching](#coverage-matching)
18. [Function Coverage Matrix](#function-coverage-matrix)
19. [Parameter Reference](#parameter-reference)

---

## Overview

The anofox-similarity extension provides about 65 functions across 9 functional categories for multi-modal product similarity detection and coverage matching. All functions are registered as SQL macros, table functions or scalar functions and follow consistent naming conventions.

### Naming Convention

- **Table Macros** (prefix `compute_`, `find_`, `infer_`) - Return result sets
- **Scalar Functions** (prefix `jaccard_`, `wl_kernel_`, `embed_`) - Return single values
- **Coverage matching** - set measures are named after the measure (`containment`, `tversky_similarity`, `weighted_*`); searches start with `find_`
- **Helper Macros** (prefix `check_`, `create_`, `bom_`) - Infrastructure utilities

### Parameterization Pattern

On DuckDB 1.5.6, named arguments of scalar functions are matched by position, so give them in the documented order; DuckDB 2.0 matches them by name.

All macros support default parameters with `:=` syntax:

```sql
SELECT * FROM macro_name(
  param1 := value1,    -- Optional, uses default if omitted
  param2 := value2
)
```

---

## Core Similarity Algorithms

### 1. Jaccard Similarity (Scalar Function)

Computes set overlap using Jaccard index. Takes component lists directly.

**Signature:**
```sql
jaccard_similarity(
    list_a LIST,
    list_b LIST
) → DOUBLE
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `list_a` | LIST(ANY) | Required | First set of elements |
| `list_b` | LIST(ANY) | Required | Second set of elements |

**Returns:**
- DOUBLE: Similarity score [0.0, 1.0]
  - 0.0 = Completely disjoint (no common elements), **and by convention two empty (or all-NULL) sets also score 0.0**
  - 1.0 = Identical element sets

**Complexity:** O(n + m) where n, m = list sizes

**Example:**
```sql
-- Direct list comparison
SELECT jaccard_similarity(['A', 'B', 'C'], ['A', 'B', 'D']) AS similarity;
-- Result: 0.5 (2 common out of 4 unique elements)

-- With BOM data (via aggregate_material_components helper)
WITH components AS (
  SELECT material_id, components FROM aggregate_material_components('bom_items')
)
SELECT jaccard_similarity(a.components, b.components) AS similarity
FROM components a, components b
WHERE a.material_id = 'PUMP-A' AND b.material_id = 'PUMP-B';
```

### 2. WL Kernel Similarity (Scalar Function)

Computes graph-structural similarity using Weisfeiler-Lehman kernel.

**Signature:**
```sql
wl_kernel_similarity(
    material_a VARCHAR,
    material_b VARCHAR,
    iterations := 3,
    bom_table := 'bom_items'
) → DOUBLE
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `material_a` | VARCHAR | Required | First material ID |
| `material_b` | VARCHAR | Required | Second material ID |
| `iterations` | INTEGER | 3 | Number of refinement iterations (clamped: values above 5 behave identically to 5) |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |

**Returns:**
- DOUBLE: Similarity score [0.0, 1.0]

**Complexity:** O(h·m) where h = iterations, m = edges

**Example:**
```sql
-- More sophisticated similarity considering BOM topology
SELECT
  material_1,
  material_2,
  wl_kernel_similarity(material_1, material_2, iterations := 5) AS topology_similarity
FROM cross_product_of_materials;
```

### 3. Predecessor Inference (Table Macro)

Identifies predecessor-successor relationships through BOM similarity combined with anti-correlation temporal analysis.

**Signature:**
```sql
infer_predecessors(
    material_id VARCHAR,           -- positional (not query_material_id :=)
    lookback_months := 24,
    min_similarity := 0.3,
    min_confidence := 0.5,
    lag_weeks := 8,
    min_overlapping_weeks := 8,
    bom_table := 'bom_items',
    movements_table := 'goods_movements'
) → TABLE (
    predecessor_id VARCHAR,
    confidence DOUBLE,
    correlation DOUBLE,
    similarity DOUBLE,
    temporal_score DOUBLE,
    predecessor_first_usage DATE,
    predecessor_last_usage DATE,
    successor_start DATE,
    overlapping_weeks BIGINT
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `material_id` | VARCHAR | Required | Material to find predecessors for (positional) |
| `lookback_months` | INTEGER | 24 | Historical window in months |
| `min_similarity` | DOUBLE | 0.3 | Minimum Jaccard similarity threshold |
| `min_confidence` | DOUBLE | 0.5 | Minimum confidence threshold |
| `lag_weeks` | INTEGER | 8 | Expected temporal lag between predecessor decline and successor rise |
| `min_overlapping_weeks` | INTEGER | 8 | Minimum aligned weekly buckets required |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |
| `movements_table` | VARCHAR | 'goods_movements' | Source movements table |

**Returns:**
- `predecessor_id` - Identified predecessor material
- `confidence` - Combined confidence score [0.0, 1.0]
- `correlation` - Temporal correlation (negative = anti-correlation)
- `similarity` - Jaccard similarity score
- `temporal_score` - Temporal succession score
- `predecessor_first_usage` - First usage date of predecessor
- `predecessor_last_usage` - Last usage date of predecessor
- `successor_start` - Start date of successor (query material)
- `overlapping_weeks` - Weeks of overlapping consumption

**Example:**
> [!IMPORTANT]
> The leading material-id argument is **positional-only** — pass it as the first argument, not
> as `query_material_id := ...`. The same applies to `k` on the search functions below. Only the
> trailing tuning arguments (`lookback_months`, `min_similarity`, ...) accept `name := value`.

```sql
SELECT
  predecessor_id,
  confidence,
  correlation,
  similarity
FROM infer_predecessors(
  'AL-7076',                    -- positional material id (required)
  lookback_months := 24,
  min_similarity := 0.4,
  min_confidence := 0.6,
  min_overlapping_weeks := 8    -- min aligned weekly buckets (default 8; lower for sparse data)
)
ORDER BY confidence DESC;
```

---

## Similarity Search

### 1. Find Similar Materials - Jaccard (Table Macro)

Finds k most similar materials using Jaccard component overlap.

**Signature:**
```sql
find_similar_materials_jaccard(
    material_id VARCHAR,
    k BIGINT,
    bom_table := 'bom_items',
    min_similarity := 0.0,
    mode := 'jaccard'
) → TABLE (
    material_id VARCHAR,
    similarity DOUBLE,
    shared_components BIGINT,
    total_components BIGINT
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | Material to find similar to |
| `k` | INTEGER | Required | Number of results |
| `min_similarity` | DOUBLE | 0.0 | Minimum similarity threshold |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |
| `mode` | VARCHAR | 'jaccard' | `'jaccard'` (symmetric) or `'containment'`: the share of the query's components a material contains. Ties are ordered by Jaccard (not returned), so `similarity` stays in [0, 1]. Like the Jaccard mode it returns materials without any shared component (similarity 0); filter with `min_similarity`. Counts components, not quantities; for quantities use [`find_covering_entities`](#find_covering_entities--find_covering_entities_for) |

**Returns:**
- `material_id` - ID of similar material
- `similarity` - Jaccard similarity score [0.0, 1.0] (containment with `mode := 'containment'`)
- `shared_components` - Count of overlapping components
- `total_components` - Count of unique components in union

**Complexity:** O(n) brute force over all materials

**Example:**
```sql
SELECT
  material_id,
  similarity,
  shared_components
FROM find_similar_materials_jaccard('PUMP-A', 10)
WHERE similarity >= 0.5;
```

### 2. Find Similar Materials - WL Kernel (Table Macro)

Finds k most similar materials using Weisfeiler-Lehman graph kernel similarity.

**Signature:**
```sql
find_similar_materials_wl_kernel(
    query_material_id VARCHAR,
    k INTEGER,
    iterations := 3,
    min_similarity := 0.0,
    bom_table := 'bom_items'
) → TABLE (
    material_id VARCHAR,
    similarity DOUBLE
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | Material to find similar to |
| `k` | INTEGER | Required | Number of results |
| `iterations` | INTEGER | 3 | Number of WL refinement iterations |
| `min_similarity` | DOUBLE | 0.0 | Minimum similarity threshold |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |

**Returns:**
- `material_id` - ID of similar material
- `similarity` - WL kernel similarity score [0.0, 1.0]

**Complexity:** O(n · h · m) where h = iterations, m = edges per material

**Example:**
```sql
SELECT
  material_id,
  similarity
FROM find_similar_materials_wl_kernel('PUMP-A', 10, iterations := 5)
WHERE similarity >= 0.5;
```

### 3. Cold Start Analogs (Table Macro)

Finds analog materials with consumption history for cold-start forecasting.

**Signature:**
```sql
cold_start_analogs(
    query_material_id VARCHAR,
    k INTEGER,
    min_history_months := 0,
    min_similarity := 0.0,
    bom_table := 'bom_items',
    movements_table := 'goods_movements'
) → TABLE (
    material_id VARCHAR,
    similarity DOUBLE,
    shared_components BIGINT,
    total_components BIGINT,
    history_months BIGINT,
    first_usage DATE,
    last_usage DATE
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | New product needing analogs |
| `k` | INTEGER | Required | Number of analog materials to return |
| `min_history_months` | INTEGER | 0 | Minimum consumption history required |
| `min_similarity` | DOUBLE | 0.0 | Minimum Jaccard similarity threshold |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |
| `movements_table` | VARCHAR | 'goods_movements' | Movements history table |

**Returns:**
- `material_id` - Similar material with history
- `similarity` - Jaccard similarity to query material
- `shared_components` - Count of shared components
- `total_components` - Count of unique components in union
- `history_months` - Months of consumption history
- `first_usage` - First usage date
- `last_usage` - Last usage date

**Example:**
```sql
SELECT
  material_id,
  similarity,
  history_months
FROM cold_start_analogs(
  'X-750-NEW',                  -- positional material id (required)
  5,                            -- positional k (required, not k := 5)
  min_history_months := 6,
  min_similarity := 0.3
)
ORDER BY similarity DESC;
```

---

## BOM Traversal & Navigation

### 1. BOM Explosion - 1 Level (Table Macro)

Returns direct children of a parent material (single-level explosion).

**Signature:**
```sql
bom_explosion_1level(
    parent_material_id VARCHAR,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    parent_material_id VARCHAR,
    child_material_id VARCHAR,
    quantity_per FLOAT
)
```

**Complexity:** O(c) where c = number of direct children

**Example:**
```sql
SELECT * FROM bom_explosion_1level('ASSY-001');
```

### 2. BOM Explosion - Multi-Level (Table Macro)

Recursive BOM explosion with depth limit.

**Signature:**
```sql
bom_explosion_multilevel(
    parent_material_id VARCHAR,
    max_depth := 10,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    parent_material_id VARCHAR,
    child_material_id VARCHAR,
    quantity_per FLOAT
)
```
> [!IMPORTANT]
> `max_depth` is clamped to a **hard ceiling of 64** regardless of the value passed in (this bounds
> traversal cost on cyclic BOMs). A BOM genuinely deeper than 64 levels is silently truncated at 64
> with no error — this is extremely rare in practice, but if you need to detect it, compare the
> returned row count against an independent depth check.

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `parent_material_id` | VARCHAR | Required | Root material ID |
| `max_depth` | INTEGER | 10 | Maximum recursion depth |
| `header_table` | VARCHAR | 'bom_header' | Header table name |
| `component_table` | VARCHAR | 'bom_component' | Component table name |

**Complexity:** O(depth × edges) — the traversal is set-based (each edge is expanded at most once
per depth level, even on cyclic or multi-path/diamond BOMs), not exponential in depth.

**Example:**
```sql
SELECT * FROM bom_explosion_multilevel('PRODUCT-001', max_depth := 5);
```

### 3. BOM Where-Used (Table Macro)

Reverse BOM: finds all parents that use a given component.

**Signature:**
```sql
bom_where_used(
    child_material_id VARCHAR,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    parent_material_id VARCHAR,
    child_material_id VARCHAR,
    quantity_per FLOAT
)
```

**Example:**
```sql
SELECT parent_material_id
FROM bom_where_used('BEARING-X')
ORDER BY parent_material_id;
```

### 4. Common Components (Table Macro)

Finds components shared between two materials.

**Signature:**
```sql
bom_common_components(
    material_1 VARCHAR,
    material_2 VARCHAR,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    child_material_id_1 VARCHAR,
    child_material_id_2 VARCHAR
)
```

**Example:**
```sql
SELECT COUNT(*) AS common_count
FROM bom_common_components('PUMP-A', 'PUMP-B');
```

---

## Textual Embeddings

### 1. Compute Textual Embeddings (Table Macro)

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

**Providers:**
- `gemma-local` - Local OpenVINO model (384-D)
- `openai` - OpenAI API (requires config: `{"api_key": "..."}`)
- `anthropic` - Anthropic API (requires config)

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

### 2. Embed Text Lambda

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

### 1. Compute Transactional Embeddings (Table Macro)

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

### 2. Recompute Embedding Statistics (Table Macro)

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

### 1. Compute Fused Embeddings (Table Macro)

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

## ERP Transformations

### 1. SAP Transformations

Convert SAP-format BOM to universal schema.

**sap_to_bom_items** - Convert MAST/STKO/STPO to universal BOM schema:
```sql
sap_to_bom_items(
    mast_table VARCHAR,
    stko_table VARCHAR,
    stpo_table VARCHAR,
    bom_alternative := '01',
    reference_date := '9999-12-31',
    bom_usage := NULL
) → TABLE (
    bom_id VARCHAR,
    parent_id VARCHAR,
    child_id VARCHAR,
    qty DOUBLE,
    unit VARCHAR,
    valid_from DATE,
    valid_to DATE,
    bom_version VARCHAR
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mast_table` | VARCHAR | Required | MAST table (material-BOM link) |
| `stko_table` | VARCHAR | Required | STKO table (BOM header) |
| `stpo_table` | VARCHAR | Required | STPO table (BOM components) |
| `bom_alternative` | VARCHAR | '01' | BOM alternative to extract |
| `reference_date` | VARCHAR | '9999-12-31' | Validity reference date |
| `bom_usage` | VARCHAR | NULL | Reserved — accepted but **not currently applied** as a filter |

> [!NOTE]
> SAP dates (`STKO.datuv`, `reference_date`) may be passed as `YYYYMMDD` strings or ISO `YYYY-MM-DD`;
> both are parsed. `valid_from` is derived from the selected header's `datuv`. `bom_usage` is a
> placeholder and does not yet filter results.
>
> **Limitations:** `reference_date` selects the applicable STKO **header** slice (and thus
> `valid_from`), but the component set is joined by BOM number only, so it does not change the
> component composition. Likewise, `bom_alternative` filters the header; if one BOM number (`stlnr`)
> carries several alternatives, all of their STPO components are returned. These require SAP STAS
> resolution, which this macro does not model — provide STPO already scoped to one alternative.
> `qty` passes through the source `MENGE` column's type (SAP `MENGE` is DECIMAL); cast if you need DOUBLE.

**sap_to_materials** - Extract materials from MARA. Requires MARA columns `matnr`, `mtart`, `matkl`,
`ersda`, **and `lvorm`** (deletion flag — rows flagged deleted are excluded). Its `description`
column is always empty; use **`sap_to_materials_with_desc`** (below) to populate descriptions from MAKT:
```sql
sap_to_materials(
    mara_table VARCHAR
) → TABLE (material_id, material_type, material_group, description, created_date)
```

**sap_to_materials_with_desc** - Materials with descriptions (MARA + MAKT):
```sql
sap_to_materials_with_desc(
    mara_table VARCHAR,
    makt_table VARCHAR,
    language := 'E'
) → TABLE (material_id, material_type, material_group, description, created_date)
```

**extract_material_descriptions** - Extract descriptions from MAKT. Requires MAKT columns `matnr`,
`spras`, `maktx`, **and `maktg`**; multiple rows per `(matnr, spras)` collapse to one:
```sql
extract_material_descriptions(
    makt_table := 'sap_makt',
    language := 'EN'
) → TABLE (material_id VARCHAR, full_text VARCHAR)
```

**Example:**
```sql
-- Convert SAP BOM to universal format
SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', bom_alternative := '01');

-- Extract materials with descriptions
SELECT * FROM sap_to_materials_with_desc('MARA', 'MAKT', language := 'E');
```

### 2. Dynamics 365 Transformations

Convert Microsoft Dynamics 365 BOM format.

These are table macros that RETURN **universal-schema** rows (materials / bom_header / bom_component
shapes) — they do not write an output table, and the component macro does **not** return flat
`bom_items`. Expected input column names are exactly as listed (they mirror D365 field names).

```sql
-- InventTable → universal materials. Reads columns: ItemId, ItemName, ItemType (0/1/2).
dynamics365_to_materials(invent_table := 'InventTable')
    → TABLE (material_id, material_number, description, material_type, material_group,
             procurement_type, base_uom, weight, cost_per_unit, source_system, is_active, created_at)

-- BOMTable + BOMVersion → universal bom_header. Reads BOMTable.(BOMId, ItemId, Status) and
-- BOMVersion.(BOMId, VersionId, ActivationDate, ApprovedStatus); keeps rows with Status = 0.
dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion')
    → TABLE (bom_id, source_system, source_bom_id, parent_material_id, bom_type, alternative_number,
             revision, base_quantity, base_uom, valid_from, valid_to, plant_id, is_approved, created_at)

-- BOM lines → universal bom_component (singular). Reads columns: BOMId, LineNum, ItemId,
-- Quantity, ScrapPercent.
dynamics365_to_bom_component(bom_lines := 'BOM')
    → TABLE (component_id, bom_id, line_number, child_material_id, quantity_per, quantity_uom,
             is_fixed_quantity, scrap_percent, effective_from, effective_to, component_type,
             supply_type, operation_sequence, is_alternative, alternative_group, created_at)
```

**Example — D365 to a `bom_items` table usable by the similarity functions:**
```sql
CREATE TABLE materials     AS SELECT * FROM dynamics365_to_materials(invent_table := 'InventTable');
CREATE TABLE bom_header    AS SELECT * FROM dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion');
CREATE TABLE bom_component AS SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM');
-- Flatten header + component into the parent_id/child_id/quantity shape the algorithms expect:
CREATE TABLE bom_items     AS SELECT * FROM bom_to_items('bom_header', 'bom_component');
```

---

## Statistics & Normalization

### 1. Check Statistics Freshness (Table Macro)

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

### 2. Recompute Embedding Statistics (Table Macro)

Computes z-score normalization statistics for all 98 transactional embedding features.
**Requires the `anofox-forecast` extension** (it derives time-series features); without it the call
fails with a catalog error naming `anofox_fcst_ts_features`.

**Signature:**
```sql
recompute_embedding_statistics(
    time_window_days := 365,
    min_observations := 3
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

**Example:**
```sql
-- Recompute statistics from last 180 days of data
SELECT * FROM recompute_embedding_statistics(time_window_days := 180);
```

### 3. Compute Domain-Specific Statistics (Table Macro)

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

### 1. Compute Jaccard Embeddings (Table Macro)

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

### 2. Extract Time-Series Features (Table Macro)

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

## Helper Macros

### 1. Aggregate Material Components (Table Macro)

Centralized BOM component aggregation helper.

**Signature:**
```sql
aggregate_material_components(
    bom_table := 'bom_items',
    material_filter := NULL
) → TABLE (
    material_id VARCHAR,
    components LIST
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |
| `material_filter` | LIST | NULL | Optional list of material IDs to filter |

**Example:**
```sql
-- Get component lists for all materials
SELECT * FROM aggregate_material_components('bom_items');

-- Filter to specific materials
SELECT * FROM aggregate_material_components('bom_items', ['MAT-001', 'MAT-002']);
```

### 2. Filter Recent Movements (Table Macro)

Centralized time-window filtering for goods movements.

**Signature:**
```sql
filter_recent_movements(
    movements_table := 'goods_movements',
    time_window_days := 365,
    min_quantity := 0
) → TABLE (
    material_id VARCHAR,
    movement_date DATE,
    quantity DOUBLE
)
```

> [!NOTE]
> The window is measured from the **most recent `movement_date` in the data**, not from wall-clock
> "today" (this works without the ICU extension and suits historical ERP extracts). Rows with NULL
> date/quantity and `quantity <= min_quantity` are filtered out. `movement_type` is **not** returned.

**Example:**
```sql
-- Get last 180 days of movements with quantity > 0
SELECT * FROM filter_recent_movements('goods_movements', 180, 0);
```

---

## Incremental Updates

> [!NOTE]
> There is **no automatic dirty-tracking / incremental-refresh subsystem**. DuckDB does not support
> `CREATE TRIGGER`, and table macros cannot contain DML, so the previously-documented
> `refresh_transactional_embeddings`, `clear_dirty_materials`, and `refresh_dirty_embeddings`
> functions could never be provided and have been removed.
>
> To refresh embeddings after BOM or movement changes, recompute them for the affected materials
> explicitly, e.g.:
> ```sql
> INSERT OR REPLACE INTO material_embeddings (material_id, transactional_embedding, updated_at)
> SELECT material_id, transactional_embedding, now()
> FROM compute_transactional_embeddings(movements_table := 'goods_movements')
> WHERE material_id IN ('CHANGED-1', 'CHANGED-2');
> ```

---

## Dependency Checks

### 1. Check anofox-forecast Available (Scalar Macro)

Checks if anofox-forecast extension is loaded.

**Signature:**
```sql
check_anofox_forecast_available() → BOOLEAN
```

**Example:**
```sql
SELECT check_anofox_forecast_available();
-- Returns: true/false
```

### 2. Check DuckPGQ Available (Scalar Macro)

Checks if DuckPGQ extension is loaded.

**Signature:**
```sql
check_duckpgq_available() → BOOLEAN
```

---

## Vector Search & Indexing

### 1. HNSW Indexes

HNSW indexes over the `material_embeddings` vector columns are **created automatically when the
extension loads** (see [HNSW Indexes](#hnsw-indexes) below for the index list). There is no
SQL-callable `CreateHNSWIndexes(...)` function — that entry point does not exist. Build vector
indexes directly with the `vss` extension's DDL if you need custom parameters, e.g.:

```sql
INSTALL vss; LOAD vss;
SET hnsw_enable_experimental_persistence = true;
CREATE INDEX my_idx ON material_embeddings USING HNSW (combined_embedding) WITH (metric = 'cosine');
```

---

## Property Graph Integration

### 1. Create BOM Property Graph (Table Macro)

Creates DuckPGQ property graph representation.

**Signature:**
```sql
create_bom_property_graph(
    source_system := 'TEST'
) → TABLE (
    graph_name VARCHAR,
    source VARCHAR,
    num_nodes INTEGER,
    num_edges INTEGER
)
```

**Note:** This macro is pure SQL over `bom_header` / `bom_component` and does **not** require the
DuckPGQ extension. (Actual property-graph traversal with DuckPGQ syntax would require it, but this
helper only returns the graph name and node/edge counts.)

**Example:**
```sql
SELECT * FROM create_bom_property_graph(source_system := 'SAP');
```

### 2. DFS Neighborhood Helper (Table Macro)

Reusable depth-first traversal for BOM graphs.

**Signature:**
```sql
bom_dfs_neighborhood(
    root_material_id VARCHAR,
    max_depth := 3,
    bom_table := 'bom_items'
) → TABLE (
    component VARCHAR
)
```
> [!IMPORTANT]
> `max_depth` is clamped to a **hard ceiling of 64** (bounds traversal cost on cyclic BOMs); a value
> above that is silently treated as 64. `max_depth := N` returns components reachable within exactly
> N hops of `root_material_id`.

**Example:**
```sql
SELECT COUNT(*) AS total_components
FROM bom_dfs_neighborhood('PUMP-A', max_depth := 5);
```

---

## Database Infrastructure

The extension creates several database objects on load for embedding storage and incremental updates.

> [!NOTE]
> **Read-only databases:** if the primary database is opened read-only, the SQL-macro API (BOM
> traversal, SAP/D365 transforms, textual/fused embeddings, statistics, etc. — everything registered
> via `CREATE MACRO`) is **not** available, since macro registration needs a writable catalog. The
> core C++ table functions (`find_similar_materials_jaccard`, `find_similar_materials_wl_kernel`,
> `cold_start_analogs`, `infer_predecessors`, `compute_jaccard_embeddings`, `find_covering_entities`,
> `find_covering_entities_for`, `quantity_units`, `tree_anchors`, `tree_class_multiset`, `validate_trees`,
> `tree_coverage`, `tree_coverage_trace`, `coverage_recall`) and all scalar functions
> still work. Open the database read-write at least once (even briefly) if you need the macro API.

### Tables

**material_embeddings** - Primary embedding storage:
```sql
CREATE TABLE material_embeddings (
    material_id VARCHAR PRIMARY KEY,
    jaccard_embedding FLOAT[128],      -- Min-hash Jaccard embedding
    structural_embedding FLOAT[256],   -- WL kernel structural embedding
    textual_embedding FLOAT[384],      -- Text description embedding
    transactional_embedding FLOAT[128],-- Time-series feature embedding
    combined_embedding FLOAT[768],     -- Fused multi-modal embedding
    updated_at TIMESTAMP,
    num_components INTEGER
);
```

**material_embeddings_dirty** - Dirty tracking for incremental updates:
```sql
CREATE TABLE material_embeddings_dirty (
    material_id VARCHAR PRIMARY KEY,
    reason VARCHAR,          -- 'insert', 'update', or 'delete'
    marked_at TIMESTAMP
);
```

**transactional_embedding_statistics** - Z-score normalization statistics:
```sql
CREATE TABLE transactional_embedding_statistics (
    feature_name VARCHAR PRIMARY KEY,
    feature_index INTEGER,
    feature_category VARCHAR,
    mean_value DOUBLE,
    std_value DOUBLE,
    min_value DOUBLE,
    max_value DOUBLE,
    num_samples INTEGER,
    updated_at TIMESTAMP,
    version INTEGER
);
```

**transactional_feature_mapping** - Feature metadata:
```sql
CREATE TABLE transactional_feature_mapping (
    feature_index INTEGER PRIMARY KEY,
    feature_name VARCHAR,
    category VARCHAR,
    description VARCHAR,
    is_advanced BOOLEAN
);
```

### HNSW Indexes

Created automatically for vector similarity search:

| Index Name | Column | Metric | Dimension |
|------------|--------|--------|-----------|
| `hnsw_structural_idx` | structural_embedding | L2-squared | 256 |
| `hnsw_textual_idx` | textual_embedding | cosine | 384 |
| `hnsw_combined_idx` | combined_embedding | cosine | 768 |

> [!NOTE]
> There is **no vector index over `jaccard_embedding`**: those are raw MinHash values, over which an
> L2/cosine distance does not approximate Jaccard. Use `find_similar_materials_jaccard()` (exact) for
> Jaccard search.

### Triggers

> [!NOTE]
> DuckDB has **no `CREATE TRIGGER` support**, so there are no automatic dirty-tracking triggers.
> The `material_embeddings_dirty` table exists but is not populated automatically; refresh
> embeddings explicitly after changes (see [Incremental Updates](#incremental-updates)).

---

## Coverage Matching

Asymmetric, explainable matching of a **request** (a specification, a BOM to source, a
profile) against **offers** (products, past solutions, modules). It answers "does B
satisfy what A asks for?", where the similarity functions above answer "how alike are A
and B?". Design: [requirements-matching/HLD.md](requirements-matching/HLD.md) (§11 lists
the amendments decided during implementation). Test dataset:
[test/data/coverage](../test/data/coverage/README.md).

On DuckDB 1.5.6, named arguments of **scalar** functions bind by position (the table functions
bind by name on both lines). For `tversky_similarity` and `weighted_tversky`, pass both `alpha`
and `beta`, `alpha` first: on 1.5.6 `(…, beta := 0.0, alpha := 1.0)` silently swaps them, and
passing only one of them does not bind.

### Directional and weighted set measures (scalars)

| Function | Formula | Empty / NULL |
|---|---|---|
| `tversky_similarity(query LIST, candidate LIST[, alpha DOUBLE, beta DOUBLE])` | `\|q∩c\| / (\|q∩c\| + α\|q\c\| + β\|c\q\|)`; default α = β = 1 (Jaccard) | both empty → 0.0; other zero denominators → NULL |
| `containment(query LIST, candidate LIST)` | `\|q∩c\| / \|q\|` | empty query → NULL |
| `weighted_jaccard(query MAP, candidate MAP)` | `Σ min(q_k, c_k) / Σ max(q_k, c_k)` | both empty → 0.0 |
| `weighted_containment(query MAP, candidate MAP)` | `Σ min(q_k, c_k) / Σ q_k` | empty or all-zero query → NULL |
| `weighted_tversky(query MAP, candidate MAP[, alpha, beta])` | `Σmin / (Σmin + α Σ(q−c)⁺ + β Σ(c−q)⁺)`; default α = β = 1 | as `weighted_jaccard` |

- Lists are sets: duplicates collapse and NULL elements are ignored, as in `jaccard_similarity`.
- Map values are weights (counts, quantities, levels). They must be numeric, finite and
  non-negative; a NULL weight counts as 1, and a zero weight means "absent". Keys may be of
  any type and are compared by their text form, so `1` and `'1'` match (in the scalars and in
  `find_covering_entities_for` against the feature column cast to VARCHAR).
- NULL arguments give NULL. `alpha` and `beta` must be non-negative.
- The parameters are called `query` and `candidate` because the measures are directional
  (`jaccard_similarity` keeps `list_a` / `list_b`).

```sql
SELECT containment(['a','b'], ['a','b','c','d']);                          -- 1.0
SELECT containment(['a','b','c','d'], ['a','b']);                          -- 0.5
SELECT weighted_containment(MAP {'massage': 4}, MAP {'massage': 2});      -- 0.5: counts matter
SELECT weighted_tversky(MAP {'a': 3}, MAP {'a': 2, 'b': 3}, 1.0, 0.0);    -- 0.6667
```

### find_covering_entities / find_covering_entities_for

Stage-1 recall: rank the entities of a generic `(entity, feature, weight)` table by how well
they cover a query. `find_covering_entities` takes an entity of the table as the query;
`find_covering_entities_for` takes a `MAP(feature, weight)`, e.g. the class multiset of a new
specification that is not in the table yet.

**Signature:**
```sql
find_covering_entities(query_entity_id VARCHAR, k BIGINT, <named>)
find_covering_entities_for(query MAP, k BIGINT, <named>)
  → TABLE(entity_id VARCHAR, score DOUBLE, containment DOUBLE, jaccard DOUBLE,
          shared_weight DOUBLE, query_weight DOUBLE, candidate_weight DOUBLE, surplus_weight DOUBLE)
```

**Named parameters:**
| Parameter | Default | Description |
|---|---|---|
| `mode` | `'containment'` | `'containment'` (Σmin/Σq), `'jaccard'` (Σmin/Σmax) or `'tversky'`; case-insensitive |
| `alpha`, `beta` | 1.0, 0.0 | Tversky weights of the missing and the surplus weight; used in mode `'tversky'` only (ignored otherwise) |
| `weighted` | true | false: every distinct (entity, feature) weighs 1 |
| `tie_break` | 0.01 | weight of Jaccard added to the score in modes `containment` and `tversky` (BR-03); ignored in mode `'jaccard'` |
| `min_score` | 0.0 | threshold on the measure itself (before the tie-break) |
| `feature_table` | `'feature_multiset'` | table or view, optionally schema-qualified |
| `entity_col`, `feature_col`, `weight_col` | `'entity_id'`, `'feature_id'`, `'weight'` | column mapping, so `bom_items(parent_id, child_id, quantity)` works as is; `weight_col := NULL` means unit weights |

**Returns:** `score` is the ranking key: the measure plus `tie_break × jaccard` (jaccard in mode
`'jaccard'`). It can exceed 1. `containment` and `jaccard` are the raw measures. `shared_weight`
= Σmin, `query_weight` = Σq, `candidate_weight` = Σc, `surplus_weight` = Σc − Σmin (what the
candidate brings on top). Rows are ordered by `score DESC, entity_id`. Only candidates that
share at least one feature with the query are returned, and the query entity itself is
excluded.

**Semantics:** weights of repeated (entity, feature) rows add up; a NULL weight counts as 1;
rows with a NULL entity or feature are ignored. A negative or non-finite weight anywhere in
the table (also on rows that are ignored) raises an error that names the row. Features
compare by their text form. `find_covering_entities`: an unknown id returns no rows, a NULL
id is an error. `find_covering_entities_for`: an empty, NULL, all-zero or unknown query
returns no rows; zero-weight entries of the query are dropped.

**Known limitation (kept by decision, HLD §4.1):** the tie-break is additive, so a nearly
complete partial cover can outrank a full but much larger cover (containment 0.995 with
jaccard 0.995 scores 1.00495; a full cover with jaccard 0.2 scores 1.002). Filter with
`min_score := 1.0` when only full covers count.

**Read-only databases:** the SQL is inlined, so both functions work where macros cannot be
created. The query argument must be constant: a literal, a parameter, or
`getvariable('name')`.

**Performance:** an inverted join on the feature. 100 000 entities with 15 features each
(1.5 M rows) take about 0.15–0.2 s per query on a laptop.

**Examples:**
```sql
-- Which past programmes contain everything a new request asks for, leanest first?
SELECT entity_id, round(score, 3), round(jaccard, 3)
FROM find_covering_entities_for(
       MAP {'seat_heating': 2, 'massage': 4, 'headrest_powered': 1, 'memory_module': 1}, 3,
       feature_table := 'prog_classes');

-- An existing BOM table, quantities included
SELECT * FROM find_covering_entities('F-100', 5, feature_table := 'frame_bom',
       entity_col := 'frame_no', feature_col := 'part_no', weight_col := 'quantity_per');

-- A request tree turned into a class multiset (repeated classes add up)
SET VARIABLE request = (
    SELECT map_from_entries(list((class_id, n)))
    FROM (SELECT class_id, sum(count)::DOUBLE AS n FROM spec_nodes
          WHERE node_kind = 'item' GROUP BY class_id));
SELECT * FROM find_covering_entities_for(getvariable('request'), 20);
```

**Many requests at once.** A table function takes one constant query, and `LATERAL` with a
column argument is not supported. To rank the archive for many requests, build one map per
request and per candidate and score all pairs with the scalars:

```sql
CREATE TABLE requests AS
SELECT request_id, map_from_entries(list((class_id, n))) AS classes
FROM (SELECT request_id, class_id, sum(count)::DOUBLE AS n FROM request_items GROUP BY ALL)
GROUP BY request_id;
CREATE TABLE candidates AS
SELECT entity_id, map_from_entries(list((feature_id, weight))) AS classes
FROM feature_multiset GROUP BY entity_id;

SELECT r.request_id, c.entity_id,
       weighted_containment(r.classes, c.classes) + 0.01 * weighted_jaccard(r.classes, c.classes) AS score
FROM requests r, candidates c
QUALIFY row_number() OVER (PARTITION BY r.request_id ORDER BY score DESC, c.entity_id) <= 20;
```

The scores equal those of `find_covering_entities_for`. The scalars process about 10⁵ pairs in
0.15 s.

**Choosing the measure (risk R1 in the BRD):** containment is the right question once the
candidates' labels are reliable (reviewed vocabulary). On unreviewed labels a mislabelled
item can make a candidate look complete, and weighted Jaccard recalls the best candidate more
often (on the benchmark, recall@20 is 0.55 for containment vs 0.64 for Jaccard; with reviewed
labels it is 0.925 vs 0.85).


### Quantities: units, parsing and direction-aware comparison

Numbers need rules, not embeddings (BRD §1): the unit is a hard gate, the value is checked
with a direction, and the distance to compliance is reported.

**`quantity_units()`**: the built-in, read-only units seed. It returns `symbol, alias
VARCHAR[], dimension, factor, offset, base_unit` (base = value × factor + offset). It covers SI
units with prefixes, time (s … year, weeks/Wochen, days/Tage), temperature (K, °C, °F), sound
level (dB(A)), percent/ppm, counts (cycles/Zyklen, pcs), data and data rates, and ISO-4217
currencies. Currencies have `factor` NULL and are never converted.

**`parse_quantity(text [, lang [, unit_table]])`** → `STRUCT(lo, hi, unit, dimension, bound, raw,
source_unit, category)`

| Text | lo, hi (base unit) | bound |
|---|---|---|
| `max. 38 dB(A)` | 38, 38 dB(A) | at_most |
| `mindestens 24 cm` | 0.24, 0.24 m | at_least |
| `6–18 V`, `40 bis 60 W`, `zwischen 15 und 25 mm/s` | range | within |
| `220 N ± 2 %`, `Nennwert 220 N` | 220, 220 N | equals |
| `38 dB(A)` | 38, 38 | NULL (unqualified) |
| `37 °C` | 310.15 K | NULL |
| `65.000 Zyklen` with lang `'de'` | 65000 (count) | NULL |
| `IP54 or better` | NULL; `category` = IP54 | at_least |
| `12 furlongs` | 12; `dimension` NULL, `unit` = furlongs | NULL |

- `lang` is `'en'`, `'de'` or a list; default `['en', 'de']`. It only decides ambiguous
  separators (`2.500` is 2500 in German and 2.5 in English); bound words of both languages are
  always recognised. A comma or dot with one or two digits after it is always a decimal
  separator.
- A bare number is a count (`unit` '1'). An unknown unit is never guessed: the number stays as
  written and `dimension` is NULL, so every comparison with it is a gate.
- A category is a single upper-case code with digits and no number in front (`IP54`, `F1`,
  `M8`); `1 F` is one farad.
- `unit_table` is a LIST of `STRUCT(symbol, alias, dimension, factor, offset)`, e.g.
  `(SELECT list(u) FROM my_units u)`. It extends the seed, and a symbol that is already in the
  seed is overridden. The seed itself can be passed back in. Pass `lang` before `unit_table`
  (on 1.5.6 scalar arguments bind by position).

**`normalize_quantity(value, unit [, unit_table])`** → `STRUCT(value, unit, dimension)`: converts
to the base unit; an unknown unit returns the input unchanged with `dimension` NULL.

**`value_satisfies(requirement, provided [, scale [, tol, soft_band [, default_bound]]])`** →
`STRUCT(score, distance, rule)`

Both arguments are structs with `lo`, `hi` and `dimension` (and optionally `unit`); the
requirement also has `bound`. `parse_quantity` output plugs straight in. A value in a
non-base unit of its dimension (`{'lo': 250, 'unit': 'mm', 'dimension': 'length'}`) is
normalised first.

| Rule | Satisfied when | `distance` |
|---|---|---|
| `at_least v` | the provided high end `p_hi ≥ v` | `(v − p_hi)/\|v\|` |
| `at_most v` | the provided low end `p_lo ≤ v` | `(p_lo − v)/\|v\|` |
| `within [lo, hi]` | the provided range covers the required range (log: share of the decades) | uncovered share |
| `equals v` | `\|v − p\| ≤ tol` relative, on every scale; `p` is the farther end of a provided range | `\|v − p\|/\|v\|` |
| `point` (no bound) | the provided value lies within `[v·10⁻⁰·⁰¹, v·10⁰·⁰¹]` (log) or ±1 % (linear) | uncovered share |
| `gate` | different or unknown dimensions, or different currencies; score 0 | NULL |
| `missing` | no provided value; score 0 | NULL |

- `score` is 1 within `tol` (default 0.02: decades on a log scale, i.e. 4.5 % relative;
  relative on a linear scale) and falls linearly to 0 across `soft_band` (default 0.05).
  With `soft_band` 0 the limit is strict.
- `scale` defaults per dimension: linear for time, currency, percent and sound level, log
  otherwise; a non-positive value falls back to linear.
- A limit of 0 uses an absolute distance and a strict score.
- **Temperature** (stored in K) uses an absolute tolerance of 1 K and a soft band of 2.5 K;
  its `distance` is in K. `tol` and `soft_band` do not apply.
- `default_bound` gives an unqualified requirement a direction, typically the bound of its
  catalogue attribute template (HLD §11.1). A stated bound always wins.
- Categorical values (`category` set) are rejected; use `compat_score`.
- `distance` is relative for the limit rules, a share for within and point, and kelvin for
  temperature. Write "misses by x %" only for the limit rules.

**`compat_score(dimension, required, provided [, rules_table := 'compatibility_rules'])`** →
DOUBLE: looks up a curated rules table `(dimension, required, provided, score)`. Without a
rule the result is 1 if the categories are equal, else 0; a NULL category gives NULL. Text
similarity is never used (BR-15).

```sql
-- Which seat-track applications meet a German spec in cm and dB(A)?
SELECT a.node_id
FROM (SELECT attr, parse_quantity(value_text, 'de') AS q FROM requirement) r
JOIN module_attributes a ON a.attr_id = r.attr,
LATERAL (SELECT value_satisfies(r.q, {'lo': a.lo, 'hi': a.hi, 'dimension': a.dimension}) AS v)
GROUP BY a.node_id HAVING min(v.score) = 1;

-- Unqualified values take the direction of their catalogue template
SELECT value_satisfies(parse_quantity('38 dB(A)'), {'lo': 35, 'hi': 35, 'dimension': 'sound_level'},
                       NULL, 0.02, 0.05, 'at_most');               -- score 1
```

**Performance:** about 0.3 s per 10⁵ values for `parse_quantity` and 0.4 s per 10⁵ pairs for
`value_satisfies`.

### Property trees

Requests and offers are trees (HLD §3.2): `tree_nodes(tree_id, node_id, parent_id, node_kind,
edge_type, label, class_id, count[, source, context])` and `tree_attributes(tree_id, node_id,
attr_id, name, dimension, unit, lo, hi[, bound, category])`, with values in base units. Item
nodes are compared. Containers and variant groups are transparent, so grouping levels do not
have to align (BR-04). OR edges mark alternatives, and counts are extensive: they multiply
down the tree, add across AND siblings and never add across OR alternatives.

| Function | Purpose |
|---|---|
| `tree_from_json(json [, tree_id [, lang [, unit_table]]])` | Scalar. Turns an extracted tree (JSON, [schema](requirements-matching/tree.schema.json)) into a LIST of node STRUCTs with normalised, keyed attributes. Works over many documents: `SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content)) AS n FROM read_text('specs/*.json'))`. Structured value fields win over the text; text-only values are parsed with `parse_quantity`. Errors name the node |
| `tree_anchors(nodes_table [, attrs_table], overrides_table, label_table, tree_ids)` | One row per item: class, reviewed, `effective_count`, `depth`, `anchor_path` (ancestors), `variant_path` (OR choices, the item included), `n_attributes` |
| `tree_class_multiset(nodes_table, overrides_table, label_table, tree_ids)` | `(entity_id, feature_id, weight, reviewed)` for stage 1; the elementwise maximum across OR alternatives (an optimistic bound) |
| `tree_multiset(nodes LIST)` | The scalar behind it, for custom pipelines |
| `validate_trees(nodes_table [, attrs_table], tree_ids := ...)` | `(tree_id, node_id, attr_id, issue, severity, detail)`. Errors: duplicate_node, bad_node_kind, bad_edge_type, bad_count, orphan, no_root, multiple_roots, cycle, unreachable, too_deep, attribute_on_container, orphan_attribute, bad_interval, bad_bound. Warnings: mixed_edge_types, unknown_unit, mixed_dimensions (per source and name) |

**Class resolution** (shared by both stages): `class_id` → `overrides_table` (the reviewed
vocabulary: `wording` matched exactly against `label`; a NULL `source` matches any source) →
`label_table` (classifier output `tree_id, node_id, class_id, p`: arg-max, ties to the lowest
class). A tree is *reviewed* when every item got its class from `class_id` or the vocabulary.
Trees deeper than 1000 levels are rejected (`tree_anchors` stops there).

### Coverage: tree_coverage, tree_coverage_trace, coverage_recall

```sql
tree_coverage(query_tree_id VARCHAR | VARCHAR[], <named>)
  → TABLE(query_id, candidate_id, coverage, stage1_score, rank_score, rank, n_requirements, n_gaps,
          reviewed, assignment_used)
tree_coverage_trace(query_tree_id, <named>)
  → TABLE(query_id, candidate_id, req_node_id, matched_node_id, matched_path, label_sim, attr_id, attr_name,
          req_value, prov_value, gate, name_sim, value_score, distance, rule, attr_score,
          count_needed, count_available, req_score, is_gap)
coverage_recall(query_tree_id, <named>)
  → TABLE(query_id, candidate_id, score, containment, jaccard, reviewed, mode_used, rank)
```

**Pipeline.** Stage 1 ranks the candidate trees by their class multisets, using containment
(+ `tie_break` × Jaccard) when every candidate tree is reviewed and weighted Jaccard otherwise
(BRD risk R1). It keeps the top `k` per query. Stage 2 runs the kernel over those candidates:

```
attr(x, y)  = gate · (mu·name_sim + 1 − mu) · value_satisfies(x, y).score     (categorical: compat_score)
a(q, c)     = mean over q's attributes of the best attr(x, ·)                 (1 without attributes)
L(q, c)     = Bhattacharyya(p(q), p(c))   or 0/1 with labels := 'hard'
score(q, c) = label_weight·L + (1 − label_weight)·L·a                         (default label_weight 0)
req(q)      = best score × min(1, Σ counts of compatible items with L ≥ count_gate / count(q))
coverage    = mean req(q);  rank_score = coverage + stage1_weight · stage1_score
```

`assignment := 'partition'` gives each candidate item to the requirement it agrees with most
(ties: the largest remaining count need, then the lowest node id). An item may then not stand
in for another function (BR-06), and one item serves one requirement. OR alternatives are
options: every tied requirement may choose them, and each requirement uses its best variant.
`'best_match'` lets requirements share items. `'auto'` (default) uses partition for reviewed
candidate trees.

**Named parameters**

| Parameter | Default | Description |
|---|---|---|
| `candidates` | NULL | explicit candidate tree ids; NULL → stage-1 top `k` (the query tree is never its own candidate) |
| `k` | 20 | stage-1 depth |
| `nodes_table`, `attrs_table` | `'tree_nodes'`, `'tree_attributes'` | the query trees; `attrs_table := NULL` for none |
| `candidate_nodes_table`, `candidate_attrs_table` | the query tables | the candidate trees |
| `overrides_table`, `label_table` | NULL | class resolution (see above) |
| `labels` | `'soft'` | `'soft'` uses the classifier's probabilities; `'hard'` uses one class |
| `assignment` | `'auto'` | `'partition'`, `'best_match'`, `'auto'` |
| `label_weight` | 0.0 | λ: label credit independent of the values (HLD §11.2) |
| `mu` | 0.2 | weight of the attribute-name similarity (names only discount, BR-16) |
| `partition_gate`, `count_gate` | 0.3, 0.5 | minimum label agreement for a block / for counting instances |
| `stage1_weight`, `tie_break` | 0.05, 0.01 | stage-1 score in the rank; Jaccard tie-break in stage 1 |
| `recall_mode` (`mode` in `coverage_recall`) | `'auto'` | `'containment'`, `'jaccard'`, `'auto'` |
| `default_bounds`, `templates_table` | NULL, `'catalogue_attributes'` | `'catalogue'`: an unqualified requirement takes the bound of its template with the same class and dimension (none when ambiguous) |
| `rules_table` | NULL | compatibility rules for categorical attributes (no rule: equal → 1) |
| `name_vectors`, `name_floor` | NULL, 0.85 | `(name, vec)`: attribute-name similarity is the cosine calibrated by the floor; without vectors, token overlap |

**Gaps.** `is_gap` marks a requirement that has no counterpart, too few instances, or a
value below 1. It does not depend on how soft the labels are. A required attribute that the
matched item lacks gives a row with `gate = true`. `distance` is relative for the limit rules,
a share for within and point, and in K for temperature.

**Example: the gap list of the best programme**

```sql
SET VARIABLE top = (SELECT candidate_id FROM tree_coverage('R07', k := 20,
                       candidate_nodes_table := 'programme_nodes', candidate_attrs_table := 'programme_attributes',
                       overrides_table := 'vocabulary_overrides', default_bounds := 'catalogue',
                       templates_table := 'catalogue_attributes') WHERE rank = 1);
SELECT req_node_id, attr_name, req_value, prov_value, round(distance, 3)
FROM tree_coverage_trace('R07', candidates := [getvariable('top')], ...)
WHERE is_gap;
```

**Benchmark** (80 requests at once, reviewed vocabulary, catalogue bounds, name vectors):
recall@20 0.925, top-1 0.925, mean regret 0.009, MAE over the top 20 0.036, unmeetable
flagged 1.0, precision at 0.9 0.985, auto-accept share 82.5 %. It runs in 0.65 s on a laptop.
---

## Function Coverage Matrix

| Category | Type | Count | Functions |
|----------|------|-------|-----------|
| **Similarity** | Scalar | 2 | jaccard_similarity, wl_kernel_similarity |
| **Search** | Macro | 3 | find_similar_materials_jaccard, find_similar_materials_wl_kernel, cold_start_analogs |
| **Inference** | Macro | 1 | infer_predecessors |
| **BOM Traversal** | Macro | 5 | bom_explosion_1level, bom_explosion_multilevel, bom_where_used, bom_common_components, bom_dfs_neighborhood |
| **Textual** | Macro + Scalar | 2 | compute_textual_embeddings, embed_text |
| **Transactional** | Macro | 2 | compute_transactional_embeddings, recompute_embedding_statistics |
| **Fusion** | Macro | 1 | compute_fused_embeddings |
| **Embedding Gen** | Macro | 2 | compute_jaccard_embeddings, extract_ts_features |
| **Statistics** | Macro | 1 | check_statistics_freshness |
| **Helpers** | Macro | 2 | aggregate_material_components, filter_recent_movements |
| **Statistics (advanced)** | Macro | 1 | compute_domain_specific_statistics |
| **Dependency** | Macro | 2 | check_anofox_forecast_available, check_duckpgq_available |
| **Transformations** | Macro | 7 | SAP and Dynamics 365 transformations |
| **Infrastructure** | Macro | 1 | create_bom_property_graph |
| **Coverage matching** | Scalar + Table + Macro | 21 | tversky_similarity, containment, weighted_jaccard, weighted_containment, weighted_tversky, find_covering_entities, find_covering_entities_for, quantity_units, parse_quantity, normalize_quantity, value_satisfies, compat_score, tree_from_json, tree_anchors, tree_class_multiset, tree_multiset, validate_trees, coverage_kernel, tree_coverage, tree_coverage_trace, coverage_recall |
| **TOTAL** | | **~63** | Complete API |

---

## Parameter Reference

### Common Parameters

**Table References:**
- `bom_table` - Default: 'bom_items' (columns: parent_id, child_id)
- `movements_table` - Default: 'goods_movements' (columns: material_id, movement_date, quantity)

**Dimension Parameters:**
- `k` - Number of results to return (default: 5)
- `max_depth` - Maximum recursion depth (default: 10)
- `iterations` - Algorithm iterations (default: 3)

**Temporal Parameters:**
- `time_window_days` - Historical window (default: 365)
- `lookback_months` - Lookback period (default: 24)
- `lag_weeks` - Expected temporal lag (default: 8)

**Quality Parameters:**
- `min_observations` - Minimum data points (default: 3)
- `min_similarity` - Minimum similarity threshold (default: 0.0)
- `min_overlapping_weeks` - Minimum aligned weekly buckets for predecessor inference (default: 8)

**Weighting Parameters:**
- `weights_structural` - Structural embedding weight (default: 0.5)
- `weights_textual` - Textual embedding weight (default: 0.5)
- `weights_transactional` - Transactional embedding weight (default: 0.0)

---

## Error Handling

All functions implement graceful error handling:

| Scenario | Behavior |
|----------|----------|
| Material not found | Returns empty result set (no error) |
| Insufficient data (< min_observations) | Filters out via HAVING clause |
| Circular BOM reference | Handled via depth limit |
| NULL embedding | Treated as missing, skipped in fusion |
| VSS extension unavailable | Falls back to brute-force k-NN |
| anofox-forecast unavailable | `compute_transactional_embeddings` raises a clear "install anofox-forecast" error; the SQL-macro functions (`extract_ts_features`, `recompute_embedding_statistics`, `compute_domain_specific_statistics`) fail at bind time with a catalog error naming `anofox_fcst_ts_features` — all require the extension in full (no partial features) |
| NULL k / iterations on a search function | Clean binder error (not an internal crash) |
| Weights not summing to 1 in fusion | Normalized automatically |

---

## Performance Notes

### Complexity Summary

| Function | Best Case | Worst Case | Notes |
|----------|-----------|-----------|-------|
| Jaccard similarity | O(n) | O(n) | Linear in component count |
| WL Kernel | O(h·m) | O(h·n²) | h=iterations, m=edges |
| Find similar (indexed) | O(log n) | O(log n + k) | With HNSW index |
| Find similar (brute) | O(n·m) | O(n²) | Without index |
| BOM explosion | O(c) | O(depth × edges) | c=children; set-based traversal, not exponential (safe on cycles/diamonds) |
| Textual embeddings | O(n) | O(n) | Linear in material count |

### Memory Considerations

- **HNSW index**: ~100 bytes per node (10K materials ≈ 1MB)
- **Embeddings**: 512 bytes per material (128D × 4B float)
- **Statistics table**: 100-200 KB for 104 features

---

## Function Names

All functions are registered in the `main` schema under the names shown in this document; call them
unqualified:
```sql
SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);
```
There is no `anofox_similarity.<function>` schema-qualified form and no `anofox_`-prefixed alias.

---

## See Also

- [README.md](../README.md) - Overview and quick start
- [CONCEPT.md](CONCEPT.md) - Algorithm theory and references
- [DATA-STRUCTURES.md](DATA-STRUCTURES.md) - Schema specifications
