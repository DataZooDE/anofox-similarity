# Guide 01: Cold-Start Forecasting for New Bicycle Models

## The Problem

> "Marketing wants to launch a new Road-200 bicycle next quarter. We have no sales history for it. How do we forecast demand?"

This is the **cold-start problem** in demand planning: new products have no consumption history, making traditional time-series forecasting impossible.

## The Solution

Use **BOM similarity** to find analogous products that share components with the new product. These analogs have established consumption patterns you can use for forecasting.

**Why it works:** Products with similar components typically have similar demand patterns. A new road bike shares 90% of its components with existing road bikes, so their sales history is predictive.

## Prerequisites

```sql
LOAD anofox_similarity;

CREATE TABLE materials AS
SELECT * FROM read_csv_auto('test/data/adventureworks/materials.csv.gz');

CREATE TABLE bom_items AS
SELECT * FROM read_csv_auto('test/data/adventureworks/bom_items.csv.gz');
```

## Step-by-Step Walkthrough

### Step 1: Identify the New Product

Let's say we're launching a new variant of the Road-150 series. We'll use AW-749 (Road-150 Red, 62) as our reference product.

```sql
SELECT material_id, description, material_type
FROM materials
WHERE material_id = 'AW-749';
```

| material_id | description | material_type |
|-------------|-------------|---------------|
| AW-749 | Road-150 Red, 62 | FERT |

### Step 2: Find Similar Products

Use `find_similar_materials_jaccard()` to discover products that share components:

```sql
SELECT
    material_id,
    ROUND(similarity, 4) AS jaccard_similarity,
    shared_components,
    total_components
FROM find_similar_materials_jaccard(
    'AW-749',
    10,
    bom_table := 'bom_items'
)
ORDER BY similarity DESC;
```

**Expected Results:**

| material_id | jaccard_similarity | shared_components | total_components |
|-------------|-------------------|-------------------|------------------|
| AW-750 | 0.8667 | 13 | 15 |
| AW-751 | 0.8667 | 13 | 15 |
| AW-752 | 0.8667 | 13 | 15 |
| AW-753 | 0.8667 | 13 | 15 |
| AW-758 | 0.8000 | 12 | 15 |
| ... | ... | ... | ... |

**Interpretation:**
- AW-750 through AW-753 are size variants of the same bike (sharing 13/15 components)
- The only differing components are the frame (different sizes) and possibly the seat post
- These products are excellent analogs for forecasting

### Step 3: Understand What They Share

Examine the shared components between your target and its best analog:

```sql
SELECT * FROM bom_common_components('AW-749', 'AW-750');
```

This shows you exactly which components are shared, helping you understand why they're similar and validate the analog choice.

### Step 4: Identify the Differences

```sql
-- Components in AW-749 but not in AW-750
SELECT b1.child_id, m.description AS component_name
FROM bom_items b1
JOIN materials m ON b1.child_id = m.material_id
WHERE b1.parent_id = 'AW-749'
  AND b1.child_id NOT IN (
    SELECT child_id FROM bom_items WHERE parent_id = 'AW-750'
  );
```

This reveals that the only difference is the frame size (62 vs 44 cm), confirming these are true product variants.

## Understanding the Results

### Jaccard Similarity Interpretation

| Similarity | Relationship | Forecasting Confidence |
|------------|--------------|----------------------|
| 0.90 - 1.00 | Size/color variant | Very High |
| 0.70 - 0.89 | Same product family | High |
| 0.50 - 0.69 | Related category | Medium |
| < 0.50 | Different product | Low |

### Choosing the Right Analog

For cold-start forecasting, prefer analogs that:
1. **High similarity** (>0.8 Jaccard) - ensures similar demand drivers
2. **Longer sales history** - more data = better forecasts
3. **Same market segment** - HL bikes forecast HL bikes, not LL bikes

## Production Usage with Consumption History

In production systems with goods movement data, use `cold_start_analogs()` which combines similarity with history filtering:

```sql
-- Production query (requires goods_movements table)
SELECT
    material_id,
    similarity,
    history_months,
    first_usage,
    last_usage
FROM cold_start_analogs(
    'NEW-PRODUCT',
    5,
    min_history_months := 12,
    bom_table := 'bom_items',
    movements_table := 'goods_movements'
);
```

This automatically filters to products with at least 12 months of consumption history, returning only analogs suitable for forecasting.

## Variations

### Stricter Similarity Threshold

For high-confidence forecasts, require higher similarity:

```sql
SELECT material_id, similarity, shared_components
FROM find_similar_materials_jaccard('AW-749', 100, min_similarity := 0.8)
ORDER BY similarity DESC;
```

### Broader Search

For products with few similar materials, lower the threshold:

```sql
SELECT material_id, similarity, shared_components
FROM find_similar_materials_jaccard('AW-749', 20, min_similarity := 0.5)
ORDER BY similarity DESC;
```

## Next Steps

- **[Guide 02: Product Families](02_product_families.md)** - Systematically identify all variant clusters
- **[Guide 03: BOM Navigation](03_bom_navigation.md)** - Explore the full component hierarchy
- **[API Reference](../../docs/API_REFERENCE.md)** - Complete function documentation
