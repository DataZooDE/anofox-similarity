# AAS: First Match

> An RFQ and the supplier answers arrive as Asset Administration Shells. Which offer meets the
> required capabilities, and what is missing in the others?

[Guides](README.md) · [AAS API](../api/17-aas.md) · [Term reference](../aas/index.md) · [Example](../../examples/aas_capability_matching/)

**Use this guide to:**
- Check AAS files before you compare them (`aas_issues`)
- Load an RFQ and offers into property trees (`aas_to_trees`)
- Rank the offers (`tree_coverage`) and list each gap (`tree_coverage_trace`)
- Send the result back as an AAS submodel (`aas_coverage_result`)

The data is the fictional servo-drive RFQ 2026-117 in `examples/aas_capability_matching/data`:
the RFQ with IDTA-02020 *Required* capabilities, and four offers as AAS JSON, XML and AASX.
Run the blocks in order from the repository root. For your own files, change the paths.

## 1. Audit the Files

`aas_issues` never fails. It lists what will not compare: values without a unit, unknown unit
ids, a decimal comma in an `xs:double`, a semanticId without a dictionary entry.

```sql
CREATE TABLE eclass_units AS SELECT * FROM read_csv('examples/aas_capability_matching/data/eclass_units.csv');

SELECT parse_filename(filename) AS file, i.issue, i.severity
FROM (SELECT filename, unnest(aas_issues(content, NULL, (SELECT list(u) FROM eclass_units u))) AS i
      FROM read_blob('examples/aas_capability_matching/data/offers/*'))
ORDER BY file, i.issue;
```

| file                 | issue                       | severity |
|----------------------|-----------------------------|----------|
| kessler_ksd10.aasx   | bad_value                   | warning  |
| kessler_ksd10.aasx   | missing_concept_description | warning  |
| orbitec_ox6.aas.json | decimal_comma               | info     |
| orbitec_ox6.aas.json | unknown_unit_id             | warning  |

`eclass_units` maps ECLASS unit IRDIs to symbols. ECLASS units are licensed, so bring your own
list (`symbol, irdi`); the example ships a stand-in. Without it, a value whose unit is given only
as an IRDI has no unit. `read_blob` reads JSON, XML and AASX packages alike.

## 2. Load the Trees

`aas_to_trees` turns each shell into one tree: capabilities become items, properties become
attributes with a dictionary key (`attr_key`, the semanticId), a value in base units and a bound.

```sql
CREATE TABLE raw AS
SELECT unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n
FROM read_text('examples/aas_capability_matching/data/rfq_2026_117.aas.json')
UNION ALL
SELECT unnest(aas_to_trees(content, NULL, (SELECT list(u) FROM eclass_units u)))
FROM read_blob('examples/aas_capability_matching/data/offers/*');
CREATE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM raw;
CREATE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;

SELECT attr_id, lo, hi, unit, bound, category
FROM tree_attributes WHERE tree_id = 'RFQ-2026-117' ORDER BY attr_id;
```

| attr_id            | lo     | hi     | unit | bound    | category         |
|--------------------|-------:|-------:|------|----------|------------------|
| AmbientTemperature | 318.15 | 318.15 | K    | at_least | NULL             |
| CycleTime          | 0.001  | 0.001  | s    | at_most  | NULL             |
| Protocol           | NULL   | NULL   | NULL | NULL     | 0173-1#07-AAZ001 |
| RatedCurrent       | 7.6    | 7.6    | A    | at_least | NULL             |
| SafeTorqueOff      | NULL   | NULL   | NULL | NULL     | true             |
| SupplyVoltage      | 380.0  | 480.0  | V    | within   | NULL             |

The second argument names the RFQ tree; with NULL the tree id is the shell id. 45 °C is stored
as 318.15 K. `validate_trees('tree_nodes', 'tree_attributes')` reports what the kernel cannot
use; here one offer value without a unit (Kessler's `AmbientTemperature`).

## 3. Rank the Offers

```sql
SET VARIABLE offers = (SELECT list(DISTINCT tree_id ORDER BY tree_id) FROM tree_nodes WHERE tree_id <> 'RFQ-2026-117');

SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('RFQ-2026-117', candidates := getvariable('offers'))
ORDER BY rank;
-- => https://lenvaro.example/aas/lx-8 | 1.0 | 0
-- => https://orbitec.example/aas/ox-6 | 0.75 | 1
-- => https://kessler-antriebe.example/aas/ksd-10 | 0.625 | 2
-- => https://driventa.example/aas/ds-12 | 0.372 | 2
```

Properties are matched by dictionary key, whatever the supplier named them, and in base units:
an offer of 8 A meets `mindestens 7637,1 mA`. A different key is never a match, even when the names or
the IRDIs look alike. `coverage` is the share of the RFQ met; `n_gaps` counts the capabilities
that fall short. Pass `candidates` so the RFQ is not ranked against itself.

## 4. Read the Gaps

```sql
CREATE TABLE trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'));

SELECT split_part(candidate_id, '/', -1) AS offer, attr_id, req_value, prov_value, rule
FROM trace WHERE attr_score < 0.99 ORDER BY offer, attr_id;
```

| offer  | attr_id            | req_value                         | prov_value                  | rule         |
|--------|--------------------|-----------------------------------|-----------------------------|--------------|
| ds-12  | AmbientTemperature | at_least 318.15 K                 | 313.15 K                    | at_least     |
| ds-12  | Protocol           | point EtherCAT (0173-1#07-AAZ001) | PROFINET (0173-1#07-AAZ002) | compat       |
| ds-12  | RatedCurrent       | at_least 7.6 A                    | 12 A                        | key_mismatch |
| ds-12  | SafeTorqueOff      | point true                        | false                       | compat       |
| ds-12  | SupplyVoltage      | within 380..480 V                 | 400..480 V                  | within       |
| ksd-10 | AmbientTemperature | at_least 318.15 K                 | 55                          | gate         |
| ksd-10 | CycleTime          | at_most 0.001 s                   | ?                           | missing      |
| ox-6   | RatedCurrent       | at_least 7.6 A                    | 6 A                         | at_least     |
| ox-6   | SupplyVoltage      | within 380..480 V                 | 200..240 V                  | within       |

Read `rule` before you reject an offer:

| rule | Means | Action |
|------|-------|--------|
| `at_least`, `at_most`, `within` | the value is stated and misses the limit | a real gap |
| `compat` | a category (protocol, flag) that does not meet the required one | a real gap |
| `key_mismatch` | the offer states another property (Driventa's 12 A is a *peak* current) | ask for the required one |
| `gate` | the value cannot be compared (no unit, other dimension) | ask; fix the file |
| `missing` | no usable value (`n/a`) | ask |

`within` means the offer covers the whole required range: 400–480 V misses 380 V.

## 5. Send the Result Back

`aas_coverage_result` writes one `CoverageResult` submodel per offer from the trace: the coverage
and every gap, as AAS JSON for the supplier portal.

```sql
SELECT split_part(candidate_id, '/', -1) AS offer, aas_json LIKE '%"idShort": "CoverageResult"%' AS is_submodel
FROM aas_coverage_result('trace') ORDER BY offer;
-- => ds-12 | true
-- => ksd-10 | true
-- => lx-8 | true
-- => ox-6 | true
```

`aas_export()` writes every loaded tree back as an AAS environment; read back, it is the same
tree. To write a file, see [step 3 of the example](../../examples/aas_capability_matching/03_write_back_as_aas.sql).

## Pitfalls

| Symptom | Cause | Fix |
|---------|-------|-----|
| Every value of one offer is `gate` | its unit ids are ECLASS IRDIs your unit list lacks | add them to the unit list; check `aas_issues` (`unknown_unit_id`) |
| An offer with plain names (no semanticIds) matches nothing | properties match by dictionary key | code the names once: [example 04](../../examples/aas_capability_matching/04_enrich_uncoded_supplier.sql) (`suggest_semantic_id`) |
| An older ECLASS release (`#007` vs `#008`) | the key ignores the version suffix | nothing; `aas_quality` counts mixed releases |
| A product family is one shell per variant | AAS has no "one of these configurations" | the [DataZoo AAS profile](10-aas-profile.md) |
| The RFQ appears in its own ranking | `candidates` not passed | pass `candidates` or keep the RFQ in its own tables |

## Next

- [The DataZoo AAS profile](10-aas-profile.md): variants and counts in one shell.
- [AAS API](../api/17-aas.md): every function, the IDTA-02020 mapping, constraints and AASX.
- [Coverage matching](04-coverage-matching.md): the kernel behind `tree_coverage`.
