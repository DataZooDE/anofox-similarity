# AAS Mapping

> How Asset Administration Shells become property trees, why dictionary keys and not wording
> decide a match, where bounds and units come from, and how trees and results go back to AAS.
> For the calls, see [guide 11](../guides/11-aas-first-match.md) and [api/17](../api/17-aas.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## What AAS Standardises, and What It Does Not

AAS v3 standardises the envelope (shells, submodels, typed elements) and dictionary-coded
properties: a `semanticId` points to an ECLASS or IEC CDD entry. IDTA-02020 (Capability
Description) marks capabilities as *Required* or *Offered*. No AAS specification says how to
match the two, rank offers or explain a gap. The extension reads AAS into the same property
trees as JSON (`tree_from_json`), so the [coverage kernel](coverage-semantics.md) does the
matching, and writes trees and results back as AAS.

## From Submodels to Trees

One shell is one tree. The structure maps by what an element holds:

- A **collection that holds values** (Property, Range, a list of values) is an **item**: the thing
  required or offered. Its class is the collection's semanticId.
- A **collection that holds only collections** is a transparent container, so nesting depth does
  not have to agree between request and offer.
- An IDTA-02020 **CapabilityContainer** is an item: the Capability's supplementalSemanticId is the
  class, its PropertySets give the attributes, and the role qualifier gives `role`.
  **ComposedOf** puts a part's item under its composite (AND); **GeneralizedBy** becomes a row of
  the class hierarchy (`aas_class_hierarchy`).
- Text, MultiLanguageProperty, File and references are description, not values: they stay out.

Node ids are idShort paths (`TechnicalData.Drive`), so a trace row names the element a person
finds in an AAS viewer. The full element-by-element table is in
[api/17](../api/17-aas.md#load-aas-aas_to_trees-aas_issues).

## Keys, Not Wording

The same property is written in several ways: an IRDI with a release, the ECLASS IRI form, an
older release, other case and spaces. `semantic_key` reduces them to one key, and the kernel
compares keys after it. The release suffix is dropped by default: a new ECLASS release usually
keeps a property's meaning, and an offer written against release 12 must match a request written
against release 14.

```sql
SELECT count(DISTINCT semantic_key(id)) AS keys, count(DISTINCT semantic_key(id, true)) AS with_release
FROM (VALUES ('0173-1#02-AAC895#008'), ('https://api.eclass-cdp.com/0173-1-02-AAC895-008'),
             (' 0173-1#02-aac895#005 ')) t(id);
-- => 1 | 2
```

Three spellings, one property; with the release kept, two. A release *can* change a meaning (a
new unit, a narrower definition), so `semantic_key(id, true)` keeps it for joins where that
matters. In matching, equal keys decide and different keys are a hard gate; wording only decides
when a side has no key. An IRDI used as a plain name is not a key: two different IRDIs share the
tokens `0173`, `1` and `02` and would get partial name credit.

## Units and ConceptDescriptions

A value needs a unit to compare. The unit comes, in this order, from the element's own IEC 61360
data specification, then field by field from the ConceptDescription of its semanticId. A
ConceptDescription is taken by its **exact release** first, and from another release only when
all releases in the input agree on the unit, because a property may change its unit between
releases. A `unitId` is resolved before the unit text. ECLASS unit IRDIs are licensed content:
the extension ships UN/ECE Rec 20 codes and takes ECLASS units from your `unit_table`. A
dictionary entry without a unit makes the value a count; a value with no unit information at all
is reported, not guessed.

## Bounds From the Data Model

AAS has no "at least". The direction comes from the element type and the IEC 61360 `levelType`:

| AAS | Bound | `bound_source` |
|---|---|---|
| Range with only min / only max / both | `at_least` / `at_most` / `within` | `range` |
| single value, `levelType` exactly min / max / nom | `at_least` / `at_most` / `equals` | `level_type` |
| Range with min and max, `levelType` exactly nom | `equals` over [min, max]: a nominal with a tolerance | `level_type` |
| IDTA-02020 BasicConstraint (`<= 1 ms`) on named properties | the constraint's bound and limit | `constraint` |
| a quantity written as text (`24 V DC`) | as `parse_quantity` reads it | `text` |

`levelType` typ, or several levels at once, give no bound. A Technical Data submodel with all
three cases:

```sql
SET VARIABLE td = '{"submodels": [{"modelType": "Submodel", "id": "urn:example:td", "idShort": "TechnicalData",
  "submodelElements": [{"modelType": "SubmodelElementCollection", "idShort": "Drive",
    "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#01-AKG243#015"}]},
    "value": [
      {"modelType": "Range", "idShort": "RatedVoltage", "valueType": "xs:double", "min": "22.8", "max": "25.2",
       "semanticId": {"type": "ExternalReference", "keys": [{"type": "GlobalReference", "value": "0173-1#02-AAC895#008"}]},
       "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360",
         "preferredName": [{"language": "en", "text": "rated voltage"}], "unit": "V",
         "levelType": {"min": false, "nom": true, "typ": false, "max": false}}}]},
      {"modelType": "Property", "idShort": "NoiseLevel", "valueType": "xs:double", "value": "38",
       "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360",
         "preferredName": [{"language": "en", "text": "noise level"}], "unit": "dB(A)",
         "levelType": {"min": false, "nom": false, "typ": false, "max": true}}}]},
      {"modelType": "Range", "idShort": "AmbientTemperature", "valueType": "xs:double", "min": "-20",
       "embeddedDataSpecifications": [{"dataSpecificationContent": {"modelType": "DataSpecificationIec61360",
         "preferredName": [{"language": "en", "text": "ambient temperature"}], "unit": "°C"}}]}]}]}]}';
CREATE OR REPLACE TABLE raw AS SELECT unnest(aas_to_trees(getvariable('td'), 'DRIVE-1')) AS n;
CREATE OR REPLACE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;

SELECT attr_id, round(lo, 2) AS lo, round(hi, 2) AS hi, unit, bound, bound_source
FROM tree_attributes ORDER BY attr_id;
-- => AmbientTemperature | 253.15 | 253.15 | K | at_least | range
-- => NoiseLevel | 38.0 | 38.0 | dB(A) | at_most | level_type
-- => RatedVoltage | 22.8 | 25.2 | V | equals | level_type
```

The rated voltage is 24 V ± 5 %: as a requirement the offer must lie inside 22.8–25.2 V, as an
offer it is the range it spans (see [quantities](../guides/05-quantities-and-specs.md)). Values
are in base units (−20 °C is 253.15 K).

**Constraints.** An IDTA-02020 BasicConstraint is the supplier's or buyer's explicit limit, so it
overrides the bound a Range implies. When it disagrees with the stated value it is used and
reported (`constraint_conflict`); a two-sided Range, including a nominal span, is kept, because a
one-sided constraint must not collapse a stated interval to a point. A constraint in another
dimension is not applied. `bound_source` shows in the trace where each bound came from.

## Roles Are Informational

`role` (Required / Offered) is read from the CapabilityRoleQualifier, but `tree_coverage` always
treats the query tree as the requirement and the candidates as offers. The same offer tree is a
candidate in one ranking and the query in another (an offer becomes a past solution), and
enforcing the role would block exactly that reuse. `validate_trees` warns when one tree mixes
roles.

## Writing Back

`trees_to_aas` is the inverse of the reader: idShort paths come from the node ids, so a tree read
from AAS reads back with the same nodes, classes, counts, keys, values, units and bounds. What
does not survive is derived on the way: text values (written in base units) and the bound's
source, which is re-derived from the written element:

```sql
SET VARIABLE back = (SELECT trees_to_aas(list(n)) FROM raw);
SELECT a.attr_id, round(a.lo, 2) AS lo, round(a.hi, 2) AS hi, a.bound, a.bound_source
FROM (SELECT unnest(n.attributes) AS a FROM (SELECT unnest(aas_to_trees(getvariable('back'))) AS n))
ORDER BY a.attr_id;
-- => AmbientTemperature | 253.15 | 253.15 | at_least | range
-- => NoiseLevel | 38.0 | 38.0 | at_most | range
-- => RatedVoltage | 22.8 | 25.2 | equals | level_type
```

The noise limit is written as a Range with only a max, so it comes back from `range`; the bound is
the same. A nominal span is written as a Range with `levelType` nom and stays `equals`.

The result of a ranking is AAS too: `aas_coverage_result` writes one `CoverageResult` submodel
per query and candidate, with the coverage, the gap count and every requirement and attribute of
the trace. Its terms are in the `https://anofox.com/aas/` namespace until a standard template
exists ([term reference](../aas/index.md)), and `aas_to_trees` reads it back.

## What AAS Cannot Say

Two things the kernel needs have no AAS construct: a configuration space (one of these
alternatives) and additive counts (2 cards of 8 channels). Without them a supplier publishes one
shell per variant. The [DataZoo AAS profile](../guides/10-aas-profile.md) adds two optional
qualifiers and one Count property for that; plain AAS without them loads unchanged.

## Why These Defaults

| Default | Why |
|---|---|
| Release suffix stripped in matching | Releases rarely change a property's meaning; mixed releases are the normal case in practice |
| Exact-release ConceptDescription first | A property may change its unit between releases; another release is used only when all agree |
| No ECLASS content shipped | ECLASS dictionaries and unit lists are licensed; bring yours through `unit_table` |
| A string is a number only with a written unit | Class ids, versions (`15.0`) and dates must not become quantities |
| BasicConstraint over the Range bound | The constraint is the explicit statement of the limit |
| `role` not enforced | The same tree is an offer in one ranking and the query in another |
| `id_prefix` `https://anofox.com/aas/results/` | Valid, collision-free ids out of the box; use your own namespace before you publish |

The design history (decisions of the AAS epic and its review) is in the
[HLD](../requirements-matching/HLD.md#12-aas-integration-2026-11-11); it records how these rules
were decided, not the current contract.
