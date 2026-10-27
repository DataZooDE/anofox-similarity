# Guide 01: SAP Data Transformation (ETL)

## The Problem

> "I have SAP tables (MARA, MAKT, MAST, STKO, STPO). How do I transform them for similarity analysis?"

SAP uses a specific table structure that differs from the universal BOM schema expected by anofox-similarity. This guide shows how to transform SAP data into the format needed for similarity analysis.

## The Solution

Use the **SAP transformation macros** to convert SAP tables to the universal schema:
- `sap_to_materials_with_desc()` - Extract material master with descriptions
- `sap_to_bom_items()` - Convert BOM structure to parent-child format

## Prerequisites

```sql
LOAD anofox_similarity;

-- Load SAP tables from parquet files
CREATE TABLE sap_mara AS SELECT * FROM read_parquet('test/data/sap/mara.parquet');
CREATE TABLE sap_makt AS SELECT * FROM read_parquet('test/data/sap/makt.parquet');
CREATE TABLE sap_mast AS SELECT * FROM read_parquet('test/data/sap/mast.parquet');
CREATE TABLE sap_stko AS SELECT * FROM read_parquet('test/data/sap/stko.parquet');
CREATE TABLE sap_stpo AS SELECT * FROM read_parquet('test/data/sap/stpo.parquet');
```

## Step-by-Step Walkthrough

### Step 1: Transform Materials with Descriptions

Convert SAP MARA (material master) and MAKT (descriptions) to the universal materials format:

```sql
CREATE TABLE materials AS
SELECT * FROM sap_to_materials_with_desc(
    mara_table := 'sap_mara',
    makt_table := 'sap_makt',
    language := 'E'  -- English descriptions
);

-- Verify transformation
SELECT COUNT(*) AS total_materials FROM materials;

-- Sample output
SELECT material_id, description, material_type, material_group, created_date
FROM materials
LIMIT 5;
```

**Output Columns:**
| Column | Source | Description |
|--------|--------|-------------|
| `material_id` | MARA.matnr | Material number (zero-padded) |
| `description` | MAKT.maktx | Material description in specified language |
| `material_type` | MARA.mtart | Material type (FERT, HALB, ROH, etc.) |
| `material_group` | MARA.matkl | Material classification group |
| `created_date` | MARA.ersda | Creation date (converted from YYYYMMDD) |

### Step 2: Transform BOM Structure

Convert SAP BOM tables (MAST, STKO, STPO) to the universal bom_items format:

```sql
CREATE TABLE bom_items AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    bom_alternative := '01',        -- Primary BOM alternative
    reference_date := CURRENT_DATE  -- Latest version
);

-- Verify transformation
SELECT COUNT(*) AS total_bom_items FROM bom_items;

-- Sample output
SELECT bom_id, parent_id, child_id, qty, unit
FROM bom_items
LIMIT 10;
```

**Output Columns:**
| Column | Source | Description |
|--------|--------|-------------|
| `bom_id` | STKO.stlnr | BOM number |
| `parent_id` | MAST.matnr | Parent material |
| `child_id` | STPO.idnrk | Component material |
| `qty` | STPO.menge | Quantity required |
| `unit` | STPO.meins | Unit of measure |

### Step 3: Verify the Transformation

Check that the data is usable for similarity analysis:

```sql
-- Materials by type
SELECT material_type, COUNT(*) AS count
FROM materials
GROUP BY material_type
ORDER BY count DESC
LIMIT 10;

-- BOMs by component count
SELECT parent_id, COUNT(DISTINCT child_id) AS components
FROM bom_items
GROUP BY parent_id
ORDER BY components DESC
LIMIT 10;
```

### Step 4: Run Similarity Analysis

Now you can use the standard similarity functions:

```sql
-- Find top 5 similar materials
SELECT material_id, similarity, shared_components, total_components
FROM find_similar_materials_jaccard(
    '000000000010561200',  -- SAP material number
    5,
    bom_table := 'bom_items'
);
```

## Understanding the Transformation

### SAP Table Relationships

```
MARA (Materials)      MAKT (Descriptions)
     │                     │
     └──── matnr ──────────┘
           │
           ▼
     MAST (BOM Assignment)
           │
           ├── stlnr ──► STKO (BOM Header)
           │                  │
           │                  ├── stlal (Alternative: 01, 02, 03)
           │                  └── datuv (Valid-from date)
           │                       │
           │                       ▼
           └──────────────► STPO (BOM Items)
                                 │
                                 └── idnrk (Component material)
```

### Key Parameters

| Parameter | Values | Description |
|-----------|--------|-------------|
| `language` | 'E', 'D', 'F', etc. | Language key for descriptions |
| `bom_alternative` | '01', '02', '03' | BOM alternative version |
| `reference_date` | DATE | Point-in-time for date versioning |

### What Gets Filtered

The transformation macros automatically:
- **Exclude deleted materials** (`lvorm = 'X'`)
- **Select correct date version** (latest valid-from before reference_date)
- **Filter by BOM alternative** (01 = primary, 02/03 = alternates)

## Variations

### German Descriptions

```sql
CREATE TABLE materials_de AS
SELECT * FROM sap_to_materials_with_desc(
    mara_table := 'sap_mara',
    makt_table := 'sap_makt',
    language := 'D'  -- German
);
```

### Specific BOM Alternative

```sql
-- Load alternate manufacturing BOM
CREATE TABLE bom_alt_02 AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    bom_alternative := '02'  -- Secondary alternative
);
```

### Historical BOM Snapshot

```sql
-- BOM as of January 2023
CREATE TABLE bom_2023 AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo',
    reference_date := '2023-01-01'::DATE
);
```

## Performance Tips

For large SAP datasets (1M+ materials):

```sql
-- Create a focused subset for faster queries
CREATE TABLE bom_items_sample AS
SELECT * FROM sap_to_bom_items(
    mast_table := 'sap_mast',
    stko_table := 'sap_stko',
    stpo_table := 'sap_stpo'
)
WHERE parent_id IN (
    SELECT DISTINCT matnr FROM sap_mara
    WHERE mtart = 'FERT'  -- Only finished goods
    LIMIT 10000
);
```

## Next Steps

- **[Guide 02: Alternative BOMs](02_alternative_boms.md)** - Compare different manufacturing approaches
- **[Guide 03: Point-in-Time Analysis](03_point_in_time_analysis.md)** - Track BOM changes over time
- **[API Reference](../../docs/API_REFERENCE.md)** - Full SAP transformation documentation
