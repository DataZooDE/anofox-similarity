# Guide 04: Similarity Search at Scale

## The Problem

> "With 1M+ materials in our SAP system, how do we efficiently find similar products without scanning the entire database?"

Brute-force Jaccard similarity is O(n) - it must compare against every material. With 1 million materials, this becomes impractically slow. This guide shows how to use **min-hash embeddings** and **HNSW indexes** to achieve logarithmic search time.

## The Solution

Use **approximate nearest neighbor (ANN)** search:
1. **Generate embeddings** - Convert component sets to 128-dimensional vectors using min-hash
2. **Build HNSW index** - Create a hierarchical graph structure for fast lookups
3. **Search efficiently** - Find similar materials in O(log n) instead of O(n)

## Prerequisites

```sql
LOAD anofox_similarity;

-- Load SAP tables (full dataset)
CREATE TABLE sap_mast AS SELECT * FROM read_parquet('test/data/sap/mast.parquet');
CREATE TABLE sap_stko AS SELECT * FROM read_parquet('test/data/sap/stko.parquet');
CREATE TABLE sap_stpo AS SELECT * FROM read_parquet('test/data/sap/stpo.parquet');

-- Transform to universal BOM format
CREATE TABLE bom_items AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    bom_alternative := '01',
    reference_date := CURRENT_DATE
);

-- Verify scale
SELECT COUNT(*) AS total_bom_items FROM bom_items;
SELECT COUNT(DISTINCT parent_id) AS unique_materials FROM bom_items;
```

## Step-by-Step Walkthrough

### Step 1: Understand the Scale Problem

Compare brute-force search time at different scales:

```sql
-- Count materials with BOMs
WITH material_counts AS (
    SELECT COUNT(DISTINCT parent_id) AS count FROM bom_items
)
SELECT
    count AS materials_with_boms,
    CASE
        WHEN count < 1000 THEN 'Small - brute force OK'
        WHEN count < 10000 THEN 'Medium - consider indexing'
        WHEN count < 100000 THEN 'Large - indexing recommended'
        ELSE 'Enterprise - indexing required'
    END AS recommendation
FROM material_counts;
```

**Expected Result for SAP dataset:**

| materials_with_boms | recommendation |
|---------------------|----------------|
| ~363,512 | Enterprise - indexing required |

### Step 2: Generate Min-Hash Embeddings

Convert component sets to 128-dimensional vectors:

```sql
-- Generate min-hash embeddings for all materials
-- This computes 128 hash values per material (one per seed)
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components, updated_at)
SELECT
    material_id,
    array_agg(minhash_value ORDER BY seed)::FLOAT[128] AS jaccard_embedding,
    MAX(num_components) AS num_components,
    CURRENT_TIMESTAMP AS updated_at
FROM compute_jaccard_embeddings(bom_table := 'bom_items')
GROUP BY material_id
ON CONFLICT (material_id) DO UPDATE SET
    jaccard_embedding = EXCLUDED.jaccard_embedding,
    num_components = EXCLUDED.num_components,
    updated_at = EXCLUDED.updated_at;

-- Verify embeddings were created
SELECT
    COUNT(*) AS materials_with_embeddings,
    AVG(num_components) AS avg_components,
    MAX(num_components) AS max_components
FROM material_embeddings
WHERE jaccard_embedding IS NOT NULL;
```

**How Min-Hash Works:**

```
Component Set: {A, B, C, D, E}
                  ↓
    128 different hash functions (seeds)
                  ↓
    For each seed: MIN(hash(A), hash(B), hash(C), hash(D), hash(E))
                  ↓
    Result: [0.123, 0.456, 0.789, ...] (128-D vector)

Key property: L2 distance between min-hash vectors ≈ Jaccard distance
```

### Step 3: Create HNSW Index

Build the hierarchical navigable small world graph for fast lookups:

```sql
-- Create HNSW index on jaccard embeddings
-- Parameters tuned for enterprise scale
CALL CreateHNSWIndexes(
    embedding_table := 'material_embeddings',
    ef := 200,     -- Higher ef = better recall, slower build
    M := 32        -- Higher M = better recall, more memory
);

-- Verify index was created
SELECT
    index_name,
    table_name,
    is_unique
FROM duckdb_indexes()
WHERE table_name = 'material_embeddings';
```

**HNSW Configuration Guide:**

| Dataset Size | ef | M | Build Time | Memory |
|--------------|----|----|------------|--------|
| < 10K | 50 | 8 | Fast | ~1 MB |
| 10K - 100K | 100 | 16 | Medium | ~10 MB |
| 100K - 1M | 200 | 32 | Slow | ~100 MB |
| > 1M | 300 | 48 | Very slow | ~500 MB |

### Step 4: Search with Index

Now similarity search uses the index automatically:

```sql
-- Fast similarity search using index
-- O(log n) instead of O(n)
SELECT
    material_id,
    similarity,
    shared_components,
    total_components
FROM find_similar_materials_jaccard(
    '000000000010561200',  -- Query material
    10,                     -- Top 10 results
    min_similarity := 0.3,
    bom_table := 'bom_items'
);
```

### Step 5: Compare Performance

Benchmark indexed vs brute-force search:

```sql
-- Create timing comparison
WITH RECURSIVE timing AS (
    -- Time indexed search (using embeddings)
    SELECT 'indexed' AS method, CURRENT_TIMESTAMP AS start_time
    UNION ALL
    SELECT 'indexed', CURRENT_TIMESTAMP
    FROM find_similar_materials_jaccard('000000000010561200', 10)
)
SELECT * FROM timing;

-- For accurate benchmarking, use EXPLAIN ANALYZE:
EXPLAIN ANALYZE
SELECT *
FROM find_similar_materials_jaccard('000000000010561200', 10);
```

**Expected Performance Improvement:**

| Materials | Brute Force | With Index | Speedup |
|-----------|-------------|------------|---------|
| 10K | ~100ms | ~5ms | 20x |
| 100K | ~1s | ~10ms | 100x |
| 363K | ~3.6s | ~15ms | 240x |
| 1M | ~10s | ~20ms | 500x |

## Understanding the Results

### Min-Hash Accuracy Trade-off

Min-hash is an **approximate** algorithm. There's a trade-off:

| Embedding Dimension | Accuracy | Memory | Speed |
|---------------------|----------|--------|-------|
| 64-D | ~90% | Low | Fast |
| 128-D | ~95% | Medium | Medium |
| 256-D | ~98% | High | Slower |

The extension uses 128-D by default, providing 95%+ accuracy for most use cases.

### When to Rebuild Index

Rebuild the index when:
- Adding many new materials (> 10% of dataset)
- Changing BOM relationships significantly
- Accuracy seems degraded

```sql
-- Check how many materials need embedding updates
SELECT COUNT(*) AS dirty_materials
FROM material_embeddings_dirty;

-- Refresh embeddings incrementally
SELECT * FROM refresh_dirty_embeddings(bom_table := 'bom_items');

-- Rebuild index after major changes
CALL CreateHNSWIndexes(ef := 200, M := 32);
```

## Variations

### Batch Processing for Initial Load

For very large datasets, process in batches:

```sql
-- Process embeddings in batches of 50,000
DO $$
DECLARE
    batch_size INT := 50000;
    total_materials INT;
    processed INT := 0;
BEGIN
    SELECT COUNT(DISTINCT parent_id) INTO total_materials FROM bom_items;

    WHILE processed < total_materials LOOP
        INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components)
        SELECT
            material_id,
            array_agg(minhash_value ORDER BY seed)::FLOAT[128],
            MAX(num_components)
        FROM compute_jaccard_embeddings(bom_table := 'bom_items')
        WHERE material_id IN (
            SELECT DISTINCT parent_id
            FROM bom_items
            ORDER BY parent_id
            LIMIT batch_size
            OFFSET processed
        )
        GROUP BY material_id
        ON CONFLICT (material_id) DO UPDATE SET
            jaccard_embedding = EXCLUDED.jaccard_embedding;

        processed := processed + batch_size;
        RAISE NOTICE 'Processed % of % materials', processed, total_materials;
    END LOOP;
END;
$$;
```

### Sampling Strategy for Testing

Work with a representative sample during development:

```sql
-- Create a 10% random sample for testing
CREATE TABLE bom_items_sample AS
SELECT * FROM bom_items
WHERE parent_id IN (
    SELECT DISTINCT parent_id
    FROM bom_items
    USING SAMPLE 10 PERCENT (BERNOULLI)
);

-- Generate embeddings for sample only
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components)
SELECT
    material_id,
    array_agg(minhash_value ORDER BY seed)::FLOAT[128],
    MAX(num_components)
FROM compute_jaccard_embeddings(bom_table := 'bom_items_sample')
GROUP BY material_id;
```

### Multi-Modal Search at Scale

Combine structural and textual similarity:

```sql
-- Generate both structural and textual embeddings
-- Structural (Jaccard min-hash)
UPDATE material_embeddings
SET jaccard_embedding = (
    SELECT array_agg(minhash_value ORDER BY seed)::FLOAT[128]
    FROM compute_jaccard_embeddings(bom_table := 'bom_items')
    WHERE material_id = material_embeddings.material_id
);

-- Textual (from descriptions)
UPDATE material_embeddings e
SET textual_embedding = t.textual_embedding
FROM compute_textual_embeddings(
    makt_table := 'sap_makt',
    language := 'E'
) t
WHERE e.material_id = t.material_id;

-- Fused search using both modalities
SELECT *
FROM compute_fused_embeddings(
    weights_structural := 0.6,
    weights_textual := 0.4
);
```

## Performance Tips

### 1. Filter Before Search

Reduce the search space with pre-filters:

```sql
-- Search only within a material type
SELECT
    f.material_id,
    f.similarity
FROM find_similar_materials_jaccard('000000000010561200', 100) f
JOIN sap_mara m ON f.material_id = m.matnr
WHERE m.mtart = 'FERT'  -- Only finished goods
ORDER BY f.similarity DESC
LIMIT 10;
```

### 2. Use Minimum Similarity Threshold

Skip low-similarity comparisons early:

```sql
-- Only return materials with >= 30% similarity
SELECT *
FROM find_similar_materials_jaccard(
    '000000000010561200',
    10,
    min_similarity := 0.3  -- Skip materials < 30% similar
);
```

### 3. Monitor Index Health

Check index statistics periodically:

```sql
-- Check embedding coverage
SELECT
    COUNT(*) AS total_materials,
    COUNT(jaccard_embedding) AS with_embeddings,
    ROUND(100.0 * COUNT(jaccard_embedding) / COUNT(*), 2) AS coverage_pct
FROM (
    SELECT DISTINCT parent_id AS material_id FROM bom_items
) all_mats
LEFT JOIN material_embeddings e USING (material_id);

-- Check for stale embeddings
SELECT
    COUNT(*) AS stale_embeddings,
    MIN(updated_at) AS oldest_update
FROM material_embeddings
WHERE updated_at < CURRENT_DATE - INTERVAL '30 days';
```

## Use Cases

### Material Consolidation

Find duplicate or near-duplicate materials across plants:

```sql
-- Find potential duplicates (>90% similar)
WITH candidates AS (
    SELECT DISTINCT parent_id FROM bom_items LIMIT 1000
)
SELECT
    c.parent_id AS material_a,
    f.material_id AS material_b,
    f.similarity
FROM candidates c
CROSS JOIN LATERAL find_similar_materials_jaccard(c.parent_id, 5, min_similarity := 0.9) f
WHERE c.parent_id < f.material_id  -- Avoid duplicates
ORDER BY f.similarity DESC
LIMIT 100;
```

### Supplier Risk Assessment

Quickly identify all materials using a component:

```sql
-- Find all materials using a risky component
WITH affected_materials AS (
    SELECT DISTINCT parent_id
    FROM bom_items
    WHERE child_id = 'RISKY-COMPONENT-001'
),
-- For each affected material, find similar alternatives
alternatives AS (
    SELECT
        a.parent_id AS affected,
        f.material_id AS alternative,
        f.similarity
    FROM affected_materials a
    CROSS JOIN LATERAL find_similar_materials_jaccard(a.parent_id, 5) f
    WHERE f.material_id NOT IN (SELECT parent_id FROM affected_materials)
)
SELECT * FROM alternatives
WHERE similarity >= 0.7
ORDER BY affected, similarity DESC;
```

## Next Steps

- **[Guide 05: Multi-Language Descriptions](05_multilanguage_descriptions.md)** - Work with localized text
- **[Guide 01: Data Transformation](01_data_transformation.md)** - Review ETL workflow
- **[API Reference](../../docs/API_REFERENCE.md)** - Full function documentation
