# Round 5: new user, docs only

## Goal

Play a new user at a mid-sized machine builder (belt conveyors). Use only the docs, as read on
GitHub, and no repository data files. Get a first useful result for three tasks on my own data:

1. Similar materials from a SAP extract (MARA, MAKT, MAST, STKO, STPO, MSEG).
2. A coverage ranking with gaps for a German customer tender against three past offers.
3. A match of a supplier offer delivered as AAS JSON.

Reading order: README quick starts, guide 01, guide 12, guide 13, guide 11, and concepts pages
when a result surprised me.

## Setup

- Stock DuckDB v1.5.6 CLI, extension loaded with `-unsigned -cmd "LOAD '.../anofox_similarity.duckdb_extension'"`,
  `DATAZOO_DISABLE_TELEMETRY=1`. `extension_version` reports `29ad5b7`.
- Work dir: `scratchpad/r5user` (empty at start). Helper `q.sh file.sql [db]` pipes a file into the CLI.
- My data (all invented, all mine):
  - `sap/*.csv`: SE16-style export, `;` separated, uppercase headers, German decimal comma
    (`1,000`), 18-digit material numbers. Four conveyors: BF-400 (`…400100`), BF-500 (`…400200`),
    BF-600 (`…400300`, new, no history), RF-300 (`…400400`). One wrong unit: BF-500's aluminium
    profile is `12000,000 MM` where the others say `12,000 M`. MSEG has BF-400 with its zeros
    dropped (`400100`), a 602 reversal and a 101 receipt.
  - Tender `LH-2026-044` as JSON (lang de): belt conveyor (Gurtbreite mind. 550 mm, Förderlänge
    6 bis 8 m, ≥ 0,4 m/s, mind. 30 kg, höchstens 70 dB(A)), drive (max. 0,75 kW, IP65, 400 V),
    light barrier (mind. 2 m). Archive of three past offers (German texts), one with `400 VAC`.
  - Supplier AAS `supplier/mueller_gm055.aas.json`: one shell, Technical Data submodel, gear
    motor with rated power `"0,55"` and unit `W` (wrong: means kW), `IP65`, `"400 V"` as text, a
    speed without any unit information.

## What worked

- Every README quick start ran on the stock CLI and gave the printed numbers (Jaccard 0.75;
  predecessor 0.79 / -1.0 / 38; cold start 1.0 / 9 months; tree coverage 1.0 and 0.5 with the
  gap row; own-column views 0.667, 1.0 / 0.333).
- Guide 01 claims hold: scalar named arguments bind by position on 1.5.6 (`1.0 | 1.0 | 0.5`);
  the `anofox_similarity.` prefix fails as stated; `k := 3` on the material search fails;
  LOAD on a writable file creates the four documented tables.
- `sap_to_bom_items` and `sap_to_materials_with_desc` took my uppercase SE16 headers unchanged.
  `client = '100'`, `bom_usage := '1'`, `language := 'D'` worked as documented.
- `parse_quantity` reads SAP units directly: `'12000.000 MM'` → 12.0 m, `'1 ST'` → count 1.
  That fixed my wrong unit for weighted coverage (see F4).
- `tree_from_json` parsed every German value of the tender correctly (`mind.`, `bis`, `≥ 0,4 m/s`,
  `höchstens 70 dB(A)`, `max. 0,75 kW` → 750 W, `IP65` → category).
- `attr_key` worked as guide 13 says: `name_rule = key` on Gurtbreite / Förderlänge.
- `unit_table` on `tree_from_json` fixed `400 VAC` (struct shape from guide 05 / API 11).
- The concepts page `aas-mapping.md` has the only inline AAS JSON template; it was enough to
  write my own supplier file. `aas_issues` found my decimal comma and the unit-less speed.
- `aas_coverage_result('trace')` wrote a CoverageResult submodel for my offer (87011 bytes).

## Findings

### F1 [high] Guides 11 and 12 and the skills run only from a repository clone

Guide 12 says its blocks "run from the repository root" on `examples/erp_extract/data`; guide 11
on `examples/aas_capability_matching/data`; guide 05 loads `test/data/coverage/compatibility_rules.csv.gz`;
the AAS skill reads `test/data/aas/...` 21 times, the coverage skill 9 times. README tells a
community user to clone for the data. No page says how to get the files without a clone (for
example `read_csv('https://raw.githubusercontent.com/...')` with httpfs). A community user
cannot run guide 11 at all: it has no inline RFQ or offer, and the ECLASS unit list is a repo
file.

Repro (guide 12:46, outside a clone):

```sql
CREATE TABLE mara AS SELECT * FROM read_csv('examples/erp_extract/data/mara.csv', all_varchar = true);
```

Not run on purpose (the files do not exist outside a clone; the result is a DuckDB IO error).
I worked around it by writing my own extract.

Docs: README.md:322-323, docs/guides/01-getting-started.md:57-58, docs/guides/12-from-erp-extract.md:11-15 and :46-54,
docs/guides/11-aas-first-match.md:14-16, :24, :51-54, docs/guides/05-quantities-and-specs.md:196-197,
examples/README.md:44, plugins/anofox-similarity/skills/anofox-similarity-aas/SKILL.md:69 ff.

Fix: give each data-backed guide a "without a clone" line with raw GitHub URLs (httpfs), or
inline a minimal data set. Guide 11 needs an inline RFQ and one inline offer (the
`aas-mapping.md` template shows it fits in 20 lines). Skills: prefer inline data in the
runnable examples, or state the clone requirement at the top.

### F2 [medium] Tender range "6 bis 8 m" scores 0.32 for an offer of 7 m

A German tender writes an acceptable band as `6 bis 8 m`. The parser makes it `within`, which
means "the offer must span the whole range". An offer of 7 m or 7.5 m gets 0.32 and the item
becomes a gap. Guide 13 (the page for tenders) does not mention this. I found the reason only in
the concepts page and in a pitfall row of guide 05.

```sql
SELECT value_satisfies(parse_quantity('6 bis 8 m', 'de'), parse_quantity('7 m')) a,
       value_satisfies(parse_quantity('6 - 8 m'), parse_quantity('8 m')) c;
```

```text
{'score': 0.32015691118604417, 'distance': 0.6798430888139558, 'rule': within}
{'score': 0.16007845559302208, 'distance': 0.8399215444069779, 'rule': within}
```

In the ranking (trace, `WHERE is_gap`):

```text
ANG-2024-017 | LH.1 | laenge | within 6..8 m | 7 m   | 0.32 | within | true
ANG-2025-003 | LH.1 | laenge | within 6..8 m | 7.5 m | 0.32 | within | true
```

Workaround: split into `at_least 6` and `at_most 8` with the same `attr_key`. ANG-2025-003 then
goes from 0.943 to 0.99 coverage, ANG-2024-017 from 0.505 to 0.671.

Docs: docs/guides/13-tender-to-trees.md:21-28 (what upstream delivers) and :53-55; explained in
docs/concepts/quantities-and-bounds.md:77-79 and docs/guides/05-quantities-and-specs.md:324.

Fix: in guide 13, "What Upstream Delivers", say that a tender band (`von … bis`, `6 bis 8 m`)
must reach the extension as two attributes (`mind. 6 m`, `max. 8 m`, same `attr_key`), and that
`bis` is read as a capability range. Show it in the tender JSON. The concepts page explained the
surprise well once found.

### F3 [medium] AAS offer against a non-AAS tender: classes and counts are not covered

Guide 11 assumes the RFQ is AAS too. My tender is JSON from extraction; the supplier sent AAS.
Two guesses were needed:

1. The AAS item's `class_id` is the ECLASS key of the collection (`0173-1#01-AAA123`). The tender
   class must equal it; I mapped my tender wording to that key in the vocabulary. No page says so.
2. An AAS type shell has `count = 1`. My tender asks for 2 drives, so the only matching item is a
   count shortfall and every row is `is_gap`, though all three values pass.

```sql
CREATE OR REPLACE TABLE aas_vocabulary AS SELECT * FROM (VALUES
  ('Antrieb Getriebemotor', NULL, '0173-1#01-AAA123')) t(wording, source, class_id);
SELECT DISTINCT req_node_id, matched_node_id, count_needed, count_available, round(req_score,3) req_score
FROM tree_coverage_trace('LH-2026-044', candidates := ['https://mueller-antriebe.example/aas/gm-055'],
  nodes_table := 'request_nodes', attrs_table := 'request_attributes',
  candidate_nodes_table := 'offer_nodes', candidate_attrs_table := 'offer_attributes',
  overrides_table := 'aas_vocabulary') ORDER BY 1;
```

```text
LH.1 | NULL                        | 2.0 | 0.0 | 0.0
LH.2 | TechnicalData.Getriebemotor | 2.0 | 1.0 | 0.5
LH.3 | NULL                        | 4.0 | 0.0 | 0.0
```

With the request counts set to 1, coverage goes from 0.167 to 0.333 (n_gaps 3 → 2).

Docs: docs/guides/11-aas-first-match.md:43-46 and :145-153; docs/concepts/aas-mapping.md:154-158
mentions counts only for the DataZoo profile.

Fix: add a guide 11 section "Your tender is not AAS": set the tender item's class to the
ECLASS class of the offer (vocabulary or `class_id`), give the tender attributes the ECLASS
property IRDIs as `attr_key`, and compare a type data sheet with counts of 1 (or say the
count is a commercial quantity, not a capability). Add a pitfall row "Every row is_gap though
the values pass → count shortfall against a type shell".

### F4 [medium] Guide 12 says "normalise" mixed units but not how

Guide 12 says Jaccard ignores quantities and to "normalise before you compare quantities". My
BF-500 states `12000 MM` where the others state `12 M`. With `weight_col := 'quantity'`, BF-400
covers BF-500 at 0.001:

```sql
SELECT entity_id, round(containment, 3) AS coverage
FROM find_covering_entities('000000000000400200', 3,
  feature_table := 'bom_items', entity_col := 'parent_id', feature_col := 'child_id', weight_col := 'quantity');
```

```text
000000000000400100 | 0.001
000000000000400300 | 0.0
000000000000400400 | 0.0
```

`parse_quantity` reads SAP MEINS directly, which nobody tells the ERP reader:

```sql
CREATE OR REPLACE VIEW bom_items_norm AS
SELECT parent_id, child_id, (parse_quantity(quantity::VARCHAR || ' ' || unit)).lo AS qty_base FROM bom_items;
-- same query on bom_items_norm / qty_base:
```

```text
000000000000400100 | 0.679
000000000000400300 | 0.302
000000000000400400 | 0.302
```

Docs: docs/guides/12-from-erp-extract.md:180.

Fix: replace "normalise" with the one-line view above, and say `ST`, `M`, `MM`, `KG`, `G` are
read as SAP writes them (check others with `quantity_units()`).

### F5 [medium] A wrong unit in AAS passes silently in the at_most direction

The supplier wrote rated power `0,55` with unit `W` (meant kW). `aas_issues` reports only the
decimal comma. The tender says `max. 0,75 kW`; 0.55 W meets it with score 1.

```sql
SELECT req_node_id, attr_id, req_value, prov_value, round(value_score,3) vs, rule, name_rule
FROM tree_coverage_trace('LH-2026-044', candidates := ['https://mueller-antriebe.example/aas/gm-055'],
  nodes_table := 'request_nodes', attrs_table := 'request_attributes',
  candidate_nodes_table := 'offer_nodes', candidate_attrs_table := 'offer_attributes',
  overrides_table := 'aas_vocabulary') WHERE req_node_id = 'LH.2' ORDER BY ALL;
```

```text
LH.2 | ip | point IP65    | IP65   | 1.0 | compat  | exact
LH.2 | p  | at_most 750 W | 0.55 W | 1.0 | at_most | tokens
LH.2 | u  | point 400 V   | 400 V  | 1.0 | point   | tokens
```

The same holds for my SAP case in the other direction. A unit slip by 1000 is the most common
data error in master data. No page warns that an `at_most` (or `at_least`) limit hides it.

Docs: docs/guides/11-aas-first-match.md:20-21 and :116-126 (read `rule` before you reject);
docs/guides/13-tender-to-trees.md:53-55 says "a clause read wrongly scores as a gap", the
opposite case (a wrong offer value scores as a pass) is not mentioned.

Fix: add to guide 11 and 13 a pitfall row "an offer value 1000× off still passes a one-sided
limit: check `prov_value` of passing rows against the requirement's order of magnitude". Option
for later: an `aas_issues` / `validate_trees` info when a value is far outside the ranges of the
same `attr_key` in the input.

### F6 [low] Guide 12 load step fails on a German SE16 export (decimal comma)

```sql
CREATE TABLE stpo AS
SELECT * REPLACE (menge::DECIMAL(13, 3) AS menge) FROM read_csv('sap/STPO.csv', all_varchar = true);
```

```text
Conversion Error: Could not convert string "1,000" to DECIMAL(13,3) when casting from source column MENGE
```

The error is clear, but guide 12 says "all columns as text" and gives a cast that only fits a
dot export. Fix used: `replace(menge, ',', '.')::DECIMAL(13, 3)`.

Docs: docs/guides/12-from-erp-extract.md:42-54.

Fix: one sentence: "A German-locale export writes `1,000`; use `replace(menge, ',', '.')` (and
strip `.` thousands separators first)."

### F7 [low] Guide 12's movement view drops unpadded material numbers silently

My MSEG has BF-400 as `400100`. The guide's view takes `matnr` as is. `cold_start_analogs`
returns only BF-500; BF-400 is missing without any message.

```sql
SELECT material_id, round(similarity, 4) AS similarity, history_months
FROM cold_start_analogs('000000000000400300', 3);
```

```text
000000000000400200 | 0.5714 | 2
```

After padding MSEG on load (`CASE WHEN regexp_full_match(matnr, '[0-9]+') THEN lpad(matnr, 18, '0') ELSE matnr END`):

```text
000000000000400200 | 0.5714 | 2
000000000000400100 | 0.25   | 3
```

Docs: docs/guides/12-from-erp-extract.md:119-125; the fix is only in the pitfall table at :176.

Fix: put the `lpad` expression into the load step of guide 12 (it is harmless on padded data),
and keep the pitfall row.

### F8 [low] README trace query `WHERE is_gap` lists passing values too

README calls `WHERE is_gap` "the gap list, value by value". `is_gap` flags the whole
requirement, so on a real item with several attributes it returns rows with score 1:

```text
ANG-2024-017 | LH.1 | last | at_least 30 kg   | 40 kg   | 1.0 | at_least | true
ANG-2024-017 | LH.1 | v    | at_least 0.4 m/s | 0.5 m/s | 1.0 | at_least | true
```

The README example has one attribute per item, so it hides this. Guide 13 filters
`value_score < 1`, guide 11 `attr_score < 0.99`, API 13:125 a third form.

Docs: README.md:246-253; explained in docs/api/13-coverage-kernel.md:115 and docs/concepts/coverage-semantics.md:150.

Fix: README filter `WHERE is_gap AND (value_score IS DISTINCT FROM 1 OR count_available < count_needed)`,
and say in one line that `is_gap` marks the requirement. Use one filter in all pages.

### F9 [low] Category values warn in validate_trees; the guide shows 0 issues and no fix

My `IP65` gives a warning on the tender and on two offers; `400 VAC` gives `unknown_unit`:

```text
LH-2026-044  | LH.2  | ip | category_without_dimension | warning | category IP65 has no dimension; rules apply only when every dimension agrees on the pair
ANG-2024-017 | A17.2 | u  | unknown_unit               | warning | a value without a known dimension
```

The detail text does not say what to do. The fix (give the attribute an `attr_key`) is only in
a parenthesis of API 12:69. With `attr_key = 'schutzart'`, `validate_trees` returns 0.

Docs: docs/guides/13-tender-to-trees.md:57-63.

Fix: guide 13: "IP codes and other categories: give them an `attr_key`; unknown units: add them
with `unit_table` (example)". Detail text: append "give the attribute an attr_key".

### F10 [low] Hand-written sap_to_bom_items signature misses the `client` column

The output has 9 columns ending in `client` (shown by my first `sap_to_bom_items` query), and
guide 12 filters on it. The hand-written signature on the API page lists 8 columns, no `client`.
The generated block below it names `client`.

Docs: docs/api/05-erp-transformations.md:26-35.

Fix: add `client VARCHAR` to the hand-written signature.

### F11 [low] Guide 01 data-model table omits `attr_key`

The tree attribute shape lists `(tree_id, node_id, attr_id, name, dimension, unit, lo, hi[, bound, category])`.
Guides 11 and 13 depend on `attr_key`; `tree_from_json` returns it.

Docs: docs/guides/01-getting-started.md:109.

Fix: add `attr_key` to the optional columns.

### F12 [low] extension_version is a commit hash, docs speak of dated releases

```sql
SELECT extension_version FROM duckdb_extensions() WHERE extension_name = 'anofox_similarity';
```

```text
29ad5b7
```

README says the community build follows tagged releases and points to UPGRADING by release.
A user cannot map `29ad5b7` to a heading in UPGRADING. (This binary stands in for the community
build, so the real one may differ.)

Docs: README.md:126-130.

Fix: say the version is the commit of the release tag, or print the CalVer.

## Two-layer split

- Usage pages to a result: yes for task 1 (README, guide 01, guide 12, API 05 column table).
  Partly for task 2: guide 13 gave a ranking at the first try, but the ranking was wrong for my
  tender (F2) and the usage page did not say why. Not for task 3: guide 11 has no inline data;
  I needed the concepts page `aas-mapping.md` for a JSON template.
- Concepts pages explained the surprises: `quantities-and-bounds.md` explained the 0.32
  (`within` is a capability) clearly; `coverage-semantics.md` explained `is_gap`. They did not
  cover the AAS-offer vs JSON-tender case (F3) or silent wrong units (F5).
- The split is right, but each usage page should carry the one-line rule its reader will hit
  (F2, F9) with a link to the concept, not only the link.

## Time to first result

Counted in pages read and queries run (no wall-clock).

| Task | Pages read | Queries to first result | Dead ends | Useful? |
|------|-----------|-------------------------|-----------|---------|
| 1 SAP similar materials | README, guide 01, guide 12, API 05 (4) | 3 (1 failed: decimal comma) | F1 (repo data), F6 | Yes: BF-500 0.5714, BF-400 0.25. Cold start complete after the padding fix (F7), 5 runs |
| 2 Tender coverage with gaps | guide 13, API 13, concepts quantities-and-bounds, guide 05, API 12 (5) | 2 | F2, F8, F9 | First ranking at run 2 (ANG-2025-003 0.943); trustworthy at run 4 after splitting the band and adding VAC (0.99 / 0.671 / 0.111) |
| 3 AAS match | guide 11, API 17, concepts aas-mapping (3) | 2 (after writing the AAS file from the concepts template) | F1, F3, F5 | Ranking at run 2 (0.167), meaningful at run 4 (0.333) after guessing class mapping and counts; CoverageResult written |
