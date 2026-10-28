# Exploratory user testing, round 4: bid engineer (tender reuse)

Persona: the bid engineer of a mid-size builder of cup filling lines. They have one extracted
RFQ (JSON), a clause list with page and must/should, a PLM export of past lines, a module
catalogue and a house glossary. They want a ranking, a must-clause view and a deviation list
the customer can sign. The result is [`examples/tender_reuse/`](../../../examples/tender_reuse/).

Tested on the DuckDB 1.5.6 shell with the extension linked in (`build/release/duckdb`, built from
the UR-3 fixes). No C++, tests or other examples were changed. Every repro below was run on that
shell. Approximate hands-on time: first ranking 10 minutes, a deviation list I would sign
2 hours, mostly for findings R4-U1 to R4-U5.

What worked without a fight: every parser trap of the spec (`mind 60 mm`, `nicht über 75 dB(A)`,
`not to exceed 5 bar`, `± 0,5 g`, `PN 16`, `mind. 2.500 h`, `höchstens 7,5 A`) reads correctly;
designations `EN 10204 3.1`, `M8x1,25 aus A4-70` and `EN 1672-2` are refused; `validate_trees`
now warns about a category without a dimension; same-class items are paired by value
(PAST-SWAP-2018 covers fully); a custom unit (`Becher/min`) plugs into `tree_from_json` through
`(SELECT list(u) FROM units u)`.

## Findings

### R4-U1 — High — `tree_coverage` passes values up to 4.5 % past a hard limit, with no way to switch it off (fixed)

The kernel uses the default tolerance of `value_satisfies` (0.02; on a log scale 0.02 decades,
about 4.5 % relative) and has no `tol` / `soft_band` parameter. The catalogue template's `scale`
column is not used either. A bid that says "every must clause met" is then wrong.

```sql
CREATE TABLE qn AS SELECT * FROM (VALUES ('Q','Q',NULL,'container','AND','spec',NULL,1.0),
  ('Q','Q.1','Q','item','AND','drive','drive',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE qa AS SELECT * FROM (VALUES ('Q','Q.1','power','power','power','W',90.0,90.0,'at_least',NULL))
  t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
CREATE TABLE cn AS SELECT * FROM (VALUES ('C','C',NULL,'container','AND','offer',NULL,1.0),
  ('C','C.1','C','item','AND','drive','drive',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE ca AS SELECT * FROM (VALUES ('C','C.1','power','power','power','W',87.0,87.0,NULL,NULL))
  t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
CREATE TABLE tpl AS SELECT * FROM (VALUES ('drive','power','power','Leistung','power','W','at_least','linear'))
  t(class_id,attr_key,name_en,name_de,dimension,base_unit,bound,scale);
SELECT req_value, prov_value, value_score FROM tree_coverage_trace('Q', candidates := ['C'],
  nodes_table := 'qn', attrs_table := 'qa', candidate_nodes_table := 'cn', candidate_attrs_table := 'ca',
  mu := 0.0, default_bounds := 'catalogue', templates_table := 'tpl');
SELECT value_satisfies(parse_quantity('mind. 90 W'), parse_quantity('87 W'), 'linear').score;
```

**Fixed after this report:** `tree_coverage` and `tree_coverage_trace` take `tol` and `soft_band`, and the
template's `scale` column is honoured (the trace above now gives 0.7333; regression in
`test/sql/hardening/ur3_regressions.test`). The example's gold check shows the kernel finding
32 of 33 deviations by default and 33 of 33 with `tol := 0, soft_band := 0`.

Actual (before the fix): `at_least 90 W | 87 W | 1.0` in the trace; `0.7333` from `value_satisfies` on the linear
scale the template asks for. Expected: a way to make limits strict on the tree path. In the
example the kernel misses 2 of 33 ground-truth deviations (PAST-TOL-2023: 87 W, 7.6 A against
7,5 A). Suggested fix: `tol` and `soft_band` parameters on `tree_coverage` / `_trace` (and in the
`coverage_kernel` options), and honour the template `scale` (or drop the column from the
documented schema). Workaround in the example: a strict re-check with
`value_satisfies(req, prov, NULL, 0.0, 0.0)` joined onto the trace (`clause_check`, 02).

### R4-U2 — High — OR alternatives are chosen per requirement item, so a module "covers" with parts of two variants

`tree_coverage` picks the best OR alternative for each requirement item separately (HLD, section
on variants), so a station is reported as fully covered when no single variant covers it. The
guide and the API page say only that counts never add across OR alternatives.

```sql
CREATE TABLE q_nodes AS SELECT * FROM (VALUES ('Q','Q',NULL,'container','AND','spec',NULL,1.0),
 ('Q','Q.1','Q','item','AND','zone','zone',1.0), ('Q','Q.2','Q','item','AND','drive','drive',1.0))
 t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE q_attrs AS SELECT * FROM (VALUES ('Q','Q.1','stroke','stroke','length','m',0.06,0.06,'at_least',NULL),
 ('Q','Q.2','power','power','power','W',90.0,90.0,'at_least',NULL)) t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
CREATE TABLE m_nodes AS SELECT * FROM (VALUES ('M','M',NULL,'variant_group','AND','module',NULL,1.0),
 ('M','M.V','M','variant_group','OR','variants',NULL,1.0),
 ('M','M.V1','M.V','container','OR','V1',NULL,1.0), ('M','M.V1.z','M.V1','item','AND','zone','zone',1.0),
 ('M','M.V1.d','M.V1','item','AND','drive','drive',1.0),
 ('M','M.V2','M.V','container','OR','V2',NULL,1.0), ('M','M.V2.z','M.V2','item','AND','zone','zone',1.0),
 ('M','M.V2.d','M.V2','item','AND','drive','drive',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE m_attrs AS SELECT * FROM (VALUES ('M','M.V1.z','stroke','stroke','length','m',0.04,0.04,NULL,NULL),
 ('M','M.V1.d','power','power','power','W',120.0,120.0,NULL,NULL), ('M','M.V2.z','stroke','stroke','length','m',0.07,0.07,NULL,NULL),
 ('M','M.V2.d','power','power','power','W',50.0,50.0,NULL,NULL)) t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
SELECT coverage, n_gaps FROM tree_coverage('Q', candidates := ['M'], nodes_table := 'q_nodes', attrs_table := 'q_attrs',
  candidate_nodes_table := 'm_nodes', candidate_attrs_table := 'm_attrs', mu := 0.0);
SELECT req_node_id, matched_node_id FROM tree_coverage_trace('Q', candidates := ['M'], nodes_table := 'q_nodes',
  attrs_table := 'q_attrs', candidate_nodes_table := 'm_nodes', candidate_attrs_table := 'm_attrs', mu := 0.0);
```

Actual: coverage `1.0`, `n_gaps 0`; Q.1 matched to `M.V2.z`, Q.2 to `M.V1.d`. V1 fails the
stroke, V2 fails the power; nothing buildable covers Q. Expected: 0.75 (best single variant), or
at least a flag. Suggested fix: an option such as `variants := 'per_group'` that scores each OR
child as a whole and takes the best, and a `variant_path` column in the trace so a mix is
visible. Document the current semantics in guide 04 and API page 13. Workaround in the
example: turn each variant into its own candidate tree (03, step 3), which agrees with the gold
file on all 10 variants; step 4 shows the mixed result.

### R4-U3 — Medium — candidate values in a non-base unit are compared as if they were base units

`value_satisfies` normalises `{'lo': 250, 'unit': 'mm'}` first, but the kernel does not, and
`validate_trees` does not warn (mm is a known unit, just not the base one).

```sql
CREATE TABLE qn AS SELECT * FROM (VALUES ('Q','Q',NULL,'container','AND','spec',NULL,1.0),
  ('Q','Q.1','Q','item','AND','frame','frame',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE qa AS SELECT * FROM (VALUES ('Q','Q.1','len','length','length','m',12.0,12.0,'at_most',NULL))
  t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
CREATE TABLE cn AS SELECT * FROM (VALUES ('C','C',NULL,'container','AND','offer',NULL,1.0),
  ('C','C.1','C','item','AND','frame','frame',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE ca AS SELECT * FROM (VALUES ('C','C.1','len','length','length','mm',10500.0,10500.0,NULL,NULL))
  t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
SELECT req_value, prov_value, value_score FROM tree_coverage_trace('Q', candidates := ['C'], nodes_table := 'qn',
  attrs_table := 'qa', candidate_nodes_table := 'cn', candidate_attrs_table := 'ca', mu := 0.0);
SELECT count(*) FROM validate_trees('cn', 'ca');
```

Actual: `at_most 12 m | 10500 mm | 0.0`, and 0 issues. Expected: 1.0 (10.5 m), or a
`validate_trees` warning. A false gap from a dirty PLM export. Suggested fix: normalise
attribute values with the unit seed when the tables are read (as `value_satisfies` does), or
add a `non_base_unit` warning to `validate_trees`. Workaround: `normalize_quantity` at load (01,
step 6, and the load block of 02 and 03).

### R4-U4 — Medium — `is_gap` in the trace marks every attribute of a gapped requirement

`WHERE is_gap` is the obvious query for a deviation list, but it returns met attributes too.

Same tables as R4-U1, with a second requirement attribute that fails (`('Q','Q.1','len','length','length','m',12.0,12.0,'at_most',NULL)` in `qa`, `('C','C.1','len','length','length','mm',10500.0,10500.0,NULL,NULL)` in `ca`):
the trace gives `power | 87 W | 1.0 | is_gap = true` because `len` failed. In the example,
`WHERE is_gap` for PAST-LEAN-2019 lists `length 10.5 m against max. 12 m` and `cycle_time 1 s
against max. 1,2 s` as gaps next to the real one (output). Expected: a per-attribute flag.
Suggested fix: add `attr_gap` (value_score < 1 or no counterpart) and rename or document
`is_gap` as "the requirement this row belongs to is a gap". Workaround: verdicts from
`value_score` and the counts.

### R4-U5 — Medium — the trace prints values in base units

`req_value` / `prov_value` read `at_least 358.15 K`, `20 Hz` (1 200 cups/min) and
`at_least 9000000 s` (2 500 h). A bid engineer cannot send that to a customer or check it at a
glance. `tree_from_json` drops `source_unit` from the attribute struct, so there is no column to
convert back with.

```sql
SELECT unnest(n.attributes, recursive := true)
FROM (SELECT unnest(tree_from_json('{"tree_id":"T","lang":"de","root":{"node_id":"T","kind":"container",
  "children":[{"node_id":"T.1","kind":"item","class_id":"cip",
  "attributes":[{"attr_id":"t","name":"Temperatur","text":"mind. 85 °C"}]}]}}')) AS n);
```

Actual: `mind. 85 °C | 358.15 | K`; no source unit. Expected: `source_unit` kept in the
attribute struct, and the trace printing both values in the requirement's source unit.
Workaround: re-parse the clause text for `source_unit` and convert with `quantity_units()`
(`clause_check` in 02 and 03); 30 lines of SQL every user would have to write.

### R4-U6 — Low — material numbers are still read as counts

```sql
SELECT parse_quantity('Werkstoff produktberührt 1.4404', 'de');
```

Actual: `lo 1.4404, unit '1', dimension count, bound NULL`. Expected (refuse-to-guess, F2):
all NULL. A decimal count with four decimals is never a count. Suggested fix: treat
`\d\.\d{4}` (DIN material number) as a designation, or refuse a non-integer count after a
word. Workaround: `kind = 'manual'` in the clause sidecar.

### R4-U7 — Low — `1400 /min` silently loses the rate

```sql
SELECT parse_quantity('mindestens 1400 /min', 'de');
```

Actual: `lo 1400, unit '1', dimension count, source_unit NULL`. Expected: 23.33 Hz (as
`mind. 1.400 1/min` gives), or the unknown form. Suggested fix: read a leading `/` after a
number as `1/`.

### R4-U8 — Low — `reviewed` in `tree_coverage` looks wrong when the request is unreviewed

With no `overrides_table`, item C1 has no class and the result shows `reviewed = true` next to
`assignment_used = best_match` (02, step 2, first row). `reviewed` describes the candidate only
(API page 14). Suggested fix: add `query_reviewed`, or say "candidate" in the column name.

### R4-U9 — Low — table function parameters must be literals

`candidates := (SELECT list(tree_id) FROM variant_nodes)` fails with `Table function cannot
contain subqueries`, and a lateral `tree_coverage_trace(r.query_id, ...)` fails with "does not
support lateral join column parameters". Every list of 10 module variants must be pasted.
Suggested fix: a `candidates_table` parameter. Workaround: put only the candidates in the
candidate tables and use `k := n`.

## Verdict

The folder demonstrates the scenario convincingly: one command per script, the numbers come from
a real run and are asserted, and the gold check shows the gap between the kernel with the default
tolerance (32 of 33 deviations) and hard limits (33 of 33). The parser traps that blocked this folder in
the round-4 discussion (F1, F2, F3, F6) are fixed and visible in the audit.

What it also shows is how much SQL a user writes around the kernel to get a signable list: the
strict re-check (R4-U1), per-attribute verdicts (R4-U4), values in the clause's unit (R4-U5),
must/should from a sidecar, and per-variant scoring (R4-U2). That is the `clause_check` table,
about 40 lines that every bid team would copy. A customer would still ask:

1. Can `tree_coverage` give me must-only coverage and strict limits directly, with a priority
   column on the requirement attributes?
2. Can the trace show which variant of a module it used, and refuse to mix variants?
3. Can the deviation list come out of one function call, with clause, page and the values in
   the units of the document?
4. Who maintains `clauses.csv`: does the extraction deliver page and must/should, or does the
   bid engineer type it per tender?
