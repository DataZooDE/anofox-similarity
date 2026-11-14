# Upgrading

> Changes that need action when you move to a newer release. Newest first.

[API index](API_REFERENCE.md) · [Conventions](api/00-conventions.md)

## v2026.11.13: embeddings as FLOAT arrays, typed universal-schema converters

The first release after v2026.10.23, with the Claude Code plugin.

### Recompute stored Jaccard embeddings

`compute_jaccard_embeddings` now scales each MinHash value to [0, 1) (it stored the raw 64-bit hash
cast to FLOAT, about 1e18). Old and new values are both `FLOAT[128]`, so mixing them is not an
error, but `minhash_similarity(old, new)` is 0.0 even for identical component sets. Recompute
every stored vector:

```sql
UPDATE material_embeddings SET jaccard_embedding = NULL;
-- then re-run your upsert from compute_jaccard_embeddings (docs/api/04-embeddings.md)
```

### Embedding types

| Function | Before | Now |
|---|---|---|
| `embed_text`, `embedding_backend`, `compute_textual_embeddings` | `FLOAT[]` | `FLOAT[384]` |
| `fuse_embeddings` arguments | `FLOAT[]`, `FLOAT[]`, `FLOAT[]` | `FLOAT[256]`, `FLOAT[384]`, `FLOAT[128]` (lists of these lengths cast in) |
| `fuse_embeddings`, `compute_fused_embeddings` result | `FLOAT[]` | `FLOAT[768]` |
| `compute_jaccard_embeddings.minhash_value` | raw hash as FLOAT | FLOAT in [0, 1) |
| `compute_jaccard_embeddings.seed` | INTEGER | BIGINT |

The array types are what DuckDB `vss` indexes and what `array_cosine_distance` and its siblings
take; `list_cosine_similarity` and other list functions still accept them. Two behaviour changes:

- A modality vector of the wrong length is now a cast error for the whole query
  ("Cannot cast list with length 2 to array with length 256"), not a NULL row. Filter or fix such
  rows first (`WHERE len(structural_embedding) = 256`).
- On DuckDB 1.5.6 a `CASE` over an array fails ("Unimplemented type for case expression"). Cast
  inside and back: `(CASE WHEN c THEN embed_text(t)::FLOAT[] END)::FLOAT[384]`.

### Other changes

| Was | Now |
|---|---|
| `items_to_bom(items, source)` with a `table_type` column | `items_to_materials(items, source)`, `items_to_bom_header(items, source)`, `items_to_bom_component(items)`, each in the universal table's columns and types |
| `items_to_bom` header `valid_from` 2024-01-01, materials `description` '' | NULL |
| `embedding_backend(..., '<any name>', ...)` returned the placeholder | only `gemma-local`, `openai`, `anthropic`, `cohere` (case-insensitive); any other name is an error |
| `embedding_backend` `provider_config` parsed an `api_key` | not read; remove keys from your SQL |
| `embed_text` / `embedding_backend` placeholder: a ramp from `std::hash` (at most 1000 distinct vectors, different per platform) | a 64-bit hash per element, the same on every platform; recompute stored textual placeholder vectors |
| `items_to_bom` quantity below 0.0000005 stored as 0 | `items_to_bom_component` raises an error; scale to a larger unit |
| `tree_class_multiset(..., attrs_table := ...)` (ignored) | the parameter is gone: drop it |
| `value_satisfies` / `tree_coverage` with `soft_band := 0`: `within` and `point` scored the covered share (380–480 V vs 400–480 V: 0.78) | strict like the one-sided bounds: 1 when covered within `tol`, else 0; hard-limit coverages can drop |
| `aas_issues`: a text value without semanticId was dropped silently | reported as `unmapped_text` (info); filter it out if you count issues yourself (`aas_quality.n_issues` already leaves it out) |
| `find_similar_materials_jaccard`, `find_similar_materials_wl_kernel`, `cold_start_analogs` filled `k` rows with materials scoring 0 | those are left out unless you pass `min_similarity := 0`; fewer than `k` rows means nothing else shares a component |
| `check_statistics_freshness` on empty statistics: `last_updated` = now − 8 days | NULL |
| `dynamics365_to_materials`, `dynamics365_to_bom_header`, `dynamics365_to_bom_component` read a column contract that is not D365's (`BOMTable(BOMId, ItemId, Status)`, `BOMVersion(BOMId, VersionId, ActivationDate, ApprovedStatus)`, `BOM(..., Quantity, ScrapPercent)`), one header per BOM, ItemType 0/1/2 as ASSEMBLY/FINISHED/COMPONENT | native D365 columns (table or OData entity names): the item comes from `BOMVersion.ItemId`, `ActivationDate` → `FromDate` (+ `ToDate`), `ApprovedStatus` → `Approved` (+ `Active`), `Quantity` → `BOMQty` (+ `BOMQtySerie`, `UnitId`), `ScrapPercent` → `ScrapVar`; `Status` and `VersionId` are not read. One header per **item** on `reference_date`, `bom_id` = `<item>/<BOMId>`; `dynamics365_to_bom_component` takes `bom_version` (and `reference_date`) to pick the same BOMs; arguments are named (`bom_version :=`, `bom_lines :=`); ItemType 0/1/2 → ITEM/BOM/SERVICE; typed columns (no untyped NULL) |
| `sap_to_bom_items` joined MAST, STKO and STPO by `stlnr` only | uses `stlty` (material BOMs only), MAST `stlal`, header and item versions (`datuv`, `stkoz`, `stlkn`, `stpoz`) and `lkenz`/`loekz` when the columns exist, so document BOM items, other alternatives' headers, deleted headers and items, and future item versions are gone; `bom_usage` now filters `MAST.stlan`; new `stas_table := 'STAS'` applies the item selection. Each `(parent_id, child_id)` comes once (lowest usage, then BOM number; quantities of its positions added per unit), so `qty` is a sum and the row count drops (test extract: 14.0 M → 4.7 M). Re-run it and rebuild the tables made from it |

Fixed without action on your side:

- `sap_to_materials` fills `created_date` when `ersda` is a DATE (it was NULL).
- `sap_to_bom_items` no longer reads STPO `stlkn`, so an STPO extract with only `stlnr, idnrk,
  menge, meins` works.
- `cosine_floor` names its parameters `table_name`, `column_name`; they are positional, and the
  old names could not be used as named arguments.
- On a read-only database `check_statistics_freshness` returns `stat_count = 0` instead of a
  Catalog Error, and `compute_transactional_embeddings` says that the table is missing.
- `find_covering_entities_lsh` in a join works on DuckDB 2.0 (it raised an INTERNAL error).

New, nothing to change: the material searches take many materials in one query.
`find_similar_materials_jaccard(k, materials_table := 'tbl')` (and the WL kernel,
`cold_start_analogs(k, materials_table := ...)`, `infer_predecessors(materials_table := ...)`)
search every `material_id` of the table and return `query_id` in front of the usual columns.
Single-id calls keep their columns and results.

Migrating from `items_to_bom`:

```sql
-- before: SELECT * FROM items_to_bom('bom_items', 'MIGRATED') WHERE table_type = 'bom_header'
CREATE TABLE bom_header AS SELECT * FROM items_to_bom_header('bom_items', 'MIGRATED');
CREATE TABLE bom_component AS SELECT * FROM items_to_bom_component('bom_items');
CREATE TABLE materials AS SELECT * FROM items_to_materials('bom_items', 'MIGRATED');
```
