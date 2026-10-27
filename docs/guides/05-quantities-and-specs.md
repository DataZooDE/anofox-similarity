# Quantities and Specifications

> Parse limits like "max. 38 dB(A)" or "mindestens 24 cm" and check offered values against them.

**Use this guide to:**
- Parse English and German quantity texts with `parse_quantity`
- Check a provided value against a requirement with `value_satisfies`
- Understand bounds, tolerance and the soft band
- Compare ordinal classes such as IP54 with `compat_score`
- Give unqualified values a direction with `default_bound` and `default_bounds := 'catalogue'`

Numbers need rules, not text similarity. The unit is a hard gate, the bound gives the direction,
and the result says how far a value misses.

## Parse a Quantity

`parse_quantity(text [, lang [, unit_table]])` returns a struct with `lo`, `hi` (in the base unit),
`unit`, `dimension`, `bound`, `category` and `source_unit`.

```sql
SELECT t AS text, q.lo, q.hi, q.unit, q.dimension, q.bound, q.category
FROM (VALUES (1, 'max. 38 dB(A)'), (2, 'mindestens 24 cm'), (3, 'zwischen 15 und 25 mm/s'),
             (4, '220 N ± 2 %'), (5, '37 °C'), (6, 'höchstens 7,5 A'), (7, 'IP 54 oder besser'),
             (8, '5 KG'), (9, '12 furlongs')) v(i, t),
     LATERAL (SELECT parse_quantity(t, 'de') AS q)
ORDER BY i;
```

| text                    | lo     | hi     | unit     | dimension   | bound    | category |
|-------------------------|-------:|-------:|----------|-------------|----------|----------|
| max. 38 dB(A)           | 38.0   | 38.0   | dB(A)    | sound_level | at_most  | NULL     |
| mindestens 24 cm        | 0.24   | 0.24   | m        | length      | at_least | NULL     |
| zwischen 15 und 25 mm/s | 0.015  | 0.025  | m/s      | speed       | within   | NULL     |
| 220 N ± 2 %             | 220.0  | 220.0  | N        | force       | equals   | NULL     |
| 37 °C                   | 310.15 | 310.15 | K        | temperature | NULL     | NULL     |
| höchstens 7,5 A         | 7.5    | 7.5    | A        | current     | at_most  | NULL     |
| IP 54 oder besser       | NULL   | NULL   | NULL     | NULL        | at_least | IP54     |
| 5 KG                    | 5.0    | 5.0    | kg       | mass        | NULL     | NULL     |
| 12 furlongs             | 12.0   | 12.0   | furlongs | NULL        | NULL     | NULL     |

Rules to know:

- Bound words of both languages are always recognised: `max.`, `höchstens`, `at least`, `mindestens`,
  `zwischen ... und`, `bis`, ranges with `–`.
- A value without a bound word has `bound` NULL (unqualified).
- An unknown unit is never guessed. The number stays, `dimension` is NULL, and every comparison with
  it is a gate.
- A class code such as `IP54` goes to `category`.
- `lang` only decides ambiguous separators. A comma or dot with one or two digits after it is
  always a decimal separator. With three digits it depends on the language:

```sql
SELECT parse_quantity('2.500 Zyklen', 'de').lo AS de,
       parse_quantity('2.500 Zyklen', 'en').lo AS en,
       parse_quantity('2.500 Zyklen').lo       AS default_lang;
```

| de     | en  | default_lang |
|-------:|----:|-------------:|
| 2500.0 | 2.5 | 2.5          |

The default is `['en', 'de']`, so English wins. Pass the document's language. The benchmark keeps
it in `requests.lang`.

`quantity_units()` lists the built-in units (SI with prefixes, time, temperature, dB(A), percent,
counts, currencies). Pass your own units as a list of structs in `unit_table`.

## Check a Value

`value_satisfies(requirement, provided [, scale [, tol, soft_band [, default_bound]]])` returns
`score`, `distance` and `rule`. Both inputs are structs with `lo`, `hi` and `dimension`.
`parse_quantity` output plugs straight in.

```sql
SELECT module, round(v.score, 4) AS score, round(v.distance, 4) AS distance, v.rule
FROM (VALUES ('H-150', 150.0), ('H-183', 183.0), ('H-190', 190.0), ('H-240', 240.0)) t(module, seconds),
     LATERAL (SELECT value_satisfies(parse_quantity('max. 180 s'),
                                     {'lo': seconds, 'hi': seconds, 'dimension': 'time'}) AS v)
ORDER BY module;
```

| module | score  | distance | rule    |
|--------|-------:|---------:|---------|
| H-150  | 1.0    | 0.0      | at_most |
| H-183  | 1.0    | 0.0167   | at_most |
| H-190  | 0.2889 | 0.0556   | at_most |
| H-240  | 0.0    | 0.3333   | at_most |

- `distance` is relative for the limit rules: H-190 misses by 5.6 %.
- `score` is 1 within the tolerance `tol` (default 0.02) and falls linearly to 0 across the soft band
  (default 0.05). H-183 is 1.7 % over the limit and still scores 1.
- Time, currency, percent and sound level use a linear scale. Other dimensions use a log scale,
  where the default tolerance is about 4.5 %.

### The Rules

| Bound | Satisfied when |
|---|---|
| `at_least v` | the provided high end ≥ v |
| `at_most v` | the provided low end ≤ v |
| `within [lo, hi]` | the provided range covers the required range |
| `equals v` | within 2 % of v |
| none (point) | the provided value is about the same (±1 % linear) |
| `gate` | different or unknown dimensions, or different currencies: score 0 |

Units never mix. Volts never satisfy amps, and USD never satisfies EUR:

```sql
SELECT offer, v.score, v.rule
FROM (VALUES ('O1', '5 V'), ('O2', '10 A'), ('O3', '4 A'), ('O4', '5 furlongs')) t(offer, rating),
     LATERAL (SELECT value_satisfies(parse_quantity('at least 5 A'), parse_quantity(rating)) AS v)
ORDER BY offer;
```

| offer | score | rule     |
|-------|------:|----------|
| O1    | 0.0   | gate     |
| O2    | 1.0   | at_least |
| O3    | 0.0   | at_least |
| O4    | 0.0   | gate     |

### Tolerance and Soft Band

Pass `tol` and `soft_band` by position after `scale`:

```sql
SELECT round(value_satisfies(parse_quantity('max. 180 s'), parse_quantity('190 s')).score, 4)    AS default_band,
       value_satisfies(parse_quantity('max. 180 s'), parse_quantity('190 s'), NULL, 0.02, 0.0).score  AS strict,
       value_satisfies(parse_quantity('max. 180 s'), parse_quantity('190 s'), NULL, 0.06, 0.05).score AS loose;
```

| default_band | strict | loose |
|-------------:|-------:|------:|
| 0.2889       | 0.0    | 1.0   |

Temperatures are an exception: the tolerance is 1 K and the band 2.5 K, absolute. `distance` is
in kelvin. `max. 43 °C` against 45 °C scores 0.6 with distance 2.0.

## Ordinal Classes

Classes such as `IP54` or `F1` are compared with a curated rules table, never by text.
`compat_score(dimension, required, provided [, rules_table])` reads `compatibility_rules` by default.

```sql
CREATE TABLE compatibility_rules AS
SELECT * FROM read_csv_auto('test/data/coverage/compatibility_rules.csv.gz');

SELECT provided, compat_score('protection_class', parse_quantity('IP54 or better').category, provided) AS score
FROM (VALUES (1, 'IP67'), (2, 'IP54'), (3, 'IP40')) t(i, provided)
ORDER BY i;
```

| provided | score |
|----------|------:|
| IP67     | 1.0   |
| IP54     | 1.0   |
| IP40     | 0.5   |

Without a rule, equal classes score 1 and different ones 0. `value_satisfies` rejects categorical
values with an error that points to `compat_score`.

## Unqualified Values

About a third of real requirements give a bare value: "noise 38 dB(A)". As a point, 35 dB(A) does
not match it. In a seat-track spec, noise is a maximum. `default_bound` gives the direction:

```sql
SELECT value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)')).score AS as_point,
       value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)'),
                       NULL, 0.02, 0.05, 'at_most').score AS with_bound;
```

| as_point | with_bound |
|---------:|-----------:|
| 0.0      | 1.0        |

A stated bound always wins over `default_bound`.

In `tree_coverage`, `default_bounds := 'catalogue'` does this per attribute. It looks up the
template with the same class and dimension in `templates_table` (default `catalogue_attributes`):

```sql
CREATE TABLE catalogue_attributes AS
SELECT * FROM read_csv_auto('test/data/coverage/catalogue_attributes.csv.gz');
CREATE TABLE tree_nodes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_nodes.csv');
CREATE TABLE tree_attributes AS SELECT * FROM read_csv_auto('test/data/coverage/mini/tree_attributes.csv');

CREATE TABLE spec_nodes AS SELECT * FROM (VALUES
  ('T1', 'T1',   NULL, 'container', 'AND', 'Track spec', NULL,         1.0),
  ('T1', 'T1.1', 'T1', 'item',      'AND', 'seat track', 'seat_track', 1.0))
  t(tree_id, node_id, parent_id, node_kind, edge_type, label, class_id, count);
CREATE TABLE spec_attrs AS
SELECT 'T1' AS tree_id, 'T1.1' AS node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (VALUES ('a1', 'Verstellweg', '24 cm'), ('a2', 'Betriebsgeräusch', '38 dB(A)')) v(attr_id, name, txt),
     LATERAL (SELECT parse_quantity(txt, 'de') AS q);

SELECT candidate_id, round(coverage, 3) AS coverage, n_gaps
FROM tree_coverage('T1', candidates := ['S1'], nodes_table := 'spec_nodes', attrs_table := 'spec_attrs',
                   candidate_nodes_table := 'tree_nodes', candidate_attrs_table := 'tree_attributes',
                   mu := 0.0, default_bounds := 'catalogue');
```

| candidate_id | coverage | n_gaps |
|--------------|---------:|-------:|
| S1           | 1.0      | 0      |

Without `default_bounds`, both values are points and S1 scores 0.307. The catalogue says travel
is `at_least` and noise is `at_most`, so S1 (0.25 m, 36 dB(A)) meets both. On the benchmark this
option lowers the mean regret from 0.024 to 0.009.

## Common Patterns

**Parse a whole archive.** Join the language per document:

```sql
CREATE TABLE requests AS SELECT * FROM read_csv_auto('test/data/coverage/requests.csv.gz');
CREATE TABLE req_attrs AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv_auto('test/data/coverage/request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);
SELECT count(*) AS n, count(*) FILTER (WHERE dimension IS NULL AND category IS NULL) AS unparsed FROM req_attrs;
```

| n    | unparsed |
|-----:|---------:|
| 2736 | 0        |

## Pitfalls

| Pitfall | Solution |
|---|---|
| `2.500` read as 2.5 | Pass the document's `lang`. |
| Named arguments in the wrong order | On DuckDB 1.5.6 scalars bind by position: `(requirement, provided, scale, tol, soft_band, default_bound)`. |
| A bare value never matches | Use `default_bound` or `default_bounds := 'catalogue'`. |
| `IP65` does not satisfy `at_least IP54` | Add a row to `compatibility_rules`. |
| Unknown unit gives score 0 | Add the unit through `unit_table`. |

## Next Steps

- [Coverage matching](04-coverage-matching.md): use parsed values in property trees
- API: [Quantities](../api/11-quantities.md)
- [Examples](../../examples/quantities)
