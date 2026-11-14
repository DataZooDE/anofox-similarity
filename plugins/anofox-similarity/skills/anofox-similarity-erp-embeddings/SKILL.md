---
name: anofox-similarity-erp-embeddings
description: >
  ERP extraction and embedding pipeline for the anofox_similarity DuckDB extension: SAP
  (`sap_to_bom_items`, `sap_to_materials_with_desc`, `extract_material_descriptions`) and
  Dynamics 365 (`dynamics365_to_bom_header`, `dynamics365_to_bom_component`) transforms,
  `bom_to_items` / `items_to_bom`, helpers, MinHash/textual/transactional embeddings,
  `fuse_embeddings` / `compute_fused_embeddings`, statistics and the material_embeddings table.
  Use when loading MARA/MAKT/MAST/STKO/STPO or D365 BOM tables, mapping your own ERP columns,
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

- **Textual embeddings are a placeholder.** No language model ships. `compute_textual_embeddings`,
  `embed_text` and `embedding_backend` return a deterministic *hash* vector for every provider
  (`gemma-local`, `openai`, `anthropic`, `cohere`, even an unknown name); similar texts do **not** get
  similar vectors (`'centrifugal pump'` vs `'centrifugal pumps'`: cosine -0.59). Plumbing only; for
  semantics bring your own model's vectors as a table and fuse (`fuse_embeddings`) or classify with them.
- **Language keys differ by function.** `sap_to_materials_with_desc` defaults to `'E'` (SAP's one-char
  `SPRAS`); `extract_material_descriptions` and `compute_textual_embeddings` default to `'EN'`. On a
  real MAKT, `'EN'` matches nothing and returns **0 rows, no error**: pass `language := 'E'`.
- **SAP dates are `YYYYMMDD` strings.** `sap_to_bom_items` parses `STKO.datuv` and `reference_date` as
  `YYYYMMDD` *or* ISO. `sap_to_materials` parses `MARA.ersda` **only** as `YYYYMMDD` (string or
  integer): an `ersda` already typed `DATE` gives `created_date = NULL`.
- **MAKT duplicates are collapsed.** Several MAKT rows per `(matnr, spras)` (one per client/MANDT)
  become one row per material (an arbitrary one, via `ANY_VALUE`). Filter `mandt` first if it matters.
- **Required columns are stricter than the docs' table sketch.** MARA needs `matnr, mtart, matkl, ersda,
  lvorm`; MAKT needs `matnr, spras, maktx` **and `maktg`** for `extract_material_descriptions` /
  `compute_textual_embeddings`; STPO needs `stlnr, stlkn, idnrk, menge, meins` (`stlkn` is read even
  though it is not output). A missing column is a Binder Error naming it.
- **Transforms emit the universal schema and write nothing** (wrap in `CREATE TABLE ... AS`).
  `sap_to_bom_items` is already flat but names the quantity `qty`, typed like MENGE (DECIMAL); rename
  and cast to `quantity`. The D365 macros emit `bom_header` + `bom_component`: flatten with `bom_to_items`.
- **Only the header is versioned.** `reference_date` and `bom_alternative` select the STKO header (and
  `valid_from`); components are joined by `stlnr` only, so all alternatives' STPO rows under one BOM
  number come through. `bom_usage` is accepted but **ignored** — filter `MAST.stlan` yourself.
- **anofox-forecast is not loaded here.** `compute_transactional_embeddings`, `extract_ts_features`,
  `recompute_embedding_statistics` and `compute_domain_specific_statistics` need `anofox_fcst_ts_features`.
  Check with `check_anofox_forecast_available` first (see the dependency section). It is never auto-installed.
- **No function produces `structural_embedding` (FLOAT[256]).** The fusion default is 0.5/0.5/0
  (structural + textual): bring structural vectors or set `weights_structural := 0`. A positive weight
  on a NULL modality silently drops the material from `compute_fused_embeddings`.
- **Loading the extension writes to a writable database file** (four tables + three HNSW indexes);
  a read-only database gets none, so functions reading them fail there (see the tables section).

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
  ('00000001', '00000001', '000000000000200001', 2.000::DECIMAL(13,3), 'ST'),
  ('00000001', '00000002', '000000000000200002', 1.000::DECIMAL(13,3), 'ST'),
  ('00000002', '00000001', '000000000000200001', 2.000::DECIMAL(13,3), 'ST'),
  ('00000002', '00000002', '000000000000200002', 1.000::DECIMAL(13,3), 'ST'),
  ('00000002', '00000003', '000000000000200003', 1.000::DECIMAL(13,3), 'ST')
) t(stlnr, stlkn, idnrk, menge, meins);
```

## SAP transforms

```text
sap_to_bom_items(mast_table, stko_table, stpo_table,
                 bom_alternative := '01', reference_date := '9999-12-31', bom_usage := NULL)
  → (bom_id, parent_id, child_id, qty, unit, valid_from DATE, valid_to DATE, bom_version)
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
| `STKO.stlnr` / `stlal` / `datuv` | `bom_id` / `bom_version` / `valid_from` | newest `datuv <= reference_date` per `(matnr, stlnr, stlal)` |
| `STPO.idnrk` (trimmed), `menge`, `meins` | `child_id`, `qty`, `unit` | `valid_to` is always 9999-12-31 |
| `MARA.matnr/mtart/matkl/ersda` | `material_id/material_type/material_group/created_date` | rows with `lvorm` not NULL/''/' ' excluded |

```sql
SELECT bom_id, parent_id, child_id, qty, valid_from
FROM sap_to_bom_items('sap_mast', 'sap_stko', 'sap_stpo')
ORDER BY parent_id, child_id;
-- 100001 rows have valid_from 2024-01-01 (newest slice); 100002 has 3 components

-- Point in time: the 2023 view picks the 2020 header slice (same components, see gotchas).
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

## Dynamics 365 transforms

```text
dynamics365_to_materials(invent_table := 'InventTable')          -- reads ItemId, ItemName, ItemType
  → (material_id, material_number, description, material_type, material_group, procurement_type,
     base_uom, weight, cost_per_unit, source_system, is_active, created_at)
     ItemType 0/1/2 → 'ASSEMBLY'/'FINISHED'/'COMPONENT', else 'OTHER'
dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion')
     reads BOMTable(BOMId, ItemId, Status), BOMVersion(BOMId, VersionId, ActivationDate, ApprovedStatus)
  → (bom_id, source_system, source_bom_id, parent_material_id, bom_type, alternative_number,
     revision, base_quantity, base_uom, valid_from, valid_to, plant_id, is_approved, created_at)
     keeps Status = 0; LEFT JOINs every BOMVersion row; is_approved = (ApprovedStatus = 1)
dynamics365_to_bom_component(bom_lines := 'BOM')     -- reads BOMId, LineNum, ItemId, Quantity, ScrapPercent
  → (component_id, bom_id, line_number, child_material_id, quantity_per, quantity_uom,
     is_fixed_quantity, scrap_percent, effective_from, effective_to, component_type,
     supply_type, operation_sequence, is_alternative, alternative_group, created_at)
```

`dynamics365_to_materials` maps the sample below to PUMP-A/PUMP-B
`FINISHED` and SEAL/MOTOR/SENSOR `COMPONENT`. Several
`BOMVersion` rows per `BOMId` duplicate the header (and so the flattened edges): pre-filter
`BOMVersion` to the active version.

```sql
CREATE TABLE InventTable AS SELECT * FROM (VALUES
  ('PUMP-A', 'Pump A', 1), ('PUMP-B', 'Pump B', 1),
  ('SEAL', 'Seal', 2), ('MOTOR', 'Motor', 2), ('SENSOR', 'Sensor', 2)
) t(ItemId, ItemName, ItemType);
```

```sql
CREATE TABLE BOMTable AS SELECT * FROM (VALUES
  ('B1', 'PUMP-A', 0), ('B2', 'PUMP-B', 0), ('B3', 'PUMP-B', 1)   -- B3: Status 1, dropped
) t(BOMId, ItemId, Status);
```

```sql
CREATE TABLE BOMVersion AS SELECT * FROM (VALUES
  ('B1', 'V1', '2024-01-01', 1), ('B2', 'V1', '2024-06-01', 0)
) t(BOMId, VersionId, ActivationDate, ApprovedStatus);
```

```sql
CREATE TABLE BOM AS SELECT * FROM (VALUES
  ('B1', 1, 'SEAL', 2.0, 0.0), ('B1', 2, 'MOTOR', 1.0, NULL),
  ('B2', 1, 'SEAL', 2.0, 0.0), ('B2', 2, 'MOTOR', 1.0, 0.0), ('B2', 3, 'SENSOR', 1.0, 5.0)
) t(BOMId, LineNum, ItemId, Quantity, ScrapPercent);
```

```sql
CREATE TABLE d365_bom_header AS
SELECT * FROM dynamics365_to_bom_header(bom_table := 'BOMTable', bom_version := 'BOMVersion');
-- B1 (PUMP-A, approved) and B2 (PUMP-B, not approved); B3 dropped
```

```sql
CREATE TABLE d365_bom_component AS
SELECT * FROM dynamics365_to_bom_component(bom_lines := 'BOM');
```

## `bom_to_items` / `items_to_bom`

```text
bom_to_items(header_table := 'bom_header', component_table := 'bom_component')
  → (parent_id = parent_material_id, child_id = child_material_id, quantity = quantity_per)
    joined on bom_id; ALL headers (no is_approved filter — add one yourself)
items_to_bom(items_table := 'bom_items', source_system := 'MIGRATED')
  → one UNION ALL of three shapes, discriminated by table_type
    ('materials' | 'bom_header' | 'bom_component'); every other column is VARCHAR and positional
```

```sql
SELECT * FROM bom_to_items(header_table := 'd365_bom_header', component_table := 'd365_bom_component')
ORDER BY parent_id, child_id;
-- PUMP-A: MOTOR, SEAL; PUMP-B: MOTOR, SEAL, SENSOR (quantity is DECIMAL, from Quantity)
-- => PUMP-A | MOTOR | 1.0
-- => PUMP-A | SEAL | 2.0
-- => PUMP-B | MOTOR | 1.0
-- => PUMP-B | SEAL | 2.0
-- => PUMP-B | SENSOR | 1.0
```

`items_to_bom` needs a `quantity` column in `items_table` (NULL → 1). Its output columns are named
after the `materials` branch (`material_id`, `material_number`, ..., `col13`–`col16`), so select by
`table_type` and position, not by name. Header ids are `'BOM_' || parent_id`, component ids
`'COMP_' || parent_id || '_' || n`; units are `'EA'`, `valid_from` is 2024-01-01.

```sql
CREATE TABLE flat_bom AS SELECT * FROM (VALUES ('P-1', 'C-1', 2.0), ('P-1', 'C-2', NULL)) t(parent_id, child_id, quantity);
SELECT table_type, count(*) AS n FROM items_to_bom(items_table := 'flat_bom') GROUP BY table_type ORDER BY table_type;
-- bom_component 2 | bom_header 1 | materials 3
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
  → (material_id, seed INTEGER 0..127, minhash_value FLOAT, num_components INTEGER)  one row per seed
```

`compute_jaccard_embeddings` returns rows and does not write. Aggregate to `FLOAT[128]` and upsert. The share of equal positions
(join two materials on `seed`, average `minhash_value` equality) estimates the Jaccard of the direct
component sets (≈0.65, 83 of 128 positions, for the two sample pumps; true value 2/3). There is **no HNSW index on `jaccard_embedding`** (L2/cosine over
raw MinHash values does not approximate Jaccard); for exact search use `find_similar_materials_jaccard`,
for scale `minhash_index` (see **anofox-similarity-evaluation-scale**). The FLOAT values do not fit
`minhash_similarity`, which takes `UBIGINT[]` signatures.

```sql
INSERT INTO material_embeddings (material_id, jaccard_embedding, num_components)
SELECT material_id, array_agg(minhash_value ORDER BY seed)::FLOAT[128], max(num_components)
FROM compute_jaccard_embeddings(bom_table := 'sap_bom_items')
GROUP BY material_id
ON CONFLICT (material_id) DO UPDATE SET
  jaccard_embedding = EXCLUDED.jaccard_embedding, num_components = EXCLUDED.num_components;
```

## Textual embeddings (placeholder)

```text
compute_textual_embeddings(makt_table := 'sap_makt', language := 'EN',
                           provider := 'gemma-local', provider_config := '')
  → (material_id, textual_embedding FLOAT[] of length 384)
    text = MAKTX || ' ' || MAKTG, one row per material; MAKT needs maktg
embedding_backend(text VARCHAR, provider VARCHAR, provider_config VARCHAR) → FLOAT[] (384)
embed_text(description) → embedding_backend(description, 'gemma-local', '')
```

All return a `FLOAT[]` list (cast to `FLOAT[384]` for the array type; inserting into
`material_embeddings` casts implicitly). `provider_config` (JSON, e.g. `{"api_key": "..."}`) is unused.

```sql
SELECT material_id, len(textual_embedding) AS dims
FROM compute_textual_embeddings(makt_table := 'sap_makt', language := 'E')
ORDER BY material_id;

-- Proof it is not semantic: near-identical texts are not close.
SELECT round(list_cosine_similarity(embed_text('centrifugal pump'), embed_text('centrifugal pumps')), 2) AS near_dupe,
       embed_text('x') = embedding_backend('x', 'gemma-local', '') AS same_backend;
```

Bring your own vectors instead: a table `(material_id, vec FLOAT[384])` from your model goes straight
into `material_embeddings.textual_embedding` (any dimension works for `fuse_embeddings`; the column and
the `hnsw_textual_idx` index want 384).

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
  30 rows, the newest updated within the last 7 days.
- No automatic refresh exists (no triggers in DuckDB; `material_embeddings_dirty` is never filled):
  recompute for the changed materials (`WHERE material_id IN (...)`) and
  `INSERT OR REPLACE INTO material_embeddings (material_id, transactional_embedding, updated_at)`.

```sql
SELECT stat_count, is_fresh FROM check_statistics_freshness();
-- 0 | false
```


## Fusion

```text
fuse_embeddings(structural_embedding FLOAT[], textual_embedding FLOAT[], transactional_embedding FLOAT[],
                weights STRUCT(structural FLOAT, textual FLOAT, transactional FLOAT)) → FLOAT[]
compute_fused_embeddings(weights_structural := 0.5, weights_textual := 0.5, weights_transactional := 0.0)
  → (material_id, combined_embedding FLOAT[] (768), fusion_weights STRUCT(structural, textual, transactional) FLOAT)
```

| Rule | Behaviour |
|---|---|
| Weights | need not sum to 1; normalised (w / sum). `fusion_weights` reports the normalised values |
| Scaling | each modality is multiplied by `sqrt(w)` then concatenated (256 + 384 + 128 = 768); two unit vectors at 0.5/0.5 give a unit vector. Use for cosine / NN only |
| Weight 0 | modality ignored, may be NULL (zero-filled block) |
| Weight > 0 on NULL, or all weights 0 | `compute_fused_embeddings` drops the material; `fuse_embeddings` returns NULL |
| Source | `compute_fused_embeddings` reads the fixed `material_embeddings` table and returns rows; it does not write `combined_embedding` |

```sql
-- Your own vectors (here synthetic): structural FLOAT[256], textual FLOAT[384].
CREATE TABLE my_vectors AS
SELECT material_id,
       [CAST(sin(i * 0.3 + j) AS FLOAT) FOR j IN range(256)]::FLOAT[256] AS struct_vec,
       [CAST(cos(i * 0.2 + j) AS FLOAT) FOR j IN range(384)]::FLOAT[384] AS text_vec
FROM (VALUES ('M1', 1), ('M2', 2), ('M3', 3)) v(material_id, i);

SELECT material_id,
       len(fuse_embeddings(struct_vec, text_vec, NULL,
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

- **Read-only database:** all functions work, but these tables are absent, so `compute_fused_embeddings`
  and `check_statistics_freshness` fail (`Catalog Error: Table with name ... does not exist!`).
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
SET combined_embedding = f.combined_embedding::FLOAT[768], updated_at = now()
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
