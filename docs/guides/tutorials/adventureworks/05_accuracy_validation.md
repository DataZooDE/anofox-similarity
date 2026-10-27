# Guide 05: Validating Similarity Accuracy

## The Problem

> "How do I know the similarity algorithm is working correctly? How can I measure its accuracy against known product relationships?"

Accuracy validation is essential for:
- **Algorithm selection** - Choosing between Jaccard and WL Kernel
- **Threshold tuning** - Setting appropriate similarity cutoffs
- **Regression testing** - Ensuring updates don't degrade quality
- **Business confidence** - Proving value to stakeholders

## The Solution

Use the **ground truth dataset** containing 100 labeled product pairs to compute accuracy metrics:
- **Precision@K** - How many top-K results are correct?
- **Recall@K** - How many correct answers did we find in top-K?
- **MRR (Mean Reciprocal Rank)** - On average, where does the correct answer appear?

## Prerequisites

```sql
LOAD anofox_similarity;

CREATE TABLE materials AS
SELECT * FROM read_csv_auto('test/data/adventureworks/materials.csv.gz');

CREATE TABLE bom_items AS
SELECT * FROM read_csv_auto('test/data/adventureworks/bom_items.csv.gz');

CREATE TABLE ground_truth AS
SELECT * FROM read_csv_auto('test/data/adventureworks/ground_truth.csv.gz');
```

## Step-by-Step Walkthrough

### Step 1: Understand the Ground Truth Dataset

Examine the labeled product pairs:

```sql
SELECT
    relationship_type,
    COUNT(*) AS pair_count,
    ROUND(AVG(expected_similarity), 4) AS avg_similarity,
    ROUND(MIN(expected_similarity), 4) AS min_similarity,
    ROUND(MAX(expected_similarity), 4) AS max_similarity
FROM ground_truth
GROUP BY relationship_type
ORDER BY avg_similarity DESC;
```

**Expected Results:**

| relationship_type | pair_count | avg_similarity | min_similarity | max_similarity |
|-------------------|------------|----------------|----------------|----------------|
| variant | 60 | 0.8733 | 0.8000 | 0.9333 |
| partial_overlap | 20 | 0.6250 | 0.4500 | 0.8000 |
| low_overlap | 20 | 0.3200 | 0.1000 | 0.4900 |

**Interpretation:**
- **Variant pairs** (60): Same bike in different sizes - high similarity (>0.8)
- **Partial overlap** (20): Related but different products - medium similarity
- **Low overlap** (20): Unrelated products - low similarity baseline

### Step 2: Verify a Known Pair

Check that Jaccard similarity matches ground truth for a specific pair:

```sql
-- First, aggregate components for each material
WITH components AS (
    SELECT parent_id AS material_id, LIST(DISTINCT child_id) AS comp_list
    FROM bom_items
    GROUP BY parent_id
)
SELECT
    g.material_a,
    g.material_b,
    g.relationship_type,
    ROUND(g.expected_similarity, 4) AS expected,
    ROUND(jaccard_similarity(a.comp_list, b.comp_list), 4) AS computed,
    g.notes
FROM ground_truth g
JOIN components a ON g.material_a = a.material_id
JOIN components b ON g.material_b = b.material_id
WHERE g.material_a = 'AW-749' AND g.material_b = 'AW-750';
```

**Expected Result:**

| material_a | material_b | relationship_type | expected | computed | notes |
|------------|------------|-------------------|----------|----------|-------|
| AW-749 | AW-750 | variant | 0.8667 | 0.8667 | 13/15 components shared |

### Step 3: Calculate Precision@K

Precision@K answers: "Of the top-K results returned, how many are in the ground truth?"

```sql
WITH
-- Get all ground truth variants for query products
variant_gt AS (
    SELECT material_a AS query_id, material_b AS expected_match
    FROM ground_truth
    WHERE relationship_type = 'variant'
),
-- Get top-10 results for each query
predictions AS (
    SELECT
        q.query_id,
        f.material_id AS predicted_match,
        ROW_NUMBER() OVER (PARTITION BY q.query_id ORDER BY f.similarity DESC) AS rank
    FROM (SELECT DISTINCT query_id FROM variant_gt) q
    CROSS JOIN LATERAL find_similar_materials_jaccard(q.query_id, 10) f
),
-- Calculate precision for each query at K=5 and K=10
precision_calc AS (
    SELECT
        p.query_id,
        COUNT(CASE WHEN p.rank <= 5 AND g.expected_match IS NOT NULL THEN 1 END) AS hits_at_5,
        COUNT(CASE WHEN p.rank <= 10 AND g.expected_match IS NOT NULL THEN 1 END) AS hits_at_10,
        5.0 AS k5,
        10.0 AS k10
    FROM predictions p
    LEFT JOIN variant_gt g ON p.query_id = g.query_id AND p.predicted_match = g.expected_match
    WHERE p.rank <= 10
    GROUP BY p.query_id
)
SELECT
    ROUND(AVG(hits_at_5 / k5), 4) AS precision_at_5,
    ROUND(AVG(hits_at_10 / k10), 4) AS precision_at_10
FROM precision_calc;
```

### Step 4: Calculate Recall@K

Recall@K answers: "Of all ground truth pairs, how many did we find in top-K?"

```sql
WITH
-- Ground truth variant pairs
variant_gt AS (
    SELECT material_a AS query_id, material_b AS expected_match
    FROM ground_truth
    WHERE relationship_type = 'variant'
),
-- Count how many ground truth matches exist per query
gt_counts AS (
    SELECT query_id, COUNT(*) AS total_expected
    FROM variant_gt
    GROUP BY query_id
),
-- Get top-10 predictions for each query
predictions AS (
    SELECT
        q.query_id,
        f.material_id AS predicted_match,
        ROW_NUMBER() OVER (PARTITION BY q.query_id ORDER BY f.similarity DESC) AS rank
    FROM (SELECT DISTINCT query_id FROM variant_gt) q
    CROSS JOIN LATERAL find_similar_materials_jaccard(q.query_id, 10) f
),
-- Count hits at K=10
recall_calc AS (
    SELECT
        p.query_id,
        gc.total_expected,
        COUNT(DISTINCT CASE WHEN g.expected_match IS NOT NULL THEN g.expected_match END) AS found
    FROM predictions p
    JOIN gt_counts gc ON p.query_id = gc.query_id
    LEFT JOIN variant_gt g ON p.query_id = g.query_id AND p.predicted_match = g.expected_match
    WHERE p.rank <= 10
    GROUP BY p.query_id, gc.total_expected
)
SELECT
    ROUND(AVG(CAST(found AS DOUBLE) / total_expected), 4) AS recall_at_10
FROM recall_calc;
```

### Step 5: Calculate MRR (Mean Reciprocal Rank)

MRR answers: "On average, at what rank does the first correct answer appear?"

```sql
WITH
variant_gt AS (
    SELECT material_a AS query_id, material_b AS expected_match
    FROM ground_truth
    WHERE relationship_type = 'variant'
),
-- Get top-20 predictions with ranks
predictions AS (
    SELECT
        q.query_id,
        f.material_id AS predicted_match,
        ROW_NUMBER() OVER (PARTITION BY q.query_id ORDER BY f.similarity DESC) AS rank
    FROM (SELECT DISTINCT query_id FROM variant_gt) q
    CROSS JOIN LATERAL find_similar_materials_jaccard(q.query_id, 20) f
),
-- Find first rank where ground truth appears
first_hit AS (
    SELECT
        g.query_id,
        MIN(p.rank) AS first_correct_rank
    FROM variant_gt g
    LEFT JOIN predictions p ON g.query_id = p.query_id AND g.expected_match = p.predicted_match
    GROUP BY g.query_id
)
SELECT
    ROUND(AVG(1.0 / first_correct_rank), 4) AS mrr
FROM first_hit
WHERE first_correct_rank IS NOT NULL;
```

**Interpretation:**
- MRR = 1.0: Correct answer is always rank 1
- MRR = 0.5: Correct answer averages rank 2
- MRR = 0.33: Correct answer averages rank 3

## Understanding the Results

### Expected Accuracy Benchmarks

Based on the AdventureWorks test file (`adventureworks_accuracy.test`):

| Metric | Target | Description |
|--------|--------|-------------|
| Recall@10 (variants) | >= 40% | Find match in top 10 at least 40% of time |
| Recall@20 (variants) | >= 50% | Find match in top 20 at least 50% of time |
| MRR (variants) | >= 0.10 | Correct answer typically in top 10 |
| Similarity ordering | variant > partial > low | Higher similarity for closer relationships |

### Why Not 100% Accuracy?

Several factors limit accuracy:
1. **Component granularity** - Some variants differ only in non-BOM attributes (color, finish)
2. **Data quality** - Missing or incorrect BOM entries
3. **Algorithm limitations** - Jaccard treats all components equally (no weighting)

### Similarity Distribution Validation

Verify that similarity bands match expectations:

```sql
WITH components AS (
    SELECT parent_id AS material_id, LIST(DISTINCT child_id) AS comp_list
    FROM bom_items
    GROUP BY parent_id
),
computed_sim AS (
    SELECT
        g.relationship_type,
        g.expected_similarity,
        jaccard_similarity(a.comp_list, b.comp_list) AS computed
    FROM ground_truth g
    JOIN components a ON g.material_a = a.material_id
    JOIN components b ON g.material_b = b.material_id
)
SELECT
    relationship_type,
    ROUND(AVG(expected_similarity), 4) AS expected_avg,
    ROUND(AVG(computed), 4) AS computed_avg,
    ROUND(ABS(AVG(expected_similarity) - AVG(computed)), 4) AS difference
FROM computed_sim
GROUP BY relationship_type
ORDER BY expected_avg DESC;
```

The computed averages should closely match expected values (difference < 0.01).

## Variations

### Compare Jaccard vs WL Kernel Accuracy

```sql
WITH components AS (
    SELECT parent_id AS material_id, LIST(DISTINCT child_id) AS comp_list
    FROM bom_items
    GROUP BY parent_id
),
jaccard_results AS (
    SELECT
        'Jaccard' AS method,
        g.relationship_type,
        g.expected_similarity,
        jaccard_similarity(a.comp_list, b.comp_list) AS similarity
    FROM ground_truth g
    JOIN components a ON g.material_a = a.material_id
    JOIN components b ON g.material_b = b.material_id
),
wl_results AS (
    SELECT
        'WL Kernel' AS method,
        g.relationship_type,
        g.expected_similarity,
        wl_kernel_similarity(g.material_a, g.material_b, bom_table := 'bom_items') AS similarity
    FROM ground_truth g
),
methods AS (
    SELECT * FROM jaccard_results
    UNION ALL
    SELECT * FROM wl_results
)
SELECT
    method,
    relationship_type,
    ROUND(CORR(similarity, expected_similarity), 4) AS correlation,
    ROUND(AVG(ABS(similarity - expected_similarity)), 4) AS mae
FROM methods
GROUP BY method, relationship_type
ORDER BY method, relationship_type;
```

### Export Accuracy Report

```sql
WITH components AS (
    SELECT parent_id AS material_id, LIST(DISTINCT child_id) AS comp_list
    FROM bom_items
    GROUP BY parent_id
)
COPY (
    SELECT
        g.material_a,
        g.material_b,
        g.relationship_type,
        ROUND(g.expected_similarity, 4) AS expected,
        ROUND(jaccard_similarity(a.comp_list, b.comp_list), 4) AS jaccard,
        ROUND(wl_kernel_similarity(g.material_a, g.material_b, bom_table := 'bom_items'), 4) AS wl_kernel,
        g.notes
    FROM ground_truth g
    JOIN components a ON g.material_a = a.material_id
    JOIN components b ON g.material_b = b.material_id
    ORDER BY g.relationship_type, g.expected_similarity DESC
) TO 'accuracy_report.csv' (HEADER, DELIMITER ',');
```

## Next Steps

- **[Guide 01: Cold-Start Forecasting](01_cold_start_forecasting.md)** - Apply validated similarity to forecasting
- **[Guide 02: Product Families](02_product_families.md)** - Use accuracy insights for family detection
- **[API Reference](../../docs/API_REFERENCE.md)** - Full function documentation
