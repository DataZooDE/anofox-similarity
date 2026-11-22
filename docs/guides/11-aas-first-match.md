# AAS: First Match

> An RFQ and the supplier answers arrive as Asset Administration Shells. Which offer meets the
> required capabilities, and what is missing in the others?

[Guides](README.md) · [AAS API](../api/17-aas.md) · [Term reference](../aas/index.md) · [Example](../../examples/aas_capability_matching/)

**Use this guide to:**
- Check AAS files before you compare them (`aas_issues`)
- Load an RFQ and offers into property trees (`aas_to_trees`)
- Rank the offers (`tree_coverage`) and list each gap (`tree_coverage_trace`)
- Send the result back as an AAS submodel (`aas_coverage_result`)
- Match AAS offers against a tender that is not AAS

The data is inline, so every block runs as it stands, also without a clone of the repository: a
fictional servo-drive RFQ (`RFQ-117`, a Technical Data submodel with four required properties) and
three offers as AAS JSON. Each property carries its ECLASS IRDI as `semanticId`, its unit and, for
a limit, the IEC 61360 `levelType` (min, max). The full example with IDTA-02020 capabilities, XML
and AASX files is in [examples/aas_capability_matching](../../examples/aas_capability_matching/).

```sql
CREATE OR REPLACE TABLE aas_files AS SELECT * FROM (VALUES
('RFQ-117', '{"submodels": [{"modelType": "Submodel", "id": "urn:example:rfq-117/sm/td", "idShort": "TechnicalData", "submodelElements": [
  {"modelType": "SubmodelElementCollection", "idShort": "ServoDrive", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#01-AKG243#015"}]}, "value": [
    {"modelType": "Property", "idShort": "RatedCurrent", "valueType": "xs:double", "value": "7.6", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-BAA120#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "A", "levelType": {"min": true, "nom": false, "typ": false, "max": false}}}]},
    {"modelType": "Range", "idShort": "SupplyVoltage", "valueType": "xs:double", "min": "380", "max": "480", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAC895#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "V"}}]},
    {"modelType": "Property", "idShort": "AmbientTemperature", "valueType": "xs:double", "value": "45", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAZ953#005"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "°C", "levelType": {"min": true, "nom": false, "typ": false, "max": false}}}]},
    {"modelType": "Property", "idShort": "CycleTime", "valueType": "xs:double", "value": "1", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAQ326#004"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "ms", "levelType": {"min": false, "nom": false, "typ": false, "max": true}}}]}
]}]}]}'),
('LX-8', '{"submodels": [{"modelType": "Submodel", "id": "urn:example:lenvaro-lx8/sm/td", "idShort": "TechnicalData", "submodelElements": [
  {"modelType": "SubmodelElementCollection", "idShort": "ServoDrive", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#01-AKG243#015"}]}, "value": [
    {"modelType": "Property", "idShort": "Nennstrom", "valueType": "xs:double", "value": "8", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-BAA120#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "A"}}]},
    {"modelType": "Range", "idShort": "Netzspannung", "valueType": "xs:double", "min": "360", "max": "500", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAC895#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "V"}}]},
    {"modelType": "Property", "idShort": "Umgebungstemperatur", "valueType": "xs:double", "value": "50", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAZ953#005"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "°C"}}]},
    {"modelType": "Property", "idShort": "Zykluszeit", "valueType": "xs:double", "value": "0.5", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAQ326#004"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "ms"}}]}
]}]}]}'),
('OX-6', '{"submodels": [{"modelType": "Submodel", "id": "urn:example:orbitec-ox6/sm/td", "idShort": "TechnicalData", "submodelElements": [
  {"modelType": "SubmodelElementCollection", "idShort": "ServoDrive", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#01-AKG243#015"}]}, "value": [
    {"modelType": "Property", "idShort": "RatedCurrent", "valueType": "xs:double", "value": "6", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-BAA120#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "A"}}]},
    {"modelType": "Range", "idShort": "SupplyVoltage", "valueType": "xs:double", "min": "380", "max": "480", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAC895#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "V"}}]},
    {"modelType": "Property", "idShort": "AmbientTemperature", "valueType": "xs:double", "value": "45", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAZ953#005"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "°C"}}]},
    {"modelType": "Property", "idShort": "CycleTime", "valueType": "xs:double", "value": "0,5", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAQ326#004"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "ms"}}]}
]}]}]}'),
('DS-12', '{"submodels": [{"modelType": "Submodel", "id": "urn:example:driventa-ds12/sm/td", "idShort": "TechnicalData", "submodelElements": [
  {"modelType": "SubmodelElementCollection", "idShort": "ServoDrive", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#01-AKG243#015"}]}, "value": [
    {"modelType": "Property", "idShort": "PeakCurrent", "valueType": "xs:double", "value": "12", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-BAA121#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "A"}}]},
    {"modelType": "Range", "idShort": "SupplyVoltage", "valueType": "xs:double", "min": "400", "max": "480", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAC895#008"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "V"}}]},
    {"modelType": "Property", "idShort": "AmbientTemperature", "valueType": "xs:double", "value": "40", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAZ953#005"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "°C"}}]},
    {"modelType": "Property", "idShort": "CycleTime", "valueType": "xs:double", "value": "1", "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAQ326#004"}]}, "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360", "unit": "msec"}}]}
]}]}]}')
) t(name, content);
```

For your own files read them instead: `SELECT filename AS name, content FROM read_text('offers/*.json')`,
or `read_blob` for JSON, XML and AASX alike.

## 1. Audit the Files

`aas_issues` never fails. It lists what will not compare: values without a unit, unknown unit
ids and unit texts, a decimal comma in an `xs:double`, a semanticId without a dictionary entry.

```sql
SELECT name, i.issue, i.severity, i.path
FROM (SELECT name, unnest(aas_issues(content)) AS i FROM aas_files)
ORDER BY name, i.issue;
-- => DS-12 | unknown_unit | warning | TechnicalData.ServoDrive.CycleTime
-- => OX-6 | decimal_comma | info | TechnicalData.ServoDrive.CycleTime
```

Driventa wrote the unit `msec`, which no dictionary knows: that value will match nothing. Orbitec
wrote `0,5` ms; the reader takes it as 0.5 and tells you. When units come as ECLASS unit
IRDIs (`unitId`), pass your unit list as the third argument (`symbol, irdi`; ECLASS units are
licensed, so bring your own).

## 2. Load the Trees

`aas_to_trees` turns each shell into one tree: the collection becomes an item with the class key
as `class_id`, properties become attributes with a dictionary key (`attr_key`, the semanticId), a
value in base units and a bound. The second argument names the tree.

```sql
CREATE OR REPLACE TABLE raw AS SELECT unnest(aas_to_trees(content, name)) AS n FROM aas_files;
CREATE OR REPLACE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM raw;
CREATE OR REPLACE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;

SELECT attr_id, lo, hi, unit, bound FROM tree_attributes WHERE tree_id = 'RFQ-117' ORDER BY attr_id;
-- => AmbientTemperature | 318.15 | 318.15 | K | at_least
-- => CycleTime | 0.001 | 0.001 | s | at_most
-- => RatedCurrent | 7.6 | 7.6 | A | at_least
-- => SupplyVoltage | 380.0 | 480.0 | V | within
```

`levelType` min gives `at_least`, max `at_most`; a Range is `within`. 45 °C is stored as 318.15 K.
`validate_trees` reports what the kernel cannot use, here Driventa's unit `msec`:

```sql
SELECT tree_id, attr_id, issue, severity FROM validate_trees('tree_nodes', 'tree_attributes');
-- => DS-12 | CycleTime | unknown_unit | warning
```

## 3. Rank the Offers

```sql
SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('RFQ-117', candidates := ['LX-8', 'OX-6', 'DS-12'])
ORDER BY rank;
-- => LX-8 | 1.0 | 0
-- => OX-6 | 0.75 | 1
-- => DS-12 | 0.244 | 1
```

Properties are matched by dictionary key, whatever the supplier named them (Lenvaro writes
`Nennstrom`), and in base units. A different key is never a match, even when the names look
alike. `coverage` is the share of the RFQ met; `n_gaps` counts the items that fall short. Pass
`candidates` so the RFQ is not ranked against itself.

## 4. Read the Gaps

```sql
CREATE OR REPLACE TABLE trace AS
SELECT * FROM tree_coverage_trace('RFQ-117', candidates := ['LX-8', 'OX-6', 'DS-12']);

SELECT candidate_id AS offer, attr_id, req_value, prov_value, rule
FROM trace WHERE attr_score < 0.99 ORDER BY offer, attr_id;
-- => DS-12 | AmbientTemperature | at_least 318.15 K | 313.15 K | at_least
-- => DS-12 | CycleTime | at_most 0.001 s | 1 msec | gate
-- => DS-12 | RatedCurrent | at_least 7.6 A | 12 A | key_mismatch
-- => DS-12 | SupplyVoltage | within 380..480 V | 400..480 V | within
-- => OX-6 | RatedCurrent | at_least 7.6 A | 6 A | at_least
```

Read `rule` before you reject an offer:

| rule | Means | Action |
|------|-------|--------|
| `at_least`, `at_most`, `within` | the value is stated and misses the limit | a real gap |
| `compat` | a category (protocol, flag) that does not meet the required one | a real gap |
| `key_mismatch` | the offer states another property (Driventa's 12 A is a *peak* current) | ask for the required one |
| `gate` | the value cannot be compared (unknown unit, other dimension) | ask; fix the file |
| `missing` | no usable value (`n/a`) | ask |

`within` means the offer covers the whole required range: 400–480 V misses 380 V. How keys,
units and `levelType` become bounds: [AAS mapping](../concepts/aas-mapping.md).

## 5. Send the Result Back

`aas_coverage_result` writes one `CoverageResult` submodel per offer from the trace: the coverage
and every gap, as AAS JSON for the supplier portal.

```sql
SELECT candidate_id AS offer, aas_json LIKE '%"idShort": "CoverageResult"%' AS is_submodel
FROM aas_coverage_result('trace') ORDER BY offer;
-- => DS-12 | true
-- => LX-8 | true
-- => OX-6 | true
```

`aas_export()` writes every loaded tree back as an AAS environment; read back, it is the same
tree. To write a file, see [step 3 of the example](../../examples/aas_capability_matching/03_write_back_as_aas.sql).

## Your Tender Is Not AAS

Often only the supplier sends AAS and your tender comes from extraction as JSON
([guide 13](13-tender-to-trees.md)). Two things must line up:

- **The class.** An AAS item's `class_id` is the ECLASS class key of its collection
  (`0173-1#01-AKG243`, without the release). Give the tender item the same class, with
  `class_id` in the JSON or through the reviewed vocabulary (`overrides_table`).
- **The keys.** Give the tender attributes the ECLASS property IRDIs as `attr_key`, so they meet
  the offer's semanticIds instead of being compared by name.

```sql
CREATE OR REPLACE TABLE tender_nodes AS SELECT unnest(n) FROM (SELECT unnest(tree_from_json('{
 "tree_id": "LH-44", "lang": "de", "root": {"node_id": "LH-44", "children": [
  {"node_id": "LH-44.1", "label": "Servoantrieb Y-Achse", "count": 2, "attributes": [
   {"attr_id": "i", "name": "Nennstrom", "text": "mind. 7,5 A", "attr_key": "0173-1#02-BAA120#008"},
   {"attr_id": "t", "name": "Umgebungstemperatur", "text": "mind. 45 °C", "attr_key": "0173-1#02-AAZ953#005"}]}]}}')) AS n);
CREATE OR REPLACE TABLE tender_attributes AS SELECT unnest(attributes, recursive := true) FROM tender_nodes;
CREATE OR REPLACE TABLE aas_vocabulary AS SELECT * FROM (VALUES
  ('Servoantrieb Y-Achse', NULL, '0173-1#01-AKG243')) t(wording, source, class_id);
SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('LH-44', candidates := ['LX-8', 'OX-6'],
  nodes_table := 'tender_nodes', attrs_table := 'tender_attributes', overrides_table := 'aas_vocabulary',
  candidate_nodes_table := 'tree_nodes', candidate_attrs_table := 'tree_attributes')
ORDER BY rank;
-- => LX-8 | 0.5 | 1
-- => OX-6 | 0.25 | 1
```

LX-8 meets both values and still scores 0.5. The tender asks for 2 drives, and an AAS type shell
describes **one** product (`count` 1), so the count is short. A type data sheet states what one
unit can do; the quantity you order is commercial, not a capability. Compare with counts of 1:

```sql
UPDATE tender_nodes SET count = 1 WHERE node_kind = 'item';
SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('LH-44', candidates := ['LX-8', 'OX-6'],
  nodes_table := 'tender_nodes', attrs_table := 'tender_attributes', overrides_table := 'aas_vocabulary',
  candidate_nodes_table := 'tree_nodes', candidate_attrs_table := 'tree_attributes')
ORDER BY rank;
-- => LX-8 | 1.0 | 0
-- => OX-6 | 0.5 | 1
```

Keep counts where the offer itself states a quantity (a module with 4 channels, see
[the DataZoo AAS profile](10-aas-profile.md)).

## Pitfalls

| Symptom | Cause | Fix |
|---------|-------|-----|
| Every value of one offer is `gate` | its unit ids are ECLASS IRDIs your unit list lacks | add them to the unit list; check `aas_issues` (`unknown_unit_id`) |
| An offer with plain names (no semanticIds) matches nothing | properties match by dictionary key | code the names once: [example 04](../../examples/aas_capability_matching/04_enrich_uncoded_supplier.sql) (`suggest_semantic_id`) |
| Every row `is_gap` though the values pass | the tender asks for n pieces, the AAS type shell is one product | compare with counts of 1 (above) |
| A non-AAS tender matches no AAS offer | the tender class is not the ECLASS class key | map the wording to the class key (`overrides_table`) |
| An older ECLASS release (`#007` vs `#008`) | the key ignores the version suffix | nothing; `aas_quality` counts mixed releases |
| A product family is one shell per variant | AAS has no "one of these configurations" | the [DataZoo AAS profile](10-aas-profile.md) |
| An offer value 1000 times off (`0.55` W for kW) passes an `at_most` limit | a unit slip passes a one-sided limit | `validate_trees` over the request and the offers together (a view with `UNION ALL`) warns `magnitude_outlier`; one file alone cannot tell |
| The RFQ appears in its own ranking | `candidates` not passed | pass `candidates` or keep the RFQ in its own tables |

## Next

- [The DataZoo AAS profile](10-aas-profile.md): variants and counts in one shell.
- [AAS API](../api/17-aas.md): every function, the IDTA-02020 mapping, constraints and AASX.
- [AAS mapping](../concepts/aas-mapping.md): how submodels, keys and units become trees.
- [Coverage matching](04-coverage-matching.md): the kernel behind `tree_coverage`.
