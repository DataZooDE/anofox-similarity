# SAP BOM Data Export Guide

This directory contains private SAP BOM data for testing the `sap_to_*` transformation macros.

**All `.parquet` files in this directory are gitignored** - they contain proprietary data and should not be committed.

---

## Required Tables

Export the following SAP tables as parquet files:

### 1. MARA (Material Master) - **Required**

```sql
SELECT
    matnr,      -- Material number (18-char with leading zeros)
    mtart,      -- Material type (FERT, HALB, ROH, HAWA, etc.)
    matkl,      -- Material group
    ersda,      -- Creation date (YYYYMMDD format)
    lvorm       -- Deletion flag (check IS NULL for active)
FROM mara
WHERE mtart IN ('FERT', 'HALB', 'ROH', 'HAWA')
```

**Output:** `mara.parquet`

---

### 2. MAST (BOM Assignment) - **Required**

Links materials to their BOM headers.

```sql
SELECT
    matnr,      -- Material number (parent)
    werks,      -- Plant code
    stlan,      -- BOM usage: '2'=Production, 'E'=Engineering, 'V'=Variant
    stlnr,      -- BOM number (internal ID)
    stlal       -- Alternative BOM: '01'=primary, '02','03'=alternates
FROM mast
WHERE stlan IN ('2', 'E', 'V')
-- Export ALL alternatives (stlal) - filter at query time
```

**Output:** `mast.parquet`

---

### 3. STKO (BOM Header) - **Required**

BOM version control with validity dates.

```sql
SELECT
    stlty,      -- BOM category: 'M'=Material BOM
    stlnr,      -- BOM number
    stlal,      -- Alternative BOM
    datuv,      -- Valid-from date (DATE or YYYYMMDD string)
    lkenz       -- Lock indicator: NULL/''=active, else locked
FROM stko
WHERE stlty = 'M'
-- Export ALL alternatives and date versions
```

**Output:** `stko.parquet`

---

### 4. STPO (BOM Items) - **Required**

Component positions and quantities.

```sql
SELECT
    stlnr,      -- BOM number
    stlkn,      -- BOM item node number
    posnr,      -- Position number
    postp,      -- Position type
    idnrk,      -- Component material number
    rekrs,      -- Recursive/phantom flag: true=phantom assembly
    menge,      -- Quantity
    meins,      -- Unit of measure
    lkenz,      -- Deletion indicator
    datuv       -- Valid-from date (if applicable)
FROM stpo
WHERE stlty = 'M'
```

**Output:** `stpo.parquet`

---

### 5. MAKT (Material Descriptions) - **Optional but Recommended**

```sql
SELECT
    matnr,      -- Material number
    spras,      -- Language key: 'E'=English, 'D'=German
    maktx       -- Material description text
FROM makt
WHERE spras = 'E'  -- or your preferred language
```

**Output:** `makt.parquet`

---

### 6. STAS (BOM Item Selection) - **Optional but Recommended**

Controls which BOM items are active at a given date.

```sql
SELECT
    stlty,      -- BOM category
    stlnr,      -- BOM number
    stlal,      -- Alternative BOM
    stlkn,      -- BOM item node number
    stasz,      -- Selection parameter
    datuv,      -- Valid-from date
    lkenz       -- Lock indicator
FROM stas
WHERE stlty = 'M'
```

**Output:** `stas.parquet`

---

## Export Methods

### Option A: SAP GUI (SE16N)

1. Run transaction `SE16N`
2. Enter table name (e.g., `MARA`)
3. Set selection criteria as shown above
4. Execute and download as spreadsheet
5. Convert to parquet using Python (see below)

### Option B: ABAP Report

```abap
REPORT z_export_bom_data.

DATA: lt_mara TYPE TABLE OF mara.

SELECT matnr mtart matkl ersda lvorm
  FROM mara
  INTO TABLE lt_mara
  WHERE mtart IN ('FERT', 'HALB', 'ROH', 'HAWA').

" Export lt_mara to file...
" Repeat for other tables
```

### Option C: SAP Data Intelligence / CDS Views

Use SAP's modern data extraction tools if available.

---

## Converting to Parquet

After exporting to CSV:

```python
import pandas as pd

# Read CSV (adjust encoding/separator as needed)
df = pd.read_csv('mara_export.csv', sep=';', encoding='utf-8')

# Ensure correct dtypes
# matnr should be string (preserve leading zeros)
df['matnr'] = df['matnr'].astype(str).str.zfill(18)

# Save as parquet
df.to_parquet('mara.parquet', index=False)
```

For dates in YYYYMMDD format:
```python
# Keep as string - DuckDB will parse
df['ersda'] = df['ersda'].astype(str)
df.to_parquet('mara.parquet', index=False)
```

---

## File Checklist

After export, you should have:

- [ ] `mara.parquet` - Material master (required)
- [ ] `mast.parquet` - BOM assignment (required)
- [ ] `stko.parquet` - BOM header (required)
- [ ] `stpo.parquet` - BOM items (required)
- [ ] `makt.parquet` - Descriptions (optional)
- [ ] `stas.parquet` - Item selection (optional)

---

## Testing

Once files are in place, run:

```bash
cd /home/jr/Projects/datazoo/anofox-similarity
make test
```

Or test individual queries:

```bash
./build/release/duckdb -c "
SELECT COUNT(*) FROM sap_to_materials(
    mara := read_parquet('test/data/sap/mara.parquet')
);
"
```

---

## Versioning Notes

SAP BOMs have **two versioning dimensions**:

1. **Explicit versions** (`stlal`): Alternative BOMs (01=primary, 02/03=alternates)
2. **Date versions** (`datuv`): Temporal history within each alternative

The `sap_to_bom_items` macro uses `LEAD()` window function to calculate validity:
- `datuv` = valid-from date
- `date_to` = next version's `datuv` (calculated via LEAD)
- Filter: `datuv <= reference_date < date_to`

Export **all versions** - filtering happens at query time via parameters.
