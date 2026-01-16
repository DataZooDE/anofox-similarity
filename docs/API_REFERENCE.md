# API Reference

**Version:** 2.0.0
**DuckDB Compatibility:** ≥ v1.0.0
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
17. [Function Coverage Matrix](#function-coverage-matrix)
18. [Parameter Reference](#parameter-reference)

---

## Overview

The anofox-similarity extension provides 40+ functions across 8 functional categories for multi-modal product similarity detection. All functions are registered as SQL macros or scalar functions and follow consistent naming conventions.

### Naming Convention

- **Table Macros** (prefix `compute_`, `find_`, `infer_`) - Return result sets
- **Scalar Functions** (prefix `jaccard_`, `wl_kernel_`, `embed_`) - Return single values
- **Helper Macros** (prefix `check_`, `create_`, `bom_`) - Infrastructure utilities

### Parameterization Pattern

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
  - 0.0 = Completely disjoint (no common elements)
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
| `iterations` | INTEGER | 3 | Number of refinement iterations |
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
    query_material_id VARCHAR,
    lookback_months := 24,
    min_similarity := 0.3,
    min_confidence := 0.5,
    lag_weeks := 8,
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
    overlapping_weeks INTEGER
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | Material to find predecessors for |
| `lookback_months` | INTEGER | 24 | Historical window in months |
| `min_similarity` | DOUBLE | 0.3 | Minimum Jaccard similarity threshold |
| `min_confidence` | DOUBLE | 0.5 | Minimum confidence threshold |
| `lag_weeks` | INTEGER | 8 | Expected temporal lag between predecessor decline and successor rise |
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
```sql
SELECT
  predecessor_id,
  confidence,
  correlation,
  similarity
FROM infer_predecessors(
  query_material_id := 'AL-7076',
  lookback_months := 24,
  min_similarity := 0.4,
  min_confidence := 0.6
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
    query_material_id VARCHAR,
    k INTEGER,
    min_similarity := 0.0,
    bom_table := 'bom_items'
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

**Returns:**
- `material_id` - ID of similar material
- `similarity` - Jaccard similarity score [0.0, 1.0]
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
  'X-750-NEW',
  k := 5,
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

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `parent_material_id` | VARCHAR | Required | Root material ID |
| `max_depth` | INTEGER | 10 | Maximum recursion depth |
| `header_table` | VARCHAR | 'bom_header' | Header table name |
| `component_table` | VARCHAR | 'bom_component' | Component table name |

**Complexity:** O(2^d) worst case where d = depth

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
    min_observations := 3
) → TABLE (
    material_id VARCHAR,
    transactional_embedding FLOAT[128]
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `movements_table` | VARCHAR | 'goods_movements' | Source movements table |
| `time_window_days` | INTEGER | 365 | Historical window |
| `min_observations` | INTEGER | 3 | Minimum data points required |

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
  time_window_days := 180,
  min_observations := 5
);
```

### 2. Recompute Embedding Statistics (Table Macro)

Computes z-score normalization statistics for transactional embeddings.

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
    std_value DOUBLE
)
```

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
    weights_transactional := 0.0,
    embedding_table := 'material_embeddings'
) → TABLE (
    material_id VARCHAR,
    fused_embedding FLOAT[768],
    fusion_weights STRUCT(structural DOUBLE, textual DOUBLE, transactional DOUBLE)
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `weights_structural` | DOUBLE | 0.5 | Structural embedding weight |
| `weights_textual` | DOUBLE | 0.5 | Textual embedding weight |
| `weights_transactional` | DOUBLE | 0.0 | Transactional embedding weight |
| `embedding_table` | VARCHAR | 'material_embeddings' | Embeddings table |

**Returns:**
- `material_id` - Material identifier
- `fused_embedding` - Combined 768-D FLOAT array
- `fusion_weights` - Struct with actual weights used

**Example:**
```sql
SELECT
  material_id,
  fused_embedding
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
| `bom_usage` | VARCHAR | NULL | Filter by BOM usage (optional) |

**sap_to_materials** - Extract materials from MARA:
```sql
sap_to_materials(
    mara_table VARCHAR,
    makt_table := NULL,
    language := 'E'
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

**extract_material_descriptions** - Extract descriptions from MAKT:
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

**Signatures:**
```sql
dynamics365_to_materials(
    items_table := 'items',
    output_table := 'materials'
) → BOOLEAN

dynamics365_to_bom_components(
    bom_table := 'bom',
    output_table := 'bom_items'
) → BOOLEAN
```

---

## Statistics & Normalization

### 1. Check Statistics Freshness (Table Macro)

Validates whether transactional embedding statistics are current and complete.

**Signature:**
```sql
check_statistics_freshness() → TABLE (
    stat_count INTEGER,
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
    material_column := 'material_id',
    date_column := 'movement_date',
    quantity_column := 'quantity',
    movement_type_column := 'movement_type',
    time_window_days := 365,
    min_observations := 3
) → TABLE (feature_name, feature_index, feature_category, mean_value, std_value, ...)
```

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
    quantity DOUBLE,
    movement_type VARCHAR
)
```

**Example:**
```sql
-- Get last 180 days of movements with quantity > 0
SELECT * FROM filter_recent_movements('goods_movements', 180, 0);
```

---

## Incremental Updates

### 1. Refresh Transactional Embeddings (Table Macro)

Refreshes embeddings only for materials marked as dirty.

**Signature:**
```sql
refresh_transactional_embeddings() → TABLE (
    material_id VARCHAR,
    updated_embedding FLOAT[128]
)
```

### 2. Clear Dirty Materials (Table Macro)

Clears dirty tracking records after refresh.

**Signature:**
```sql
clear_dirty_materials(
    material_ids := NULL
) → TABLE (
    material_id VARCHAR,
    reason VARCHAR,
    marked_at TIMESTAMP
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `material_ids` | LIST | NULL | Specific IDs to clear (NULL = all) |

### 3. Refresh Dirty Embeddings (Table Macro)

Combined helper: refreshes and clears dirty materials.

**Signature:**
```sql
refresh_dirty_embeddings(
    bom_table := 'bom_items'
) → TABLE (material_id VARCHAR)
```

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

### 1. Create HNSW Indexes

Creates vector indexes for fast similarity search.

**Signature:**
```sql
CreateHNSWIndexes(
    embedding_table := 'material_embeddings',
    ef := 100,
    M := 16
) → VOID
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `embedding_table` | VARCHAR | 'material_embeddings' | Table with embeddings |
| `ef` | INTEGER | 100 | Construction effort |
| `M` | INTEGER | 16 | Maximum connections per node |

**Configuration Guide:**
- **Small datasets** (< 10K): ef=50, M=8
- **Medium datasets** (10K-100K): ef=100, M=16 ✓ Default
- **Large datasets** (> 100K): ef=200, M=32

**Example:**
```sql
-- Create indexes with defaults
CALL CreateHNSWIndexes();

-- Fine-tune for large dataset
CALL CreateHNSWIndexes(ef := 200, M := 32);
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

**Note:** Requires DuckPGQ extension loaded.

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

**Example:**
```sql
SELECT COUNT(*) AS total_components
FROM bom_dfs_neighborhood('PUMP-A', max_depth := 5);
```

---

## Database Infrastructure

The extension creates several database objects on load for embedding storage and incremental updates.

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
| `jaccard_idx` | jaccard_embedding | L2-squared | 128 |
| `hnsw_structural_idx` | structural_embedding | L2-squared | 256 |
| `hnsw_textual_idx` | textual_embedding | cosine | 384 |
| `hnsw_combined_idx` | combined_embedding | cosine | 768 |

### Triggers

Automatically mark materials as dirty when BOM changes:

- `bom_items_insert_trigger` - Marks parent_id dirty on INSERT
- `bom_items_update_trigger` - Marks parent_id dirty on UPDATE
- `bom_items_delete_trigger` - Marks parent_id dirty on DELETE

**Note:** Triggers require a `bom_items` table to exist. They are created with OPTIONAL failure mode.

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
| **Statistics** | Macro | 2 | check_statistics_freshness, compute_domain_specific_statistics |
| **Helpers** | Macro | 2 | aggregate_material_components, filter_recent_movements |
| **Incremental** | Macro | 3 | refresh_transactional_embeddings, clear_dirty_materials, refresh_dirty_embeddings |
| **Dependency** | Macro | 2 | check_anofox_forecast_available, check_duckpgq_available |
| **Transformations** | Macro | 7 | SAP and Dynamics 365 transformations |
| **Infrastructure** | Macro | 2 | create_bom_property_graph, CreateHNSWIndexes |
| **TOTAL** | | **~42** | Complete API |

---

## Parameter Reference

### Common Parameters

**Table References:**
- `bom_table` - Default: 'bom_items' (columns: parent_id, child_id, quantity)
- `movements_table` - Default: 'goods_movements' (columns: material_id, movement_date, quantity)
- `embedding_table` - Default: 'material_embeddings' (columns: material_id, embedding vectors)

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
- `use_index` - Use HNSW index (default: true)

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
| anofox-forecast unavailable | Transactional embeddings return NULL |

---

## Performance Notes

### Complexity Summary

| Function | Best Case | Worst Case | Notes |
|----------|-----------|-----------|-------|
| Jaccard similarity | O(n) | O(n) | Linear in component count |
| WL Kernel | O(h·m) | O(h·n²) | h=iterations, m=edges |
| Find similar (indexed) | O(log n) | O(log n + k) | With HNSW index |
| Find similar (brute) | O(n·m) | O(n²) | Without index |
| BOM explosion | O(c) | O(2^d) | c=children, d=depth |
| Textual embeddings | O(n) | O(n) | Linear in material count |

### Memory Considerations

- **HNSW index**: ~100 bytes per node (10K materials ≈ 1MB)
- **Embeddings**: 512 bytes per material (128D × 4B float)
- **Statistics table**: 100-200 KB for 104 features

---

## Aliases & Backward Compatibility

All functions support calling without the `anofox_` prefix:
```sql
-- These are equivalent:
SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);
SELECT * FROM anofox_similarity.find_similar_materials_jaccard('MAT-001', 10);
```

---

## See Also

- [README.md](../README.md) - Overview and quick start
- [CONCEPT.md](CONCEPT.md) - Algorithm theory and references
- [DATA-STRUCTURES.md](DATA-STRUCTURES.md) - Schema specifications
