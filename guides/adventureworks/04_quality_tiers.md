# Guide 04: Quality Tier Analysis (Component Substitution)

## The Problem

> "We're cost-cutting. What if we substitute ML (mid-range) components for HL (high-end) components in our premium bikes? Which products would be affected, and how would similarity change?"

Understanding quality tier relationships helps with:
- **Cost optimization** - Identify substitution opportunities
- **Product line rationalization** - Consolidate similar tier products
- **Impact analysis** - Understand effects of component changes
- **Pricing validation** - Ensure tier-appropriate pricing

## The Solution

AdventureWorks uses a consistent naming convention:
- **HL** = High-end (premium quality)
- **ML** = Mid-range (standard quality)
- **LL** = Low-end (budget quality)

By analyzing components across tiers, we can understand substitution possibilities and their impact on product similarity.

## Prerequisites

```sql
LOAD anofox_similarity;

CREATE TABLE materials AS
SELECT * FROM read_csv_auto('test/data/adventureworks/materials.csv.gz');

CREATE TABLE bom_items AS
SELECT * FROM read_csv_auto('test/data/adventureworks/bom_items.csv.gz');
```

## Step-by-Step Walkthrough

### Step 1: Identify Quality Tier Components

Find all components with quality tier prefixes:

```sql
SELECT
    CASE
        WHEN description LIKE 'HL %' THEN 'HL (High-end)'
        WHEN description LIKE 'ML %' THEN 'ML (Mid-range)'
        WHEN description LIKE 'LL %' THEN 'LL (Low-end)'
        ELSE 'No Tier'
    END AS quality_tier,
    COUNT(*) AS component_count,
    material_type
FROM materials
WHERE description LIKE 'HL %' OR description LIKE 'ML %' OR description LIKE 'LL %'
GROUP BY quality_tier, material_type
ORDER BY quality_tier, material_type;
```

**Expected Results:**

| quality_tier | component_count | material_type |
|--------------|-----------------|---------------|
| HL (High-end) | 45 | FERT |
| HL (High-end) | 12 | HALB |
| HL (High-end) | 8 | ROH |
| ML (Mid-range) | 30 | FERT |
| ... | ... | ... |

### Step 2: Map Component Tier Equivalents

Identify which HL components have ML/LL equivalents:

```sql
WITH tier_components AS (
    SELECT
        material_id,
        description,
        -- Extract base component name (remove tier prefix)
        TRIM(REGEXP_REPLACE(description, '^(HL|ML|LL)\s+', '')) AS base_name,
        CASE
            WHEN description LIKE 'HL %' THEN 'HL'
            WHEN description LIKE 'ML %' THEN 'ML'
            WHEN description LIKE 'LL %' THEN 'LL'
        END AS tier
    FROM materials
    WHERE description LIKE 'HL %' OR description LIKE 'ML %' OR description LIKE 'LL %'
)
SELECT
    base_name,
    MAX(CASE WHEN tier = 'HL' THEN material_id END) AS hl_id,
    MAX(CASE WHEN tier = 'ML' THEN material_id END) AS ml_id,
    MAX(CASE WHEN tier = 'LL' THEN material_id END) AS ll_id,
    COUNT(DISTINCT tier) AS available_tiers
FROM tier_components
GROUP BY base_name
HAVING COUNT(DISTINCT tier) > 1
ORDER BY base_name
LIMIT 10;
```

**Expected Results:**

| base_name | hl_id | ml_id | ll_id | available_tiers |
|-----------|-------|-------|-------|-----------------|
| Hub | AW-401 | NULL | AW-400 | 2 |
| Mountain Seat Assembly | AW-516 | AW-515 | AW-514 | 3 |
| Mountain Seat/Saddle | AW-910 | AW-909 | AW-908 | 3 |
| Road Seat Assembly | AW-519 | AW-518 | AW-517 | 3 |
| ... | ... | ... | ... | ... |

### Step 3: Find Products Using High-End Components

Identify premium bikes (using HL components):

```sql
SELECT DISTINCT
    b.parent_id,
    m_parent.description AS bicycle_name,
    COUNT(DISTINCT b.child_id) AS hl_component_count
FROM bom_items b
JOIN materials m_child ON b.child_id = m_child.material_id
JOIN materials m_parent ON b.parent_id = m_parent.material_id
WHERE m_child.description LIKE 'HL %'
  AND m_parent.material_type = 'FERT'
GROUP BY b.parent_id, m_parent.description
HAVING COUNT(DISTINCT b.child_id) >= 3
ORDER BY hl_component_count DESC
LIMIT 10;
```

### Step 4: Simulate Component Substitution Impact

Compare similarity before and after substituting HL for ML seat assembly:

```sql
-- Current similarity between two HL bikes
WITH components AS (
    SELECT parent_id AS material_id, LIST(DISTINCT child_id) AS comp_list
    FROM bom_items
    GROUP BY parent_id
)
SELECT
    'Before substitution' AS scenario,
    ROUND(jaccard_similarity(a.comp_list, b.comp_list), 4) AS similarity
FROM components a, components b
WHERE a.material_id = 'AW-749' AND b.material_id = 'AW-750';
```

Now imagine if we replaced HL Road Seat Assembly (AW-519) with ML Road Seat Assembly (AW-518) in one bike:

```sql
-- Create a modified BOM with substitution
WITH modified_components AS (
    SELECT
        parent_id AS material_id,
        LIST(DISTINCT CASE
            WHEN child_id = 'AW-519' AND parent_id = 'AW-749' THEN 'AW-518'
            ELSE child_id
        END) AS comp_list
    FROM bom_items
    GROUP BY parent_id
)
SELECT
    'After HL→ML seat substitution' AS scenario,
    ROUND(jaccard_similarity(a.comp_list, b.comp_list), 4) AS similarity
FROM modified_components a, modified_components b
WHERE a.material_id = 'AW-749' AND b.material_id = 'AW-750';
```

The similarity would decrease because AW-749 now has a different seat assembly.

### Step 5: Analyze Cross-Tier Similarity

Compare products across quality tiers:

```sql
WITH hl_products AS (
    SELECT DISTINCT b.parent_id AS material_id
    FROM bom_items b
    JOIN materials m ON b.child_id = m.material_id
    WHERE m.description LIKE 'HL %'
),
ml_products AS (
    SELECT DISTINCT b.parent_id AS material_id
    FROM bom_items b
    JOIN materials m ON b.child_id = m.material_id
    WHERE m.description LIKE 'ML %'
)
SELECT
    h.material_id AS hl_product,
    m_hl.description AS hl_name,
    (
        SELECT TOP 1 f.material_id
        FROM find_similar_materials_jaccard(h.material_id, 10, bom_table := 'bom_items') f
        WHERE f.material_id IN (SELECT material_id FROM ml_products)
    ) AS most_similar_ml,
    (
        SELECT TOP 1 ROUND(f.similarity, 4)
        FROM find_similar_materials_jaccard(h.material_id, 10, bom_table := 'bom_items') f
        WHERE f.material_id IN (SELECT material_id FROM ml_products)
    ) AS similarity_to_ml
FROM hl_products h
JOIN materials m_hl ON h.material_id = m_hl.material_id
WHERE m_hl.material_type = 'FERT'
LIMIT 10;
```

## Understanding the Results

### Tier Substitution Impact on Similarity

| Substitution | Typical Similarity Change | Business Impact |
|--------------|---------------------------|-----------------|
| Same tier (size variant) | 0.85-0.95 | Minimal |
| Adjacent tier (HL→ML) | 0.70-0.85 | Moderate |
| Skip tier (HL→LL) | 0.50-0.70 | Significant |
| Different category | < 0.50 | Major |

### Quality Tier Component Categories

AdventureWorks has tiered versions of:
- **Frames** - HL/ML/LL Road/Mountain/Touring frames
- **Wheels** - HL/ML/LL Front and Rear wheels
- **Seat Assemblies** - HL/ML/LL for each bike type
- **Handlebars** - HL/ML/LL variants
- **Hubs, Spindles, Shells** - Raw material quality tiers

## Variations

### Cost Impact Analysis

If materials had cost data, analyze tier substitution savings:

```sql
-- Hypothetical cost analysis (requires cost data in materials table)
WITH tier_costs AS (
    SELECT
        TRIM(REGEXP_REPLACE(description, '^(HL|ML|LL)\s+', '')) AS base_component,
        CASE
            WHEN description LIKE 'HL %' THEN 'HL'
            WHEN description LIKE 'ML %' THEN 'ML'
            WHEN description LIKE 'LL %' THEN 'LL'
        END AS tier,
        material_id
        -- cost_per_unit  -- would need this column
    FROM materials
    WHERE description LIKE 'HL %' OR description LIKE 'ML %' OR description LIKE 'LL %'
)
SELECT base_component, tier, material_id
FROM tier_costs
WHERE base_component = 'Road Seat Assembly'
ORDER BY tier;
```

### Structural vs Component Similarity

Use WL kernel to detect when products have same components but different assembly structure:

```sql
SELECT
    'Jaccard (component overlap)' AS method,
    ROUND(jaccard_similarity('AW-749', 'AW-750', 'bom_items'), 4) AS similarity
UNION ALL
SELECT
    'WL Kernel (structural)' AS method,
    ROUND(wl_kernel_similarity('AW-749', 'AW-750', bom_table := 'bom_items'), 4) AS similarity;
```

If Jaccard is high but WL Kernel is lower, products share components but assemble them differently.

## Next Steps

- **[Guide 05: Accuracy Validation](05_accuracy_validation.md)** - Validate tier detection accuracy
- **[Guide 02: Product Families](02_product_families.md)** - See how tiers affect family clustering
- **[API Reference](../../docs/API_REFERENCE.md)** - Full similarity function documentation
