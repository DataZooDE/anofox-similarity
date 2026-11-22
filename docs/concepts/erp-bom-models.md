# ERP BOM Models

> How SAP and Dynamics 365 store bills of materials, which rules turn their tables into the BOM
> edges the searches read, and where consumption comes from. For the calls, see
> [guide 12](../guides/12-from-erp-extract.md) and [api/05](../api/05-erp-transformations.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## The Target: Edges

The similarity searches read flat edges: `bom_items(parent_id, child_id, quantity)`, one row per
parent and direct component. An ERP does not store that. It stores a BOM with a header and items,
in versions, per alternative, usage, plant and client or company. Converting means choosing one
BOM per material and date, and only its valid items. A wrong choice does not fail: it returns
plausible edges of a BOM that does not exist.

| Concept | SAP | Dynamics 365 |
|---|---|---|
| Material → BOM | MAST (`matnr` → `stlnr`, per plant `werks`, usage `stlan`, alternative `stlal`) | BOMVersion (`ItemId` → `BOMId`) |
| BOM header | STKO (per `stlnr` and `stlal`, versioned) | BOMTable (`BOMId`, `SiteId`) |
| BOM items | STPO (per `stlnr`, item node `stlkn`, versioned) | BOM lines (`BOMId`, `ItemId`, `BOMQty`) |
| Which item in which alternative | STAS | one `BOMId` per version: no shared items |
| Validity | `datuv` on header, item and selection versions | `FromDate`/`ToDate` on version and line |
| Deleted | `lkenz` (and `loekz` on STKO) | not deleted: not `Active`, not `Approved` |
| Tenant | client `mandt` | company `DataAreaId` |

## SAP: One BOM Number, Several Alternatives

SAP numbers BOMs per category. A material BOM (`stlty = 'M'`) can share its number `stlnr` with a
document BOM (`'D'`), so only material BOMs are read. A material can have several alternatives
(`stlal` 01, 02, …), for example grey iron and stainless casings. The alternatives share **one**
BOM number and one pool of item nodes; STAS says which nodes belong to which alternative. Without
STAS every item of the number is returned, whichever alternative it belongs to.

In the example extract, KP-50 (`stlnr` 00000002) has five item nodes; alternative 01 selects four,
alternative 02 another four:

```sql
CREATE OR REPLACE TABLE mast AS SELECT * FROM read_csv('examples/erp_extract/data/mast.csv', all_varchar = true);
CREATE OR REPLACE TABLE stko AS SELECT * FROM read_csv('examples/erp_extract/data/stko.csv', all_varchar = true);
CREATE OR REPLACE TABLE stpo AS
SELECT * REPLACE (menge::DECIMAL(13, 3) AS menge) FROM read_csv('examples/erp_extract/data/stpo.csv', all_varchar = true);
CREATE OR REPLACE TABLE stas AS SELECT * FROM read_csv('examples/erp_extract/data/stas.csv', all_varchar = true);

SELECT stlal AS alternative, list(stlkn ORDER BY stlkn) AS item_nodes
FROM stas WHERE mandt = '100' AND stlnr = '00000002' GROUP BY ALL ORDER BY ALL;
-- => 01 | [00000001, 00000002, 00000003, 00000004]
-- => 02 | [00000001, 00000002, 00000003, 00000005]
```

So alternative 01 has four components with STAS and five without: the stainless casing of
alternative 02 leaks in. A Jaccard search on the leaked BOM scores KP-65 against KP-50 0.8
instead of 0.6.

```sql
SELECT (SELECT count(*) FROM sap_to_bom_items('mast', 'stko', 'stpo', bom_usage := '1')
        WHERE client = '100' AND parent_id = '000000000000100200') AS without_stas,
       (SELECT count(*) FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1')
        WHERE client = '100' AND parent_id = '000000000000100200') AS with_stas;
-- => 5 | 4
```

## SAP: Versions on a Date

Headers, items and selections are versioned. Each version has a start date `datuv` and a counter
(`stkoz`, `stpoz`, `stasz`); a change creates a new version instead of overwriting the old one. On
a reference date the newest version that has started counts, and if that version carries the
deletion flag `lkenz`, the header, item or selection is gone from that date on. KP-40's item node 5
has a 2019 version and a 2023 version with `lkenz` set:

```sql
SELECT (SELECT count(*) FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1')
        WHERE client = '100' AND parent_id = '000000000000100100') AS today,
       (SELECT count(*) FROM sap_to_bom_items('mast', 'stko', 'stpo', stas_table := 'stas', bom_usage := '1',
                                              reference_date := '2022-06-30')
        WHERE client = '100' AND parent_id = '000000000000100100') AS in_2022;
-- => 4 | 5
```

Usage (`stlan`: 1 production, 2 engineering, 3 universal, …) and plant (`werks`) select among
several BOMs of a material. A material with the same BOM in several plants is one set of edges;
without `bom_usage` an edge is taken from the lowest usage, so the result can mix usages.

## Clients and Companies

An SAP system holds several clients (`mandt`: production, test, training) in the same tables. A
D365 database holds several legal entities (`DataAreaId`). Both are part of a row's identity: the
same material number in two clients or companies is two materials. The readers keep them apart
(`client`, `company` columns) and never join rows across them; the searches do not read these
columns, so filter one client or company before searching.

## Dynamics 365: Versions Choose a BOM

In D365 a BOM (`BOMId`) is a list of lines; BOMVersion says which BOM an item uses, from when to
when, and whether that version is active and approved. One item can have an expired version, a
current one and a draft. On the reference date the valid versions compete: active first, approved
first, then the newest `FromDate`:

```sql
CREATE OR REPLACE TABLE BOMVersion AS SELECT * FROM read_csv('examples/erp_extract/data/bomversion.csv');
CREATE OR REPLACE TABLE BOM AS SELECT * FROM read_csv('examples/erp_extract/data/bom.csv');
SELECT (SELECT source_bom_id FROM dynamics365_to_bom_header(bom_version := 'BOMVersion')
        WHERE company = 'usmf' AND parent_material_id = 'PUMP-40') AS today,
       (SELECT source_bom_id FROM dynamics365_to_bom_header(bom_version := 'BOMVersion', reference_date := '2022-06-30')
        WHERE company = 'usmf' AND parent_material_id = 'PUMP-40') AS in_2022;
-- => B-40-2024 | B-40-2019
```

PUMP-50's newer draft (neither active nor approved) loses to its approved BOM. A line's quantity is
per `BOMQtySerie` parents: 250 g of paint per 100 pumps is 2.5 g per pump:

```sql
SELECT child_material_id, quantity_per, quantity_uom
FROM dynamics365_to_bom_component(bom_lines := 'BOM', bom_version := 'BOMVersion')
WHERE child_material_id = 'PAINT-RAL5015';
-- => PAINT-RAL5015 | 2.500000 | g
```

D365 returns universal header and component rows (`bom_header`, `bom_component`), because a BOM
can serve several items and carries more than edges; `bom_to_items` flattens them. One version per
company and item is a simplification: versions per site, from-quantity or configuration are not
told apart.

## Consumption From MSEG

Predecessor and cold-start functions read consumption: what left stock to production or to a
customer. MSEG (on S/4HANA `MATDOC`) holds every goods movement. The movement type `bwart` says
what happened (101 goods receipt, 261 issue to an order, 201 to a cost centre, 601 delivery; the
even type after each is its reversal), and the debit/credit flag `shkzg` says the direction (`H`
stock out, `S` stock in). Consumption is the issues and deliveries, signed by `shkzg`, so that
reversals net out; receipts, transfers and stock corrections are not demand. The posting date is
in MKPF (`BUDAT`), copied to MSEG as `BUDAT_MKPF`. Guide 12 has the view.

## Why These Rules

| Rule | Why |
|---|---|
| Material BOMs only (`stlty = 'M'`) | Document and class BOMs share numbers but are not products' parts |
| STAS when present | Alternatives share item nodes; without the selection they mix |
| Newest started version, deleted from then on | That is SAP's own reading of change numbers on a date |
| `reference_date` default `9999-12-31` | The newest state, which is what a similarity search usually compares |
| Clients and companies never merged | The same number in two tenants is two materials |
| D365: active, approved, newest | An item's current production BOM is the one planners use |

The design history (the explosion algorithms and their trade-offs) is in the
[SAP](../background/SAP_BOM_Explosion_Technical_Memo.md) and
[Dynamics 365](../background/Microsoft_Dynamics_365_BOM_Explosion_Technical_Memo.md) memos; they
record how the rules were derived, not the current contract.
