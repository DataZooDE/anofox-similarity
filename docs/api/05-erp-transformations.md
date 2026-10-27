# ERP transformations

> Turn SAP and Dynamics 365 extracts into the extension's input tables.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## ERP Transformations

### SAP Transformations

Convert SAP-format BOM to universal schema.

**sap_to_bom_items** - Convert MAST/STKO/STPO to universal BOM schema:
```sql
sap_to_bom_items(
    mast_table VARCHAR,
    stko_table VARCHAR,
    stpo_table VARCHAR,
    bom_alternative := '01',
    reference_date := '9999-12-31',
    bom_usage := NULL
) → TABLE (
    bom_id VARCHAR,
    parent_id VARCHAR,
    child_id VARCHAR,
    qty DOUBLE,
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
| `stpo_table` | VARCHAR | Required | STPO table (BOM components) |
| `bom_alternative` | VARCHAR | '01' | BOM alternative to extract |
| `reference_date` | VARCHAR | '9999-12-31' | Validity reference date |
| `bom_usage` | VARCHAR | NULL | Reserved — accepted but **not currently applied** as a filter |

> [!NOTE]
> SAP dates (`STKO.datuv`, `reference_date`) may be passed as `YYYYMMDD` strings or ISO `YYYY-MM-DD`;
> both are parsed. `valid_from` is derived from the selected header's `datuv`. `bom_usage` is a
> placeholder and does not yet filter results.
>
> **Limitations:** `reference_date` selects the applicable STKO **header** slice (and thus
> `valid_from`), but the component set is joined by BOM number only, so it does not change the
> component composition. Likewise, `bom_alternative` filters the header; if one BOM number (`stlnr`)
> carries several alternatives, all of their STPO components are returned. These require SAP STAS
> resolution, which this macro does not model — provide STPO already scoped to one alternative.
> `qty` passes through the source `MENGE` column's type (SAP `MENGE` is DECIMAL); cast if you need DOUBLE.

**sap_to_materials** - Extract materials from MARA. Requires MARA columns `matnr`, `mtart`, `matkl`,
`ersda`, **and `lvorm`** (deletion flag — rows flagged deleted are excluded). Its `description`
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
SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', bom_alternative := '01');

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

*Table macro*

Converts the SAP MAST/STKO/STPO trio into the universal BOM shape. 'bom_alternative' picks the alternative BOM, 'reference_date' selects the version valid on that date, and 'bom_usage' filters by usage (NULL means any). Quantity passes through the source MENGE column's type, which is DECIMAL in SAP -- cast if you need DOUBLE.

```sql
sap_to_bom_items(mast_table, stko_table, stpo_table, bom_alternative, reference_date, bom_usage)
```

**Examples**

```sql
SELECT * FROM sap_to_bom_items('MAST', 'STKO', 'STPO', bom_alternative := '01');
```

### `sap_to_materials`

*Table macro*

Extracts materials from a SAP MARA table into the universal materials shape. Requires MARA columns matnr, mtart, matkl, ersda and lvorm; rows flagged deleted via lvorm are excluded. The description column is always empty here -- use sap_to_materials_with_desc to populate it from MAKT.

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

Extracts material descriptions from a SAP MAKT table for one language, returning (material_id, description).

```sql
extract_material_descriptions(makt_table, language)
```

**Examples**

```sql
SELECT * FROM extract_material_descriptions(makt_table := 'sap_makt', language := 'EN');
```

### `dynamics365_to_bom_header`

*Table macro*

Maps Dynamics 365 BOMTable joined to BOMVersion into the universal bom_header shape, keeping rows with Status = 0. Reads BOMTable.(BOMId, ItemId, Status) and BOMVersion.(BOMId, VersionId, ActivationDate, ApprovedStatus).

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
