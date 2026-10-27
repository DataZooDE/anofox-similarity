# Guide 02: Alternative BOM Comparison

## The Problem

> "We have multiple manufacturing approaches for the same product. How do we compare them to understand the differences?"

SAP supports **alternative BOMs** - different ways to manufacture the same product. This is used for:
- **Cost optimization**: Compare material costs between alternatives
- **Supplier flexibility**: Different components from different suppliers
- **Process variants**: Different manufacturing processes for same output
- **Regional variations**: Different components available in different regions

## The Solution

Load different BOM alternatives using the `bom_alternative` parameter and compare their component structures using Jaccard similarity.

## Prerequisites

```sql
LOAD anofox_similarity;

-- Load SAP tables
CREATE TABLE sap_mast AS SELECT * FROM read_parquet('test/data/sap/mast.parquet');
CREATE TABLE sap_stko AS SELECT * FROM read_parquet('test/data/sap/stko.parquet');
CREATE TABLE sap_stpo AS SELECT * FROM read_parquet('test/data/sap/stpo.parquet');
```

## Step-by-Step Walkthrough

### Step 1: Check Available Alternatives

First, see which materials have multiple BOM alternatives:

```sql
-- Find materials with multiple alternatives
SELECT
    m.matnr AS material_id,
    COUNT(DISTINCT h.stlal) AS alternative_count,
    STRING_AGG(DISTINCT h.stlal, ', ' ORDER BY h.stlal) AS alternatives
FROM sap_mast m
JOIN sap_stko h ON m.stlnr = h.stlnr
GROUP BY m.matnr
HAVING COUNT(DISTINCT h.stlal) > 1
ORDER BY alternative_count DESC
LIMIT 10;
```

### Step 2: Load Primary Alternative (01)

```sql
CREATE TABLE bom_alt_01 AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    bom_alternative := '01',
    reference_date := CURRENT_DATE
);

SELECT COUNT(*) AS items_alt_01 FROM bom_alt_01;
```

### Step 3: Load Secondary Alternative (02)

```sql
CREATE TABLE bom_alt_02 AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    bom_alternative := '02',
    reference_date := CURRENT_DATE
);

SELECT COUNT(*) AS items_alt_02 FROM bom_alt_02;
```

### Step 4: Compare Component Overlap

Find materials where alternatives differ:

```sql
WITH
alt_01_components AS (
    SELECT parent_id, LIST(DISTINCT child_id ORDER BY child_id) AS components_01
    FROM bom_alt_01
    GROUP BY parent_id
),
alt_02_components AS (
    SELECT parent_id, LIST(DISTINCT child_id ORDER BY child_id) AS components_02
    FROM bom_alt_02
    GROUP BY parent_id
),
comparison AS (
    SELECT
        COALESCE(a.parent_id, b.parent_id) AS material_id,
        a.components_01,
        b.components_02,
        CASE
            WHEN a.components_01 IS NULL THEN 0
            WHEN b.components_02 IS NULL THEN 0
            ELSE jaccard_similarity(a.components_01, b.components_02)
        END AS similarity
    FROM alt_01_components a
    FULL OUTER JOIN alt_02_components b ON a.parent_id = b.parent_id
)
SELECT
    material_id,
    LEN(components_01) AS components_in_01,
    LEN(components_02) AS components_in_02,
    ROUND(similarity, 4) AS jaccard_similarity
FROM comparison
WHERE components_01 IS NOT NULL AND components_02 IS NOT NULL
ORDER BY similarity ASC  -- Most different first
LIMIT 20;
```

### Step 5: Detailed Component Diff

For a specific material, see exactly what differs between alternatives:

```sql
-- Pick a material with both alternatives
WITH target AS (
    SELECT '000000000010561200' AS material_id  -- Replace with actual ID
),
alt_01 AS (
    SELECT child_id FROM bom_alt_01, target WHERE parent_id = target.material_id
),
alt_02 AS (
    SELECT child_id FROM bom_alt_02, target WHERE parent_id = target.material_id
)
SELECT
    'Only in Alt 01' AS presence,
    child_id
FROM alt_01
WHERE child_id NOT IN (SELECT child_id FROM alt_02)
UNION ALL
SELECT
    'Only in Alt 02' AS presence,
    child_id
FROM alt_02
WHERE child_id NOT IN (SELECT child_id FROM alt_01)
UNION ALL
SELECT
    'In Both' AS presence,
    child_id
FROM alt_01
WHERE child_id IN (SELECT child_id FROM alt_02);
```

## Understanding the Results

### Alternative BOM Usage in SAP

| Alternative | Typical Use |
|-------------|-------------|
| **01** | Primary/standard BOM |
| **02** | Secondary/alternate process |
| **03** | Tertiary/regional variant |

### Similarity Interpretation

| Similarity | Meaning | Action |
|------------|---------|--------|
| **1.0** | Identical components | Alternatives have same structure |
| **0.8-0.99** | Minor differences | Small component substitutions |
| **0.5-0.79** | Significant differences | Major process variation |
| **< 0.5** | Very different | Essentially different products |

## Variations

### Compare All Three Alternatives

```sql
-- Load all three alternatives
CREATE TABLE bom_alt_03 AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    bom_alternative := '03'
);

-- Pairwise comparison matrix
WITH components AS (
    SELECT '01' AS alt, parent_id, LIST(DISTINCT child_id) AS comp FROM bom_alt_01 GROUP BY parent_id
    UNION ALL
    SELECT '02' AS alt, parent_id, LIST(DISTINCT child_id) AS comp FROM bom_alt_02 GROUP BY parent_id
    UNION ALL
    SELECT '03' AS alt, parent_id, LIST(DISTINCT child_id) AS comp FROM bom_alt_03 GROUP BY parent_id
)
SELECT
    a.parent_id,
    a.alt AS alt_a,
    b.alt AS alt_b,
    ROUND(jaccard_similarity(a.comp, b.comp), 4) AS similarity
FROM components a
JOIN components b ON a.parent_id = b.parent_id AND a.alt < b.alt
ORDER BY similarity ASC
LIMIT 20;
```

### Find Products with Highest Alternative Divergence

```sql
WITH
alt_01_comp AS (
    SELECT parent_id, LIST(DISTINCT child_id) AS comp FROM bom_alt_01 GROUP BY parent_id
),
alt_02_comp AS (
    SELECT parent_id, LIST(DISTINCT child_id) AS comp FROM bom_alt_02 GROUP BY parent_id
)
SELECT
    a.parent_id,
    LEN(a.comp) AS components_01,
    LEN(b.comp) AS components_02,
    ROUND(jaccard_similarity(a.comp, b.comp), 4) AS similarity
FROM alt_01_comp a
JOIN alt_02_comp b ON a.parent_id = b.parent_id
WHERE jaccard_similarity(a.comp, b.comp) < 0.5  -- High divergence
ORDER BY similarity ASC
LIMIT 10;
```

## Use Cases

### Cost Optimization Analysis

When evaluating which alternative to use, compare:
1. Load both alternatives
2. Calculate component overlap
3. Join with material cost data to see cost differences
4. Decide which alternative is more cost-effective

### Supply Chain Flexibility

When a component becomes unavailable:
1. Find materials using that component in Alt 01
2. Check if Alt 02 provides a viable substitute
3. Assess similarity to understand scope of change

## Next Steps

- **[Guide 03: Point-in-Time Analysis](03_point_in_time_analysis.md)** - Track BOM changes over time
- **[Guide 04: Similarity at Scale](04_similarity_at_scale.md)** - Efficient search across 1M+ materials
- **[API Reference](../../docs/API_REFERENCE.md)** - Full function documentation
