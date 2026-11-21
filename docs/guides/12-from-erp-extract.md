# From an ERP Extract

> Export the right SAP or Dynamics 365 tables and turn them into the BOM and movement tables the searches read.

**Use this guide to:**
- Know which SAP and D365 tables and columns to export, and which filters matter
- Build `bom_items`, `materials` and `goods_movements` from the raw tables
- Find similar materials and cold-start analogs on that data
- Avoid the traps that give plausible but wrong results: no STAS, two clients, two companies

The blocks run from the repository root on the fictional extract in
[`examples/erp_extract/data`](../../examples/erp_extract/README.md): a pump maker with SAP for its
plants and D365 for two legal entities. The question: the new pump KP-65 has no history; which
pumps are built like it?

## What to Export

**SAP** (SE16, an ODP extractor or a replication tool; all columns as text):

| Table | Columns | Why |
|-------|---------|-----|
| MARA | `mandt, matnr, mtart, matkl, ersda, lvorm` | Materials; `lvorm = X` is dropped |
| MAKT | `mandt, matnr, spras, maktx, maktg` | Descriptions per language |
| MAST | `mandt, matnr, werks, stlan, stlnr, stlal` | Material → BOM number, usage, alternative |
| STKO | `mandt, stlty, stlnr, stlal, stkoz, datuv, lkenz, loekz` | BOM header versions |
| STPO | `mandt, stlty, stlnr, stlkn, stpoz, datuv, idnrk, menge, meins, lkenz` | BOM items and their versions |
| STAS | `mandt, stlty, stlnr, stlal, stlkn, stasz, datuv, lkenz` | Which item belongs to which alternative |
| MSEG | `mandt, matnr, werks, bwart, shkzg, menge, meins, budat_mkpf` | Goods movements (S/4HANA: `MATDOC`) |

Filter the plants you compare in MAST (`werks`), the usage with `bom_usage` (`'1'` production),
and one client after the conversion. Without STAS, `sap_to_bom_items` returns every item of a BOM
number, whichever alternative it belongs to.

**Dynamics 365** (table field names; map an OData entity export with a view, see
[ERP transformations](../api/05-erp-transformations.md)): `BOMVersion(DataAreaId, ItemId, BOMId,
FromDate, ToDate, Active, Approved)` and the lines `BOM(DataAreaId, BOMId, LineNum, ItemId, BOMQty,
BOMQtySerie, UnitId)`, optionally `InventTable` for item names.

## SAP: Load as Text

Material numbers keep their leading zeros and dates stay `YYYYMMDD`, which the functions read.
Only quantities become numbers:

```sql
CREATE TABLE mara AS SELECT * FROM read_csv('examples/erp_extract/data/mara.csv', all_varchar = true);
CREATE TABLE makt AS SELECT * FROM read_csv('examples/erp_extract/data/makt.csv', all_varchar = true);
CREATE TABLE mast AS SELECT * FROM read_csv('examples/erp_extract/data/mast.csv', all_varchar = true);
CREATE TABLE stko AS SELECT * FROM read_csv('examples/erp_extract/data/stko.csv', all_varchar = true);
CREATE TABLE stpo AS
SELECT * REPLACE (menge::DECIMAL(13, 3) AS menge) FROM read_csv('examples/erp_extract/data/stpo.csv', all_varchar = true);
CREATE TABLE stas AS SELECT * FROM read_csv('examples/erp_extract/data/stas.csv', all_varchar = true);
CREATE TABLE mseg AS
SELECT * REPLACE (menge::DECIMAL(13, 3) AS menge) FROM read_csv('examples/erp_extract/data/mseg.csv', all_varchar = true);
```

## SAP: BOM Edges

`sap_to_bom_items` applies the SAP rules: material BOMs only (`stlty`), the alternative, header and
item versions on `reference_date`, deleted items, and the STAS selection. With `mandt` in every
table it keeps the clients apart in the `client` column; the searches do not read it, so keep one:

```sql
CREATE TABLE bom_items AS
SELECT parent_id, child_id, qty AS quantity, unit
FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1')
WHERE client = '100';
SELECT parent_id, count(*) AS components FROM bom_items GROUP BY ALL ORDER BY parent_id;
-- => 000000000000100100 | 4
-- => 000000000000100200 | 4
-- => 000000000000100300 | 4
```

KP-50 (`…100200`) has two alternatives: grey iron (01) and stainless (02). Without `stas_table`
its alternative 01 also gets the stainless casing, and the drawing item of the document BOM never
shows up either way:

```sql
SELECT parent_id, count(*) AS components
FROM sap_to_bom_items('mast', 'stko', 'stpo', bom_usage := '1')
WHERE client = '100' AND parent_id = '000000000000100200' GROUP BY ALL;
-- => 000000000000100200 | 5
```

## SAP: Materials

`sap_to_materials_with_desc` does not read `mandt`. Filter the client first, or a material that
exists in two clients comes twice:

```sql
CREATE VIEW mara_100 AS SELECT * FROM mara WHERE mandt = '100';
CREATE VIEW makt_100 AS SELECT * FROM makt WHERE mandt = '100';
CREATE TABLE materials AS SELECT * FROM sap_to_materials_with_desc('mara_100', 'makt_100', language := 'E');
SELECT material_id, description FROM materials WHERE material_type = 'FERT' ORDER BY material_id;
-- => 000000000000100100 | Centrifugal pump KP-40
-- => 000000000000100200 | Centrifugal pump KP-50
-- => 000000000000100300 |
```

KP-65 has only a German text: `language := 'D'` (or `'DE'`; either form matches `spras`) gives
it. The deleted seal (`lvorm = X`) is not in `materials`.

## SAP: Similar Materials

```sql
SELECT material_id, round(similarity, 4) AS similarity, shared_components, total_components
FROM find_similar_materials_jaccard('000000000000100300', 5);
-- => 000000000000100200 | 0.6 | 3 | 5
-- => 000000000000100100 | 0.1429 | 1 | 7
```

KP-50 shares the seal, the impeller and the motor. Read without STAS it would score 0.8.

## SAP: Consumption and Cold Start

MSEG also holds goods receipts (101) and stock corrections, which are not demand. Keep deliveries
and issues and sign by `shkzg` (`H` stock out: +, `S`: −, so reversals net out):

```sql
CREATE VIEW goods_movements AS
SELECT matnr AS material_id,
       strptime(budat_mkpf, '%Y%m%d')::DATE AS movement_date,
       CASE shkzg WHEN 'H' THEN menge ELSE -menge END AS quantity
FROM mseg
WHERE mandt = '100' AND bwart IN ('201', '202', '261', '262', '601', '602');
SELECT material_id, round(similarity, 4) AS similarity, history_months
FROM cold_start_analogs('000000000000100300', 3);
-- => 000000000000100200 | 0.6 | 3
-- => 000000000000100100 | 0.1429 | 3
```

KP-50 is the analog: built like KP-65, with three months of deliveries. See
[Predecessors and cold start](03-predecessors-and-cold-start.md) for the parameters.

## Dynamics 365

The D365 functions return the universal header and component rows; `bom_to_items` flattens them.
One BOM per company and item is chosen on `reference_date` (default `9999-12-31`, the newest):
active first, approved first, then the newest `FromDate`:

```sql
CREATE TABLE BOMVersion AS SELECT * FROM read_csv('examples/erp_extract/data/bomversion.csv');
CREATE TABLE BOM AS SELECT * FROM read_csv('examples/erp_extract/data/bom.csv');
CREATE TABLE bom_header AS SELECT * FROM dynamics365_to_bom_header(bom_version := 'BOMVersion');
CREATE TABLE bom_component AS SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion');
SELECT company, parent_material_id, source_bom_id FROM bom_header ORDER BY company, parent_material_id;
-- => demf | PUMP-40 | B-40-DE
-- => usmf | PUMP-40 | B-40-2024
-- => usmf | PUMP-50 | B-50
-- => usmf | PUMP-65 | B-65
```

PUMP-40's 2019 BOM ended in 2023, and PUMP-50's newer draft is neither active nor approved.
`quantity_per` is `BOMQty / BOMQtySerie`: 250 g of paint per 100 pumps is 2.5 g.

`bom_to_items` has no company column. Filter one company first, or the two PUMP-40 become one
parent with the children of both. The new tables replace the SAP ones of the sections above:

```sql
CREATE VIEW bom_header_usmf AS SELECT * FROM bom_header WHERE company = 'usmf';
CREATE OR REPLACE TABLE bom_items AS SELECT * FROM bom_to_items('bom_header_usmf', 'bom_component');
SELECT material_id, round(similarity, 4) AS similarity, shared_components, total_components
FROM find_similar_materials_jaccard('PUMP-65', 5);
-- => PUMP-50 | 0.5 | 3 | 6
-- => PUMP-40 | 0.125 | 1 | 8
```

## Pitfalls

| Symptom | Cause and fix |
|---------|---------------|
| Similarities too high between variants | No STAS: items of every alternative under one BOM number. Export STAS, pass `stas_table` |
| A material twice in `materials`, test BOMs in the results | Two clients. Filter `mandt` in a view; keep one `client` of `sap_to_bom_items` |
| BOMs of all usages mixed | No `bom_usage`: an edge comes from the lowest usage. Pass `bom_usage := '1'` |
| MAKT, MSEG and STPO do not join | Material numbers in two formats (internal `000000000000100100`, ALPHA-converted `100100`). Read as text, pad numeric ones with `lpad(matnr, 18, '0')` |
| Empty `description` | No text in that language; `language` takes `'E'` or `'EN'` |
| Demand too high | Goods receipts in the movements. Filter `bwart`, sign by `shkzg` |
| One D365 parent with two BOMs' children | Two companies flattened together. Filter `company` before `bom_to_items` |
| Quantities in mixed units | `unit` / `quantity_uom` pass through. Jaccard ignores quantities; normalise before you compare quantities |

## Next Steps

- [Material similarity](02-material-similarity.md): Jaccard and WL search on `bom_items`
- [Predecessors and cold start](03-predecessors-and-cold-start.md): `infer_predecessors`, `cold_start_analogs`
- API: [ERP transformations](../api/05-erp-transformations.md) (every SAP and D365 rule)
- Depth: the [SAP tutorials](tutorials/sap/README.md) on a large extract
- [Example](../../examples/erp_extract/README.md): the two scripts with checked numbers
