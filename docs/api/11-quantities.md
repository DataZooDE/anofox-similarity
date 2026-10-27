# Quantities

> Parse, normalise and compare values with units and direction.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Quantities: units, parsing and direction-aware comparison

Numbers need rules, not embeddings: the unit is a hard gate, the value is checked
with a direction, and the distance to compliance is reported.

**`quantity_units()`**: the built-in, read-only units seed. It returns `symbol, alias
VARCHAR[], dimension, factor, offset, base_unit` (base = value × factor + offset). It covers SI
units with prefixes, time (s … year, weeks/Wochen, days/Tage), temperature (K, °C, °F), sound
level (dB(A)), percent/ppm, counts (cycles/Zyklen, pcs), data and data rates, and ISO-4217
currencies. Currencies have `factor` NULL and are never converted.

**`parse_quantity(text [, lang [, unit_table]])`** → `STRUCT(lo, hi, unit, dimension, bound, raw,
source_unit, category)`

| Text | lo, hi (base unit) | bound |
|---|---|---|
| `max. 38 dB(A)` | 38, 38 dB(A) | at_most |
| `mindestens 24 cm` | 0.24, 0.24 m | at_least |
| `6–18 V`, `40 bis 60 W`, `zwischen 15 und 25 mm/s` | range | within |
| `220 N ± 2 %`, `Nennwert 220 N` | 220, 220 N | equals |
| `38 dB(A)` | 38, 38 | NULL (unqualified) |
| `37 °C` | 310.15 K | NULL |
| `65.000 Zyklen` with lang `'de'` | 65000 (count) | NULL |
| `IP54 or better` | NULL; `category` = IP54 | at_least |
| `12 furlongs` | 12; `dimension` NULL, `unit` = furlongs | NULL |

- `lang` is `'en'`, `'de'` or a list; default `['en', 'de']`. It only decides ambiguous
  separators (`2.500` is 2500 in German and 2.5 in English); bound words of both languages are
  always recognised. A comma or dot with one or two digits after it is always a decimal
  separator.
- A bare number is a count (`unit` '1'). An unknown unit is never guessed: the number stays as
  written and `dimension` is NULL, so every comparison with it is a gate.
- A category is a single upper-case code with digits and no number in front (`IP54`, `F1`,
  `M8`); `1 F` is one farad.
- `unit_table` is a LIST of `STRUCT(symbol, alias, dimension, factor, offset)`, e.g.
  `(SELECT list(u) FROM my_units u)`. It extends the seed, and a symbol that is already in the
  seed is overridden. The seed itself can be passed back in. Pass `lang` before `unit_table`
  (on 1.5.6 scalar arguments bind by position).

**`normalize_quantity(value, unit [, unit_table])`** → `STRUCT(value, unit, dimension)`: converts
to the base unit; an unknown unit returns the input unchanged with `dimension` NULL.

**`value_satisfies(requirement, provided [, scale [, tol, soft_band [, default_bound]]])`** →
`STRUCT(score, distance, rule)`

Both arguments are structs with `lo`, `hi` and `dimension` (and optionally `unit`); the
requirement also has `bound`. `parse_quantity` output plugs straight in. A value in a
non-base unit of its dimension (`{'lo': 250, 'unit': 'mm', 'dimension': 'length'}`) is
normalised first.

| Rule | Satisfied when | `distance` |
|---|---|---|
| `at_least v` | the provided high end `p_hi ≥ v` | `(v − p_hi)/\|v\|` |
| `at_most v` | the provided low end `p_lo ≤ v` | `(p_lo − v)/\|v\|` |
| `within [lo, hi]` | the provided range covers the required range (log: share of the decades) | uncovered share |
| `equals v` | `\|v − p\| ≤ tol` relative, on every scale; `p` is the farther end of a provided range | `\|v − p\|/\|v\|` |
| `point` (no bound) | the provided value lies within `[v·10⁻⁰·⁰¹, v·10⁰·⁰¹]` (log) or ±1 % (linear) | uncovered share |
| `gate` | different or unknown dimensions, or different currencies; score 0 | NULL |
| `missing` | no provided value; score 0 | NULL |

- `score` is 1 within `tol` (default 0.02: decades on a log scale, i.e. 4.5 % relative;
  relative on a linear scale) and falls linearly to 0 across `soft_band` (default 0.05).
  With `soft_band` 0 the limit is strict.
- `scale` defaults per dimension: linear for time, currency, percent and sound level, log
  otherwise; a non-positive value falls back to linear.
- A limit of 0 uses an absolute distance and a strict score.
- **Temperature** (stored in K) uses an absolute tolerance of 1 K and a soft band of 2.5 K;
  its `distance` is in K. `tol` and `soft_band` do not apply.
- `default_bound` gives an unqualified requirement a direction, typically the bound of its
  catalogue attribute template. A stated bound always wins.
- Categorical values (`category` set) are rejected; use `compat_score`.
- `distance` is relative for the limit rules, a share for within and point, and kelvin for
  temperature. Write "misses by x %" only for the limit rules.

**`compat_score(dimension, required, provided [, rules_table := 'compatibility_rules'])`** →
DOUBLE: looks up a curated rules table `(dimension, required, provided, score)`. The table must
exist (it may be empty). Without a rule the result is 1 if the categories are equal, else 0, so
`at_least IP65` is not met by `IP67` unless a rule says so; a NULL category gives NULL. Text
similarity is never used.

```sql
-- Which seat-track applications meet a German spec in cm and dB(A)?
SELECT a.node_id
FROM (SELECT attr, parse_quantity(value_text, 'de') AS q FROM requirement) r
JOIN module_attributes a ON a.attr_id = r.attr,
LATERAL (SELECT value_satisfies(r.q, {'lo': a.lo, 'hi': a.hi, 'dimension': a.dimension}) AS v)
GROUP BY a.node_id HAVING min(v.score) = 1;

-- Unqualified values take the direction of their catalogue template
SELECT value_satisfies(parse_quantity('38 dB(A)'), {'lo': 35, 'hi': 35, 'dimension': 'sound_level'},
                       NULL, 0.02, 0.05, 'at_most');               -- score 1
```

**Performance:** about 0.3 s per 10⁵ values for `parse_quantity` and 0.4 s per 10⁵ pairs for
`value_satisfies`.

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `quantity_units`

*Table function*

The built-in, read-only units seed of the quantity layer: symbol, alias (list), dimension, factor, offset (base = value * factor + offset) and base_unit. Currencies have factor NULL and are never converted. Copy it into a table to extend it and pass (SELECT list(u) FROM my_units u) as unit_table.

```sql
quantity_units()
```

**Examples**

```sql
SELECT * FROM quantity_units() WHERE dimension = 'length';
CREATE TABLE my_units AS SELECT symbol, alias, dimension, factor, "offset" FROM quantity_units();
```

### `parse_quantity`

*Scalar function*

Parses a quantity written in English or German ('max. 38 dB(A)', 'mindestens 24 cm', '6–18 V', 'zwischen 15 und 25 mm/s', '220 N ± 2 %', 'IP54 or better') into STRUCT(lo, hi, unit, dimension, bound, raw, source_unit, category). lo/hi are in the base unit of the dimension (unit); bound is at_least, at_most, within, equals or NULL when the text states none. lang ('en', 'de' or a list, first entry decides ambiguous separators such as '2.500') defaults to ['en', 'de']. unit_table is a LIST of STRUCT(symbol, alias, dimension, factor, offset) that extends or overrides the seed (see quantity_units()). A bare number is a count; an unknown unit keeps the number and has dimension NULL. Pass lang before unit_table (DuckDB 1.5.6 binds by position).

```sql
parse_quantity(text VARCHAR) → STRUCT(lo DOUBLE, hi DOUBLE, unit VARCHAR, dimension VARCHAR, bound VARCHAR, raw VARCHAR, source_unit VARCHAR, category VARCHAR)
parse_quantity(text VARCHAR, lang VARCHAR) → STRUCT(lo DOUBLE, hi DOUBLE, unit VARCHAR, dimension VARCHAR, bound VARCHAR, raw VARCHAR, source_unit VARCHAR, category VARCHAR)
parse_quantity(text VARCHAR, lang VARCHAR[]) → STRUCT(lo DOUBLE, hi DOUBLE, unit VARCHAR, dimension VARCHAR, bound VARCHAR, raw VARCHAR, source_unit VARCHAR, category VARCHAR)
parse_quantity(text VARCHAR, lang VARCHAR, unit_table ANY) → STRUCT(lo DOUBLE, hi DOUBLE, unit VARCHAR, dimension VARCHAR, bound VARCHAR, raw VARCHAR, source_unit VARCHAR, category VARCHAR)
parse_quantity(text VARCHAR, lang VARCHAR[], unit_table ANY) → STRUCT(lo DOUBLE, hi DOUBLE, unit VARCHAR, dimension VARCHAR, bound VARCHAR, raw VARCHAR, source_unit VARCHAR, category VARCHAR)
```

**Examples**

```sql
SELECT parse_quantity('max. 38 dB(A)');
SELECT parse_quantity('65.000 Zyklen', 'de');
SELECT parse_quantity('2.500 mA', ['de', 'en']);
SELECT parse_quantity('3 Stk.', 'de', (SELECT list(u) FROM my_units u));
SELECT parse_quantity('3 Stk.', ['de'], (SELECT list(u) FROM my_units u));
```

### `normalize_quantity`

*Scalar function*

Converts a value in a unit to the base unit of its dimension: STRUCT(value, unit, dimension). An unknown unit is never guessed: value and unit come back unchanged with dimension NULL. Currencies keep their value (same-currency comparison only). unit_table extends the seed as in parse_quantity.

```sql
normalize_quantity(value DOUBLE, unit VARCHAR) → STRUCT("value" DOUBLE, unit VARCHAR, dimension VARCHAR)
normalize_quantity(value DOUBLE, unit VARCHAR, unit_table ANY) → STRUCT("value" DOUBLE, unit VARCHAR, dimension VARCHAR)
```

**Examples**

```sql
SELECT normalize_quantity(2500, 'mA'); -- 2.5 A
SELECT normalize_quantity(3, 'lanes', (SELECT list(u) FROM my_units u));
```

### `value_satisfies`

*Scalar function*

Direction-aware comparison of a requirement with a provided value (both STRUCTs with lo, hi, dimension; the requirement also bound, either may carry unit). Returns STRUCT(score, distance, rule). at_least uses the high end of a provided range, at_most the low end, within the share of the required range covered, equals the relative difference; an unqualified value is a point widened by ±0.01 decades unless default_bound gives it a direction. score is 1 within tol (decades on log scale, relative on linear scale), falling to 0 across soft_band; distance is relative (absolute in K for temperature, which uses a 1 K tolerance and 2.5 K band). Different dimensions, or different currencies, give rule 'gate' and score 0. scale defaults per dimension (linear for time, currency, percent, sound level). Pass the options in order: scale, tol, soft_band, default_bound.

```sql
value_satisfies(requirement ANY, provided ANY) → STRUCT(score DOUBLE, distance DOUBLE, "rule" VARCHAR)
value_satisfies(requirement ANY, provided ANY, scale VARCHAR) → STRUCT(score DOUBLE, distance DOUBLE, "rule" VARCHAR)
value_satisfies(requirement ANY, provided ANY, scale VARCHAR, tol DOUBLE, soft_band DOUBLE) → STRUCT(score DOUBLE, distance DOUBLE, "rule" VARCHAR)
value_satisfies(requirement ANY, provided ANY, scale VARCHAR, tol DOUBLE, soft_band DOUBLE, default_bound VARCHAR) → STRUCT(score DOUBLE, distance DOUBLE, "rule" VARCHAR)
```

**Examples**

```sql
SELECT value_satisfies(parse_quantity('max. 38 dB(A)'), parse_quantity('35 dB(A)'));
SELECT value_satisfies(parse_quantity('at least 24 cm'), parse_quantity('0.235 m'), 'linear');
SELECT value_satisfies(parse_quantity('at least 24 cm'), parse_quantity('23 cm'), NULL, 0.05, 0.1);
SELECT value_satisfies(parse_quantity('24 cm'), parse_quantity('26 cm'), NULL, 0.02, 0.05, 'at_least');
```

### `compat_score`

*Scalar macro*

Compatibility of a provided category with a required one for an ordinal or categorical dimension (protection class, certification level, qualifier), looked up in a curated rules table (dimension, required, provided, score). Without a rule: 1 if equal, else 0. Text similarity is never used.

```sql
compat_score(dimension, required, provided, rules_table)
```

**Examples**

```sql
SELECT compat_score('protection_class', 'IP54', 'IP65');
SELECT compat_score('cert_level', 'silver', 'gold', rules_table := 'my_rules');
```

<!-- generated:functions:end -->
