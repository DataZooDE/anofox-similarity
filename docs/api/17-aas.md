# Asset Administration Shell (AAS)

> Read AAS data into property trees, match on dictionary keys, and explain the gaps.

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
| `CapabilityComposedOf` (first is composed of second) | the part's item moves under the composite (`AND`) |
| `CapabilityGeneralizedBy` (second is more general) | a row of `aas_class_hierarchy`; second may be an external ontology IRI |
| IEC 61360 `levelType` of a single value: exactly min / max / nom | `bound` `at_least` / `at_most` / `equals` |
| IDTA-02020 `BasicConstraint` (`<= 1 ms`) on the properties its `ConstraintHasProperty` names | `bound` from the expression (overrides the Range) |
| `Property` with a number (`xs:double`, `xs:int`, ...) | point attribute (`lo = hi`); an absent or unparseable number is kept without value (a gap) and reported |
| `Range` | `lo` = min, `hi` = max; only min → `bound = 'at_least'`, only max → `'at_most'`, both → `'within'` |
| `Property` with `valueId` (enumeration) | `category` = `semantic_key(valueId)` |
| `Property` `xs:boolean` | `category` = `'true'` / `'false'` (only true, false, 1, 0) |
| `Property` `xs:string` with a code (`IP65`), or a short text of a property with a dictionary id (`EtherCAT`) | `category` = the text |
| SubmodelElementList children without valueType / semanticId | take `valueTypeListElement` / `semanticIdListElement` |
| `Property` `xs:string` with a quantity and unit ("24 V DC") | parsed like `parse_quantity` |
| SubmodelElementList of values | one attribute per value, named and keyed by the list |
| descriptive text, MultiLanguageProperty, File, Blob, references | not mapped |

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
- **`role`** comes from the `CapabilityRoleQualifier` and is informational ([HLD §12](../requirements-matching/HLD.md)):
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
| `unresolved_relation` | warning | a ComposedOf / GeneralizedBy reference names no mapped capability |
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

The capability class hierarchy of IDTA-02020 CapabilityGeneralizedBy relations: LIST of STRUCT(class_id, parent_class_id), class ids as in aas_to_trees (the second element may also be an external ontology IRI). Store it as a table and pass it to tree_coverage as class_hierarchy with a generalization_credit, so a requirement for a specific capability gets partial credit from a more general offer and vice versa. Same arguments as aas_to_trees.

```sql
aas_class_hierarchy(content BLOB) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
aas_class_hierarchy(content VARCHAR) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
aas_class_hierarchy(content BLOB, tree_id VARCHAR) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
aas_class_hierarchy(content VARCHAR, tree_id VARCHAR) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
aas_class_hierarchy(content BLOB, tree_id VARCHAR, unit_table ANY) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
aas_class_hierarchy(content VARCHAR, tree_id VARCHAR, unit_table ANY) → STRUCT(class_id VARCHAR, parent_class_id VARCHAR)[]
```

**Examples**

```sql
SELECT aas_class_hierarchy(content) FROM read_blob('offers/*.aasx');
SELECT aas_class_hierarchy(content) FROM read_text('offers/*.json');
SELECT aas_class_hierarchy(content, 'OFF-A') FROM read_blob('offer_a.aasx');
SELECT aas_class_hierarchy(content, 'OFF-A') FROM read_text('offer_a.xml');
SELECT aas_class_hierarchy(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM read_blob('offer_a.aasx');
SELECT aas_class_hierarchy(content, NULL, (SELECT list(u) FROM eclass_units u)) FROM read_text('offer_a.json');
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

<!-- generated:functions:end -->
