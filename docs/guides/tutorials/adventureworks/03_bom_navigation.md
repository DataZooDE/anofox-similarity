# Guide 03: Navigating Multi-Level BOMs

## The Problem

> "What components go into the Mountain-500 bicycle? If we change the wheel supplier, which products are affected?"

Understanding BOM structure is essential for:
- **Engineering changes** - Impact analysis before modifying components
- **Cost roll-up** - Calculating total product cost from components
- **Sourcing decisions** - Understanding supplier dependencies
- **Similarity analysis** - Knowing why products are similar

## The Solution

Use BOM traversal functions to explore product hierarchies and component relationships:
- **Explosion** - See all components in a product (forward traversal)
- **Where-used** - Find all products using a component (reverse traversal)
- **Common components** - Compare what two products share

## Prerequisites

```sql
LOAD anofox_similarity;

CREATE TABLE materials AS
SELECT * FROM read_csv_auto('test/data/adventureworks/materials.csv.gz');

CREATE TABLE bom_items AS
SELECT * FROM read_csv_auto('test/data/adventureworks/bom_items.csv.gz');
```

## Step-by-Step Walkthrough

### Step 1: Understand the BOM Structure

First, examine the BOM data format:

```sql
SELECT parent_id, child_id, quantity, level, position
FROM bom_items
WHERE parent_id = 'AW-749'
ORDER BY level, position
LIMIT 10;
```

**Expected Results:**

| parent_id | child_id | quantity | level | position |
|-----------|----------|----------|-------|----------|
| AW-749 | AW-519 | 2.0 | 1 | 1 |
| AW-749 | AW-717 | 1.0 | 1 | 2 |
| AW-749 | AW-807 | 1.0 | 1 | 3 |
| AW-749 | AW-813 | 1.0 | 1 | 4 |
| ... | ... | ... | ... | ... |

**Level interpretation:**
- Level 1 = Direct components of the bicycle
- Level 2 = Components of subassemblies
- Level 3+ = Deeper hierarchy

### Step 2: BOM Explosion (What's in this product?)

Get all components of a bicycle with their descriptions:

```sql
SELECT
    b.child_id,
    m.description AS component_name,
    m.material_type,
    b.quantity,
    b.level
FROM bom_items b
JOIN materials m ON b.child_id = m.material_id
WHERE b.parent_id = 'AW-749'
ORDER BY b.level, b.position;
```

**Expected Results:**

| child_id | component_name | material_type | quantity | level |
|----------|----------------|---------------|----------|-------|
| AW-519 | HL Road Seat Assembly | HALB | 2.0 | 1 |
| AW-717 | HL Road Frame - Red, 62 | FERT | 1.0 | 1 |
| AW-807 | HL Headset | FERT | 1.0 | 1 |
| AW-813 | HL Road Handlebars | FERT | 1.0 | 1 |
| AW-820 | HL Road Front Wheel | FERT | 1.0 | 1 |
| AW-828 | HL Road Rear Wheel | FERT | 2.0 | 1 |

### Step 3: Multi-Level Explosion (Recursive)

For products with subassemblies, expand the full hierarchy:

```sql
WITH RECURSIVE full_bom AS (
    -- Level 1: Direct components
    SELECT
        parent_id AS root_product,
        child_id,
        quantity,
        1 AS depth,
        parent_id || ' -> ' || child_id AS path
    FROM bom_items
    WHERE parent_id = 'AW-749' AND level = 1

    UNION ALL

    -- Deeper levels: Components of components
    SELECT
        fb.root_product,
        b.child_id,
        fb.quantity * b.quantity AS quantity,
        fb.depth + 1,
        fb.path || ' -> ' || b.child_id
    FROM full_bom fb
    JOIN bom_items b ON fb.child_id = b.parent_id
    WHERE fb.depth < 4  -- Limit recursion depth
)
SELECT
    fb.child_id,
    m.description,
    fb.quantity,
    fb.depth,
    fb.path
FROM full_bom fb
JOIN materials m ON fb.child_id = m.material_id
ORDER BY fb.depth, fb.child_id;
```

### Step 4: Where-Used (Which products use this component?)

Find all bicycles using a specific wheel:

```sql
SELECT DISTINCT
    b.parent_id,
    m.description AS product_name,
    m.material_type
FROM bom_items b
JOIN materials m ON b.parent_id = m.material_id
WHERE b.child_id = 'AW-820'  -- HL Road Front Wheel
ORDER BY b.parent_id;
```

This answers: "If the HL Road Front Wheel has a quality issue, which bicycles are affected?"

### Step 5: Common Components Analysis

Compare two products to understand their relationship:

```sql
WITH prod_a_components AS (
    SELECT child_id FROM bom_items WHERE parent_id = 'AW-749'
),
prod_b_components AS (
    SELECT child_id FROM bom_items WHERE parent_id = 'AW-750'
),
comparison AS (
    SELECT
        COALESCE(a.child_id, b.child_id) AS component,
        CASE
            WHEN a.child_id IS NOT NULL AND b.child_id IS NOT NULL THEN 'SHARED'
            WHEN a.child_id IS NOT NULL THEN 'ONLY_AW-749'
            ELSE 'ONLY_AW-750'
        END AS presence
    FROM prod_a_components a
    FULL OUTER JOIN prod_b_components b ON a.child_id = b.child_id
)
SELECT
    c.presence,
    COUNT(*) AS component_count,
    STRING_AGG(m.description, ', ' ORDER BY c.component) AS components
FROM comparison c
JOIN materials m ON c.component = m.material_id
GROUP BY c.presence;
```

**Expected Results:**

| presence | component_count | components |
|----------|----------------|------------|
| SHARED | 13 | HL Headset, HL Road Front Wheel, HL Road Handlebars... |
| ONLY_AW-749 | 2 | HL Road Frame - Red, 62, ... |
| ONLY_AW-750 | 2 | HL Road Frame - Red, 44, ... |

This confirms: AW-749 and AW-750 differ only in frame size.

## Understanding the Results

### Hierarchy Depth in AdventureWorks

```
Level 1: Bicycle → Frame, Wheels, Handlebars, Seat Assembly (direct)
Level 2: Seat Assembly → Seat Post, Saddle (subassembly components)
Level 3: Saddle → Foam, Cover, Rails (deeper breakdown)
Level 4: Foam → Raw Material (deepest level)
```

### Impact Analysis Matrix

| Question | Query Type | Use Case |
|----------|------------|----------|
| "What's in product X?" | BOM Explosion | Cost roll-up, engineering review |
| "Where is component Y used?" | Where-Used | Impact analysis, sourcing |
| "Why are X and Y similar?" | Common Components | Variant validation |

## Using the Full BOM Functions

For production systems using the universal BOM schema, anofox-similarity provides dedicated functions:

```sql
-- Requires bom_header and bom_component tables in universal schema
SELECT * FROM bom_explosion_multilevel('PRODUCT-001', max_depth := 5);
SELECT * FROM bom_where_used('COMPONENT-X');
SELECT * FROM bom_common_components('PRODUCT-A', 'PRODUCT-B');
```

See the [API Reference](../../docs/API_REFERENCE.md#bom-traversal) for full function signatures.

## Variations

### Count Components by Type

```sql
SELECT
    m.material_type,
    COUNT(DISTINCT b.child_id) AS unique_components,
    SUM(b.quantity) AS total_quantity
FROM bom_items b
JOIN materials m ON b.child_id = m.material_id
WHERE b.parent_id = 'AW-749'
GROUP BY m.material_type
ORDER BY unique_components DESC;
```

### Find Products with Most Components

```sql
SELECT
    b.parent_id,
    m.description,
    COUNT(DISTINCT b.child_id) AS component_count
FROM bom_items b
JOIN materials m ON b.parent_id = m.material_id
WHERE b.level = 1  -- Direct components only
GROUP BY b.parent_id, m.description
ORDER BY component_count DESC
LIMIT 10;
```

## Next Steps

- **[Guide 04: Quality Tiers](04_quality_tiers.md)** - Understand HL/ML/LL component differences
- **[Guide 02: Product Families](02_product_families.md)** - Use BOM insights to validate families
- **[API Reference](../../docs/API_REFERENCE.md)** - Full BOM function documentation
