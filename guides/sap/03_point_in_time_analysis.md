# Guide 03: Point-in-Time BOM Analysis

## The Problem

> "How has our BOM changed over the past 2 years? What components were added or removed?"

SAP maintains **date-versioned BOMs** through the `datuv` (valid-from) field. This allows you to:
- Query BOMs as they existed at any historical date
- Track component additions, removals, and quantity changes
- Understand the evolution of product structures over time

## The Solution

Use the `reference_date` parameter in `sap_to_bom_items()` to query BOMs at specific points in time, then compare the results.

## Prerequisites

```sql
LOAD anofox_similarity;

-- Load SAP tables
CREATE TABLE sap_mast AS SELECT * FROM read_parquet('test/data/sap/mast.parquet');
CREATE TABLE sap_stko AS SELECT * FROM read_parquet('test/data/sap/stko.parquet');
CREATE TABLE sap_stpo AS SELECT * FROM read_parquet('test/data/sap/stpo.parquet');
```

## Step-by-Step Walkthrough

### Step 1: Understand Date Versioning

Check the date versions available in your data:

```sql
-- See the range of valid-from dates
SELECT
    MIN(datuv) AS earliest_version,
    MAX(datuv) AS latest_version,
    COUNT(DISTINCT datuv) AS unique_dates
FROM sap_stko;

-- Distribution of BOM versions by year
SELECT
    EXTRACT(YEAR FROM CAST(datuv AS DATE)) AS year,
    COUNT(*) AS bom_versions
FROM sap_stko
WHERE datuv IS NOT NULL AND datuv != ''
GROUP BY year
ORDER BY year DESC;
```

### Step 2: Load Historical BOM Snapshot

Query the BOM as it existed on January 1, 2023:

```sql
CREATE TABLE bom_2023 AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    reference_date := '2023-01-01'::DATE
);

SELECT COUNT(*) AS items_2023 FROM bom_2023;
```

### Step 3: Load Current BOM Snapshot

Query the latest BOM version:

```sql
CREATE TABLE bom_current AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    reference_date := CURRENT_DATE
);

SELECT COUNT(*) AS items_current FROM bom_current;
```

### Step 4: Compare the Snapshots

Find what changed between the two dates:

```sql
WITH
bom_2023_set AS (
    SELECT parent_id, child_id, qty FROM bom_2023
),
bom_current_set AS (
    SELECT parent_id, child_id, qty FROM bom_current
),
changes AS (
    -- Components added (in current but not in 2023)
    SELECT 'ADDED' AS change_type, c.parent_id, c.child_id, NULL AS old_qty, c.qty AS new_qty
    FROM bom_current_set c
    LEFT JOIN bom_2023_set o ON c.parent_id = o.parent_id AND c.child_id = o.child_id
    WHERE o.parent_id IS NULL

    UNION ALL

    -- Components removed (in 2023 but not in current)
    SELECT 'REMOVED' AS change_type, o.parent_id, o.child_id, o.qty AS old_qty, NULL AS new_qty
    FROM bom_2023_set o
    LEFT JOIN bom_current_set c ON o.parent_id = c.parent_id AND o.child_id = c.child_id
    WHERE c.parent_id IS NULL

    UNION ALL

    -- Quantity changes (same component, different quantity)
    SELECT 'QTY_CHANGED' AS change_type, c.parent_id, c.child_id, o.qty AS old_qty, c.qty AS new_qty
    FROM bom_current_set c
    JOIN bom_2023_set o ON c.parent_id = o.parent_id AND c.child_id = o.child_id
    WHERE c.qty != o.qty
)
SELECT * FROM changes
ORDER BY change_type, parent_id
LIMIT 50;
```

### Step 5: Summarize Changes by Material

Get an overview of which materials changed the most:

```sql
WITH changes AS (
    SELECT c.parent_id, c.child_id,
           CASE
               WHEN o.child_id IS NULL THEN 'ADDED'
               WHEN c.child_id IS NULL THEN 'REMOVED'
               WHEN c.qty != o.qty THEN 'QTY_CHANGED'
               ELSE 'UNCHANGED'
           END AS change_type
    FROM bom_current c
    FULL OUTER JOIN bom_2023 o ON c.parent_id = o.parent_id AND c.child_id = o.child_id
)
SELECT
    parent_id,
    COUNT(CASE WHEN change_type = 'ADDED' THEN 1 END) AS components_added,
    COUNT(CASE WHEN change_type = 'REMOVED' THEN 1 END) AS components_removed,
    COUNT(CASE WHEN change_type = 'QTY_CHANGED' THEN 1 END) AS quantities_changed,
    COUNT(CASE WHEN change_type = 'UNCHANGED' THEN 1 END) AS unchanged
FROM changes
WHERE parent_id IS NOT NULL
GROUP BY parent_id
HAVING COUNT(CASE WHEN change_type != 'UNCHANGED' THEN 1 END) > 0
ORDER BY (components_added + components_removed + quantities_changed) DESC
LIMIT 20;
```

## Understanding the Results

### How Date Versioning Works in SAP

```
Material X BOM History:
├── 2020-01-01: Version 1 (Components A, B, C)
├── 2022-06-15: Version 2 (Components A, B, D) ← Component C replaced by D
├── 2024-01-01: Version 3 (Components A, B, D, E) ← Component E added
└── Current

Query with reference_date = '2023-01-01' → Returns Version 2
Query with reference_date = '2024-06-01' → Returns Version 3
```

### Change Type Interpretation

| Change Type | Meaning | Business Impact |
|-------------|---------|-----------------|
| **ADDED** | New component in current | Engineering change, new feature |
| **REMOVED** | Component discontinued | Simplification, obsolescence |
| **QTY_CHANGED** | Same component, different quantity | Process optimization, yield change |

## Variations

### Monthly Snapshots for Trend Analysis

```sql
-- Create snapshots for each quarter
CREATE TABLE bom_q1_2023 AS SELECT *, '2023-Q1' AS period FROM sap_to_bom_items(..., reference_date := '2023-03-31'::DATE);
CREATE TABLE bom_q2_2023 AS SELECT *, '2023-Q2' AS period FROM sap_to_bom_items(..., reference_date := '2023-06-30'::DATE);
CREATE TABLE bom_q3_2023 AS SELECT *, '2023-Q3' AS period FROM sap_to_bom_items(..., reference_date := '2023-09-30'::DATE);
CREATE TABLE bom_q4_2023 AS SELECT *, '2023-Q4' AS period FROM sap_to_bom_items(..., reference_date := '2023-12-31'::DATE);

-- Combine for trend analysis
CREATE TABLE bom_trend AS
SELECT * FROM bom_q1_2023
UNION ALL SELECT * FROM bom_q2_2023
UNION ALL SELECT * FROM bom_q3_2023
UNION ALL SELECT * FROM bom_q4_2023;
```

### Track Similarity Evolution

See how product similarity changed over time:

```sql
WITH
comp_2023 AS (
    SELECT parent_id, LIST(DISTINCT child_id) AS components
    FROM bom_2023 GROUP BY parent_id
),
comp_current AS (
    SELECT parent_id, LIST(DISTINCT child_id) AS components
    FROM bom_current GROUP BY parent_id
)
SELECT
    c.parent_id,
    LEN(o.components) AS components_2023,
    LEN(c.components) AS components_current,
    ROUND(jaccard_similarity(o.components, c.components), 4) AS self_similarity_over_time
FROM comp_current c
JOIN comp_2023 o ON c.parent_id = o.parent_id
WHERE jaccard_similarity(o.components, c.components) < 0.9  -- Changed significantly
ORDER BY self_similarity_over_time ASC
LIMIT 20;
```

### Find Recently Changed Products

```sql
-- Products with BOM changes in last 6 months
SELECT DISTINCT m.matnr AS material_id
FROM sap_mast m
JOIN sap_stko h ON m.stlnr = h.stlnr
WHERE CAST(h.datuv AS DATE) > CURRENT_DATE - INTERVAL '6 months'
ORDER BY material_id
LIMIT 100;
```

## Use Cases

### Engineering Change Impact

When a component is redesigned:
1. Load BOM before the change date
2. Load BOM after the change date
3. Identify all materials affected by the change
4. Assess the scope of downstream impacts

### Audit Trail

For regulatory compliance:
1. Query BOM at specific audit dates
2. Compare against current structure
3. Document all changes with timestamps
4. Maintain traceable history

## Next Steps

- **[Guide 04: Similarity at Scale](04_similarity_at_scale.md)** - Efficient search across 1M+ materials
- **[Guide 05: Multi-Language Descriptions](05_multilanguage_descriptions.md)** - Work with localized text
- **[API Reference](../../docs/API_REFERENCE.md)** - Full function documentation
