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
| all | | `mandt`: when every table has it, rows of different SAP clients never join or merge, and the output column `client` carries it (NULL otherwise); filter one client before searching |

With `stlty` on STKO, STPO (and STAS) need `stlty` too: otherwise a document item could replace a
material item of the same node, so this is a Binder Error. Version counters (`stkoz`, `stpoz`,
`stasz`) are compared as numbers; a blank or `'00000000'` `datuv` counts as undated.
`reference_date` is checked when the query is bound.

Each `(parent_id, child_id)` edge comes once. Without `bom_usage` every usage (production,
engineering, sales ...) and every BOM number of a material is read, and an edge is taken from the
lowest usage, then the lowest BOM number; the quantities of that BOM's positions of the child are
added per unit (a child in two units gives two rows). The result can therefore hold parts of
several BOMs of a material: pass `bom_usage := '1'` (or the usage you compare) for one usage.

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

Convert Microsoft Dynamics 365 (Finance & Operations / Supply Chain Management) BOMs.

These table functions RETURN **universal-schema** rows (materials / bom_header / bom_component
shapes). They do not write an output table, and the component function does **not** return flat
`bom_items`: flatten with `bom_to_items`. They read the native D365 columns, in either spelling,
detected per table: the table field names (`BOMVersion.ItemId`, `FromDate`, ...) or the OData
data-entity names (`BillOfMaterialsVersionsV2.ItemNumber`, `ValidFromDate`, ...).

```sql
dynamics365_to_materials(invent_table := 'InventTable')     -- also positional
    → TABLE (material_id, material_number, description, material_type, material_group,
             procurement_type, base_uom, weight, cost_per_unit, source_system, is_active, created_at)

dynamics365_to_bom_header(bom_version := 'BOMVersion', bom_table := NULL, reference_date := '9999-12-31')
    → TABLE (bom_id, source_system, source_bom_id, parent_material_id, bom_type, alternative_number,
             revision, base_quantity, base_uom, valid_from, valid_to, plant_id, is_approved, created_at)

dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion', reference_date := '9999-12-31')
    → TABLE (component_id, bom_id, line_number, child_material_id, quantity_per, quantity_uom,
             is_fixed_quantity, scrap_percent, effective_from, effective_to, component_type,
             supply_type, operation_sequence, is_alternative, alternative_group, created_at)
```

**Input columns** (table field / entity field):

| Table | Required | Optional, used when present |
|-------|----------|-----------------------------|
| InventTable / ReleasedProductsV2 | `ItemId` / `ItemNumber` | `ItemName` (or `ProductName`, `SearchName`, `NameAlias`), `ItemGroupId`, `ItemType` (0 Item, 1 BOM, 2 Service, or the names) |
| BOMVersion / BillOfMaterialsVersionsV2 | `ItemId` / `ItemNumber`, `BOMId` | `FromDate` / `ValidFromDate`, `ToDate` / `ValidToDate`, `Active` / `IsActive`, `Approved` / `IsApproved`, `DataAreaId` |
| BOM / BillOfMaterialsLinesV2 | `BOMId`, `ItemId` / `ItemNumber`, `BOMQty` / `Quantity` | `BOMQtySerie` / `QuantityDenominator`, `UnitId` / `UnitSymbol`, `LineNum` / `LineNumber`, `FromDate` / `ValidFromDate`, `ToDate` / `ValidToDate`, `BOMType` / `BOMLineType`, `BOMConsump`, `ScrapVar`, `OprNum`, `DataAreaId` |
| BOMTable / BillOfMaterialsHeadersV2 (`bom_table`, optional) | `BOMId` | `SiteId` (→ `plant_id`), `DataAreaId` |

Dates may be DATE, TIMESTAMP or ISO text (`2024-01-01T00:00:00Z`); `1900-01-01` means "no date".
Flags may be BOOLEAN, 0/1 or `Yes`/`No`.

**Rules:**

- **One BOM per item.** BOMVersion links items to BOMs. On `reference_date` (default
  `9999-12-31`, the newest) a version counts when `FromDate <= date` and `ToDate` is empty or
  `>= date`; of those, active first, approved first, then the newest `FromDate`, then the lowest
  `BOMId`. An item with only an unapproved version still gets it, with `is_approved = false`.
- **`bom_id` is `<item>/<BOMId>`** (with `DataAreaId`: `<company>/<item>/<BOMId>`) because a BOM can
  serve several items; `source_bom_id` is the BOMId. Both functions build the same ids, so pass them
  the same `bom_version` and `reference_date`.
- **Lines** of the chosen BOMs count when valid on `reference_date`. `quantity_per` = `BOMQty /
  BOMQtySerie` (per one parent; a series of 0 or none counts as 1), `quantity_uom` = `UnitId`
  (`EA` without a unit column). A child on several lines of a BOM gives one row per unit with the
  quantities added; `component_type` is `ITEM`, `PHANTOM`, `PEGGED` or `VENDOR` from `BOMType`,
  `is_fixed_quantity` is `BOMConsump` = Constant, `scrap_percent` is `ScrapVar`.
- **Companies.** With `DataAreaId` in BOMVersion, the company is part of `bom_id`; lines and
  BOMTable rows are matched on it when they have it too.
- Columns D365 does not have are typed NULLs (`revision`, `alternative_number`, `base_uom`,
  `supply_type`, ...).

**Example — D365 to a `bom_items` table usable by the similarity functions:**
```sql
CREATE TABLE materials     AS SELECT * FROM dynamics365_to_materials('InventTable');
CREATE TABLE bom_header    AS SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', bom_table := 'BOMTable');
CREATE TABLE bom_component AS SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion');
-- Flatten header + component into the parent_id/child_id/quantity shape the algorithms expect:
CREATE TABLE bom_items     AS SELECT * FROM bom_to_items('bom_header', 'bom_component');
```

---

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `sap_to_bom_items`

*Table function*

Converts SAP MAST/STKO/STPO (and optionally STAS) into flat BOM edges (bom_id, parent_id, child_id, qty, unit, valid_from, valid_to, bom_version, client). Needs MAST matnr, stlnr; STKO stlnr, stlal; STPO stlnr, idnrk, menge, meins. Each further SAP column is used when the table has it: stlty keeps material BOMs (M) only; MAST.stlal must equal bom_alternative (default '01', also matched on STKO.stlal); datuv picks the STKO header and, per STPO item node stlkn, the item version valid on reference_date (YYYYMMDD or a date, default 9999-12-31), stkoz / stpoz breaking ties; a header version with lkenz or loekz set and an item version with lkenz set are dropped. bom_usage filters MAST.stlan. stas_table := 'STAS' applies the item selection: an item is kept only when its newest STAS row for the alternative on reference_date exists and is not deleted (lkenz). With stlty on STKO, STPO (and STAS) need it too; with mandt in every table, clients are kept apart. Each (parent_id, child_id) edge comes once: without bom_usage every usage and BOM number of a material is read, and an edge is taken from the lowest usage, then the lowest BOM number, per SAP client (the client column is MANDT when every table has it, else NULL; filter one client before searching, the searches do not see it), with the quantities of that BOM's positions of the child added per unit (pass bom_usage, e.g. '1' for production, to compare one usage). qty is the sum of MENGE (a DECIMAL MENGE becomes DECIMAL(38, scale)).

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

*Table function*

Maps Dynamics 365 BOMVersion (table fields ItemId, BOMId, FromDate, ToDate, Active, Approved, or the BillOfMaterialsVersionsV2 entity fields ItemNumber, ValidFromDate, ValidToDate, IsActive, IsApproved) to the universal bom_header shape: one header per manufactured item, the version valid on reference_date (default 9999-12-31; ToDate empty or 1900-01-01 = open), active first, approved first, newest FromDate, lowest BOMId. bom_id is '<item>/<BOMId>' (with DataAreaId '<company>/<item>/<BOMId>'), source_bom_id the BOMId. bom_table (BOMTable, optional) gives plant_id from SiteId.

```sql
dynamics365_to_bom_header(bom_version, bom_table, reference_date)
```

**Examples**

```sql
SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', bom_table := 'BOMTable');
SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', reference_date := '2024-06-30');
```

### `dynamics365_to_bom_component`

*Table function*

Maps Dynamics 365 BOM lines (table fields BOMId, ItemId, BOMQty, BOMQtySerie, UnitId, LineNum, FromDate, ToDate, BOMType, BOMConsump, ScrapVar, OprNum, or the BillOfMaterialsLinesV2 entity fields ItemNumber, Quantity, QuantityDenominator, UnitSymbol, LineNumber, ValidFromDate, ValidToDate, BOMLineType) of the BOMs that dynamics365_to_bom_header chooses (same bom_version and reference_date) to the universal bom_component shape. quantity_per = BOMQty / BOMQtySerie (per one parent), lines valid on reference_date only, one row per BOM, child and unit (the quantities of a child's lines are added). bom_to_items over the two gives one edge per parent and child and unit.

```sql
dynamics365_to_bom_component(bom_lines, bom_version, reference_date)
```

**Examples**

```sql
SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion');
```

### `dynamics365_to_materials`

*Table function*

Maps a Dynamics 365 InventTable (or the ReleasedProductsV2 entity) to the universal materials shape. Needs ItemId (or ItemNumber); uses ItemName (or ProductName, SearchName, NameAlias), ItemGroupId and ItemType (0 Item, 1 BOM, 2 Service, or the enum names) when present. Columns not in D365 are typed NULLs.

```sql
dynamics365_to_materials(invent_table)
```

**Examples**

```sql
SELECT * FROM dynamics365_to_materials('InventTable');
```

<!-- generated:functions:end -->
