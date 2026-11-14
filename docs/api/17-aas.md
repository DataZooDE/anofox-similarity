# Asset Administration Shell (AAS)

> Read AAS data into property trees, match on dictionary keys, explain the gaps, and write
> trees and results back as AAS.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

AAS v3.x standardises the data envelope and dictionary-coded properties (semanticId → ECLASS or
IEC CDD). IDTA-02020 (Capability Description, April 2026) marks capabilities as *Required* or
*Offered*. No AAS specification says how to match the two, rank offers or explain a gap. The
coverage kernel does that. This page covers reading AAS into property trees and the identifier
rules. The [coverage kernel](13-coverage-kernel.md) and [quantities](11-quantities.md) pages
cover `attr_key` and unit identifiers.

## Load AAS: aas_to_trees, aas_issues

Audit a folder first. `aas_issues` never raises: a broken file gives one `parse_error` row,
so the query names the file. `aas_to_trees` is strict and stops on the first broken file.

```sql
SELECT filename, i.issue, i.severity, i.path, i.detail
FROM (SELECT filename, unnest(aas_issues(content)) AS i FROM read_blob('offers/*'))
ORDER BY i.severity, filename;
```

Then load:

```sql
CREATE TABLE raw AS
SELECT unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n FROM read_text('rfq_2026_117.aas.json')
UNION ALL
SELECT unnest(aas_to_trees(content, NULL, (SELECT list(u) FROM eclass_units u))) FROM read_blob('offers/*');

CREATE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM raw;
CREATE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;

SELECT * FROM validate_trees('tree_nodes', 'tree_attributes');
SELECT unnest(aas_issues(content), recursive := true) FROM read_blob('offers/*');
```

**`aas_to_trees(content [, tree_id [, unit_table]])`** → `LIST` of the node STRUCTs of
[`tree_from_json`](12-property-trees.md), so `validate_trees`, `tree_coverage` and
`tree_coverage_trace` work on the result unchanged.

- `content` is VARCHAR or BLOB. The format is detected from the first bytes:
  - AAS Part 1 **JSON**: an Environment, a single Submodel (what `GET /submodels/{id}` returns)
    or a Part 2 paged result `{"result": [...]}`;
  - AAS Part 1 **XML**: `<environment>` or `<submodel>`, namespace 3.0 or 3.1, any prefix;
  - an **AASX** package: the zip is opened in memory and the `aasx-origin → aas-spec`
    relationships are followed. The spec parts may be XML (most published packages) or JSON.

  Use `read_blob` for AASX. `read_blob` also reads JSON and XML, so `read_blob('offers/*')`
  loads mixed folders.
- One tree per shell. `tree_id` is the shell id. With the `tree_id` argument the first tree
  (the shell) gets exactly that id; further trees in the same input (more shells, submodels no
  shell references) get `tree_id/idShort` and an info issue `tree_id_suffixed`.
- `unit_table` is as in [`parse_quantity`](11-quantities.md), and carries `unit_id`s (e.g. your
  licensed ECLASS unit list).

**Mapping**

| AAS | Tree |
|---|---|
| AssetAdministrationShell | root container (`node_id` = tree id) |
| Submodel | container, or an item when it holds values directly |
| SubmodelElementCollection / List / Entity holding values directly | item, `class_id` = `semantic_key(semanticId)` |
| ... holding no values directly | transparent container |
| IDTA-02020 `CapabilityContainer` | item: `class_id` = the Capability's supplementalSemanticId, `label` = the Capability, `role` from the `CapabilityRoleQualifier` (Required → `required`, Offered → `provided`); attributes from its `PropertySet`s |
| `Capability` elsewhere | item without attributes |
| `CapabilityComposedOf` (first is composed of second) | the part's item moves under the composite (`AND`); a composite without properties of its own becomes a container, so only its parts are requirements |
| `CapabilityGeneralizedBy` (second is more general) | a row of `aas_class_hierarchy`; second may be an external ontology IRI |
| IEC 61360 `levelType` of a single value: exactly min / max / nom | `bound` `at_least` / `at_most` / `equals` |
| IDTA-02020 `BasicConstraint` (`<= 1 ms`) on the properties its `ConstraintHasProperty` names (the value element, a list holding it or its PropertyContainer) | `bound` and limit from the expression; a different stated value or a two-sided Range is reported (`constraint_conflict`), and a Range keeps `within` |
| collection with a dictionary semanticId and only a profile count | item: "8 digital inputs" without further properties |
| `Property` with a number (`xs:double`, `xs:int`, ...) | point attribute (`lo = hi`); an absent or unparseable number is kept without value (a gap) and reported |
| `Range` | `lo` = min, `hi` = max; only min → `bound = 'at_least'`, only max → `'at_most'`, both → `'within'` |
| `Property` with `valueId` (enumeration) | `category` = `semantic_key(valueId)` |
| `Property` `xs:boolean` | `category` = `'true'` / `'false'` (only true, false, 1, 0) |
| `Property` `xs:string` with a code (`IP65`), or a short text of a property with a dictionary id (`EtherCAT`) | `category` = the text |
| SubmodelElementList children without valueType / semanticId | take `valueTypeListElement` / `semanticIdListElement` |
| `Property` `xs:string` with a quantity and unit ("24 V DC") | parsed like `parse_quantity` |
| SubmodelElementList of values | one attribute per value, named and keyed by the list |
| descriptive text, MultiLanguageProperty, File, Blob, references | not mapped; a text or MultiLanguageProperty without any semanticId is reported (`unmapped_text`, info) |

- `node_id` is the idShort path (`TechnicalData.TechnicalProperties`, `[i]` for list entries).
  `context` is the parent's path.
- `attr_key` is the semanticId **as written** (with its release); the kernel compares it with
  `semantic_key`. `class_id` and enumeration `category` values are stored canonical. When the
  semanticId is an IDTA-02020 template id
  (`.../CapabilityPropertyType/...`), the first supplementalSemanticId is used instead.
  `attr_id` is the idShort; for IDTA-02020 it is the PropertyContainer's idShort.
- `name` is the IEC 61360 `preferredName` (English), else the idShort in words
  (`RatedCurrent` → `Rated current`).
- **Units.** The IEC 61360 data specification of the element is used first (IDTA-02020 attaches
  it to the property), completed field by field from the ConceptDescription of its semanticId or
  a supplementalSemanticId. A ConceptDescription is found by its **exact release** first; another
  release of the same property only when all releases in the input agree on the unit
  (`ambiguous_concept_description` otherwise, since a property may change its unit between
  releases). `unitId` is resolved first, then the `unit` text. Values are converted to base units. A
  dictionary entry without any unit gives a dimensionless count (`assumed_dimensionless`).
- **Numbers** follow XSD: integer types take no fraction, and a comma is a decimal separator only
  when it cannot be a thousands separator (`2,5` → 2.5 with `decimal_comma`; `1,500` →
  `bad_value`). Template submodels (`kind = Template`) contribute only stated values.
- **`bound_source`** says where a bound came from: `range`, `level_type`, `constraint`, `text`
  (a quantity written as text), and `explicit` for `tree_from_json` bounds. The trace shows it.
- **References** in relations and constraints resolve by submodel **id** across the whole input,
  so two submodels with the same idShort never mix, and a GeneralizedBy may point into another
  submodel of the same input.
- **`role`** comes from the `CapabilityRoleQualifier` (values true/false/1/0, else `bad_value`) and is informational ([HLD §12](../requirements-matching/HLD.md)):
  `tree_coverage` reads the query tree as the requirement. `validate_trees` warns (`mixed_roles`)
  when one tree mixes both roles.
- **Text is not a number.** A string value counts as a quantity only with a written unit. Product
  class ids (`0112/2///62683#ACC501#002`), versions (`15.0`) and dates stay out.

**`aas_issues(content [, tree_id [, unit_table]])`** → `LIST(STRUCT(tree_id, path, issue,
severity, detail))`. These are findings, not errors; the mapping goes on.

| issue | severity | meaning |
|---|---|---|
| `parse_error` | error | the input is not readable AAS (only `aas_issues`; `aas_to_trees` raises) |
| `bad_value` | warning | a value that does not parse as its `valueType` (`"n/a"` in an `xs:double`, `"yes"` in an `xs:boolean`, `"1.5"` in an `xs:int`, the ambiguous `"1,500"`). The attribute keeps its text and shows up as a gap |
| `empty_value` | warning | a numeric Property or Range without a value: kept as a gap |
| `decimal_comma` | info | `"6,0"` in an `xs:double`, read as `6.0` |
| `unmapped_text` | info | a text Property (no code, no quantity) or MultiLanguageProperty without any semanticId: descriptive text, left out of the tree. These are the names to give a dictionary id (`suggest_semantic_id`); `aas_quality.n_issues` does not count them |
| `ambiguous_concept_description` | warning | no ConceptDescription for this release, and the other releases disagree on the unit |
| `assumed_dimensionless` | info | the dictionary entry states no unit: the value is a count |
| `duplicate_id_short` | warning | two siblings share an idShort; the second is kept as `X~2` |
| `tree_id_suffixed` | info | several trees in one input with a given `tree_id` |
| `no_shell_or_submodel`, `malformed_member` | warning | an empty input, or a member of the wrong type (`"submodels": {}`) |
| `missing_concept_description` | warning | no dictionary entry for the semanticId: the unit is unknown (`validate_trees` reports `unknown_unit`) |
| `no_unit_information` | warning | a number without semanticId or data specification |
| `unknown_unit_id` | warning | the `unitId` is not in the unit table; the `unit` text is used |
| `capability_missing` | warning | a CapabilityContainer without a Capability |
| `missing_submodel` | info | a shell references a submodel that is not in the input |
| `unresolved_constraint` | warning | a `ConstraintHasProperty` names no mapped property |
| `unparsed_constraint` | info | a `BasicConstraint` without a comparison (`see data sheet`) |
| `unresolved_relation` | warning | a ComposedOf / GeneralizedBy reference names no mapped capability (an ExternalReference in GeneralizedBy is an ontology class, not an error) |
| `constraint_conflict` | warning | a BasicConstraint disagrees with the stated value (the constraint is used), names a two-sided Range (kept), or is in another dimension (not applied) |
| `unclassed_capability` | info | a Capability without supplementalSemanticId: its idShort is the class |
| `composition_cycle` | warning | a capability cannot be part of one it contains; the relation is skipped |
| `multiple_composition` | info | a capability is part of two composites; the first is kept |

Malformed input (broken JSON or XML, a zip without a spec part) is an error with the reader's
message, e.g. `AAS JSON: ... parse error at line 1`.

**From an AAS repository (Part 2 REST).** Submodel ids go into the path base64url-encoded:

```sql
INSTALL httpfs; LOAD httpfs;
SELECT unnest(aas_to_trees(content))
FROM read_text('http://localhost:8081/submodels/' || aas_id_b64url('https://lenvaro.example/aas/lx-8/sm/capabilities'));
```

`GET /submodels` pages its result (`paging_metadata.cursor`). Fetch the pages with a recursive
CTE that appends `?cursor=` until no cursor comes back, and pass each page to `aas_to_trees`:
a paged result is a valid input.

**Capability hierarchy.** `aas_class_hierarchy(content)` reads the GeneralizedBy relations of a
capability taxonomy (any AAS with IDTA-02020 capabilities) as `(class_id, parent_class_id)`.
Pass the table to `tree_coverage` with a credit:

```sql
CREATE TABLE class_hierarchy AS
SELECT unnest(aas_class_hierarchy(content), recursive := true) FROM read_text('capability_taxonomy.aas.json');

SELECT * FROM tree_coverage('RFQ-2026-204', candidates := ['https://varipack.example/aas/vp-9'],
                            labels := 'hard', class_hierarchy := 'class_hierarchy', generalization_credit := 0.5);
```

## Write AAS: trees_to_aas, aas_export, aas_coverage_result

```sql
-- every tree of the tables as an AAS Environment, one row per tree
SELECT tree_id, aas_json FROM aas_export(catalogue_table := 'capability_catalogue');

-- the gap list of a ranking as a CoverageResult submodel per query and candidate
CREATE TABLE trace AS SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'));
SELECT candidate_id, aas_environment FROM aas_coverage_result('trace', id_prefix := 'urn:kroeger:results:');
```

**Writing a file.** Write **one row per file**. Several environments in one file are not valid JSON.
The CSV writer with quoting off writes the text unchanged:

```sql
COPY (SELECT aas_json FROM aas_export(tree_ids := ['RFQ-2026-117']))
TO 'rfq_2026_117.aas.json' (FORMAT csv, HEADER false, QUOTE '', ESCAPE '', DELIMITER '\x1f');
```

**`trees_to_aas(nodes [, catalogue])`** → VARCHAR, an AAS v3 Environment in Part 1 JSON. `nodes` is
a LIST of node STRUCTs as `aas_to_trees` and `tree_from_json` produce them. It is the inverse of
the reader: a tree read from AAS is written with the same idShort paths, so
`aas_to_trees(trees_to_aas(t))` gives `t` again: node ids, parents, kinds, OR edges, classes,
counts, attribute keys, values, units, bounds, roles and categories (`test/sql/aas/uc_08`). What
does not survive:

- `value_text`: values are written in base units, so 45 °C comes back as 318.15.
- `bound_source`: the bound survives but is re-derived. A `constraint` or `text` bound comes back
  as `range`; `equals` comes back as `level_type`.
- the submodel `kind`: everything is written as `Instance`.
- idShorts are made valid: at most 128 characters, umlauts and ß transliterated (`Größe` →
  `Groesse`), other characters `_`. Trees from `tree_from_json` with such ids therefore get new
  node ids; their classes, values and bounds survive.

A tree without a root is an error. Lists keep their order (`X[10]` after `X[9]`), and non-finite
numbers are written as `NaN`, `INF`, `-INF`.

| Tree | AAS |
|---|---|
| tree (root) | AssetAdministrationShell, `id` = tree id |
| nodes below the root | Submodels (`semanticId` = class of an item) |
| item whose attributes carry a role, or an item without attributes | IDTA-02020 CapabilityContainer: Capability (`supplementalSemanticId` = class, role qualifiers Required / Offered / NotAssigned), PropertySet with one PropertyContainer per attribute |
| other item | SubmodelElementCollection (`semanticId` = class) with its Property / Range elements |
| container, `[i]` children | SubmodelElementCollection, SubmodelElementList |
| `variant_group`, `count` ≠ 1 | qualifiers `anofox:VariantGroup`, `anofox:Count` ([profile](../guides/10-aas-profile.md)) |
| a capability's children | written next to it, with `CapabilityComposedOf` relations |
| attribute `at_least` / `at_most` / `within` | Range with min / max / both |
| `equals`; an ordinal category with a bound (`IP40 or better`) | Property with IEC 61360 `levelType` nom; min / max |
| category | Property; an identifier-like category as `valueId`; `true` / `false` as `xs:boolean` |
| attribute without value | Property `xs:double` without value (reads back as `empty_value`) |
| `attr_key` | semanticId (or supplementalSemanticId for IDTA-02020), and a ConceptDescription with IEC 61360 name and unit |

`catalogue` (LIST of STRUCT(class_id, label, definition)) adds ConceptDescriptions for classes.
A number whose unit is unknown is written without a dictionary entry, so it reads back as unknown,
not as a count.

**`aas_export(nodes_table := 'tree_nodes', attrs_table := 'tree_attributes', catalogue_table, tree_ids)`**
→ `tree_id, aas_json`: `trees_to_aas` over tables.

**`aas_coverage_result(trace_table [, id_prefix])`** → `query_id, candidate_id, aas_json,
aas_environment`: a Submodel `CoverageResult` per ranking (`coverage_result_to_aas` does one), and
the same wrapped as an Environment ready to upload. Its id is `id_prefix` (default
`https://anofox.com/aas/results/`) plus the base64url-encoded query and candidate ids, so
ids never collide and stay valid IRIs. Use your own prefix before you publish results. Contents:
`Query`, `Candidate`, `Coverage` (the kernel's coverage: the mean score of the counted
requirements), `GapCount` and a list of `Requirements`, each with the matched node and
path, label similarity and rule, counts, score, `IsGap` and the `Attributes` with required and
offered value, rule, name rule, bound source and score. A metric the trace lacks is written
without a value, never as 0. The semanticIds are in the
`https://anofox.com/aas/CoverageResult/` namespace until a standard template exists; every
term is listed in the [term reference](../aas/index.md). The
result is AAS itself: `aas_to_trees` reads it back.

**Validation.** The output validates against the official AAS v3.2 JSON schema
(`scripts/tests/test_aas_schema.py`, schema in `test/data/aas/schema/`, run in CI with the release
artifact; it includes generated edge cases). It opens in Eclipse BaSyx
(`scripts/aas_basyx_smoke.sh`: upload, list the shells, fetch a submodel over REST and read it back
unchanged). AASX Package Explorer, a Windows application, has not been tried.

## Query loaded AAS by value: attrs_satisfying

"Which assets offer a rated current that covers 7.6 A?" The AAS Query Language addresses
semanticId, idShort and plain values, not Range bounds or units. Over loaded attributes:

```sql
SELECT tree_id, node_id, value, rule
FROM attrs_satisfying('0173-1#02-BAA120#008', parse_quantity('mindestens 7637,1 mA', 'de'))
WHERE satisfied;
-- https://kessler-antriebe.example/aas/ksd-10 | CapabilityDescription...RatedCurrent... | 10 A | at_least
-- https://lenvaro.example/aas/lx-8            | ...                                      | 8 A  | at_least
```

**`attrs_satisfying(attr_key, requirement)`** with named `attrs_table` (default
`'tree_attributes'`), `tol`, `soft_band`, `scale`, `default_bound` as in
[`value_satisfies`](11-quantities.md).

- Rows whose `attr_key` equals `attr_key` after `semantic_key` match: any release, IRDI or
  ECLASS IRI. Rows without `attr_key` (hand-made tables, `tree_from_json`) match by `name`,
  case-insensitively.
- `requirement` is a STRUCT with `lo`, `hi`, `dimension`, `bound` (`parse_quantity` output).
  The dimension gate applies: 5 V never satisfies 5 A (`rule = 'gate'`, score 0).
- Returns `tree_id, node_id, attr_id, name, attr_key, value, score, distance, rule, satisfied`,
  best first. `satisfied` is score 1; use `tol := 0, soft_band := 0` for hard limits.

## Data quality and enrichment: aas_quality, suggest_semantic_id

Data quality, not tooling, is what holds AAS matching back: semanticIds are optional,
ConceptDescriptions are often missing, dictionary releases are mixed.

```sql
SELECT filename, q.submodel, q.semantic_id_share, q.n_missing_concept_description, q.n_unresolved_units,
       q.n_unparseable, q.n_mixed_releases
FROM (SELECT filename, unnest(aas_quality(content)) AS q FROM read_blob('offers/*'));
```

**`aas_quality(content [, unit_table])`** → `LIST(STRUCT(tree_id, submodel_id, submodel, kind,
n_values, n_with_semantic_id, semantic_id_share, n_missing_concept_description,
n_unresolved_units, n_unparseable, n_mixed_releases, n_issues))`, one entry per submodel.
`n_values` counts Property, Range and MultiLanguageProperty elements; list members count with the
list's key. `n_mixed_releases` counts concepts referenced in more than one dictionary release
anywhere in the input. The report uses the same reading as `aas_to_trees` and `aas_issues`. It
does not run the coverage kernel.

**Uncoded attributes and capabilities.** `suggest_semantic_id` proposes dictionary ids from your
own embeddings, so an engineer confirms them once with the
[vocabulary review](14-classification-and-review.md):

```sql
-- the supplier's uncoded names and your dictionary of example phrasings, both with your vectors
CREATE TABLE suggestions AS
SELECT * FROM suggest_semantic_id('uncoded_names', 'dictionary_examples', min_similarity := 0.35);

-- confirm what fits; the rest stays in the queue
CREATE TABLE review AS SELECT wording, top1 AS class_id, 'jr' AS reviewed_by FROM suggestions WHERE fits;
CREATE TABLE attribute_keys AS SELECT * FROM apply_vocabulary_review('review', overrides_table := NULL);

-- attach the confirmed ids; from now on the supplier is matched by key
UPDATE tree_attributes SET attr_key = k.class_id
FROM attribute_keys k WHERE tree_attributes.name = k.wording AND tree_attributes.attr_key IS NULL;
```

**`suggest_semantic_id(items_table, dictionary_table)`**, named `mode` (`'late_interaction'`
default, or `'centroid'`), `temperature` (0.1), `center_by_source` (false), `overrides_table`,
`min_similarity`.

- It runs [`classify_to_catalogue`](14-classification-and-review.md). `items_table` is its item
  table (`node_id, vec[, tree_id, source, label, tokens]`; the wording is `label`, else
  `node_id`). `dictionary_table` is its catalogue (`class_id` = the dictionary id, `vec[,
  exemplar_tokens]`).
- Prefer **example values and phrasings** over dictionary definitions. On an ECU benchmark,
  exemplar late interaction reached 95 % top-1 against 74.5 % for a definition pivot, and ECLASS
  13.0 has definitions for only about 68 % of its classes (arXiv:2604.19664).
- Returns the columns of `vocabulary_review_queue` (`wording, source, n_occurrences,
  n_trees_affected, top1, p1, top2, p2, margin, impact`) plus `similarity` and `fits`.
- **`p1` is relative to the dictionary**: the best of several poor candidates looks certain
  (p1 0.999 for a wording nothing fits). `similarity` is the raw maxsim (or cosine) of `top1`, an
  absolute fit; with `min_similarity` a row below it has `fits = false` and goes to the top of the
  queue (impact = n_trees_affected). Set the floor from your corpus (`cosine_floor`) or from
  reviewed examples.
- Reviewed wordings (`overrides_table`) leave the queue. For capability labels, use the
  confirmed table as `overrides_table` of `tree_coverage`, so the labels resolve to classes.
- No dictionary content ships with the extension. Bring your licensed ECLASS phrasings and your
  own vectors.

## Identifiers: semantic_key

The same dictionary concept arrives in several spellings:

| Written | `semantic_key` |
|---|---|
| `0173-1#02-AAC895#008` (ECLASS IRDI) | `0173-1#02-AAC895` |
| `https://api.eclass-cdp.com/0173-1-02-AAC895-008` (ECLASS IRI) | `0173-1#02-AAC895` |
| ` 0173-1#02-aac895#005 ` (older release, lower case) | `0173-1#02-AAC895` |
| `0112/2///61360_4#AAA001#001` (IEC CDD) | `0112/2///61360_4#AAA001` |
| `0173-1#02-ABK161#002/0173-1#01-AHX838#002` (property in class) | `0173-1#02-ABK161/0173-1#01-AHX838` |
| `HTTPS://Admin-Shell.IO/ZVEI/nameplate/2/0/Nameplate` | `https://admin-shell.io/ZVEI/nameplate/2/0/Nameplate` |
| `URN:Supplier:Drive` | `urn:Supplier:Drive` |
| `Rated current` (not an identifier) | `Rated current` |
| NULL, blank | NULL |

- **Version suffixes are stripped by default.** A new ECLASS release usually keeps the meaning of
  a property, and an offer written against release 12 should match a request written against
  release 14. But a version bump *may* change the meaning (a new unit, a narrower definition).
  `semantic_key(id, true)` keeps the version for joins where that matters. The coverage kernel
  always compares keys without the version.
- An IRDI is `<RAI>#<item>[#<version>]`: the RAI starts with four digits and `-` or `/`, and
  the version is all digits. The ECLASS IRI form is recognised on any host whose name contains
  `eclass`, with the IRDI in the last path segment (`-` for `#`).
- Other URIs keep their path as written (paths are case-sensitive); only scheme and host are
  lower-cased. Text without a URI scheme is only trimmed. `Max:5` is not a URI.

## Limits

- Not mapped: Operation, BasicEventElement, File, Blob, ReferenceElement, MultiLanguageProperty
  values, and IDTA-02020 transition constraints.
- No ECLASS dictionary content is shipped: no unit IRDIs and no property definitions. Bring your
  licensed lists through `unit_table` (units).
- The reader holds one AAS input in memory at a time. Large AASX packages with many
  supplementary files are fine: only the spec parts are extracted, each at most 256 MB
  uncompressed. Relationship targets that leave the package are an error.

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `semantic_key`

*Scalar function*

Canonical join key for an AAS semanticId. An ECLASS or IEC CDD IRDI ('0173-1#02-AAC895#008', '0112/2///61360_4#AAA001#001') and the ECLASS IRI form ('https://api.eclass-cdp.com/0173-1-02-AAC895-008') give the same key: the upper-cased IRDI without its version suffix. keep_version (default false) keeps the version; use it when a dictionary release changed the meaning of a property. Other URIs pass through with scheme and host lower-cased, any other text is only trimmed, and NULL or a blank give NULL (no key).

```sql
semantic_key(id VARCHAR) → VARCHAR
semantic_key(id VARCHAR, keep_version BOOLEAN) → VARCHAR
```

**Examples**

```sql
SELECT semantic_key('https://api.eclass-cdp.com/0173-1-02-AAC895-008'); -- 0173-1#02-AAC895
SELECT semantic_key('0173-1#02-AAC895#008', true); -- 0173-1#02-AAC895#008
```

### `aas_to_trees`

*Scalar function*

Reads Asset Administration Shell data (AAS v3.x: Part 1 JSON or XML, an AASX package, a single Submodel or a Part 2 paged result) into property trees: a LIST of the node STRUCTs of tree_from_json with their attributes. One tree per shell (tree_id: the shell id, or the tree_id argument), submodels and collections as nodes; a collection that directly holds values is an item (class_id: its semanticId), otherwise a container. Property and Range give attributes: attr_key is the semanticId (the supplementalSemanticId for IDTA-02020 property types), the unit comes from the IEC 61360 data specification (unitId first, then unit) and values are converted to base units. An IDTA-02020 CapabilityContainer is an item whose class is the Capability's supplementalSemanticId and whose attributes come from its PropertySets; the CapabilityRoleQualifier gives role. Pass read_blob(...) for AASX packages. Findings about the input are in aas_issues(). unit_table as in parse_quantity.

```sql
aas_to_trees(content BLOB) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR, attr_key VARCHAR, bound_source VARCHAR)[])[]
aas_to_trees(content VARCHAR) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR, attr_key VARCHAR, bound_source VARCHAR)[])[]
aas_to_trees(content BLOB, tree_id VARCHAR) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR, attr_key VARCHAR, bound_source VARCHAR)[])[]
aas_to_trees(content VARCHAR, tree_id VARCHAR) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR, attr_key VARCHAR, bound_source VARCHAR)[])[]
aas_to_trees(content BLOB, tree_id VARCHAR, unit_table ANY) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR, attr_key VARCHAR, bound_source VARCHAR)[])[]
aas_to_trees(content VARCHAR, tree_id VARCHAR, unit_table ANY) → STRUCT(tree_id VARCHAR, node_id VARCHAR, parent_id VARCHAR, node_kind VARCHAR, edge_type VARCHAR, "label" VARCHAR, context VARCHAR, class_id VARCHAR, count DOUBLE, "source" VARCHAR, attributes STRUCT(tree_id VARCHAR, node_id VARCHAR, attr_id VARCHAR, "name" VARCHAR, value_text VARCHAR, dimension VARCHAR, unit VARCHAR, lo DOUBLE, hi DOUBLE, "role" VARCHAR, bound VARCHAR, qualifier VARCHAR, category VARCHAR, attr_key VARCHAR, bound_source VARCHAR)[])[]
```

**Examples**

```sql
SELECT aas_to_trees(content) FROM read_blob('offers/*.aasx');
SELECT aas_to_trees(content) FROM read_text('offers/*.json');
SELECT aas_to_trees(content, 'OFF-A') FROM read_blob('offer_a.aasx');
SELECT aas_to_trees(content, 'OFF-A') FROM read_text('offer_a.xml');
SELECT aas_to_trees(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM read_blob('offer_a.aasx');
SELECT aas_to_trees(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM read_text('offer_a.json');
```

### `aas_issues`

*Scalar function*

Findings of aas_to_trees about the input: LIST of STRUCT(tree_id, path, issue, severity, detail). It never raises on malformed input: a broken file gives one parse_error row with the reader's message, so SELECT filename, unnest(aas_issues(content)) FROM read_blob('offers/*') audits a folder. See the API page for the issue codes (bad_value, empty_value, decimal_comma, missing_concept_description, ambiguous_concept_description, unknown_unit_id, assumed_dimensionless, duplicate_id_short, ...). path is the idShort path, submodel first. Same arguments as aas_to_trees.

```sql
aas_issues(content BLOB) → STRUCT(tree_id VARCHAR, path VARCHAR, issue VARCHAR, severity VARCHAR, detail VARCHAR)[]
aas_issues(content VARCHAR) → STRUCT(tree_id VARCHAR, path VARCHAR, issue VARCHAR, severity VARCHAR, detail VARCHAR)[]
aas_issues(content BLOB, tree_id VARCHAR) → STRUCT(tree_id VARCHAR, path VARCHAR, issue VARCHAR, severity VARCHAR, detail VARCHAR)[]
aas_issues(content VARCHAR, tree_id VARCHAR) → STRUCT(tree_id VARCHAR, path VARCHAR, issue VARCHAR, severity VARCHAR, detail VARCHAR)[]
aas_issues(content BLOB, tree_id VARCHAR, unit_table ANY) → STRUCT(tree_id VARCHAR, path VARCHAR, issue VARCHAR, severity VARCHAR, detail VARCHAR)[]
aas_issues(content VARCHAR, tree_id VARCHAR, unit_table ANY) → STRUCT(tree_id VARCHAR, path VARCHAR, issue VARCHAR, severity VARCHAR, detail VARCHAR)[]
```

**Examples**

```sql
SELECT aas_issues(content) FROM read_blob('offers/*.aasx');
SELECT aas_issues(content) FROM read_text('offers/*.json');
SELECT aas_issues(content, 'OFF-A') FROM read_blob('offer_a.aasx');
SELECT aas_issues(content, 'OFF-A') FROM read_text('offer_a.xml');
SELECT aas_issues(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM read_blob('offer_a.aasx');
SELECT aas_issues(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM read_text('offer_a.json');
```

### `aas_class_hierarchy`

*Scalar function*

The capability class hierarchy of IDTA-02020 CapabilityGeneralizedBy relations: LIST of STRUCT(class_id, parent_class_id), class ids as in aas_to_trees (the second element may also be an external ontology IRI). Store it as a table and pass it to tree_coverage as class_hierarchy: an offer of a subclass then fully meets a requirement of its superclass (is-a), and with a generalization_credit c a superclass offer gets c^d for a subclass requirement. Unresolvable references are in aas_issues (unresolved_relation).

```sql
aas_class_hierarchy(content BLOB) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
aas_class_hierarchy(content VARCHAR) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
```

**Examples**

```sql
SELECT unnest(aas_class_hierarchy(content), recursive := true) FROM read_blob('taxonomy.aasx');
SELECT unnest(aas_class_hierarchy(content), recursive := true) FROM read_text('capability_taxonomy.aas.json');
```

### `aas_id_b64url`

*Scalar function*

An AAS identifier encoded for an AAS Part 2 REST path: base64url (RFC 4648 §5) without padding, as in GET /submodels/{submodelIdentifier}. Use it with read_text over httpfs to load a submodel from an AAS repository (BaSyx, FA³ST).

```sql
aas_id_b64url(id VARCHAR) → VARCHAR
```

**Examples**

```sql
SELECT aas_to_trees(content) FROM read_text('http://localhost:8081/submodels/' || aas_id_b64url('https://example.com/ids/sm/TechnicalData'));
```

### `attrs_satisfying`

*Table function*

Which attributes satisfy a requirement: every row of attrs_table (default 'tree_attributes') whose attr_key equals attr_key after semantic_key (any release, IRDI or ECLASS IRI; rows without attr_key match by name, case-insensitively), checked with value_satisfies against requirement (a STRUCT such as parse_quantity('mindestens 7637,1 mA')): direction (at_least, at_most, within, equals), units and the dimension gate (5 V never satisfies 5 A). Returns tree_id, node_id, attr_id, name, attr_key, value (the offered value in base units), score, distance, rule and satisfied (score 1), best first. tol, soft_band, scale and default_bound as in value_satisfies; tol := 0, soft_band := 0 for hard limits. A unit- and direction-aware query over AAS data that the AAS Query Language does not offer.

```sql
attrs_satisfying(attr_key, requirement, attrs_table, tol, soft_band, scale, default_bound)
```

**Examples**

```sql
SELECT tree_id, node_id, value FROM attrs_satisfying('0173-1#02-BAA120#008', parse_quantity('mindestens 7637,1 mA', 'de')) WHERE satisfied;
```

### `trees_to_aas`

*Scalar function*

Writes property trees as an AAS v3 Environment (Part 1 JSON): one shell per tree_id, the level below the root as submodels, collections below. nodes is a LIST of node STRUCTs as aas_to_trees and tree_from_json produce them. Items whose attributes carry a role (required, provided) become IDTA-02020 CapabilityContainers (Required / Offered, class as supplementalSemanticId, properties as PropertyRange / PropertyProperty); other items SubmodelElementCollections with their Property / Range elements. Bounds become ranges (at_least: min only, at_most: max only), equals a levelType nom; variant groups and counts use the DataZoo AAS profile; ComposedOf parts are written next to their composite with CapabilityComposedOf relations. Every attr_key becomes a ConceptDescription (IEC 61360 name and unit); catalogue (LIST of STRUCT(class_id, label, definition)) adds class entries. A tree read from AAS reads back the same: aas_to_trees(trees_to_aas(t)).

```sql
trees_to_aas(nodes ANY) → VARCHAR
trees_to_aas(nodes ANY, catalogue ANY) → VARCHAR
```

**Examples**

```sql
SELECT trees_to_aas(aas_to_trees(content)) FROM read_text('offer.aas.json');
SELECT trees_to_aas(aas_to_trees(content), (SELECT list(c) FROM catalogue c)) FROM read_text('offer.aas.json');
```

### `aas_export`

*Table function*

Exports property trees as AAS: one row per tree with tree_id and aas_json, an AAS v3 Environment (Part 1 JSON) as trees_to_aas writes it. Trees come from nodes_table / attrs_table (default 'tree_nodes' / 'tree_attributes'), tree_ids selects some; catalogue_table (class_id[, label | name][, definition]) adds ConceptDescriptions for the classes.

```sql
aas_export(nodes_table, attrs_table, catalogue_table, tree_ids)
```

**Examples**

```sql
SELECT tree_id, aas_json FROM aas_export(tree_ids := ['RFQ-2026-117']);
```

### `aas_coverage_result`

*Table function*

The rankings of a tree_coverage_trace result as AAS: one row per query and candidate with aas_json, a 'CoverageResult' Submodel (coverage_result_to_aas), and aas_environment, the same wrapped as an Environment ready to upload. coverage is the mean requirement score of the trace, i.e. tree_coverage's coverage. The submodel id is id_prefix (default https://anofox.com/aas/results/) plus the base64url-encoded query and candidate ids.

```sql
aas_coverage_result(trace_table, id_prefix)
```

**Examples**

```sql
CREATE TABLE trace AS SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := ['OFF-A']);
SELECT aas_json FROM aas_coverage_result('trace');
```

### `coverage_result_to_aas`

*Scalar function*

The explanation of one ranking as an AAS Submodel (Part 1 JSON) 'CoverageResult': Query, Candidate, Coverage, GapCount and a list of Requirements with matched node and path, label similarity and rule, counts, score, IsGap and per attribute the required and offered value, rule and score. semanticIds in the https://anofox.com/aas/CoverageResult/ namespace. rows are tree_coverage_trace rows (STRUCTs); aas_coverage_result builds them from a trace table.

```sql
coverage_result_to_aas(query_id VARCHAR, candidate_id VARCHAR, coverage DOUBLE, rows ANY) → VARCHAR
coverage_result_to_aas(query_id VARCHAR, candidate_id VARCHAR, coverage DOUBLE, rows ANY, id_prefix VARCHAR) → VARCHAR
```

**Examples**

```sql
SELECT * FROM aas_coverage_result('trace');
SELECT * FROM aas_coverage_result('trace', id_prefix := 'urn:acme:results:');
```

### `aas_quality`

*Scalar function*

Data quality of an AAS input per submodel, without the coverage kernel: LIST of STRUCT(tree_id, submodel_id, submodel, kind, n_values (Property, Range, MultiLanguageProperty), n_with_semantic_id, semantic_id_share, n_missing_concept_description, n_unresolved_units (numbers whose unit is unknown), n_unparseable (bad_value), n_mixed_releases (concepts referenced in more than one dictionary release in the input), n_issues (aas_issues findings except unmapped_text, which n_values - n_with_semantic_id already counts)). The counts come from the same reading as aas_to_trees and aas_issues. unit_table as in parse_quantity.

```sql
aas_quality(content BLOB) → STRUCT(tree_id VARCHAR, submodel_id VARCHAR, submodel VARCHAR, kind VARCHAR, n_values BIGINT, n_with_semantic_id BIGINT, semantic_id_share DOUBLE, n_missing_concept_description BIGINT, n_unresolved_units BIGINT, n_unparseable BIGINT, n_mixed_releases BIGINT, n_issues BIGINT)[]
aas_quality(content VARCHAR) → STRUCT(tree_id VARCHAR, submodel_id VARCHAR, submodel VARCHAR, kind VARCHAR, n_values BIGINT, n_with_semantic_id BIGINT, semantic_id_share DOUBLE, n_missing_concept_description BIGINT, n_unresolved_units BIGINT, n_unparseable BIGINT, n_mixed_releases BIGINT, n_issues BIGINT)[]
aas_quality(content BLOB, unit_table ANY) → STRUCT(tree_id VARCHAR, submodel_id VARCHAR, submodel VARCHAR, kind VARCHAR, n_values BIGINT, n_with_semantic_id BIGINT, semantic_id_share DOUBLE, n_missing_concept_description BIGINT, n_unresolved_units BIGINT, n_unparseable BIGINT, n_mixed_releases BIGINT, n_issues BIGINT)[]
aas_quality(content VARCHAR, unit_table ANY) → STRUCT(tree_id VARCHAR, submodel_id VARCHAR, submodel VARCHAR, kind VARCHAR, n_values BIGINT, n_with_semantic_id BIGINT, semantic_id_share DOUBLE, n_missing_concept_description BIGINT, n_unresolved_units BIGINT, n_unparseable BIGINT, n_mixed_releases BIGINT, n_issues BIGINT)[]
```

**Examples**

```sql
SELECT filename, unnest(aas_quality(content), recursive := true) FROM read_blob('offers/*');
SELECT filename, unnest(aas_quality(content), recursive := true) FROM read_text('offers/*.json');
SELECT unnest(aas_quality(content, (SELECT list(u) FROM eclass_units u)), recursive := true) FROM read_blob('offer.aasx');
SELECT unnest(aas_quality(content, (SELECT list(u) FROM eclass_units u)), recursive := true) FROM read_text('offer.json');
```

### `suggest_semantic_id`

*Table function*

Suggests dictionary ids (ECLASS or your own catalogue) for uncoded attribute names or capability labels, as a review queue in the shape of vocabulary_review_queue: wording, source, n_occurrences, n_trees_affected, top1, p1, top2, p2, margin, similarity (the raw maxsim or cosine of top1: an absolute fit, since p is relative to the dictionary and the best of poor candidates looks certain), fits (similarity >= min_similarity, NULL without one) and impact = n_trees_affected x (1 - margin), or n_trees_affected when fits is false, highest impact first. It runs classify_to_catalogue: items_table (node_id, vec[, tree_id, source, label, tokens]; the wording is label, else node_id) against dictionary_table (class_id = the dictionary id, vec[, exemplar_tokens]). mode 'late_interaction' (default; maxsim of the wording's tokens over example phrasings, the strongest signal) or 'centroid'; temperature default 0.1; center_by_source default false. Wordings in overrides_table (wording[, source], class_id) are reviewed and leave the queue. Confirm suggestions with apply_vocabulary_review and use the result as attr_key or as overrides_table. No dictionary content ships with the extension: bring your own vectors.

```sql
suggest_semantic_id(items_table, dictionary_table, mode, temperature, center_by_source, overrides_table, min_similarity)
```

**Examples**

```sql
SELECT wording, top1, p1, margin FROM suggest_semantic_id('uncoded_names', 'eclass_examples') LIMIT 20;
```

<!-- generated:functions:end -->
