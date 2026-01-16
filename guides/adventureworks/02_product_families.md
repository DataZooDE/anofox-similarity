# Guide 02: Detecting Product Families and Variants

## The Problem

> "We have 240 bicycle models in our catalog. Which ones are variants of each other? How do we identify product families for portfolio management?"

Understanding product families helps with:
- **Inventory optimization** - Consolidate similar parts across variants
- **Pricing strategy** - Ensure variant pricing is consistent
- **Lifecycle management** - Phase out variants together
- **Forecast aggregation** - Roll up demand at family level

## The Solution

Use **Jaccard similarity** to cluster products by component overlap. Products sharing >80% of components are typically variants of the same base product.

## Prerequisites

```sql
LOAD anofox_similarity;

CREATE TABLE materials AS
SELECT * FROM read_csv_auto('test/data/adventureworks/materials.csv.gz');

CREATE TABLE bom_items AS
SELECT * FROM read_csv_auto('test/data/adventureworks/bom_items.csv.gz');
```

## Step-by-Step Walkthrough

### Step 1: Pick a Reference Product

Start with any finished bicycle and find its family:

```sql
SELECT material_id, description
FROM materials
WHERE material_id = 'AW-749';
```

| material_id | description |
|-------------|-------------|
| AW-749 | Road-150 Red, 62 |

### Step 2: Find All Variants (High Similarity)

```sql
SELECT
    m.material_id,
    m.description,
    ROUND(f.similarity, 4) AS jaccard,
    f.shared_components,
    f.total_components
FROM find_similar_materials_jaccard('AW-749', 50, min_similarity := 0.7) f
JOIN materials m ON f.material_id = m.material_id
ORDER BY f.similarity DESC;
```

**Expected Results:**

| material_id | description | jaccard | shared_components | total_components |
|-------------|-------------|---------|-------------------|------------------|
| AW-750 | Road-150 Red, 44 | 0.8667 | 13 | 15 |
| AW-751 | Road-150 Red, 48 | 0.8667 | 13 | 15 |
| AW-752 | Road-150 Red, 52 | 0.8667 | 13 | 15 |
| AW-753 | Road-150 Red, 56 | 0.8667 | 13 | 15 |

**Interpretation:** The Road-150 Red family has 5 size variants (44, 48, 52, 56, 62 cm frames).

### Step 3: Build a Family Clustering View

Create a reusable view to identify families across the entire catalog:

```sql
CREATE OR REPLACE VIEW product_families AS
WITH similarity_pairs AS (
    SELECT
        parent.material_id AS product_a,
        f.material_id AS product_b,
        f.similarity
    FROM materials parent
    CROSS JOIN LATERAL find_similar_materials_jaccard(
        parent.material_id, 10,
        min_similarity := 0.8,
        bom_table := 'bom_items'
    ) f
    WHERE parent.material_type = 'FERT'
)
SELECT * FROM similarity_pairs
WHERE product_a < product_b;  -- Deduplicate pairs
```

### Step 4: Visualize the Similarity Distribution

Understand how similarity distributes across your catalog:

```sql
SELECT
    CASE
        WHEN similarity >= 0.9 THEN '90-100% (size variants)'
        WHEN similarity >= 0.8 THEN '80-89% (model variants)'
        WHEN similarity >= 0.7 THEN '70-79% (related products)'
        ELSE '< 70% (different products)'
    END AS similarity_band,
    COUNT(*) AS pair_count
FROM (
    SELECT material_a, material_b, expected_similarity AS similarity
    FROM read_csv_auto('test/data/adventureworks/ground_truth.csv.gz')
)
GROUP BY 1
ORDER BY 1 DESC;
```

### Step 5: Find Isolated Products

Identify products that have NO close variants (potential simplification candidates):

```sql
WITH has_variant AS (
    SELECT DISTINCT parent_id AS material_id
    FROM bom_items
    WHERE parent_id IN (SELECT material_id FROM materials WHERE material_type = 'FERT')
)
SELECT m.material_id, m.description,
    (SELECT MAX(similarity)
     FROM find_similar_materials_jaccard(m.material_id, 5, bom_table := 'bom_items')
    ) AS max_similarity
FROM materials m
WHERE m.material_type = 'FERT'
ORDER BY max_similarity ASC
LIMIT 10;
```

Products with low max_similarity are unique offerings without variants.

## Understanding the Results

### Similarity Thresholds for Family Detection

| Threshold | Meaning | Typical Cause |
|-----------|---------|---------------|
| **= 1.0** | Identical BOMs | Duplicate or phantom product |
| **0.85-0.99** | Size/color variant | Frame size, color coating |
| **0.70-0.84** | Model variant | Different quality tier components |
| **0.50-0.69** | Same category | Shared wheels/brakes, different frame |
| **< 0.50** | Different category | Mountain vs Road vs Touring |

### Family Hierarchy Example

```
Road-150 Family (AW-749 cluster)
├── Road-150 Red, 44 (AW-750)
├── Road-150 Red, 48 (AW-751)
├── Road-150 Red, 52 (AW-752)
├── Road-150 Red, 56 (AW-753)
└── Road-150 Red, 62 (AW-749)
    └── Shared: HL Road Wheels, HL Handlebars, HL Headset...
    └── Differs: HL Road Frame - Red, {44|48|52|56|62}
```

## Variations

### Cluster by Description Pattern

When product naming is consistent, combine similarity with description matching:

```sql
SELECT
    REGEXP_EXTRACT(description, '^([A-Za-z]+-\d+)') AS model_family,
    COUNT(*) AS variants,
    AVG(jaccard) AS avg_internal_similarity
FROM (
    SELECT m.description, f.similarity AS jaccard
    FROM materials m
    CROSS JOIN LATERAL find_similar_materials_jaccard(
        m.material_id, 5, min_similarity := 0.8
    ) f
    WHERE m.material_type = 'FERT'
)
GROUP BY 1
HAVING COUNT(*) > 1
ORDER BY variants DESC;
```

### Export Families for External Analysis

```sql
COPY (
    SELECT
        m1.material_id AS product_a,
        m1.description AS desc_a,
        m2.material_id AS product_b,
        m2.description AS desc_b,
        jaccard_similarity(m1.material_id, m2.material_id, 'bom_items') AS similarity
    FROM materials m1
    CROSS JOIN materials m2
    WHERE m1.material_id < m2.material_id
      AND m1.material_type = 'FERT'
      AND m2.material_type = 'FERT'
      AND jaccard_similarity(m1.material_id, m2.material_id, 'bom_items') > 0.7
) TO 'product_families.csv' (HEADER, DELIMITER ',');
```

## Next Steps

- **[Guide 03: BOM Navigation](03_bom_navigation.md)** - Understand what makes products similar
- **[Guide 04: Quality Tiers](04_quality_tiers.md)** - Analyze HL/ML/LL component differences
- **[Guide 05: Accuracy Validation](05_accuracy_validation.md)** - Validate family detection accuracy
