# The DataZoo AAS Profile: Variants and Counts

> Say "one of these configurations" and "8 channels per card, 2 cards" in an Asset
> Administration Shell, so offers match without one shell per variant.

[Guides](README.md) · [AAS API](../api/17-aas.md) · [Examples](../../examples/aas_capability_matching/)

## What AAS cannot say

The coverage kernel needs two things that AAS v3 has no construct for:

- **A configuration space.** A product family (a base module, configurations, applications)
  offers *alternatives*; the best one should count. ECLASS polymorphism picks one variant per
  *instance*. IDTA-02020 has no "one of". Without a convention, a supplier has to publish one
  shell per variant, which hides the engineering choice and multiplies near-duplicates.
- **Additive counts.** "2 cards with 8 channels each" means 16 channels. ECLASS cardinality
  blocks describe instance structure; IDTA-02020 has no multiplicity.

The profile is three small, optional markers. Plain AAS without them still loads. The markers are
ordinary AAS (qualifiers and a semanticId), so every AAS tool keeps reading the file.

| Marker | Where | Meaning | Tree |
|---|---|---|---|
| qualifier `type = "anofox:VariantGroup"` | a SubmodelElementCollection | its child collections are alternatives | `node_kind = 'variant_group'`, children `edge_type = 'OR'` |
| qualifier `type = "anofox:Count"`, `value = "2"` | a collection | the collection occurs that many times | node `count` |
| Property with semanticId `https://data-zoo.de/anofox/aas/Count/1/0` | inside a collection | the same, as a property (e.g. channels per card) | node `count`; not an attribute |

Counts multiply along the path: a card with `anofox:Count = 2` holding a channel block with a
Count property of 8 gives 16 channels. The kernel sums counts only within one alternative, never
across alternatives of a variant group.

## Example

A remote I/O station in three configurations (abridged from
`examples/aas_capability_matching/data/io_profile/ostwald_ios.aas.json`):

```json
{"modelType": "SubmodelElementCollection", "idShort": "Configuration",
 "qualifiers": [{"type": "anofox:VariantGroup", "valueType": "xs:string", "value": "true"}],
 "value": [
   {"modelType": "SubmodelElementCollection", "idShort": "Config16DI8DO", "value": [
     {"modelType": "SubmodelElementCollection", "idShort": "DI8Card",
      "qualifiers": [{"type": "anofox:Count", "valueType": "xs:int", "value": "2"}],
      "value": [
        {"modelType": "SubmodelElementCollection", "idShort": "DigitalInputs",
         "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#01-AAZ100#001"}]},
         "value": [
           {"modelType": "Property", "idShort": "ChannelCount", "valueType": "xs:int", "value": "8",
            "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference",
                           "value": "https://data-zoo.de/anofox/aas/Count/1/0"}]}},
           {"modelType": "Property", "idShort": "InputVoltage", "valueType": "xs:double", "value": "24",
            "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAZ101#001"}]}}]}]},
     {"modelType": "SubmodelElementCollection", "idShort": "DigitalOutputs", "...": "8 channels, 2 A"}]},
   {"modelType": "SubmodelElementCollection", "idShort": "Config16DI", "...": "2 cards of 8 inputs"},
   {"modelType": "SubmodelElementCollection", "idShort": "Config8DI8DO", "...": "8 inputs, 8 outputs of 0.5 A"}]}
```

A request for 20 inputs and 8 outputs then scores 0.9: the best configuration has 16 of the 20
inputs and all 8 outputs.

```sql
CREATE TABLE raw AS
SELECT unnest(aas_to_trees(content, 'RFQ-2026-311')) AS n FROM read_text('examples/aas_capability_matching/data/io_profile/rfq_2026_311.aas.json')
UNION ALL SELECT unnest(aas_to_trees(content, 'IO-S')) FROM read_text('examples/aas_capability_matching/data/io_profile/ostwald_ios.aas.json');
CREATE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM raw;
CREATE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;

SELECT candidate_id, coverage, n_gaps FROM tree_coverage('RFQ-2026-311', candidates := ['IO-S'], labels := 'hard');
-- IO-S | 0.9 | 1
```

The same file without the markers (`ostwald_ios_plain.aas.json`) loads too, but every channel
block counts once and the configurations are siblings: coverage 0.2.

## Rules and limits

- A value directly inside a variant group is reported (`value_in_variant_group`) and ignored: a
  variant group holds alternatives, not values.
- A count that is not a positive number is reported (`bad_count`) and 1 is used.
- **Each requirement picks its best alternative** (HLD §4.6, request alternatives). The kernel does
  not force all requirements of one request onto the same configuration. In the example the
  inputs come from `Config16DI` and the outputs from `Config16DI8DO`. Read the trace's
  `matched_path` when the configuration must be one.
- The writer (`aas_export`, issue #34) writes the markers back, so a tree round-trips.
