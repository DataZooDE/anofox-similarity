# anofox AAS terms

> The identifiers under `https://anofox.com/aas/` that the anofox_similarity DuckDB extension
> reads and writes in Asset Administration Shells (AAS v3).

*This page is meant to be published at <https://anofox.com/aas/>. For the ids to resolve, the site
has to send every path under `/aas/` to this page, at the term's anchor (`/aas/Count/1/0` →
`/aas/#count`, `/aas/CoverageResult/LabelRule/1/0` → `/aas/#labelrule`). Ids under
`/aas/results/` name single results and are not meant to resolve. The source of this page is
`docs/aas/index.md` in the anofox-similarity repository.*

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
| <a id="variantgroup"></a>`anofox:VariantGroup` | qualifier `type`; the value is not read (the extension writes `xs:boolean` `true`) | a SubmodelElementCollection | Its child collections are **alternatives**: one of them is chosen, not all. A request is met by one alternative as a whole. |
| <a id="count"></a>`anofox:Count` | qualifier `type`, value a positive number of any valueType (the extension writes `xs:double`) | a SubmodelElementCollection | The collection is there that many times (e.g. 2 cards). |
| `https://anofox.com/aas/Count/1/0` | semanticId of a Property, value a positive number | a Property inside a collection | The same as `anofox:Count`, written as a property (e.g. 8 channels per card). It is a count, not a technical value. `http://`, `www.` and any version (`/2/0`) are read the same; a longer term such as `CountryCode` is not a count. |

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
| <a id="query"></a>`Query` | string | id of the request tree |
| <a id="candidate"></a>`Candidate` | string | id of the offer tree |
| <a id="coverage"></a>`Coverage` | double, 0–1 | how much of the request the offer covers |
| <a id="gapcount"></a>`GapCount` | int | requirements the offer does not meet |
| <a id="requirements"></a>`Requirements` | SubmodelElementList of `Requirement` | one entry per requirement of the request |
| <a id="requirement"></a>`Requirement` | collection | one requirement and what met it |
| <a id="requirementnode"></a>`RequirementNode` | string | the requirement's node in the request tree |
| <a id="matchednode"></a>`MatchedNode` | string | the offer node that meets it best (empty: nothing does) |
| <a id="matchedpath"></a>`MatchedPath` | string | the path to that node in the offer |
| <a id="labelsimilarity"></a>`LabelSimilarity` | double, 0–1 | how well the capability classes agree |
| <a id="labelrule"></a>`LabelRule` | string | how classes were compared: `exact`, `soft`, `generalized` (a more general class was offered, via a class hierarchy), `specialized` (a subclass of the requested class was offered) |
| <a id="countneeded"></a><a id="countavailable"></a>`CountNeeded`, `CountAvailable` | double | required and offered count, when counts matter |
| <a id="requirementscore"></a>`RequirementScore` | double, 0–1 | the requirement's score |
| <a id="isgap"></a>`IsGap` | boolean | the requirement is not met |
| <a id="attributes"></a>`Attributes` | SubmodelElementList of `Attribute` | the property comparisons of the requirement |
| <a id="attribute"></a>`Attribute` | collection | one required property against the offered one |
| <a id="attributeid"></a><a id="attributename"></a>`AttributeId`, `AttributeName` | string | the required property |
| <a id="required"></a><a id="offered"></a>`Required`, `Offered` | string | required bound and offered value, as text with units (`at_least 7.6 A`, `12 A`) |
| <a id="rule"></a>`Rule` | string | how the value was judged: the bound it was checked against (`at_least`, `at_most`, `within`), `compat` (categories), `missing` (no offered property), `key_mismatch` (only properties with another dictionary id), `gate` (other dimension), `point` (a value without a bound, compared as a point) |
| <a id="namerule"></a>`NameRule` | string | how the properties were paired: `key` (same dictionary id), `vector`, `exact`, `tokens` |
| <a id="boundsource"></a>`BoundSource` | string | where the bound came from: `range`, `level_type`, `constraint`, `text`, `explicit`, `template` (the catalogue's default bound, `default_bounds := 'catalogue'`) |
| <a id="score"></a>`Score` | double, 0–1 | the score of this property |

The meaning of every rule value is in the
[coverage kernel reference](https://github.com/DataZooDE/anofox-similarity/blob/main/docs/api/13-coverage-kernel.md).

## Status

These terms belong to the anofox_similarity extension. They are not an IDTA submodel template.
If an IDTA template for matching results is published, the extension will write that one as well.
