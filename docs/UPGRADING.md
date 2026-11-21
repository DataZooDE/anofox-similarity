# Upgrading

> Changes that need action when you move to a newer release. Newest first.

[API index](API_REFERENCE.md) · [Conventions](api/00-conventions.md)

## Unreleased: no name discount without name vectors, SAP language keys in either form

Coverage scores rise where attribute names were compared without `name_vectors`; nothing else changes. Check these:

| Before | Now |
|---|---|
| `mu` (default 0.2) discounted every value match by the name similarity; without `name_vectors` that was token overlap, so a German offer name against an English request name cost 20 % of the attribute (`tree_coverage('Q1', k := 3)` on the mini fixture: S1 0.858 with 0 gaps) | `mu` discounts only name-vector agreement (`name_rule` `vector`). Without vectors the names still choose which offered value answers a requirement, but do not lower its score (S1 1.0). Same as passing `mu := 0.0` before; drop that argument if you only used it for this. With `name_vectors` nothing changes. `fit_coverage_weights` over `mu` without vectors now finds every `mu` equal |
| `extract_material_descriptions` and `compute_textual_embeddings` defaulted to `language := 'EN'`, which matched no SAP `MAKT.spras` (`'E'`): 0 rows, no error. `language := 'E'` did not match an extract with ISO codes (`spras = 'EN'`) | all three MAKT readers (also `sap_to_materials_with_desc`) default to `'E'` and match the SAP key and the ISO code in either direction (`'E'`/`'EN'`, `'D'`/`'DE'`, ... for 23 common languages, case-insensitive). Other codes match only literally |

## v2026.11.21: sourcing bounds, tolerances as spans, closest value in the trace, unknown designations

Scores change for requirements written with a tolerance (`20 kg ± 1 kg`); otherwise only parser output and trace text. Check these:

| Before | Now |
|---|---|
| `parse_quantity('3.1 acc. to EN 10204')` = 3.1 with unit `acc`, `'420 x 310 x 180 mm'` / `'120x80 mm'` = 420 / 120 with unit `x` / `x80` (dimension NULL in all) | empty (`lo`, `unit` NULL): an unknown word followed by another number is part of a designation or a box (also `'Klasse 1 nach DIN 4102'`, `'12 furlongs per 2 weeks'`). `'12 furlongs'` alone is unchanged. Re-parse stored values if you count unparsed rows by `lo IS NULL` |
| `parse_quantity('20 kg ± 1 kg')` = 20 kg equals (`'220 N ± 2 %'` = 220 N, the tolerance dropped) | `equals` with lo 19, hi 21 (215.6–224.4 N): a requirement now accepts an offer inside the drawing's tolerance (plus `tol`) and scores the farther end outside it. Scores of `±` requirements change (benchmark MAE 0.0358 → 0.0361); `± 0,05 mm` without a nominal is unchanged (`at_most`). Pass `tol := 0` to check the drawing's tolerance only. In an offer the span counts as a range (`at_least` uses its high end). `50 % ± 5 %` is 45–55 % (percentage points); `max. 20 kg ± 1 kg` is `at_most` 21 kg; glued `20kg±1kg` and `+-` read like `±`. `trees_to_aas` writes the span as a Range with `levelType` nom (not for an unknown unit), and `aas_to_trees` reads that as `equals` |
| `parse_quantity('€ 40 pro 2 Stück')` = 40 with unit `pro` (no dimension) | 40 EUR (a currency before the number now wins over an unknown word after it) |
| on equal scores `tree_coverage_trace` showed the first offered value (`EN10204-2.2` against a required 3.2, also offered 3.1) | the nearest one (smallest `distance`). Categories that fail: the one meeting most levels in `rules_table`; that pass: the exact match, else the lowest level that is enough. Scores and gaps are the same; `prov_value` and `distance` of such rows, and `aas_coverage_result` built from them, can change |
| a template's bound only filled an empty bound | unchanged by default. New: a `bound_policy` column on the templates table, `'override'` replaces a stated `at_least`/`at_most` on one value (`bound_source` `template_override`) |
| trace values in base units only (`3.2e-06 m`) | unchanged by default. New: an attributes-table column `source_unit` (from `parse_quantity`) shows the written unit (`3.2 µm`) for built-in units; leave it NULL for units your `unit_table` redefines |

Fixed without action on your side: `trees_to_aas` on a tree from `tree_from_json` with a `role` and an item two levels below the root raised "cannot create std::vector larger than max_size()".

## v2026.11.14: embeddings as FLOAT arrays, native SAP and Dynamics 365 readers, typed universal-schema converters

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
| `dynamics365_to_materials`, `dynamics365_to_bom_header`, `dynamics365_to_bom_component` read a column contract that is not D365's (`BOMTable(BOMId, ItemId, Status)`, `BOMVersion(BOMId, VersionId, ActivationDate, ApprovedStatus)`, `BOM(..., Quantity, ScrapPercent)`), one header per BOM, ItemType 0/1/2 as ASSEMBLY/FINISHED/COMPONENT | D365 table field names (map an OData entity export with a view, see docs/api/05): the item comes from `BOMVersion.ItemId`, `ActivationDate` → `FromDate` (+ `ToDate`), `ApprovedStatus` → `Approved` (+ `Active`), `Quantity` → `BOMQty` (+ `BOMQtySerie`, `UnitId`), `ScrapPercent` → `ScrapVar`; `Status` and `VersionId` are not read. One header per **company and item** on `reference_date`, `bom_id` = `<item>/<BOMId>` (`<company>/<item>/<BOMId>` with `DataAreaId`; `/` and `\` inside a segment escaped with `\`), `component_id` = `COMP/<bom_id>/<child>/<unit>/constant\|variable`; a `company` column (lower-cased `DataAreaId`) on all three outputs; constant and variable lines of a child stay apart; `dynamics365_to_bom_component` takes `bom_version` (and `reference_date`) to pick the same BOMs; arguments are named (`bom_version :=`, `bom_lines :=`); ItemType 0/1/2 → ITEM/BOM/SERVICE; typed columns (no untyped NULL) |
| universal `materials`, `bom_header`, `bom_component` and the `items_to_*` converters | a `company VARCHAR` column at the end (NULL for single-company data); tables you created earlier need `ALTER TABLE materials ADD COLUMN company VARCHAR` (and the same for `bom_header`, `bom_component`) before `INSERT ... SELECT * FROM` a converter |
| `sap_to_bom_items` joined MAST, STKO and STPO by `stlnr` only | uses `stlty` (material BOMs only), MAST `stlal`, header and item versions (`datuv`, `stkoz`, `stlkn`, `stpoz`) and `lkenz`/`loekz` when the columns exist, so document BOM items, other alternatives' headers, deleted headers and items, and future item versions are gone; `bom_usage` now filters `MAST.stlan`; new `stas_table := 'STAS'` applies the item selection. Each `(parent_id, child_id)` comes once (lowest usage, then BOM number; quantities of its positions added per unit), so `qty` is a sum and the row count drops (test extract: 14.0 M → 4.7 M). A new last column `client` (MANDT, or NULL) keeps SAP clients apart; `qty` of a DECIMAL MENGE is DECIMAL(38, scale). Re-run it and rebuild the tables made from it |

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
