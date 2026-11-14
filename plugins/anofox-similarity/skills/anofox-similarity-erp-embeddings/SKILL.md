---
name: anofox-similarity-erp-embeddings
description: >
  ERP extraction and embedding pipeline for the anofox_similarity DuckDB extension: SAP
  (`sap_to_bom_items`, `sap_to_materials_with_desc`, `extract_material_descriptions`) and
  Dynamics 365 (`dynamics365_to_bom_header`, `dynamics365_to_bom_component`) transforms,
  `bom_to_items` / `items_to_materials` / `items_to_bom_header` / `items_to_bom_component`, helpers, MinHash/textual/transactional embeddings,
  `fuse_embeddings` / `compute_fused_embeddings`, statistics and the material_embeddings table.
  Use when loading MARA/MAKT/MAST/STKO/STPO or D365 BOM tables, mapping your own ERP columns,
  converting a flat parent/child/quantity export into the universal schema (materials, bom_header,
  bom_component),
  building or fusing material embeddings, or checking the anofox-forecast dependency.
version: 2026.11.13
user-invocable: false
---

# Anofox Similarity — ERP Transforms & Embeddings Cheat Sheet

**Extension:** `anofox_similarity` v2026.11.13 | **DuckDB:** v2.0 / v1.5.6 | **Reference:** [`docs/api/04-embeddings.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/04-embeddings.md), [`docs/api/05-erp-transformations.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/05-erp-transformations.md), [`docs/api/06-infrastructure.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/06-infrastructure.md)

Gets ERP data into the flat `bom_items` edge list (`parent_id`, `child_id`, optional `quantity`) that
every similarity search reads, and builds per-material vectors in `material_embeddings`. The searches (`find_similar_materials_jaccard`, WL kernel, predecessors) are in
**anofox-similarity-material-search**; MinHash/LSH indexes over feature multisets are in
**anofox-similarity-evaluation-scale**; classifying with your own text vectors is in
**anofox-similarity-classification-review**.

## Critical gotchas

- **Stored `jaccard_embedding`s from before the FLOAT-array release are incompatible.** MinHash values
  are now scaled to [0, 1); old ones (~1e18) compare as 0.0 with new ones, silently. Recompute them
  all (`UPDATE material_embeddings SET jaccard_embedding = NULL`, then re-run the upsert below).
  Full list of changes: `docs/UPGRADING.md`.
- **LOAD writes into a writable database.** It creates `material_embeddings` and three other tables
  and, with `vss`, three HNSW indexes on them (`hnsw_structural_idx` uses `l2sq`, the other two
  `cosine`). Nothing fills them. For your own vector search, create your own table and index, or
  drop and recreate an index with the metric you need; see "Tables the extension creates" below.
  To keep a file clean, open the shell without it and `ATTACH` it.
- **`minhash_similarity` takes `FLOAT[n]` arrays, not `FLOAT[]` lists.** A list such as
  `list(minhash_value ORDER BY seed)` needs `::FLOAT[128]` first ("No function matches" otherwise).
- **DuckDB 1.5.6 cannot `CASE` over a `FLOAT[n]`** ("Unimplemented type for case expression"). Cast
  inside and back: `(CASE WHEN c THEN embed_text(t)::FLOAT[] END)::FLOAT[384]`.
- **Textual embeddings are a placeholder.** No language model ships. `compute_textual_embeddings`,
  `embed_text` and `embedding_backend` return a deterministic *hash* vector for every provider
  (`gemma-local`, `openai`, `anthropic`, `cohere`; any other name is an error); similar texts do **not** get
  similar vectors (`'centrifugal pump'` vs `'centrifugal pumps'`: cosine 0.03). The vectors are the
  same on every platform, and distinct texts get distinct vectors. Plumbing only; for
  semantics bring your own model's vectors as a table and fuse (`fuse_embeddings`) or classify with them.
- **Language keys differ by function.** `sap_to_materials_with_desc` defaults to `'E'` (SAP's one-char
  `SPRAS`); `extract_material_descriptions` and `compute_textual_embeddings` default to `'EN'`. On a
  real MAKT, `'EN'` matches nothing and returns **0 rows, no error**: pass `language := 'E'`.
- **Embeddings are `FLOAT[n]` arrays** (`FLOAT[384]` textual, `FLOAT[128]` transactional/Jaccard,
  `FLOAT[256]` structural, `FLOAT[768]` fused), the type vss HNSW indexes take: compare with
  `array_cosine_distance` / `array_distance`, not the `list_*` functions. A list of the right length
  casts in; a wrong length is a `Conversion Error`, not NULL.
- **HNSW is only used for a constant query vector.** `ORDER BY array_cosine_distance(e, q) LIMIT k`
  takes the index when `q` is a constant: `SET VARIABLE q = (SELECT e FROM t WHERE id = 'X');` then
  `... array_cosine_distance(e, getvariable('q')::FLOAT[384]) LIMIT 10` (check with `EXPLAIN`:
  `HNSW_INDEX_SCAN`); a correlated subquery or a join scans everything. An index in a database file
  needs `SET hnsw_enable_experimental_persistence = true` before `CREATE INDEX` (a vss rule).
- **MAKT duplicates are collapsed.** Several MAKT rows per `(matnr, spras)` (one per client/MANDT)
  become one row per material (an arbitrary one, via `ANY_VALUE`). Filter `mandt` first if it matters.
- **Required columns.** MARA needs `matnr, mtart, matkl, ersda, lvorm` (`ersda` as `YYYYMMDD` text,
  `DATE` or ISO string); MAKT needs `matnr, spras, maktx` **and `maktg`** for
  `extract_material_descriptions` / `compute_textual_embeddings`; MAST `matnr, stlnr`; STKO
  `stlnr, stlal`; STPO `stlnr, idnrk, menge, meins`. A missing column is a Binder Error naming it.
- **Transforms emit the universal schema and write nothing** (wrap in `CREATE TABLE ... AS`).
  `sap_to_bom_items` is already flat but names the quantity `qty` (the sum of MENGE: DECIMAL(38, s) for a DECIMAL MENGE); rename
  and cast to `quantity`. The D365 functions emit `bom_header` + `bom_component`: flatten with `bom_to_items`.
- **`sap_to_bom_items` uses the SAP columns your extract has; pass STAS.** With `stlty` only material
  BOMs (`'M'`) are read (a document BOM can share the `stlnr`); MAST `stlal` must be the
  `bom_alternative`; `datuv` + `stkoz` / `stlkn` + `stpoz` pick the header and item versions valid on
  `reference_date`; `lkenz` / `loekz` drop deleted headers and items; `bom_usage` filters
  `MAST.stlan`. Without `stas_table := 'STAS'` every item of the BOM number comes through,
  whichever alternative selects it, and items STAS removed are kept. A missing optional column just
  skips its rule, so check that a full extract really has `stlty`, `stlkn` and `lkenz`. With `stlty`
  on STKO, STPO (and STAS) must have it too (else a Binder Error); `mandt` in every table keeps SAP
  clients apart. `reference_date` is checked at once ('garbage' is an error); a blank or
  `'00000000'` `datuv` counts as undated.
- **An empty `sap_to_bom_items` result usually means the BOMs are not valid on `reference_date`**
  (default 9999-12-31, i.e. today's state): headers or items deleted (`lkenz`/`loekz`), or removed
  by STAS. Re-run with an earlier `reference_date` (`'20030101'`) to see a deleted BOM, and check
  the STAS rows of the BOM number.
- **One edge per parent and child, from every usage.** Without `bom_usage` every usage
  (engineering, production, sales ...) and BOM number of a material is read; each `(parent_id,
  child_id)` comes once, from the lowest usage, then the lowest BOM number, with the quantities of
  its positions added per unit (`bom_id` says which BOM). A material can so mix parts of several
  BOMs: pass `bom_usage := '1'` (production) or the usage you compare.
- **SAP clients stay apart in the `client` column** (`mandt` when every table has it, else NULL):
  the same material in clients 100 and 200 gives two sets of rows, never summed. The searches do
  not see `client`: filter one client (`WHERE client = '100'`) before you build `bom_items`.
- **anofox-forecast is not loaded here.** `compute_transactional_embeddings`, `extract_ts_features`,
  `recompute_embedding_statistics` and `compute_domain_specific_statistics` need `anofox_fcst_ts_features`.
  Check with `check_anofox_forecast_available` first (see the dependency section). It is never auto-installed.
- **No function produces `structural_embedding` (FLOAT[256]).** The fusion default is 0.5/0.5/0
  (structural + textual): bring structural vectors or set `weights_structural := 0`. A positive weight
  on a NULL modality silently drops the material from `compute_fused_embeddings`.
- **Loading the extension writes to a writable database file** (four tables + three HNSW indexes);
  a read-only database gets none, so `compute_fused_embeddings` (Catalog Error) and
  `compute_transactional_embeddings` (clear error) fail there; open the file writable once.

## SAP sample tables (used by the examples below)

```sql
-- MARA: one row per material; lvorm = 'X' marks a deletion. ersda is YYYYMMDD text.
CREATE TABLE sap_mara AS SELECT * FROM (VALUES
  ('000000000000100001', 'FERT', 'PUMPS', '20210115', ''),
  ('000000000000100002', 'FERT', 'PUMPS', '20220301', ''),
  ('000000000000200001', 'ROH',  'SEALS', '20190520', ''),
  ('000000000000200002', 'ROH',  'MOTOR', '20190520', ''),
  ('000000000000200003', 'ROH',  'SENSR', '20200101', ''),
  ('000000000000999999', 'FERT', 'PUMPS', '20180101', 'X')
) t(matnr, mtart, matkl, ersda, lvorm);
```

```sql
-- MAKT: two English rows for 100001 (two clients) and one German row.
CREATE TABLE sap_makt AS SELECT * FROM (VALUES
  ('000000000000100001', 'E', 'Centrifugal pump 5 kW',             'CENTRIFUGAL PUMP 5 KW'),
  ('000000000000100001', 'E', 'Centrifugal pump 5kW',              'CENTRIFUGAL PUMP 5KW'),
  ('000000000000100001', 'D', 'Kreiselpumpe 5 kW',                 'KREISELPUMPE 5 KW'),
  ('000000000000100002', 'E', 'Centrifugal pump 7 kW with sensor', 'CENTRIFUGAL PUMP 7 KW WITH SENSOR'),
  ('000000000000200001', 'E', 'Mechanical seal',                   'MECHANICAL SEAL'),
  ('000000000000200002', 'E', 'Electric motor',                    'ELECTRIC MOTOR')
) t(matnr, spras, maktx, maktg);
```

```sql
CREATE TABLE sap_mast AS SELECT * FROM (VALUES
  ('000000000000100001', '1000', '1', '00000001'),
  ('000000000000100002', '1000', '1', '00000002')
) t(matnr, werks, stlan, stlnr);
```

```sql
-- STKO: BOM 00000001 has two validity slices (2020 and 2024).
CREATE TABLE sap_stko AS SELECT * FROM (VALUES
  ('00000001', '01', '20200101', 1.0),
  ('00000001', '01', '20240101', 1.0),
  ('00000002', '01', '20220301', 1.0)
) t(stlnr, stlal, datuv, bmeng);
```

```sql
CREATE TABLE sap_stpo AS SELECT * FROM (VALUES
  ('00000001', '000000000000200001', 2.000::DECIMAL(13,3), 'ST'),
  ('00000001', '000000000000200002', 1.000::DECIMAL(13,3), 'ST'),
  ('00000002', '000000000000200001', 2.000::DECIMAL(13,3), 'ST'),
  ('00000002', '000000000000200002', 1.000::DECIMAL(13,3), 'ST'),
  ('00000002', '000000000000200003', 1.000::DECIMAL(13,3), 'ST')
) t(stlnr, idnrk, menge, meins);
```

## SAP transforms

```text
sap_to_bom_items(mast_table, stko_table, stpo_table, bom_alternative := '01',
                 reference_date := '9999-12-31', bom_usage := NULL, stas_table := NULL)
  → (bom_id, parent_id, child_id, qty, unit, valid_from DATE, valid_to DATE, bom_version, client)
sap_to_materials(mara_table)                          -- makt_table/language accepted, ignored
  → (material_id, material_type, material_group, description = '', created_date DATE)
sap_to_materials_with_desc(mara_table, makt_table, language := 'E')
  → same columns, description = MAKTX in that language ('' if none)
extract_material_descriptions(makt_table := 'sap_makt', language := 'EN')
  → (material_id, full_text = MAKTX || ' ' || MAKTG)
```

| Input column | Output | Notes |
|---|---|---|
| `MAST.matnr` (trimmed) | `parent_id` | leading zeros kept |
| `STKO.stlnr` / `stlal` / `datuv` | `bom_id` / `bom_version` / `valid_from` | newest header version with `datuv <= reference_date`; dropped if it has `lkenz`/`loekz` |
| `STPO.idnrk` (trimmed), `menge`, `meins` | `child_id`, `qty`, `unit` | per `stlkn` the version valid on `reference_date`, without `lkenz`; `valid_to` is always 9999-12-31 |
| `STAS` (with `stas_table`) | which items stay | the alternative's newest selection row on `reference_date`, without `lkenz` |
| `MARA.matnr/mtart/matkl/ersda` | `material_id/material_type/material_group/created_date` | rows with `lvorm` not NULL/''/' ' excluded; `ersda` YYYYMMDD, DATE or ISO |

```sql
SELECT bom_id, parent_id, child_id, qty, valid_from
FROM sap_to_bom_items('sap_mast', 'sap_stko', 'sap_stpo')
ORDER BY parent_id, child_id;
-- 100001 rows have valid_from 2024-01-01 (newest slice); 100002 has 3 components

-- Point in time: the 2023 view picks the 2020 header slice (no item versions in this STPO).
SELECT DISTINCT parent_id, valid_from
FROM sap_to_bom_items('sap_mast', 'sap_stko', 'sap_stpo', reference_date := '20230101')
ORDER BY parent_id;
```

```sql
-- Deleted 999999 is dropped; 200003 has no MAKT row so description = ''; 100001 appears once.
SELECT material_id, material_type, description, created_date
FROM sap_to_materials_with_desc('sap_mara', 'sap_makt', language := 'E')
ORDER BY material_id;

SELECT * FROM extract_material_descriptions(makt_table := 'sap_makt', language := 'E')
ORDER BY material_id;
-- 000000000000200001 | Mechanical seal MECHANICAL SEAL  (4 rows; language := 'EN' would give 0)
```

`sap_to_materials('sap_mara')` gives the same rows with an empty `description`.

A full extract: categories, item versions, deletion flags and STAS item selection.

```sql
CREATE TABLE f_mast AS SELECT * FROM (VALUES ('PUMP', '1000', '1', '0001', '01')) t(matnr, werks, stlan, stlnr, stlal);
CREATE TABLE f_stko AS SELECT * FROM (VALUES
  ('M', '0001', '01', '1', '20200101', NULL, NULL),
  ('D', '0001', '01', '1', '20200101', NULL, NULL)      -- a document BOM with the same number
) t(stlty, stlnr, stlal, stkoz, datuv, lkenz, loekz);
CREATE TABLE f_stpo AS SELECT * FROM (VALUES
  ('M', '0001', '1', '1', '20200101', NULL, 'MOTOR',  1.0, 'ST'),
  ('M', '0001', '2', '1', '20200101', NULL, 'SEAL',   2.0, 'ST'),
  ('M', '0001', '2', '2', '20240101', NULL, 'SEAL',   4.0, 'ST'),  -- newer version of item 2
  ('M', '0001', '3', '1', '20200101', 'X',  'OLD',    1.0, 'ST'),  -- deleted item
  ('M', '0001', '4', '1', '20200101', NULL, 'SENSOR', 1.0, 'ST'),  -- STAS removed it in 2022
  ('D', '0001', '1', '1', '20200101', NULL, 'DRAWING', 1.0, 'ST')
) t(stlty, stlnr, stlkn, stpoz, datuv, lkenz, idnrk, menge, meins);
CREATE TABLE f_stas AS SELECT * FROM (VALUES
  ('M', '0001', '01', '1', '1', '20200101', NULL),
  ('M', '0001', '01', '2', '1', '20200101', NULL),
  ('M', '0001', '01', '4', '1', '20200101', NULL),
  ('M', '0001', '01', '4', '2', '20220101', 'X')
) t(stlty, stlnr, stlal, stlkn, stasz, datuv, lkenz);
SELECT child_id, qty
FROM sap_to_bom_items('f_mast', 'f_stko', 'f_stpo', stas_table := 'f_stas')
ORDER BY child_id;
-- no DRAWING (document BOM), no OLD (deleted), no SENSOR (removed by STAS); SEAL at its newest version
-- => MOTOR | 1.0
-- => SEAL | 4.0
```


## Dynamics 365 transforms

D365 table field names (`BOMVersion.ItemId`, `FromDate`, `BOM.BOMQty`, ...). A data-entity
(OData) export names them differently: map it with a view (`SELECT ItemNumber AS ItemId, ...`)
first. Full column table and the entity view: `docs/api/05-erp-transformations.md`.

```text
dynamics365_to_materials(invent_table := 'InventTable')       -- also positional
     needs ItemId; uses ItemName, ItemGroupId, ItemType (0 Item, 1 BOM, 2 Service), DataAreaId
  → (material_id, material_number, description, material_type 'ITEM'/'BOM'/'SERVICE', material_group,
     procurement_type, base_uom, weight, cost_per_unit, source_system, is_active, created_at, company)
dynamics365_to_bom_header(bom_version := 'BOMVersion', bom_table := NULL, reference_date := '9999-12-31')
     needs BOMVersion ItemId, BOMId; uses FromDate, ToDate, Active, Approved, DataAreaId;
     bom_table (BOMTable) only adds plant_id from SiteId
  → (bom_id '<item>/<BOMId>', source_system, source_bom_id, parent_material_id, bom_type,
     alternative_number, revision, base_quantity, base_uom, valid_from, valid_to, plant_id,
     is_approved, created_at, company)
dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion', reference_date := '9999-12-31')
     needs BOM BOMId, ItemId, BOMQty; uses BOMQtySerie, UnitId, LineNum, FromDate, ToDate,
     BOMType, BOMConsump, ScrapVar, OprNum, DataAreaId
  → (component_id, bom_id, line_number, child_material_id, quantity_per DECIMAL(18,6), quantity_uom,
     is_fixed_quantity, scrap_percent, effective_from, effective_to, component_type,
     supply_type, operation_sequence, is_alternative, alternative_group, created_at, company)
```

- **One BOM per company and item on `reference_date`**: versions with `FromDate <= date` and `ToDate` empty,
  `1900-01-01` (D365's "none") or `>= date`; active first, approved first, newest `FromDate`,
  lowest `BOMId`. The default date `9999-12-31` takes the newest version, so pass today's date
  (or the planning date) for "what is built now".
- **Pass both functions the same `bom_version` and `reference_date`**: the component function
  picks the lines of the BOMs the header function chooses, and `bom_id` is built the same way.
  `bom_id` is `<item>/<BOMId>` (a BOM can serve several items; `/` and `\` inside an id are
  escaped with `\`, so ids never collide); `source_bom_id` is the BOMId.
- `quantity_per` = `BOMQty / BOMQtySerie` (per one parent); one row per BOM, child, unit and
  consumption type (constant `BOMConsump` lines are not added to variable ones), the quantities of
  a child's lines added. Lines outside their `FromDate`/`ToDate` are dropped.
- **Several companies (`DataAreaId`) stay apart**: a `company` column (lower case) on every output
  and in `bom_id`. `bom_to_items` has no company column and `materials.material_id` is a key, so
  filter one company first: `CREATE VIEW h AS SELECT * FROM d365_bom_header WHERE company =
  'usmf'`. Versions per site, from-quantity or configuration are not distinguished (one version
  per company and item).
- The column contract of earlier releases (`Status`, `VersionId`, `ActivationDate`,
  `ApprovedStatus`, `Quantity`, `ScrapPercent`) is gone; the error names it, `docs/UPGRADING.md`
  maps it. `reference_date` is `YYYY-MM-DD` (a time may follow) or `YYYYMMDD`.

```sql
CREATE TABLE InventTable AS SELECT * FROM (VALUES
  ('PUMP-A', 'Pump A', 1), ('PUMP-B', 'Pump B', 1),
  ('SEAL', 'Seal', 0), ('MOTOR', 'Motor', 0), ('SENSOR', 'Sensor', 0)
) t(ItemId, ItemName, ItemType);
```

```sql
-- PUMP-B has an older version (expired 2023) and the current one; ToDate 1900-01-01 = open
CREATE TABLE BOMVersion AS SELECT * FROM (VALUES
  ('PUMP-A', 'B1', DATE '2024-01-01', DATE '1900-01-01', 1, 1),
  ('PUMP-B', 'B2-OLD', DATE '2020-01-01', DATE '2023-12-31', 1, 1),
  ('PUMP-B', 'B2', DATE '2024-06-01', DATE '1900-01-01', 1, 0)
) t(ItemId, BOMId, FromDate, ToDate, Active, Approved);
```

```sql
-- SEAL of B1 on two lines; B2 lists 100 sensors per series of 100 pumps
CREATE TABLE BOM AS SELECT * FROM (VALUES
  ('B1', 1, 'SEAL', 1.0, 1.0, 'pcs'), ('B1', 2, 'MOTOR', 1.0, 1.0, 'pcs'), ('B1', 3, 'SEAL', 1.0, 1.0, 'pcs'),
  ('B2-OLD', 1, 'MOTOR', 1.0, 1.0, 'pcs'),
  ('B2', 1, 'SEAL', 2.0, 1.0, 'pcs'), ('B2', 2, 'MOTOR', 1.0, 1.0, 'pcs'), ('B2', 3, 'SENSOR', 100.0, 100.0, 'pcs')
) t(BOMId, LineNum, ItemId, BOMQty, BOMQtySerie, UnitId);
```

```sql
CREATE TABLE d365_bom_header AS
SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', reference_date := '2024-12-31');
SELECT bom_id, source_bom_id, is_approved FROM d365_bom_header ORDER BY bom_id;
-- => PUMP-A/B1 | B1 | true
-- => PUMP-B/B2 | B2 | false
```

```sql
CREATE TABLE d365_bom_component AS
SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion', reference_date := '2024-12-31');
```

## `bom_to_items` and the `items_to_*` converters

```text
bom_to_items(header_table := 'bom_header', component_table := 'bom_component')
  → (parent_id = parent_material_id, child_id = child_material_id, quantity = quantity_per)
    joined on bom_id; ALL headers (no is_approved filter — add one yourself)
items_to_materials(items_table, source_system)       -- one row per distinct parent or child id
  → universal materials columns (material_id, material_number, ..., source_system, is_active, created_at,
     company NULL)
items_to_bom_header(items_table, source_system)      -- one row per parent, bom_id 'BOM_' || parent_id
  → universal bom_header columns (bom_id, source_system, ..., parent_material_id, ..., is_approved, created_at,
     company NULL)
items_to_bom_component(items_table)                  -- one row per edge, component_id 'COMP_' || parent_id || '_' || n
  → universal bom_component columns (component_id, bom_id, line_number, child_material_id,
     quantity_per DECIMAL(18,6), quantity_uom 'EA', ..., created_at, company NULL)
```

```sql
SELECT * FROM bom_to_items(header_table := 'd365_bom_header', component_table := 'd365_bom_component')
ORDER BY parent_id, child_id;
-- PUMP-A: MOTOR, SEAL (two lines added); PUMP-B: MOTOR, SEAL, SENSOR (100 per 100 = 1)
-- => PUMP-A | MOTOR | 1.000000
-- => PUMP-A | SEAL | 2.000000
-- => PUMP-B | MOTOR | 1.000000
-- => PUMP-B | SEAL | 2.000000
-- => PUMP-B | SENSOR | 1.000000
```

The `items_to_*` converters go the other way. `items_table` needs `parent_id` and `child_id`;
`items_to_bom_component` also needs `quantity` (a NULL quantity counts as 1; for an edge list
without one pass `(SELECT *, NULL::DOUBLE AS quantity FROM ...)` as a view). `quantity_per` is
DECIMAL(18,6): a non-zero quantity that rounds to 0 at 6 decimals is an error, so scale tiny
per-piece amounts (adhesive in kg) to a smaller unit first. Each returns the universal table's own columns and types;
store the header and component output as tables and `bom_to_items` over them gives the edges back
(and the `bom_explosion_*` / `bom_where_used` traversals can read them).

```sql
CREATE TABLE flat_bom AS SELECT * FROM (VALUES ('P-1', 'C-1', 2.0), ('P-1', 'C-2', NULL)) t(parent_id, child_id, quantity);
CREATE TABLE mig_materials     AS SELECT * FROM items_to_materials('flat_bom', 'MIGRATED');
CREATE TABLE mig_bom_header    AS SELECT * FROM items_to_bom_header('flat_bom', 'MIGRATED');
CREATE TABLE mig_bom_component AS SELECT * FROM items_to_bom_component('flat_bom');
SELECT * FROM bom_to_items(header_table := 'mig_bom_header', component_table := 'mig_bom_component')
ORDER BY child_id;
-- 3 materials, 1 header (BOM_P-1), 2 components; the NULL quantity came back as 1
-- => P-1 | C-1 | 2.000000
-- => P-1 | C-2 | 1.000000
```

## Use your own ERP columns

Any table or view with `parent_id` and `child_id` (optional `quantity`) works; no transform is needed.

```sql
CREATE TABLE erp_bom AS SELECT * FROM (VALUES
  ('P-100', 'MOTOR', 1), ('P-100', 'SEAL', 2),
  ('P-200', 'MOTOR', 1), ('P-200', 'SEAL', 2), ('P-200', 'SENSOR', 1)
) t(ProductNumber, ComponentNumber, QtyPer);
CREATE VIEW erp_bom_items AS
SELECT ProductNumber AS parent_id, ComponentNumber AS child_id, QtyPer AS quantity FROM erp_bom;

SELECT material_id, round(similarity, 3) AS similarity
FROM find_similar_materials_jaccard('P-100', 5, bom_table := 'erp_bom_items', min_similarity := 0.01);
-- P-200 | 0.667
-- => P-200 | 0.667
```

## Helpers

```text
aggregate_material_components(bom_table := 'bom_items', material_filter := NULL)
  → (material_id = parent_id, components VARCHAR[] sorted)        material_filter: list of parents
filter_recent_movements(movements_table := 'goods_movements', time_window_days := 365, min_quantity := 0)
  → (material_id, movement_date DATE, quantity DOUBLE)            movement_type is NOT returned
check_anofox_forecast_available() → BOOLEAN
check_duckpgq_available()         → BOOLEAN
```

`aggregate_material_components` lists each parent's direct children (the input shape of the
Jaccard and cold-start functions). `filter_recent_movements` anchors the window on the **latest `movement_date` in the data**, not today,
and keeps only `quantity > min_quantity` with non-NULL date and quantity. A text date such as
`'02.01.2023'` yields nothing: convert in a view with `strptime(movement_date, '%d.%m.%Y')::DATE`.

```sql
CREATE TABLE goods_movements AS SELECT * FROM (VALUES
  ('M1', DATE '2024-01-10', 5.0, '261'), ('M1', DATE '2024-03-10', 7.0, '261'),
  ('M1', DATE '2024-06-10', 0.0, '101'), ('M2', DATE '2023-01-10', 3.0, '261'),
  ('M2', DATE '2024-05-10', 4.0, '262')
) t(material_id, movement_date, quantity, movement_type);

SELECT * FROM filter_recent_movements(movements_table := 'goods_movements', time_window_days := 180)
ORDER BY material_id, movement_date;
-- 3 rows: M1 2024-01-10, M1 2024-03-10, M2 2024-05-10 (the 0.0 row and the 2023 row are dropped)
```

```sql
CREATE TABLE sap_bom_items AS
SELECT parent_id, child_id, qty::DOUBLE AS quantity
FROM sap_to_bom_items('sap_mast', 'sap_stko', 'sap_stpo');

SELECT * FROM aggregate_material_components(bom_table := 'sap_bom_items',
                                            material_filter := ['000000000000100002']);
-- 000000000000100002 | [000000000000200001, 000000000000200002, 000000000000200003]
```

## Dependency on anofox-forecast

```sql
SELECT check_anofox_forecast_available() AS forecast, check_duckpgq_available() AS duckpgq;
-- false | false in this build
```

`check_duckpgq_available` only matters for BOM graph traversal (see **anofox-similarity-material-search**).

Without anofox-forecast these fail at bind time (no partial result, nothing to catch in SQL):

```text
SELECT * FROM compute_transactional_embeddings();
-- Binder Error: compute_transactional_embeddings requires the anofox-forecast extension, which is
--   not loaded. Install and load it first: INSTALL anofox_forecast FROM community; LOAD anofox_forecast;
SELECT * FROM extract_ts_features();  -- with goods_movements present; also recompute_embedding_statistics(),
                                                     -- compute_domain_specific_statistics()
-- Catalog Error: Scalar Function with name anofox_fcst_ts_features does not exist!
```

In a script, gate on the check (e.g. `SELECT error('load anofox_forecast first') WHERE NOT check_anofox_forecast_available();` under `-bail`).

## Jaccard (MinHash) embeddings

```text
compute_jaccard_embeddings(bom_table := 'bom_items')     -- needs parent_id, child_id
  → (material_id, seed BIGINT 0..127, minhash_value FLOAT in [0, 1), num_components INTEGER)  one row per seed
minhash_similarity(a FLOAT[n], b FLOAT[n]) → DOUBLE   share of equal positions (also UBIGINT[] signatures)
```

`compute_jaccard_embeddings` returns rows and does not write. Aggregate in seed order to `FLOAT[128]`
and upsert. `minhash_similarity` over two such vectors (the share of equal positions) estimates the
Jaccard of the direct component sets. There is **no HNSW index on `jaccard_embedding`** (L2/cosine over
MinHash values does not approximate Jaccard); for exact search use `find_similar_materials_jaccard`,
for scale `minhash_index` (see **anofox-similarity-evaluation-scale**).

```sql
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components)
SELECT material_id, array_agg(minhash_value ORDER BY seed)::FLOAT[128], max(num_components)
FROM compute_jaccard_embeddings(bom_table := 'sap_bom_items')
GROUP BY material_id
ON CONFLICT (material_id) DO UPDATE SET
  jaccard_embedding = EXCLUDED.jaccard_embedding, num_components = EXCLUDED.num_components;

-- The two sample pumps share 2 of 3 direct components (true Jaccard 0.667).
SELECT round(minhash_similarity(a.jaccard_embedding, b.jaccard_embedding), 2) AS est_jaccard
FROM material_embeddings a, material_embeddings b
WHERE a.material_id = '000000000000100001' AND b.material_id = '000000000000100002';
-- => 0.65
```

## Textual embeddings (placeholder)

```text
compute_textual_embeddings(makt_table := 'sap_makt', language := 'EN',
                           provider := 'gemma-local', provider_config := '')
  → (material_id, textual_embedding FLOAT[384])
    text = MAKTX || ' ' || MAKTG, one row per material; MAKT needs maktg
embedding_backend(text VARCHAR, provider VARCHAR, provider_config VARCHAR) → FLOAT[384]
embed_text(description) → embedding_backend(description, 'gemma-local', '')
```

`provider` must be `gemma-local`, `openai`, `anthropic` or `cohere` (anything else is an
`Invalid Input Error`). `provider_config` is not read yet; never put API keys in it. Provider names are case-insensitive.

```sql
SELECT material_id, array_length(textual_embedding) AS dims
FROM compute_textual_embeddings(makt_table := 'sap_makt', language := 'E')
ORDER BY material_id;

-- Proof it is not semantic: near-identical texts are not close.
SELECT round(1 - array_cosine_distance(embed_text('centrifugal pump'), embed_text('centrifugal pumps')), 2) AS near_dupe,
       embed_text('x') = embedding_backend('x', 'gemma-local', '') AS same_backend;
```

Bring your own vectors instead: a table `(material_id, vec FLOAT[384])` from your model goes straight
into `material_embeddings.textual_embedding` (`fuse_embeddings`, the column and the
`hnsw_textual_idx` index all want 384; another dimension is a cast error).

## Transactional embeddings and statistics (need anofox-forecast)

```text
compute_transactional_embeddings(movements_table := 'goods_movements', time_window_days := 365,
                                 batch_size := NULL, batch_offset := 0)
  → (material_id, transactional_embedding FLOAT[128])       returns rows; does not write
extract_ts_features(movements_table := 'goods_movements', time_window_days := 365,
                    min_observations := 3, batch_size := NULL, batch_offset := 0)
  → (material_id, features STRUCT)
recompute_embedding_statistics(time_window_days := 365, min_observations := 3,
                               movements_table := 'goods_movements')
  → (feature_name, feature_index, feature_category, mean_value, std_value, min_value, max_value, num_samples)
compute_domain_specific_statistics(movements_table := 'goods_movements', time_window_days := 365,
                                   min_observations := 3)
  → same statistics shape, features 92-97 (receipt/reversal ratio, weekday pattern, lifecycle)
check_statistics_freshness()
  → (stat_count, max_samples, current_version, last_updated, is_fresh)
```

- Movements need `material_id, movement_date, quantity`; `movement_type` is optional for
  `compute_transactional_embeddings` (NULL when absent) but required by
  `compute_domain_specific_statistics`. The window is relative to the latest date in the data.
- `recompute_embedding_statistics` **returns** the z-score statistics; persist them yourself into
  `transactional_embedding_statistics` (it also has `updated_at`, `version`).
- `check_statistics_freshness` reads that table (no anofox-forecast needed): `is_fresh` needs at least
  30 rows, the newest updated within the last 7 days. `last_updated` is NULL with no statistics; on a
  read-only database (no table) it returns `0` / NULL / `false` instead of an error.
- No automatic refresh exists (no triggers in DuckDB; `material_embeddings_dirty` is never filled):
  recompute for the changed materials (`WHERE material_id IN (...)`) and
  `INSERT OR REPLACE INTO material_embeddings (material_id, transactional_embedding, updated_at)`.
  A material's Jaccard MinHash depends only on its own children, so computing it for a subset (a
  BOM table filtered to the changed parents) gives the same values as a full run. Detect changes
  by a per-parent hash: `SELECT parent_id, hash(list(child_id ORDER BY child_id)) FROM bom_items
  GROUP BY 1`, compared with the hash stored at the last run.

```sql
SELECT stat_count, last_updated, is_fresh FROM check_statistics_freshness();
-- 0 | NULL | false
```


## Fusion

```text
fuse_embeddings(structural_embedding FLOAT[256], textual_embedding FLOAT[384], transactional_embedding FLOAT[128],
                weights STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT)) → FLOAT[768]
compute_fused_embeddings(weights_structural := 0.5, weights_textual := 0.5, weights_transactional := 0.0)
  → (material_id, combined_embedding FLOAT[768], fusion_weights STRUCT(structural, textual, transactional) FLOAT)
```

| Rule | Behaviour |
|---|---|
| Weights | need not sum to 1; normalised (w / sum). `fusion_weights` reports the normalised values |
| Scaling | each modality is multiplied by `sqrt(w)` then concatenated (256 + 384 + 128 = 768); two unit vectors at 0.5/0.5 give a unit vector. Use for cosine / NN only |
| Weight 0 | modality ignored, may be NULL (zero-filled block) |
| Weight > 0 on a NULL modality | `compute_fused_embeddings` drops the material; `fuse_embeddings` returns NULL |
| All weights 0 | `compute_fused_embeddings` keeps the material with a NULL `combined_embedding`; `fuse_embeddings` returns NULL |
| Wrong length | a list of the right length casts in; another length is `Conversion Error: Cannot cast list with length 2 to array with length 256` |
| Source | `compute_fused_embeddings` reads the fixed `material_embeddings` table and returns rows; it does not write `combined_embedding` |

```sql
-- Your own vectors (here synthetic): structural FLOAT[256], textual FLOAT[384].
CREATE TABLE my_vectors AS
SELECT material_id,
       [CAST(sin(i * 0.3 + j) AS FLOAT) FOR j IN range(256)]::FLOAT[256] AS struct_vec,
       [CAST(cos(i * 0.2 + j) AS FLOAT) FOR j IN range(384)]::FLOAT[384] AS text_vec
FROM (VALUES ('M1', 1), ('M2', 2), ('M3', 3)) v(material_id, i);

SELECT material_id,
       array_length(fuse_embeddings(struct_vec, text_vec, NULL,
                           {'structural': 0.5, 'textual': 0.5, 'transactional': 0.0})) AS dims
FROM my_vectors ORDER BY material_id;
-- 768 each
```

## Tables the extension creates

On LOAD into a **writable** database (including in-memory) the extension creates, if missing:

| Object | Purpose |
|---|---|
| `material_embeddings`: `material_id` PK, `jaccard_embedding FLOAT[128], structural_embedding FLOAT[256], textual_embedding FLOAT[384], transactional_embedding FLOAT[128], combined_embedding FLOAT[768], updated_at, num_components` | vector store; nothing fills it automatically |
| `material_embeddings_dirty`: `material_id` PK, `reason`, `marked_at` | reserved, never populated |
| `transactional_embedding_statistics`: `feature_name` PK, `feature_index, feature_category, mean_value, std_value, min_value, max_value, num_samples, updated_at, version` | z-score stats |
| `transactional_feature_mapping`: `feature_index` PK, `feature_name, category, description, is_advanced, created_at` | feature metadata (empty) |
| HNSW `hnsw_structural_idx` (l2sq), `hnsw_textual_idx` (cosine), `hnsw_combined_idx` (cosine) | only with a newly created `material_embeddings` |

- **Read-only database:** these tables are absent. Every function works except those reading them:
  `compute_fused_embeddings` (`Catalog Error: Table with name material_embeddings does not exist!`) and
  `compute_transactional_embeddings` (an error saying so). `check_statistics_freshness` returns
  `0` / NULL / `false`. Open the database writable once to create them.
- A file holding the HNSW indexes needs the `vss` extension to insert into `material_embeddings`
  later. On DuckDB 2.0 without an installable `vss` the tables are created without indexes.
- To keep them out of your file, start the shell with no file and `ATTACH` it, or drop the three
  indexes (they stay dropped). See `docs/guides/09-nightly-pipelines.md`.

## Canonical ERP → embeddings pipeline

```sql
-- 1. Transform (SAP sample tables from above) into the flat edge list and the material master.
CREATE OR REPLACE TABLE bom_items AS
SELECT parent_id, child_id, qty::DOUBLE AS quantity
FROM sap_to_bom_items('sap_mast', 'sap_stko', 'sap_stpo', bom_alternative := '01');
CREATE OR REPLACE TABLE materials AS
SELECT * FROM sap_to_materials_with_desc('sap_mara', 'sap_makt', language := 'E');

-- 2. Gate: an empty extract would silently give empty results.
SELECT error('gate: bom_items is empty') FROM bom_items HAVING count(*) = 0;

-- 3. MinHash embeddings per parent.
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components, updated_at)
SELECT material_id, array_agg(minhash_value ORDER BY seed)::FLOAT[128], max(num_components), now()
FROM compute_jaccard_embeddings(bom_table := 'bom_items')
GROUP BY material_id
ON CONFLICT (material_id) DO UPDATE SET jaccard_embedding = EXCLUDED.jaccard_embedding,
  num_components = EXCLUDED.num_components, updated_at = EXCLUDED.updated_at;

-- 4. Your own structural / textual vectors (synthetic stand-ins here; embed_text is NOT semantic).
CREATE OR REPLACE TABLE ext_vectors AS
SELECT material_id,
       [CAST(sin(i + j) AS FLOAT) FOR j IN range(256)]::FLOAT[256] AS struct_vec,
       [CAST(cos(i + j) AS FLOAT) FOR j IN range(384)]::FLOAT[384] AS text_vec
FROM (SELECT material_id, row_number() OVER (ORDER BY material_id) AS i
      FROM materials WHERE material_type = 'FERT');
INSERT INTO material_embeddings (material_id, structural_embedding, textual_embedding, updated_at)
SELECT material_id, struct_vec, text_vec, now() FROM ext_vectors
ON CONFLICT (material_id) DO UPDATE SET structural_embedding = EXCLUDED.structural_embedding,
  textual_embedding = EXCLUDED.textual_embedding, updated_at = EXCLUDED.updated_at;

-- 5. Fuse and store the combined vector.
UPDATE material_embeddings e
SET combined_embedding = f.combined_embedding, updated_at = now()
FROM compute_fused_embeddings(weights_structural := 0.6, weights_textual := 0.4) f
WHERE e.material_id = f.material_id;

-- 6. Nearest neighbours by cosine distance on the fused vector.
SELECT e.material_id, m.description,
       round(array_cosine_distance(e.combined_embedding,
             (SELECT combined_embedding FROM material_embeddings
              WHERE material_id = '000000000000100001')), 4) AS dist
FROM material_embeddings e JOIN materials m USING (material_id)
WHERE e.combined_embedding IS NOT NULL
ORDER BY dist;
```
