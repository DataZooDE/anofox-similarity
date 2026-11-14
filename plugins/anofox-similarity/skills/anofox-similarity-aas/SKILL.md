---
name: anofox-similarity-aas
description: >
  Asset Administration Shell (AAS v3) support of anofox_similarity: read JSON / XML / AASX into
  property trees (`aas_to_trees`, `aas_issues`, `aas_quality`, `aas_class_hierarchy`), dictionary
  keys (`semantic_key`, `aas_id_b64url`), value queries (`attrs_satisfying`), enrichment
  (`suggest_semantic_id`) and writing back (`trees_to_aas`, `aas_export`, `aas_coverage_result`,
  `coverage_result_to_aas`). Use when the user asks "load these AAS / AASX files", "match offers
  by ECLASS IRDI", "which assets deliver 7.6 A", "audit the supplier's AAS", "export as AAS".
version: 2026.11.13
user-invocable: false
---

# Anofox Similarity — Asset Administration Shell (AAS) Cheat Sheet

**Extension:** `anofox_similarity` v2026.11.13 | **DuckDB:** v2.0 / v1.5.6 | **Reference:** [`docs/api/17-aas.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/17-aas.md), [`docs/guides/10-aas-profile.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/guides/10-aas-profile.md), [`docs/aas/index.md`](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/aas/index.md)

> Paths like `test/data/...` are relative to a clone of the anofox-similarity repository; in
> your own project, substitute your tables. Examples that build inline tables run anywhere.

AAS v3 standardises the envelope and dictionary-coded properties (semanticId → ECLASS / IEC CDD);
IDTA-02020 marks capabilities *Required* / *Offered*. Nothing in AAS says how to match the two.
This skill covers the bridge: AAS in → property trees (`tree_nodes`, `tree_attributes`) → the
coverage kernel (see **anofox-similarity-coverage** for `tree_coverage` / `tree_coverage_trace`)
→ trees and gap lists back out as AAS. Plus audit, quality report and dictionary-id suggestions.

## Critical gotchas

- **`aas_to_trees` is strict, `aas_issues` never raises.** One broken file stops an
  `aas_to_trees` over a folder (`AAS JSON: ... parse error ...`). Audit with `aas_issues` first:
  a broken file gives one `parse_error` / `error` row naming it.
- **Use `read_blob` for AASX** (zip). `read_blob` also reads JSON and XML, so `read_blob('dir/*')`
  loads a mixed folder; `read_text` only for the text formats. Format is sniffed from the bytes.
- **The functions return a LIST of node STRUCTs**, not a table. Unnest once into a raw table, then
  split: nodes = `n.* EXCLUDE (attributes)`, attributes = unnest of `n.attributes` with
  `recursive := true`. The kernel's default table names are `tree_nodes` / `tree_attributes`.
- **`attr_key` is stored as written (with release `#008`)**; compare via `semantic_key`, which
  strips the version. `class_id` and enumeration `category` are stored already canonical.
- **ECLASS unit IRDIs resolve only with your unit list** (`unit_table`, a LIST of STRUCT
  `(symbol, unit_id)`, e.g. built from your licensed ECLASS list). Without it every unitId gives
  `unknown_unit_id` and the `unit` text is used. No ECLASS content ships with the extension.
- **A tree id is the shell id** unless you pass `tree_id`. With `tree_id`, further trees in the
  same input (more shells, orphan submodels) become `tree_id/idShort` (`tree_id_suffixed` info).
- **Values are in base units**: 45 °C reads as `318.15` K, 1 ms as `0.001` s. `value_text` keeps
  the text as written.
- **`attrs_satisfying` scans every tree in `attrs_table`, including the request tree** — filter it
  out (`WHERE tree_id <> 'RFQ-...'`). Soft by default: use `tol := 0, soft_band := 0` for hard limits.
- **Writing files: one row = one file.** Several environments concatenated are not valid JSON.
  Write with `COPY ... (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f')`.
- **Inspect written AAS with `aas_to_trees`** (round-trip into trees) rather than parsing the JSON
  by hand; `json_extract_string` works too where the `json` extension is available.
- **CoverageResult ids default to `https://anofox.com/aas/results/`.** Set your own `id_prefix`
  before publishing.
- Table-function args must be constants: pass a list of candidates with `SET VARIABLE` +
  `getvariable()`, not a subquery.

## Setup used by the examples below

The RFQ (IDTA-02020 Required capabilities, JSON) and four supplier offers (JSON, XML, AASX) from
`test/data/aas/scenario/`.

```sql
CREATE TABLE aas_raw AS
SELECT unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n
FROM read_text('test/data/aas/scenario/rfq_2026_117.aas.json')
UNION ALL
SELECT unnest(aas_to_trees(content, NULL,
           (SELECT list(u) FROM read_csv('test/data/aas/scenario/eclass_units.csv') u)))
FROM read_blob('test/data/aas/scenario/offers/*');
```

```sql
CREATE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM aas_raw;
```

```sql
CREATE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM aas_raw;
```

## `aas_to_trees` — read AAS into property trees

```text
aas_to_trees(content VARCHAR|BLOB [, tree_id VARCHAR [, unit_table LIST]])
  → LIST(STRUCT(tree_id, node_id, parent_id, node_kind, edge_type, label, context, class_id,
                count DOUBLE, source,
                attributes LIST(STRUCT(tree_id, node_id, attr_id, name, value_text, dimension,
                    unit, lo DOUBLE, hi DOUBLE, role, bound, qualifier, category, attr_key,
                    bound_source))))
```

Same node STRUCTs as `tree_from_json`, so `validate_trees`, `tree_coverage`, `tree_coverage_trace`
work unchanged. NULL input → NULL. Inputs: Part 1 JSON (Environment, a single Submodel as from
`GET /submodels/{id}`, or a Part 2 paged `{"result": [...]}`), Part 1 XML (`<environment>` or
`<submodel>`, namespace 3.0/3.1), AASX (follows `aasx-origin → aas-spec`; spec part XML or JSON).

| AAS | Tree |
|---|---|
| AssetAdministrationShell | root container, `node_id` = tree id |
| Submodel / SMC / SML / Entity holding values directly | item, `class_id` = `semantic_key(semanticId)` |
| ... holding no values directly | container |
| IDTA-02020 `CapabilityContainer` | item; `class_id` = Capability's supplementalSemanticId; attribute `role` from `CapabilityRoleQualifier` (Required → `required`, Offered → `provided`) |
| `CapabilityComposedOf` | part moves under the composite (AND) |
| `CapabilityGeneralizedBy` | not a node: a row of `aas_class_hierarchy` |
| `Property` number | point attribute (`lo = hi`) |
| `Range` | min only → `bound = 'at_least'`, max only → `'at_most'`, both → `'within'` |
| IEC 61360 `levelType` min / max / nom | `at_least` / `at_most` / `equals` |
| IDTA-02020 `BasicConstraint` (`<= 1 ms`) | bound + limit from the expression |
| Property with `valueId` | `category` = `semantic_key(valueId)` |
| `xs:boolean` | `category` = `'true'` / `'false'` |
| `xs:string` code (`IP65`, `EtherCAT`) | `category` = text; `"24 V DC"` parsed as a quantity |
| SubmodelElementList of values | one attribute per value (`Protocol[0]`, `Protocol[1]`) |
| MultiLanguageProperty, File, Blob, references, Operation, events | not mapped (a text or MultiLanguageProperty without any semanticId → `aas_issues` `unmapped_text`) |

- `node_id` = idShort path (`CapabilityDescription.CapabilitySet.XContainer`, `[i]` for lists);
  `context` = parent path. `attr_id` = idShort (PropertyContainer's for IDTA-02020).
- `name` = IEC 61360 English preferredName, else idShort in words (`RatedCurrent` → `Rated current`).
- `attr_key` = semanticId as written; for an IDTA-02020 template id (`.../CapabilityPropertyType/...`)
  the first supplementalSemanticId.
- Units: element's IEC 61360 data spec first, completed from the ConceptDescription (exact
  release first; another release only if all releases agree on the unit). `unitId` before `unit`.
- `bound_source`: `range`, `level_type`, `constraint`, `text`, `explicit` (`tree_from_json`).
- Text is not a number: product class ids, versions (`15.0`), dates stay out.
- DataZoo profile (variants, counts): qualifier `anofox:VariantGroup` → `node_kind =
  'variant_group'` with `OR` children; qualifier `anofox:Count` or a Property with semanticId
  `https://anofox.com/aas/Count/1/0` → node `count` (multiplies along the path).

```sql
-- the RFQ's required properties: keys, base units, bounds
SELECT attr_id, attr_key, dimension, lo, hi, bound, category, role
FROM tree_attributes WHERE tree_id = 'RFQ-2026-117' ORDER BY attr_id;
```

```sql
-- the same offer as JSON, XML and AASX gives the same nodes
SELECT 'json' AS f, count(*) FROM (SELECT unnest(aas_to_trees(content)) FROM read_text('test/data/aas/scenario/formats/lenvaro_lx8.aas.json'))
UNION ALL SELECT 'xml', count(*) FROM (SELECT unnest(aas_to_trees(content)) FROM read_text('test/data/aas/scenario/formats/lenvaro_lx8.aas.xml'))
UNION ALL SELECT 'aasx', count(*) FROM (SELECT unnest(aas_to_trees(content)) FROM read_blob('test/data/aas/scenario/formats/lenvaro_lx8.aasx'));
```

Errors (malformed input raises, with the reader's message):

```text
SELECT aas_to_trees('{"submodels": [');          -- AAS JSON: ... parse error ...
SELECT aas_to_trees('<environment><submodels>'); -- AAS XML: ...
SELECT aas_to_trees('Manufacturer: Lenvaro');    -- AAS: expected JSON, XML or an AASX package (zip)
```

## `aas_issues` — audit findings (never raises)

```text
aas_issues(content [, tree_id [, unit_table]])
  → LIST(STRUCT(tree_id, path, issue, severity, detail))
```

Same arguments and reading as `aas_to_trees`; `path` is the idShort path, submodel first.

| issue | severity | meaning |
|---|---|---|
| `parse_error` | error | not readable AAS (empty input too) |
| `bad_value` | warning | value does not parse as its valueType (`"n/a"` in `xs:double`, `"1,500"`) — kept as a gap |
| `empty_value` | warning | numeric Property / Range without value — a gap |
| `decimal_comma` | info | `"6,0"` read as 6.0 |
| `unmapped_text` | info | text Property (no code, no quantity) or MultiLanguageProperty without any semanticId: left out of the tree. The candidates for `suggest_semantic_id`; not counted in `aas_quality.n_issues` |
| `missing_concept_description` | warning | no dictionary entry: unit unknown (`validate_trees` → `unknown_unit`) |
| `ambiguous_concept_description` | warning | no CD for this release, others disagree on unit |
| `unknown_unit_id` | warning | unitId not in `unit_table`; unit text used |
| `no_unit_information` | warning | number without semanticId or data spec |
| `assumed_dimensionless` | info | dictionary entry has no unit: a count |
| `duplicate_id_short` | warning | second sibling kept as `X~2` |
| `tree_id_suffixed`, `missing_submodel` | info | several trees with a given `tree_id`; shell references an absent submodel |
| `no_shell_or_submodel`, `malformed_member` | warning | empty input, wrong member type |
| `capability_missing`, `unresolved_constraint`, `unresolved_relation`, `constraint_conflict`, `composition_cycle` | warning | IDTA-02020 structure problems |
| `unparsed_constraint`, `unclassed_capability`, `multiple_composition` | info | IDTA-02020 notes |
| `value_in_variant_group`, `bad_count` | — | profile: value directly in a variant group (ignored); non-positive count (1 used) |

```sql
-- audit a folder: the truncated file shows up as parse_error instead of aborting
SELECT parse_filename(filename) AS file, i.issue, i.severity, i.detail
FROM (SELECT filename, unnest(aas_issues(content)) AS i
      FROM read_blob('test/data/aas/fixtures/mixed_folder/*'))
ORDER BY i.severity, file;
```

```sql
-- with the unit list: only the real findings remain
SELECT split_part(i.tree_id, '/', 3) AS supplier, i.path, i.issue, i.severity
FROM (SELECT unnest(aas_issues(content, NULL,
          (SELECT list(u) FROM read_csv('test/data/aas/scenario/eclass_units.csv') u)), recursive := true)
      FROM read_blob('test/data/aas/scenario/offers/*')) i
ORDER BY ALL;
```

```sql
-- uncoded text values: what a mapping review or suggest_semantic_id should look at
SELECT i.path, i.detail
FROM (SELECT unnest(aas_issues(content)) AS i
      FROM read_text('test/data/aas/scenario/enrichment/nordwerk_nd5.aas.json'))
WHERE i.issue = 'unmapped_text' AND i.path LIKE '%Feldbus%';
-- => TechnicalData.TechnicalProperties.Feldbusschnittstelle | Feldbusschnittstelle = 'EtherCAT' has no semanticId: kept out of the tree (descriptive text); give it a dictionary id (suggest_semantic_id) if it should be compared
```

## `aas_quality` — per-submodel data quality report

```text
aas_quality(content [, unit_table])
  → LIST(STRUCT(tree_id, submodel_id, submodel, kind, n_values, n_with_semantic_id,
                semantic_id_share DOUBLE, n_missing_concept_description, n_unresolved_units,
                n_unparseable, n_mixed_releases, n_issues))
```

One entry per submodel; no coverage kernel. `n_values` counts Property, Range,
MultiLanguageProperty (list members count with the list's key). `n_mixed_releases` = concepts
referenced in more than one dictionary release anywhere in the input. NULL input → NULL.

```sql
SELECT parse_filename(filename) AS file, q.submodel, q.n_values, q.n_with_semantic_id,
       round(q.semantic_id_share, 3) AS share, q.n_missing_concept_description,
       q.n_unresolved_units, q.n_unparseable, q.n_mixed_releases
FROM (SELECT filename, unnest(aas_quality(content)) AS q
      FROM read_blob(['test/data/aas/scenario/enrichment/nordwerk_nd5.aas.json',
                      'test/data/aas/scenario/offers/*']))
ORDER BY file, q.submodel;
-- nordwerk_nd5: 10 values, share 0.0 (no semanticIds at all) -> candidate for suggest_semantic_id
```

## `semantic_key` — canonical dictionary key

```text
semantic_key(id VARCHAR [, keep_version BOOLEAN = false]) → VARCHAR
```

| Written | `semantic_key` |
|---|---|
| `0173-1#02-AAC895#008` (ECLASS IRDI) | `0173-1#02-AAC895` |
| `https://api.eclass-cdp.com/0173-1-02-AAC895-008` (ECLASS IRI) | `0173-1#02-AAC895` |
| ` 0173-1#02-aac895#005 ` (older release, lower case) | `0173-1#02-AAC895` |
| `0112/2///61360_4#AAA001#001` (IEC CDD) | `0112/2///61360_4#AAA001` |
| `0173-1#02-ABK161#002/0173-1#01-AHX838#002` (property in class) | `0173-1#02-ABK161/0173-1#01-AHX838` |
| `HTTPS://Admin-Shell.IO/ZVEI/nameplate/2/0/Nameplate` | scheme + host lower-cased, path as written |
| `URN:Supplier:Drive` | `urn:Supplier:Drive` |
| `Rated current`, `Max:5` (not identifiers) | trimmed text |
| NULL, blank | NULL |

The version suffix is stripped by default (offer written against release 12 matches a request
against 14). `keep_version := true` keeps it, upper-cased, for joins where a release changed meaning.
The coverage kernel always compares without the version. ECLASS IRI form is recognised on any host
containing `eclass`.

```sql
-- (the ECLASS IRI form gives the same key: see the table above)
SELECT semantic_key('0173-1#02-AAC895#008')                            AS irdi,
       semantic_key(' 0173-1#02-aac895#005 ')                         AS old_release,
       semantic_key('0173-1#02-AAC895#008', true)                      AS versioned,
       semantic_key('URN:Supplier:Drive')                              AS urn;
```

```sql
-- join offers on the dictionary concept, whatever release / spelling they used
SELECT semantic_key(attr_key) AS concept, count(DISTINCT tree_id) AS n_trees,
       list(DISTINCT attr_key ORDER BY attr_key) AS spellings
FROM tree_attributes WHERE attr_key IS NOT NULL GROUP BY 1 ORDER BY 1;
```

## `aas_id_b64url` — ids for AAS Part 2 REST paths

```text
aas_id_b64url(id VARCHAR) → VARCHAR   -- base64url, no padding (RFC 4648 §5)
```

```sql
SELECT aas_id_b64url('urn:example:sm:TechnicalData');
-- dXJuOmV4YW1wbGU6c206VGVjaG5pY2FsRGF0YQ
-- => dXJuOmV4YW1wbGU6c206VGVjaG5pY2FsRGF0YQ
```

Loading from a repository (BaSyx, FA³ST) needs httpfs and a URL, so it is shown as text:

```text
SELECT unnest(aas_to_trees(content))
FROM read_text('http://localhost:8081/submodels/' || aas_id_b64url('https://lenvaro.example/aas/lx-8/sm/capabilities'));
-- GET /submodels pages its result (paging_metadata.cursor): a paged result is a valid input.
```

## `aas_class_hierarchy` — capability taxonomy for the kernel

```text
aas_class_hierarchy(content VARCHAR|BLOB) → LIST(STRUCT(class_id, parent_class_id))
```

Reads IDTA-02020 `CapabilityGeneralizedBy` relations (parent may be an external ontology IRI).
Store as a table and pass to `tree_coverage(..., class_hierarchy := 'class_hierarchy')`: a subclass
offer fully meets a superclass requirement (is-a); with `generalization_credit := c` a superclass
offer gets c^d for a subclass requirement (`label_rule = 'generalized'`). Unresolvable references →
`aas_issues` `unresolved_relation`.

```sql
CREATE TABLE class_hierarchy AS
SELECT unnest(aas_class_hierarchy(content), recursive := true)
FROM read_text('test/data/aas/scenario/sealing/capability_taxonomy.aas.json');
```

```sql
SELECT regexp_replace(class_id, '.*/', '') AS cls,
       regexp_replace(parent_class_id, '.*/', '') AS parent
FROM class_hierarchy ORDER BY 1;
-- FilmCutting → Cutting, HotPlateSealing → HeatSealing, ImpulseSealing → HeatSealing
-- => FilmCutting | Cutting
-- => HotPlateSealing | HeatSealing
-- => ImpulseSealing | HeatSealing
```

## `attrs_satisfying` — unit- and direction-aware value query

```text
attrs_satisfying(attr_key VARCHAR, requirement STRUCT,
                 attrs_table := 'tree_attributes', tol, soft_band, scale, default_bound)
  → tree_id, node_id, attr_id, name, attr_key, value, score, distance, rule, satisfied
```

"Which assets state a rated current covering 7.6 A?" — the AAS Query Language cannot compare
Ranges or units. Matching rows: `semantic_key(row.attr_key) = semantic_key(attr_key)` (any release,
IRDI or IRI); rows without `attr_key` match by `name`, case-insensitively. `requirement` is a
`parse_quantity` STRUCT (`lo`, `hi`, `dimension`, `bound`). Dimension gate: 5 V never satisfies
5 A (`rule = 'gate'`, score 0). `value` is the offered value as text in base units; `satisfied`
= score 1; best first. `tol`, `soft_band`, `scale`, `default_bound` as in `value_satisfies`.
Errors: NULL `attr_key`; unknown `attrs_table`.

```sql
-- key of a newer release (#009; the ECLASS IRI form works too), requirement as German text
SELECT split_part(tree_id, '/', 3) AS supplier, value, score, rule, satisfied
FROM attrs_satisfying('0173-1#02-BAA120#009',
                      parse_quantity('mindestens 7637,1 mA', 'de'),
                      attrs_table := 'tree_attributes')
WHERE tree_id <> 'RFQ-2026-117' AND node_id LIKE 'CapabilityDescription%'
ORDER BY score DESC, supplier;
```

```sql
-- hard range check: who covers a 380..480 V supply?
SELECT split_part(tree_id, '/', 3) AS supplier, value, satisfied
FROM attrs_satisfying('0173-1#02-AAB381#005', parse_quantity('380-480 V'),
                      attrs_table := 'tree_attributes', tol := 0, soft_band := 0)
WHERE tree_id <> 'RFQ-2026-117' AND node_id LIKE 'CapabilityDescription%' ORDER BY 1;
```

## `trees_to_aas` — trees → AAS v3 Environment (JSON)

```text
trees_to_aas(nodes LIST(node STRUCT) [, catalogue LIST(STRUCT(class_id, label, definition))]) → VARCHAR
```

Inverse of the reader: `aas_to_trees(trees_to_aas(t))` gives `t` back (ids, parents, kinds, OR
edges, classes, counts, keys, values, units, bounds, roles, categories). Not preserved:
`value_text` (written in base units), `bound_source` (re-derived: `constraint`/`text` → `range`,
`equals` → `level_type`), submodel `kind` (always `Instance`); idShorts are sanitised (≤128 chars,
`Größe` → `Groesse`, other chars `_`), so `tree_from_json` trees may get new node ids. A tree
without a root is an error.

| Tree | AAS |
|---|---|
| root | AssetAdministrationShell, `id` = tree id |
| nodes below root | Submodels |
| item with role-carrying attributes, or no attributes | IDTA-02020 CapabilityContainer (Required / Offered / NotAssigned) |
| other item | SubmodelElementCollection (`semanticId` = class) |
| `[i]` children | SubmodelElementList |
| `variant_group`, `count` ≠ 1 | qualifiers `anofox:VariantGroup`, `anofox:Count` |
| `at_least` / `at_most` / `within` | Range min / max / both |
| `equals`, ordinal category with bound | Property with IEC 61360 `levelType` |
| category | Property; identifier-like → `valueId`; `true`/`false` → `xs:boolean` |
| `attr_key` | semanticId + ConceptDescription (IEC 61360 name, unit) |

```sql
-- round trip one file: same node count back, profile markers written again
SELECT len(aas_to_trees(content)) AS once,
       len(aas_to_trees(trees_to_aas(aas_to_trees(content)))) AS twice,
       trees_to_aas(aas_to_trees(content)) LIKE '%"anofox:VariantGroup"%' AS has_variant_group
FROM read_text('test/data/aas/scenario/io_profile/ostwald_ios.aas.json');
```

## `aas_export` — tables → one Environment per tree

```text
aas_export(nodes_table := 'tree_nodes', attrs_table := 'tree_attributes',
           catalogue_table := NULL, tree_ids := NULL) → tree_id, aas_json
```

`catalogue_table` columns: `class_id[, label | name][, definition]` → ConceptDescriptions for classes.

```sql
-- normally your own catalogue table; here derived from the classes the RFQ uses
CREATE TABLE capability_catalogue AS
SELECT DISTINCT class_id,
       regexp_replace(class_id, '.*/', '') AS label,
       CASE WHEN class_id LIKE '%/FieldbusCommunication'
            THEN 'Exchanging cyclic process data over an industrial fieldbus.'
            ELSE 'Moving an axis to a commanded position under closed-loop control.' END AS definition
FROM tree_nodes WHERE tree_id = 'RFQ-2026-117' AND node_kind = 'item';
```

```sql
-- write the RFQ as an AAS JSON file (one row = one file), then read it back
COPY (SELECT aas_json FROM aas_export(nodes_table := 'tree_nodes', attrs_table := 'tree_attributes',
                                      catalogue_table := 'capability_catalogue',
                                      tree_ids := ['RFQ-2026-117']))
TO 'rfq_2026_117.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
SELECT n.node_id, n.node_kind, n.class_id
FROM (SELECT unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n FROM read_text('rfq_2026_117.aas.json'));
```

## `aas_coverage_result` / `coverage_result_to_aas` — gap list as AAS

```text
aas_coverage_result(trace_table VARCHAR, id_prefix := 'https://anofox.com/aas/results/')
  → query_id, candidate_id, aas_json (Submodel), aas_environment (same, wrapped as Environment)
coverage_result_to_aas(query_id, candidate_id, coverage DOUBLE, rows LIST(trace STRUCT)
                       [, id_prefix VARCHAR]) → VARCHAR   -- one Submodel
```

`trace_table` holds `tree_coverage_trace` rows (see **anofox-similarity-coverage**). One
`CoverageResult` submodel per (query, candidate); `coverage` = mean requirement score (=
`tree_coverage`'s coverage). Id = `id_prefix` + the base64url-encoded query id + `/` + the encoded candidate id.
Submodel semanticId `https://anofox.com/aas/CoverageResult/1/0`; every element
`https://anofox.com/aas/CoverageResult/<Term>/1/0`:

| Term | Content |
|---|---|
| `Query`, `Candidate`, `Coverage`, `GapCount` | header |
| `Requirements` → `Requirement` | `RequirementNode`, `MatchedNode`, `MatchedPath`, `LabelSimilarity`, `LabelRule` (`exact`/`soft`/`generalized`), `CountNeeded`, `CountAvailable`, `RequirementScore`, `IsGap` |
| `Attributes` → `Attribute` | `AttributeId`, `AttributeName`, `Required` (`at_least 7.6 A`), `Offered` (`12 A`), `Rule` (`at_least`/`at_most`/`within`/`compat`/`missing`/`key_mismatch`/`gate`), `NameRule` (`key`/`vector`/`exact`/`tokens`), `BoundSource`, `Score` |

A metric the trace lacks is written without a value (≠ 0). The result is AAS: `aas_to_trees`
reads it back (`Coverage`, `GapCount` as attributes).

```sql
SET VARIABLE two_offers = (SELECT list(DISTINCT tree_id ORDER BY tree_id) FROM tree_nodes
                           WHERE tree_id LIKE '%lenvaro%' OR tree_id LIKE '%driventa%');
CREATE TABLE trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('two_offers'),
    nodes_table := 'tree_nodes', attrs_table := 'tree_attributes', labels := 'hard');
```

```sql
SELECT split_part(candidate_id, '/', 3) AS supplier,
       regexp_extract(aas_json, '"id": "([^"]+)"', 1) AS submodel_id,
       aas_json LIKE '%"value": "key_mismatch"%' AS has_key_mismatch
FROM aas_coverage_result('trace', id_prefix := 'urn:acme:results:') ORDER BY 1;
```

```sql
-- one submodel by hand: rows are the trace STRUCTs of one (query, candidate)
SELECT t.candidate_id,
       length(coverage_result_to_aas(t.query_id, t.candidate_id, avg(t.req_score), list(t))) > 0 AS ok
FROM trace t GROUP BY t.query_id, t.candidate_id;
```

Note: `avg(req_score)` over raw trace rows weights requirements by their attribute count; for the
kernel's exact coverage, join `tree_coverage` and pass its `coverage` column, or just use
`aas_coverage_result`.

## `suggest_semantic_id` — dictionary ids for uncoded names

```text
suggest_semantic_id(items_table, dictionary_table,
                    mode := 'late_interaction' | 'centroid', temperature := 0.1,
                    center_by_source := false, overrides_table := NULL, min_similarity := NULL)
  → wording, source, n_occurrences, n_trees_affected, top1, p1, top2, p2, margin, impact,
    similarity, fits
```

Runs `classify_to_catalogue` (see **anofox-similarity-classification-review**). `items_table`:
`node_id, vec[, tree_id, source, label, tokens]` (wording = `label`, else `node_id`).
`dictionary_table`: `class_id` (the dictionary id), `vec[, exemplar_tokens]`. Bring your own
vectors; prefer example phrasings over definitions.
**`p1` is relative**: the best of poor candidates looks certain (p1 0.999). `similarity` is the
absolute fit; with `min_similarity`, rows below get `fits = false` and `impact = n_trees_affected`
(top of the queue). Wordings in `overrides_table` leave the queue.

```sql
CREATE TABLE uncoded_names AS
SELECT node_id, tree_id, label, vec::DOUBLE[] AS vec, tokens::DOUBLE[][] AS tokens
FROM read_csv('test/data/aas/scenario/enrichment/uncoded_names.csv');
```

```sql
CREATE TABLE eclass_examples AS
SELECT class_id, label, vec::DOUBLE[] AS vec, exemplar_tokens::DOUBLE[][] AS exemplar_tokens
FROM read_csv('test/data/aas/scenario/enrichment/dictionary_examples.csv');
```

```sql
CREATE TABLE suggestions AS
SELECT * FROM suggest_semantic_id('uncoded_names', 'eclass_examples', min_similarity := 0.35);
```

```sql
SELECT wording, top1, round(p1, 3) AS p1, round(similarity, 3) AS sim, fits
FROM suggestions ORDER BY impact DESC, wording LIMIT 3;
-- 'Gewicht' first: p1 0.999 but fits = false (nothing in the dictionary fits)
```

Confirm, then attach the keys (same as uc_09):

```sql
CREATE TABLE review AS
SELECT wording, top1 AS class_id, 'jr' AS reviewed_by FROM suggestions WHERE fits;
```

```sql
CREATE TABLE attribute_keys AS SELECT * FROM apply_vocabulary_review('review', overrides_table := NULL);
```

```sql
CREATE TABLE nordwerk_attrs AS
SELECT unnest(n.attributes, recursive := true)
FROM (SELECT unnest(aas_to_trees(content)) AS n
      FROM read_text('test/data/aas/scenario/enrichment/nordwerk_nd5.aas.json'));
UPDATE nordwerk_attrs SET attr_key = k.class_id FROM attribute_keys k
WHERE nordwerk_attrs.name = k.wording AND nordwerk_attrs.attr_key IS NULL;
SELECT name, value FROM attrs_satisfying('0173-1#02-BAA120#008', parse_quantity('mindestens 7637,1 mA', 'de'),
                                         attrs_table := 'nordwerk_attrs')
WHERE satisfied ORDER BY 1;
-- Ausgangsnennstrom 9 A, Nennstrom 9 A
```

## Canonical AAS pipeline

Audit → load → validate → rank (coverage skill) → explain → write back.

```sql
-- 1. audit (never raises) and quality per submodel
SELECT parse_filename(filename) AS file, i.issue, i.severity, i.path
FROM (SELECT filename, unnest(aas_issues(content, NULL,
          (SELECT list(u) FROM read_csv('test/data/aas/scenario/eclass_units.csv') u))) AS i
      FROM read_blob('test/data/aas/scenario/offers/*'))
WHERE i.severity <> 'info' ORDER BY file;
```

```sql
-- 2. load (setup blocks aas_raw / tree_nodes / tree_attributes above), 3. validate
SELECT tree_id, attr_id, issue, severity FROM validate_trees('tree_nodes', 'tree_attributes');
```

```sql
-- 4. rank the offers against the RFQ's Required capabilities
SET VARIABLE offers = (SELECT list(DISTINCT tree_id ORDER BY tree_id) FROM tree_nodes WHERE tree_id <> 'RFQ-2026-117');
SELECT split_part(candidate_id, '/', 3) AS supplier, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('RFQ-2026-117', candidates := getvariable('offers'),
                   nodes_table := 'tree_nodes', attrs_table := 'tree_attributes', labels := 'hard')
ORDER BY rank;
-- lenvaro 1.0 / orbitec 0.75 / kessler 0.625 / driventa 0.372
-- => lenvaro.example | 1.0 | 0
-- => orbitec.example | 0.75 | 1
-- => kessler-antriebe.example | 0.625 | 2
-- => driventa.example | 0.372 | 2
```

`n_gaps` counts the RFQ's capabilities (items) that are not met, not the properties: Driventa's 2
gaps hide five failing properties. For a per-requirement verdict read `tree_coverage_trace` (one
row per requirement and attribute, with `rule` and `score`). The default soft tolerance scores
near misses above 0 (400–480 V against 380–480 V: 0.978); pass `tol := 0, soft_band := 0` when the
RFQ's limits are hard.

```sql
-- 5. explain + 6. write the gap lists back as AAS environments, one file per ranking
SET VARIABLE offers = (SELECT list(DISTINCT tree_id ORDER BY tree_id) FROM tree_nodes WHERE tree_id <> 'RFQ-2026-117');
CREATE TABLE ranking_trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'),
                                  nodes_table := 'tree_nodes', attrs_table := 'tree_attributes', labels := 'hard');
COPY (SELECT aas_environment FROM aas_coverage_result('ranking_trace', id_prefix := 'urn:acme:results:')
      WHERE candidate_id LIKE '%lenvaro%')
TO 'result_lenvaro.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
SELECT a.attr_id, a.lo
FROM (SELECT unnest(n.attributes) AS a
      FROM (SELECT unnest(aas_to_trees(content)) AS n FROM read_text('result_lenvaro.aas.json')))
WHERE a.attr_id IN ('Coverage', 'GapCount');
```

Related: the full worked example is `examples/aas_capability_matching/`; the identifier terms are
in `docs/aas/index.md` (published at https://anofox.com/aas/); output validates against the AAS
v3.2 JSON schema (`scripts/tests/test_aas_schema.py`).
