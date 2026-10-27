# Material search, predecessors and cold start

> Rank similar materials, infer predecessors and find analogs for new products.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Predecessor Inference
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

## Find Similar Materials - Jaccard
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
| `material_id` | VARCHAR | Required | Material to find similar to (positional) |
| `k` | INTEGER | Required | Number of results |
| `min_similarity` | DOUBLE | 0.0 | Minimum similarity threshold |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |
| `mode` | VARCHAR | 'jaccard' | `'jaccard'` (symmetric) or `'containment'`: the share of the query's components a material contains. Ties are ordered by Jaccard (not returned), so `similarity` stays in [0, 1]. Like the Jaccard mode it returns materials without any shared component (similarity 0); filter with `min_similarity`. Counts components, not quantities; for quantities use [`find_covering_entities`](10-covering-search.md#find_covering_entities) |

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

## Find Similar Materials - WL Kernel
Finds k most similar materials using Weisfeiler-Lehman graph kernel similarity.

**Signature:**
```sql
find_similar_materials_wl_kernel(
    material_id VARCHAR,
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
| `material_id` | VARCHAR | Required | Material to find similar to (positional) |
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

## Cold Start Analogs
Finds analog materials with consumption history for cold-start forecasting.

**Signature:**
```sql
cold_start_analogs(
    material_id VARCHAR,
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
| `material_id` | VARCHAR | Required | New product needing analogs (positional) |
| `k` | INTEGER | Required | Number of analog materials to return |
| `min_history_months` | INTEGER | 0 | Minimum months between the first and the last movement |
| `min_similarity` | DOUBLE | 0.0 | Minimum Jaccard similarity threshold |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |
| `movements_table` | VARCHAR | 'goods_movements' | Movements history table |

**Returns:**
- `material_id` - Similar material with history
- `similarity` - Jaccard similarity to query material
- `shared_components` - Count of shared components
- `total_components` - Count of unique components in union
- `history_months` - Calendar months from the first to the last movement (not months with movements)
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

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `find_similar_materials_jaccard`

*Table function*

Finds the k most structurally similar materials to a query material using exact Jaccard similarity on BOM component sets. Returns material_id, similarity, shared_components, and total_components. Filtered by min_similarity threshold. mode := 'containment' ranks by the share of the query's components each material contains instead (default 'jaccard').

```sql
find_similar_materials_jaccard(material_id, k, bom_table, min_similarity, mode)
```

**Examples**

```sql
SELECT * FROM find_similar_materials_jaccard('MAT-001', 10);
SELECT * FROM find_similar_materials_jaccard('MAT-001', 20, min_similarity := 0.3);
SELECT * FROM find_similar_materials_jaccard('MAT-001', 10, mode := 'containment');
```

### `find_similar_materials_wl_kernel`

*Table function*

Finds the k most structurally similar materials using the Weisfeiler-Lehman graph kernel, which captures multi-hop BOM structure beyond direct component overlap. iterations controls the number of WL refinement rounds (default 3).

```sql
find_similar_materials_wl_kernel(material_id, k, bom_table, min_similarity, iterations)
```

**Examples**

```sql
SELECT * FROM find_similar_materials_wl_kernel('MAT-001', 10);
SELECT * FROM find_similar_materials_wl_kernel('MAT-001', 10, iterations := 3, min_similarity := 0.2);
```

### `infer_predecessors`

*Table function*

Identifies predecessor (superseded) materials for a given successor by combining BOM structural similarity with anti-correlated consumption patterns. Returns predecessor candidates ranked by confidence score along with correlation, similarity, and temporal overlap data. Useful for lifecycle transition planning and inventory run-down.

```sql
infer_predecessors(material_id, movements_table, bom_table, min_overlapping_weeks, lag_weeks, min_confidence, min_similarity, lookback_months)
```

**Examples**

```sql
SELECT * FROM infer_predecessors('NEW-PART');
SELECT * FROM infer_predecessors('NEW-PART', lookback_months := 24, min_similarity := 0.5, min_confidence := 0.6, lag_weeks := 4);
```

### `cold_start_analogs`

*Table function*

Finds structural analogs for a cold-start material (one with little or no demand history). Returns established materials that are structurally similar and have at least min_history_months of goods movement data, enabling surrogate forecasting.

```sql
cold_start_analogs(material_id, k, movements_table, bom_table, min_similarity, min_history_months)
```

**Examples**

```sql
SELECT * FROM cold_start_analogs('NEW-PART', 5);
SELECT * FROM cold_start_analogs('NEW-PART', 10, min_history_months := 12, min_similarity := 0.4);
```

<!-- generated:functions:end -->
