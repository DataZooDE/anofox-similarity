# ERP transformations

> Turn SAP and Dynamics 365 extracts into the extension's input tables.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## ERP Transformations

### SAP Transformations

Convert SAP-format BOM to universal schema.

**sap_to_bom_items** - Convert MAST/STKO/STPO (and optionally STAS) to flat BOM edges:
```sql
sap_to_bom_items(
    mast_table VARCHAR,
    stko_table VARCHAR,
    stpo_table VARCHAR,
    bom_alternative := '01',
    reference_date := '9999-12-31',
    bom_usage := NULL,
    stas_table := NULL
) → TABLE (
    bom_id VARCHAR,
    parent_id VARCHAR,
    child_id VARCHAR,
    qty,              -- type of STPO.menge (DECIMAL in SAP)
    unit VARCHAR,
    valid_from DATE,
    valid_to DATE,
    bom_version VARCHAR
)
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mast_table` | VARCHAR | Required | MAST table (material-BOM link) |
| `stko_table` | VARCHAR | Required | STKO table (BOM header) |
| `stpo_table` | VARCHAR | Required | STPO table (BOM items) |
| `bom_alternative` | VARCHAR | '01' | BOM alternative (`stlal`) to extract |
| `reference_date` | VARCHAR or DATE | '9999-12-31' | Header, item and selection versions valid on this date (`YYYYMMDD` or `YYYY-MM-DD`; anything else is an error) |
| `bom_usage` | VARCHAR | NULL | Keep only MAST rows with this `stlan` (NULL: every usage) |
| `stas_table` | VARCHAR | NULL | STAS table: apply the item selection of the alternative |

**Input columns.** The required columns must exist (a missing one is a Binder Error naming it).
Every optional column is used when the table has it, so a minimal extract works and a full SAP
extract gets the SAP rules:

| Table | Required | Optional, used when present |
|-------|----------|-----------------------------|
| MAST | `matnr`, `stlnr` | `stlal` (must equal `bom_alternative`), `stlan` (required with `bom_usage`) |
| STKO | `stlnr`, `stlal` | `stlty` (only `'M'`, material BOMs), `datuv` + `stkoz` (header version), `lkenz`, `loekz` (deleted header) |
| STPO | `stlnr`, `idnrk`, `menge`, `meins` | `stlty` (only `'M'`), `stlkn` + `datuv` + `stpoz` (item version), `lkenz` (deleted item) |
| STAS | `stlnr`, `stlal`, `stlkn` (with `stas_table`) | `stlty`, `datuv` + `stasz` (selection version), `lkenz` (item removed from the alternative) |

**Rules:**

- **Category.** SAP numbers BOMs per category, so a document BOM (`stlty = 'D'`) can share
  `stlnr` with a material BOM. With `stlty`, only material BOMs are read.
- **Alternative.** The material's MAST row and the STKO header must both be of `bom_alternative`.
- **Versions.** Of the header versions with `datuv <= reference_date`, the newest (then the highest
  `stkoz`) is used; it gives `valid_from`, and the BOM is dropped when that version has `lkenz` or
  `loekz` set. Items are versioned the same way per item node `stlkn` (`stpoz` breaks ties): an
  item whose valid version is deleted (`lkenz`) or that starts after `reference_date` is left out.
- **Item selection (STAS).** With `stas_table`, an item is kept only when the alternative selects it:
  its newest STAS row for the alternative on `reference_date` exists and has no `lkenz`. Without
  STAS, every item of the BOM number is returned, whichever alternative it belongs to.
- **Plants** collapse: a material with the same BOM in several plants gives one set of rows.
- `qty` passes through the source `MENGE` column's type (SAP `MENGE` is DECIMAL); cast if you need DOUBLE.

> [!NOTE]
> On a real SAP extract the rules matter: on the test extract in `test/data/sap`, joining by BOM
> number alone returned document and class BOM items and items that STAS had removed. Pass
> `stas_table` whenever you have STAS.

**sap_to_materials** - Extract materials from MARA. Requires MARA columns `matnr`, `mtart`, `matkl`,
`ersda`, **and `lvorm`** (deletion flag — rows flagged deleted are excluded). `ersda` may be SAP
`YYYYMMDD` text (or integer), a `DATE`, or an ISO `YYYY-MM-DD` string. Its `description`
column is always empty; use **`sap_to_materials_with_desc`** (below) to populate descriptions from MAKT:
```sql
sap_to_materials(
    mara_table VARCHAR
) → TABLE (material_id, material_type, material_group, description, created_date)
```

**sap_to_materials_with_desc** - Materials with descriptions (MARA + MAKT):
```sql
sap_to_materials_with_desc(
    mara_table VARCHAR,
    makt_table VARCHAR,
    language := 'E'
) → TABLE (material_id, material_type, material_group, description, created_date)
```

**extract_material_descriptions** - Extract descriptions from MAKT. Requires MAKT columns `matnr`,
`spras`, `maktx`, **and `maktg`**; multiple rows per `(matnr, spras)` collapse to one:
```sql
extract_material_descriptions(
    makt_table := 'sap_makt',
    language := 'EN'
) → TABLE (material_id VARCHAR, full_text VARCHAR)
```

**Example:**
```sql
-- Convert SAP BOM to universal format
SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', stas_table := 'STAS', bom_alternative := '01');

-- Extract materials with descriptions
SELECT * FROM sap_to_materials_with_desc('MARA', 'MAKT', language := 'E');
```

### Dynamics 365 Transformations

Convert Microsoft Dynamics 365 BOM format.

These are table macros that RETURN **universal-schema** rows (materials / bom_header / bom_component
shapes) — they do not write an output table, and the component macro does **not** return flat
`bom_items`. Expected input column names are exactly as listed (they mirror D365 field names).

```sql
-- InventTable → universal materials. Reads columns: ItemId, ItemName, ItemType (0/1/2).
dynamics365_to_materials(invent_table := 'InventTable')
    → TABLE (material_id, material_number, description, material_type, material_group,
             procurement_type, base_uom, weight, cost_per_unit, source_system, is_active, created_at)

-- BOMTable + BOMVersion → universal bom_header. Reads BOMTable.(BOMId, ItemId, Status) and
-- BOMVersion.(BOMId, VersionId, ActivationDate, ApprovedStatus); keeps rows with Status = 0.
-- One header per BOM: with several BOMVersion rows it picks ApprovedStatus = 1 first, then the
-- latest ActivationDate.
dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion')
    → TABLE (bom_id, source_system, source_bom_id, parent_material_id, bom_type, alternative_number,
             revision, base_quantity, base_uom, valid_from, valid_to, plant_id, is_approved, created_at)

-- BOM lines → universal bom_component (singular). Reads columns: BOMId, LineNum, ItemId,
-- Quantity, ScrapPercent.
dynamics365_to_bom_component(bom_lines := 'BOM')
    → TABLE (component_id, bom_id, line_number, child_material_id, quantity_per, quantity_uom,
             is_fixed_quantity, scrap_percent, effective_from, effective_to, component_type,
             supply_type, operation_sequence, is_alternative, alternative_group, created_at)
```

**Example — D365 to a `bom_items` table usable by the similarity functions:**
```sql
CREATE TABLE materials     AS SELECT * FROM dynamics365_to_materials(invent_table := 'InventTable');
CREATE TABLE bom_header    AS SELECT * FROM dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion');
CREATE TABLE bom_component AS SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM');
-- Flatten header + component into the parent_id/child_id/quantity shape the algorithms expect:
CREATE TABLE bom_items     AS SELECT * FROM bom_to_items('bom_header', 'bom_component');
```

---

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `sap_to_bom_items`

*Table function*

Converts SAP MAST/STKO/STPO (and optionally STAS) into flat BOM edges (bom_id, parent_id, child_id, qty, unit, valid_from, valid_to, bom_version). Needs MAST matnr, stlnr; STKO stlnr, stlal; STPO stlnr, idnrk, menge, meins. Each further SAP column is used when the table has it: stlty keeps material BOMs (M) only; MAST.stlal must equal bom_alternative (default '01', also matched on STKO.stlal); datuv picks the STKO header and, per STPO item node stlkn, the item version valid on reference_date (YYYYMMDD or a date, default 9999-12-31), stkoz / stpoz breaking ties; a header version with lkenz or loekz set and an item version with lkenz set are dropped. bom_usage filters MAST.stlan. stas_table := 'STAS' applies the item selection: an item is kept only when its newest STAS row for the alternative on reference_date exists and is not deleted (lkenz). Plants collapse to one row per material and BOM. qty keeps the MENGE type.

```sql
sap_to_bom_items(mast_table, stko_table, stpo_table, bom_alternative, reference_date, bom_usage, stas_table)
```

**Examples**

```sql
SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO');
SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', stas_table := 'STAS', reference_date := '2024-01-01', bom_usage := '1');
```

### `sap_to_materials`

*Table macro*

Extracts materials from a SAP MARA table into the universal materials shape. Requires MARA columns matnr, mtart, matkl, ersda and lvorm; rows flagged deleted via lvorm are excluded. The description column is always empty here -- use sap_to_materials_with_desc to populate it from MAKT; the makt_table and language parameters are accepted for compatibility and ignored.

```sql
sap_to_materials(mara_table, makt_table, language)
```

**Examples**

```sql
SELECT * FROM sap_to_materials('MARA');
```

### `sap_to_materials_with_desc`

*Table macro*

Like sap_to_materials, but joins MAKT to populate the description column in the requested 'language' (SAP single-character language key, e.g. 'E').

```sql
sap_to_materials_with_desc(mara_table, makt_table, language)
```

**Examples**

```sql
SELECT * FROM sap_to_materials_with_desc('MARA', 'MAKT', language := 'E');
```

### `extract_material_descriptions`

*Table macro*

Extracts material descriptions from a SAP MAKT table for one language, returning (material_id, full_text): the description (MAKTX) and the search text (MAKTG) joined by a space, one row per material.

```sql
extract_material_descriptions(makt_table, language)
```

**Examples**

```sql
SELECT * FROM extract_material_descriptions(makt_table := 'sap_makt', language := 'EN');
```

### `dynamics365_to_bom_header`

*Table macro*

Maps Dynamics 365 BOMTable joined to BOMVersion into the universal bom_header shape, keeping rows with Status = 0. Reads BOMTable.(BOMId, ItemId, Status) and BOMVersion.(BOMId, VersionId, ActivationDate, ApprovedStatus); with several versions per BOM it takes one: approved first, then the latest ActivationDate (as a date), then the highest trailing number in VersionId.

```sql
dynamics365_to_bom_header(bom_table, bom_version)
```

**Examples**

```sql
SELECT * FROM dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion');
```

### `dynamics365_to_bom_component`

*Table macro*

Maps Dynamics 365 BOM lines into the universal bom_component shape (singular) — note this is not the flat bom_items form. Reads BOMId, LineNum, ItemId, Quantity and ScrapPercent.

```sql
dynamics365_to_bom_component(bom_lines)
```

**Examples**

```sql
SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM');
```

### `dynamics365_to_materials`

*Table macro*

Maps a Dynamics 365 InventTable to the universal materials shape. Reads ItemId, ItemName and ItemType (0/1/2) and returns the full materials column set. Projects a shape; it does not write an output table.

```sql
dynamics365_to_materials(invent_table)
```

**Examples**

```sql
SELECT * FROM dynamics365_to_materials(invent_table := 'InventTable');
```

<!-- generated:functions:end -->
