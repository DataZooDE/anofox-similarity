# Helpers and infrastructure

> Helper macros, refreshing embeddings, dependency checks and the tables the extension creates.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Helper Macros

### Aggregate Material Components
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

### Filter Recent Movements
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

### Check anofox-forecast Available
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

### Check DuckPGQ Available
Checks if DuckPGQ extension is loaded.

**Signature:**
```sql
check_duckpgq_available() → BOOLEAN
```

---

## Database Infrastructure

The extension creates several database objects on load to store embeddings.

> [!NOTE]
> **Read-only databases:** every function works on a database opened read-only, the SQL macros
> (BOM traversal, ERP transforms, embeddings, statistics) included: they are registered inside
> the extension, not in your catalog. On a read-only database the embedding tables below are
> not created.
>
> **What LOAD writes:** on a writable database, loading the extension creates the tables and
> HNSW indexes below in that database file. A file that contains them needs the `vss`
> extension to insert into `material_embeddings` later. Use a separate database file, or attach
> your data read-only, if you do not want them in your own file.

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

**material_embeddings_dirty** - Reserved for dirty tracking; nothing populates it (see [Incremental Updates](#incremental-updates)):
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

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `aggregate_material_components`

*Table macro*

Groups a BOM edge list into one row per parent material with its child components as a sorted list — the shape the Jaccard and cold-start functions expect. 'material_filter' restricts the result to the listed parents; NULL means all of them.

```sql
aggregate_material_components(bom_table, material_filter)
```

**Examples**

```sql
SELECT * FROM aggregate_material_components(bom_table := 'bom_items');
```

### `filter_recent_movements`

*Table macro*

Filters a goods-movement table to a recent window and a minimum quantity. The window is anchored on the most recent movement date IN THE DATA rather than wall-clock today, which works without the ICU extension, is deterministic for tests, and is what planners want on historical ERP extracts where 'today' may be years after the last movement.

```sql
filter_recent_movements(movements_table, time_window_days, min_quantity)
```

**Examples**

```sql
SELECT * FROM filter_recent_movements(movements_table := 'goods_movements', time_window_days := 180);
```

### `check_anofox_forecast_available`

*Scalar macro*

Returns true when the anofox_forecast extension is loaded, so a caller can choose the richer time-series feature path over the built-in one. anofox_forecast is an optional soft dependency and is never auto-installed.

```sql
check_anofox_forecast_available()
```

**Examples**

```sql
SELECT check_anofox_forecast_available();
```

### `check_duckpgq_available`

*Scalar macro*

Returns true when the DuckPGQ extension is loaded, so a caller can choose the graph-based BOM traversal path over the SQL fallback. DuckPGQ is an optional soft dependency and is never auto-installed.

```sql
check_duckpgq_available()
```

**Examples**

```sql
SELECT check_duckpgq_available();
```

<!-- generated:functions:end -->
