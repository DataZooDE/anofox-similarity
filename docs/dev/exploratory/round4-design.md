# Round 4: category manager, second source after a foundry exit

**Goal.** Build `examples/second_source/` as a buyer would: 40 single-sourced cast parts, 14
other foundries' capability sheets, German and English text, a gold file. The task is to rank
approved foundries per part and list the parts that need a new supplier, using the coverage
kernel and its trace on non-BOM data, with a rules table for PN and EN 10204 ladders.

**Setup.** DuckDB 1.5.6 build shell (`build/release/duckdb`, current code with the UR-3 parser
fixes), run from the repository root. Data generator:
`scripts/src/scripts/examples/second_source.py`. Every number below was printed by that shell.

## What worked

- **The kernel agrees with the gold file on 479 of 480 part × foundry pairs** (full cover or
  not), with no tuning. The one disagreement is the planted 2 % near miss (F1 below).
- **UR-3 parser fixes hold on procurement text.** `EN-GJS-400-15`, `GJS-400 … GJS-700`,
  `EN 10204 3.1` and `GGG-50` all come back empty (refuse to guess). `PN 16` and `PN16` are both
  the category `PN16`. `± 0,05 mm` is `at_most` on the half-width. `max. 1.800 kg`, `bis 1,8 t`,
  `ca. 400 kg` and `+/- 0.05 mm` parse correctly in their row's language.
- **H3 holds:** category values without a dimension still find their rule in the kernel.
- **One call for all 40 parts:** `tree_coverage(list, candidates := …)` ranks 480 pairs, and the
  trace for all of them takes under a second.
- **The trace is enough for a strict re-check.** `distance` is reported even when `value_score`
  is 1, so `is_gap OR distance > 0` turns the kernel into a hard-limit check without
  re-joining the attribute tables.

## Findings

### F1 [high] `tree_coverage` has no tolerance setting: a 38 kg part is "fully covered" by a foundry that pours 37 kg

The kernel builds `ValueOptions vo;` with the defaults (`coverage_kernel.cpp`, `MatchAttr`). That
means `tol` 0.02 on a log scale for mass (about 4.7 % relative) plus a soft band. Neither
`tree_coverage` nor `tree_coverage_trace` exposes these, and `value_satisfies` does. For
sourcing, MRO and tenders, a weight, envelope or pressure limit is hard.

```sql
CREATE TABLE n AS SELECT * FROM (VALUES
 ('Q','Q',NULL,'container','AND','part',NULL,1.0), ('Q','Q.w','Q','item','AND','w','piece_weight',1.0),
 ('S','S',NULL,'container','AND','foundry',NULL,1.0), ('S','S.w','S','item','AND','w','piece_weight',1.0))
 t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE a AS SELECT * FROM (VALUES
 ('Q','Q.w','w','weight','mass','kg',38.0,38.0,'at_least',NULL),
 ('S','S.w','w','weight','mass','kg',37.0,37.0,NULL,NULL))
 t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
SELECT coverage, n_gaps FROM tree_coverage('Q', candidates := ['S'], nodes_table := 'n', attrs_table := 'a');
-- actual: 1.0 | 0          expected with a hard limit: 0.0 | 1
SELECT value_score, round(distance, 4), is_gap
FROM tree_coverage_trace('Q', candidates := ['S'], nodes_table := 'n', attrs_table := 'a');
-- actual: 1.0 | 0.0263 | false
```

In the example this produces one false second source (P-1010 / Fonderia Veneta) and hides one
part that needs a new supplier. A related effect: P-1039 scores 0.936 against a foundry whose
flask is 600 mm for a 650 mm part, because 8 % over falls in the soft band. The value score is
not 0, so `coverage >= 0.9` accepts it.

**Suggested fix:** add `tol` and `soft_band` named parameters to `tree_coverage` and
`tree_coverage_trace`, passed into `ValueOptions`, or a single `strict := true`. Mention in
api/13 that `distance > 0` with `value_score = 1` means "inside the tolerance, outside the
limit". Workaround used: `is_gap OR distance > 0` over the trace.

### F2 [medium] A stated bound always wins, but drawing wording describes the part, not the capability

`max. Stückgewicht 38 kg` parses as `at_most 38 kg`. For sourcing the requirement is "the
foundry pours **at least** 38 kg". Neither `default_bound` nor `default_bounds := 'catalogue'`
can override a stated bound:

```sql
SELECT value_satisfies(parse_quantity('max. Stückgewicht 38 kg'), parse_quantity('37 kg'),
                       NULL, 0.02, 0.05, 'at_least');
-- actual: {score 1.0, rule at_most}: a 37 kg foundry "meets" a 38 kg part
```

33 of the 42 weight lines in the example parse as `at_most`. A user who passes the parsed struct
straight into the kernel gets every weight check backwards, with no warning. Templates do not
help, because they only fill an empty bound.

**Suggested fix:** a template option that overrides the stated bound (for example
`default_bounds := 'catalogue_override'`, or a `bound_policy` column on the template table), and
a sentence in guide 05: "the bound word describes the document's subject. When the subject
differs (part vs. supplier capability), set the bound from the class." Workaround used: the
loader writes `bound` from `capability_classes` and keeps the parsed bound only for the audit.

### F3 [medium] Certificate levels are still read as numbers

The UR-3 refuse-to-guess rule covers a number *after* `EN`/`DIN`/`ISO` and numbers joined by
`-`/`x`. It does not cover a number before the standard, or after an ordinary word:

```sql
SELECT t, parse_quantity(t, 'de') FROM (VALUES ('Werkszeugnis 2.2'), ('Abnahmeprüfzeugnis 3.1 nach EN 10204'),
                                               ('3.1 acc. to EN 10204')) v(t);
-- actual:   2.2 count (unit '1')     | 3.1, unit 'nach', no dimension | 3.1, unit 'acc', no dimension
-- expected: empty (unknown) for all three
SELECT value_satisfies(parse_quantity('Werkszeugnis 3.1', 'de'), parse_quantity('Werkszeugnis 2.2', 'de'),
                       NULL, 0.02, 0.05, 'at_least');
-- actual: a numeric at_least check, 2.2 vs 3.1 (it fails here only by coincidence of the numbering)
```

`Werkszeugnis 2.2` gets dimension `count` and therefore takes part in numeric comparisons. The other two
are safe (gates) but print a misleading unit. In the example, 21 of 33 certificate lines would
be read as numbers.

**Suggested fix:** treat `d.d` followed or preceded by `EN 10204` / `nach EN` / `acc. to EN` in
the same text as unknown, and do not take a word that is not a unit (`nach`, `acc`) as the unit.
A number with no recognised unit followed by more words should give the unknown form, not a
unit.

### F4 [medium] `validate_trees` does not warn about a category without a dimension

The round-4 action item asked for this warning. It is not there:

```sql
CREATE TABLE n2 AS SELECT * FROM (VALUES ('Q','Q',NULL,'container','AND','part',NULL,1.0),
  ('Q','Q.c','Q','item','AND','c','cert',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE a3 AS SELECT * FROM (VALUES ('Q','Q.c','c','grade',NULL,NULL,NULL::DOUBLE,NULL::DOUBLE,NULL,'GJS-400'))
  t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
SELECT count(*) FROM validate_trees('n2', 'a3');
-- actual: 0     expected: 1 warning (category_without_dimension)
```

H3 made the kernel tolerant of it, so the effect is smaller than in round 4's F4. A category with
no dimension is still compared with every category attribute of the matched item. The rule
falls back to "any dimension that agrees", which breaks as soon as two ladders share a code (for
example `A`/`B` classes). The example plants it (`material_grade`, 128 values) and catches it
with its own SQL gate.

**Suggested fix:** a `category_without_dimension` warning in `validate_trees` next to
`unknown_unit`.

### F5 [low] The trace shows the first provided category on a tie, not the closest one

A foundry that issues 2.2 and 3.1 certificates, against a 3.2 requirement: both score 0 and the
trace reports `EN10204-2.2`. A buyer reads this as "they only do 2.2".

```sql
CREATE OR REPLACE TABLE n2 AS SELECT * FROM (VALUES ('Q','Q',NULL,'container','AND','part',NULL,1.0),
  ('Q','Q.c','Q','item','AND','c','cert',1.0), ('S','S',NULL,'container','AND','foundry',NULL,1.0),
  ('S','S.c','S','item','AND','c','cert',1.0)) t(tree_id,node_id,parent_id,node_kind,edge_type,label,class_id,count);
CREATE TABLE a2 AS SELECT * FROM (VALUES
  ('Q','Q.c','c','cert','cert_level',NULL,NULL::DOUBLE,NULL::DOUBLE,NULL,'EN10204-3.2'),
  ('S','S.c','c1','cert','cert_level',NULL,NULL,NULL,NULL,'EN10204-2.2'),
  ('S','S.c','c2','cert','cert_level',NULL,NULL,NULL,NULL,'EN10204-3.1'))
  t(tree_id,node_id,attr_id,name,dimension,unit,lo,hi,bound,category);
SELECT req_value, prov_value FROM tree_coverage_trace('Q', candidates := ['S'], nodes_table := 'n2', attrs_table := 'a2');
-- actual: point EN10204-3.2 | EN10204-2.2     expected: EN10204-3.1 (or all provided values)
```

**Suggested fix:** on equal scores, prefer the provided value with the smallest `distance`. For
categories, prefer the highest rung that has any rule with the required value. Alternatively,
list all provided values in `prov_value` when every one of them fails.

### F6 [low] Numeric attributes on one item compete across the whole dimension

`MatchAttr` takes the best provided attribute **of the same dimension**. With `mu = 0` the names
do not matter at all. With the default `mu = 0.2` they only discount by 20 %. On one item,
a foundry's `Ra 3.2 µm` (length) can "meet" a part's `± 0.05 mm` tolerance (length, at_most).
This is documented as "names only discount", but nothing warns a first-time user who models one
part as one item with all its attributes. That is the natural layout of a characteristic
table.

**Suggested fix:** in guide 04/05, a "one item per class when several attributes share a
dimension" recipe, which this example uses. Optionally, a `name_gate` threshold below which an
attribute pair is not compared.

### F7 [low] Usability of the parameter set

- `tree_coverage` and `tree_coverage_trace` need the same 7 named parameters, and a table macro
  cannot forward its arguments to them. A macro parameter fails with `Failed to cast value:
  Type VARCHAR with value '0_macro_parametersrank_parts.cands' can't be cast to VARCHAR[]`.
  That is DuckDB macro binding, but the result is copy-pasted parameter blocks. A macro without
  parameters that reads `getvariable(...)` works.
- The trace prints base units in scientific notation (`at_most 3.2e-06 m` for Ra 3.2 µm). A
  `source_unit`-aware display (`3.2 µm`) would make the gap list readable for a buyer.
- `parse_quantity('max. 420 x 310 x 180 mm')` returns 420 with unit `x`. That is safe (no
  dimension), but a box is a common value on drawings and capability sheets. The unknown form
  would be clearer.

## Verdict

The folder demonstrates the scenario convincingly. A real category manager can run the two
scripts, see why each part is or is not covered, and act on the RFQ list and the
new-supplier list. Recall@3 is 1.0 and precision 1.0 after the strict re-check, which is a
check the kernel does not offer itself. The extension carries the comparison logic: direction,
units, ladders, best-of-many, gaps and their distances. The data design carries the rest:
classes, normalisers, one item per class. That split holds up, but two of the extension's
defaults (F1 tolerance, F2 stated bound) would produce wrong RFQ lists for a user who does not
read the trace.

What a customer would still ask:

1. "Can I set hard limits per class?" (F1). Today it is only possible through the
   `distance > 0` workaround.
2. "Can you rank the full covers by price, distance and free capacity?" That is plain SQL after
   `rfq_list`, but the example does not show it.
3. "GJS-500 is fine where GJS-400 is drawn, if engineering agrees. Where do I record that?" In
   `compatibility_rules`, per approved pair. There is no range-of-ordinals feature, and grades are
   nominal on purpose.
4. "Our drawings are PDFs." Extraction into `part_requirements` is out of scope (BRD), as in
   the other scenario folders.
5. "Which foundry gets too many RFQs?" A capacity view over `rfq_list` grouped by foundry is
   the next step a buyer would want.
