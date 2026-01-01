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
10. [Vector Search & Indexing](#vector-search--indexing)
11. [Property Graph Integration](#property-graph-integration)
12. [Function Coverage Matrix](#function-coverage-matrix)
13. [Parameter Reference](#parameter-reference)

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

Computes component overlap between two materials using Jaccard index.

**Signature:**
```sql
jaccard_similarity(
    material_a VARCHAR,
    material_b VARCHAR,
    bom_table := 'bom_items'
) → DOUBLE
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `material_a` | VARCHAR | Required | First material ID |
| `material_b` | VARCHAR | Required | Second material ID |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |

**Returns:**
- DOUBLE: Similarity score [0.0, 1.0]
  - 0.0 = Completely disjoint (no common components)
  - 1.0 = Identical component sets

**Complexity:** O(n) where n = average number of components

**Example:**
```sql
SELECT
  jaccard_similarity('PUMP-A', 'PUMP-B') AS similarity_score;
-- Result: 0.667 (2 common out of 3 total components)
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

Identifies predecessor-successor relationships through anti-correlation temporal analysis.

**Signature:**
```sql
infer_predecessors(
    query_material_id VARCHAR,
    lookback_months := 12,
    lag_weeks := 8,
    goods_movements_table := 'goods_movements'
) → TABLE (
    predecessor_material_id VARCHAR,
    confidence_score DOUBLE,
    temporal_strength DOUBLE,
    overlap_weeks INTEGER
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | Material to find predecessors for |
| `lookback_months` | INTEGER | 12 | Historical window in months |
| `lag_weeks` | INTEGER | 8 | Expected temporal lag |
| `goods_movements_table` | VARCHAR | 'goods_movements' | Source movements table |

**Returns:**
- `predecessor_material_id` - Identified predecessor material
- `confidence_score` - Confidence [0.0, 1.0]
- `temporal_strength` - Strength of anti-correlation
- `overlap_weeks` - Weeks of overlapping usage

**Example:**
```sql
SELECT
  predecessor_material_id,
  confidence_score
FROM infer_predecessors(
  query_material_id := 'AL-7076',
  lookback_months := 24
)
WHERE confidence_score > 0.8
ORDER BY confidence_score DESC;
```

---

## Similarity Search

### 1. Find Similar Materials (Table Macro)

Finds k most similar materials using specified algorithm.

**Signature:**
```sql
find_similar_materials(
    query_material_id VARCHAR,
    k := 5,
    method := 'structural',
    min_similarity := 0.0,
    use_index := true,
    bom_table := 'bom_items'
) → TABLE (
    similar_material_id VARCHAR,
    jaccard_similarity DOUBLE,
    similarity_rank INTEGER
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | Material to find similar to |
| `k` | INTEGER | 5 | Number of results |
| `method` | VARCHAR | 'structural' | 'structural' or 'combined' |
| `min_similarity` | DOUBLE | 0.0 | Minimum similarity threshold |
| `use_index` | BOOLEAN | true | Use HNSW index if available |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |

**Returns:**
- `similar_material_id` - ID of similar material
- `jaccard_similarity` - Computed similarity score
- `similarity_rank` - Rank (1 = most similar)

**Complexity:**
- With index: O(log n + k) average case
- Without index: O(n) brute force

**Example:**
```sql
SELECT
  similar_material_id,
  jaccard_similarity,
  similarity_rank
FROM find_similar_materials('PUMP-A', k := 10)
WHERE similarity_rank <= 5;
```

### 2. Cold Start Analogs (Table Macro)

Generates cold-start forecasting candidates with weighted predictions.

**Signature:**
```sql
cold_start_analogs(
    query_material_id VARCHAR,
    k := 3,
    forecast_periods := 12,
    method := 'jaccard',
    use_transactional_embedding := false,
    movements_table := 'goods_movements'
) → TABLE (
    analog_material_id VARCHAR,
    similarity_score DOUBLE,
    analog_rank INTEGER,
    forecast_weight DOUBLE
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | New product needing forecast |
| `k` | INTEGER | 3 | Number of analog materials |
| `forecast_periods` | INTEGER | 12 | Future periods to forecast |
| `method` | VARCHAR | 'jaccard' | Similarity method to use |
| `use_transactional_embedding` | BOOLEAN | false | Include time-series features |
| `movements_table` | VARCHAR | 'goods_movements' | Movements history |

**Returns:**
- `analog_material_id` - Similar material with history
- `similarity_score` - Similarity to query material
- `analog_rank` - Rank (1 = best match)
- `forecast_weight` - Weight for weighted average forecast

**Example:**
```sql
SELECT
  analog_material_id,
  similarity_score,
  forecast_weight
FROM cold_start_analogs('X-750-NEW', k := 5)
ORDER BY analog_rank;
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
- **Phase 2C Features** (92-97): Movement types, lifecycle indicators

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

**Signatures:**
```sql
-- Convert MAST/STPO to universal schema
sap_to_bom_items(
    mast_table := 'MAST',
    stpo_table := 'STPO',
    output_table := 'bom_items'
) → BOOLEAN

-- Extract material descriptions from MAKT
extract_material_descriptions(
    makt_table := 'MAKT',
    language := 'EN'
) → TABLE (
    material_id VARCHAR,
    description VARCHAR
)

-- Full SAP master data conversion
sap_to_materials(
    mara_table := 'MARA',
    makt_table := 'MAKT',
    output_table := 'materials'
) → BOOLEAN
```

**Example:**
```sql
-- Convert SAP BOM to universal format
CALL sap_to_bom_items('MAST', 'STPO', 'bom_items');

-- Extract descriptions
SELECT *
FROM extract_material_descriptions(language := 'EN');
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

Validates whether statistics are current and complete.

**Signature:**
```sql
check_statistics_freshness(
    freshness_threshold_days := 7
) → TABLE (
    is_fresh BOOLEAN,
    last_updated TIMESTAMP,
    statistics_count INTEGER,
    message VARCHAR
)
```

**Example:**
```sql
SELECT * FROM check_statistics_freshness();
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

## Function Coverage Matrix

| Category | Type | Count | Functions |
|----------|------|-------|-----------|
| **Similarity** | Scalar | 2 | jaccard_similarity, wl_kernel_similarity |
| **Search** | Macro | 2 | find_similar_materials, cold_start_analogs |
| **Inference** | Macro | 1 | infer_predecessors |
| **BOM Traversal** | Macro | 4 | bom_explosion_1level, bom_explosion_multilevel, bom_where_used, bom_common_components |
| **Textual** | Macro + Scalar | 2 | compute_textual_embeddings, embed_text |
| **Transactional** | Macro | 2 | compute_transactional_embeddings, recompute_embedding_statistics |
| **Fusion** | Macro | 1 | compute_fused_embeddings |
| **Transformations** | Macro | 7 | SAP and Dynamics 365 transformations |
| **Infrastructure** | Macro + Procedure | 6 | Statistics, indexing, property graphs |
| **Utilities** | Macro | 4 | Helper and check functions |
| **TOTAL** | | **31** | Complete API |

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
- `lookback_months` - Lookback period (default: 12)
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
SELECT find_similar_materials('MAT-001');
SELECT anofox_similarity.find_similar_materials('MAT-001');
```

---

## See Also

- [README.md](../README.md) - Overview and quick start
- [CONCEPT.md](CONCEPT.md) - Algorithm theory and references
- [DATA-STRUCTURES.md](DATA-STRUCTURES.md) - Schema specifications
