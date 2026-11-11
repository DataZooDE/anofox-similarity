# anofox AAS terms

> The identifiers under `https://anofox.com/aas/` that the anofox_similarity DuckDB extension
> reads and writes in Asset Administration Shells (AAS v3).

*This page is meant to be published at <https://anofox.com/aas/>, so that every id below resolves.
Its source is `docs/aas/index.md` in the anofox-similarity repository.*

There are two groups of terms. The **profile** is the few markers a supplier or buyer puts in an
AAS to say what AAS itself cannot express: alternative configurations, and counts that add up.
The **CoverageResult** is the submodel the extension writes to explain how well an offer covers a
request. Every term is versioned the IDTA way (`…/1/0`). A new meaning gets a new version; the
old one keeps its meaning.

All markers are ordinary AAS (qualifiers, semanticIds), so any AAS tool keeps reading the files.
A file without them is still valid and still loads.

## Profile

| Term | Kind | Put it on | Meaning |
|---|---|---|---|
| `anofox:VariantGroup` | qualifier `type`, value `true` (`xs:string`) | a SubmodelElementCollection | Its child collections are **alternatives**: one of them is chosen, not all. A request is met by one alternative as a whole. |
| `anofox:Count` | qualifier `type`, value a positive integer (`xs:int`) | a SubmodelElementCollection | The collection is there that many times (e.g. 2 cards). |
| `https://anofox.com/aas/Count/1/0` | semanticId of a Property, value a positive integer | a Property inside a collection | The same as `anofox:Count`, written as a property (e.g. 8 channels per card). It is a count, not a technical value. |

Counts multiply along the path: 2 cards × 8 channels = 16 channels. They are added up only inside
one alternative of a variant group, never across alternatives.

```json
{"modelType": "SubmodelElementCollection", "idShort": "DI8Card",
 "qualifiers": [{"type": "anofox:Count", "valueType": "xs:int", "value": "2"}],
 "value": [
   {"modelType": "Property", "idShort": "ChannelCount", "valueType": "xs:int", "value": "8",
    "semanticId": {"type": "ExternalReference",
                   "keys": [{"type": "GlobalReference", "value": "https://anofox.com/aas/Count/1/0"}]}}]}
```

A full example and the rules (a value placed directly in a variant group, a count that is not a
number) are in the [AAS profile guide](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/guides/10-aas-profile.md).

## CoverageResult

A Submodel with semanticId `https://anofox.com/aas/CoverageResult/1/0` explains one ranking: how
well one candidate (an offer) covers one query (a request). The extension writes it with
`aas_coverage_result` and `coverage_result_to_aas`. Its id is
`https://anofox.com/aas/results/<query>/<candidate>` (both base64url-encoded), unless the caller sets
its own prefix.

Every element's semanticId is `https://anofox.com/aas/CoverageResult/<Term>/1/0`. A metric that was not
computed is written without a value, which is not the same as 0.

| Term | Type | Meaning |
|---|---|---|
| `Query` | string | id of the request tree |
| `Candidate` | string | id of the offer tree |
| `Coverage` | double, 0–1 | how much of the request the offer covers |
| `GapCount` | int | requirements the offer does not meet |
| `Requirements` | SubmodelElementList of `Requirement` | one entry per requirement of the request |
| `Requirement` | collection | one requirement and what met it |
| `RequirementNode` | string | the requirement's node in the request tree |
| `MatchedNode` | string | the offer node that meets it best (empty: nothing does) |
| `MatchedPath` | string | the path to that node in the offer |
| `LabelSimilarity` | double, 0–1 | how well the capability classes agree |
| `LabelRule` | string | how classes were compared: `exact`, `soft`, `generalized` (via a class hierarchy) |
| `CountNeeded`, `CountAvailable` | double | required and offered count, when counts matter |
| `RequirementScore` | double, 0–1 | the requirement's score |
| `IsGap` | boolean | the requirement is not met |
| `Attributes` | SubmodelElementList of `Attribute` | the property comparisons of the requirement |
| `Attribute` | collection | one required property against the offered one |
| `AttributeId`, `AttributeName` | string | the required property |
| `Required`, `Offered` | string | required bound and offered value, as text with units (`at_least 7.6 A`, `12 A`) |
| `Rule` | string | how the value was judged: the bound it was checked against (`at_least`, `at_most`, `within`), `compat` (categories), `missing` (no offered property), `key_mismatch` (only properties with another dictionary id), `gate` (other dimension) |
| `NameRule` | string | how the properties were paired: `key` (same dictionary id), `vector`, `exact`, `tokens` |
| `BoundSource` | string | where the bound came from: `range`, `level_type`, `constraint`, `text`, `explicit` |
| `Score` | double, 0–1 | the score of this property |

The meaning of every rule value is in the
[coverage kernel reference](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/13-coverage-kernel.md).

## Status

These terms belong to the anofox_similarity extension. They are not an IDTA submodel template.
If an IDTA template for matching results is published, the extension will write that one as well.
