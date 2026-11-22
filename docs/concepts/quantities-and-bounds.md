# Quantities and Bounds

> How a specification value becomes a number with a unit and a direction, and how an offered value
> is scored against it. For the calls, see [guide 05](../guides/05-quantities-and-specs.md) and
> [api/11](../api/11-quantities.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Numbers Need Rules

A value in a specification is not text. `max. 38 dB(A)` and `höchstens 38 dB(A)` mean the same
limit; `38 dB(A)` and `38 V` share a number and nothing else. So values are never compared by text
or embeddings. Each one is reduced to three parts:

| Part | Example (`mindestens 24 cm`) | Role |
|---|---|---|
| Interval in the base unit | `lo` 0.24, `hi` 0.24 (metres) | what is compared |
| Dimension | `length` | a hard gate: only equal dimensions compare |
| Bound | `at_least` | the direction: which side of the value is good |

## Base Units and Dimensions

Every built-in unit (`quantity_units()`) has a dimension, a factor and an offset to its base unit:
base = value × factor + offset. Centimetres become metres, minutes seconds, °C kelvin. Values in
different units of one dimension then compare directly: `max. 3 min` against `150 s` is
`180 s` against `150 s`.

- A **currency** has no factor: EUR never converts to USD, so two currencies are a gate.
- An **unknown unit** is never guessed. The number stays as written with dimension NULL, and every
  comparison with it is a gate. Your own units extend the seed through `unit_table`.
- A **bare number** is a count (unit `1`). An unknown word followed by another number
  (`3.1 acc. to EN 10204`) is a designation and parses to nothing.

## Bounds

Bound words in English and German give the direction: `at least`, `min.`, `mindestens`, `≥` give
`at_least`; `at most`, `max.`, `höchstens`, `bis zu`, `≤` give `at_most`; `6–18 V`, `zwischen 15
und 25 mm/s` give `within`; `nominal`, `Nennwert` give `equals`. Without a bound word the value is
**unqualified** (bound NULL): `38 dB(A)` alone does not say whether more or less is better.
`default_bound` (or `default_bounds := 'catalogue'` in the kernel) gives it the direction of its
attribute template.

A **nominal with a tolerance** is `equals` over the span the tolerance allows: `220 N ± 2 %` is
215.6–224.4 N, `20 kg ± 1 kg` is 19–21 kg, and `50 % ± 5 %` is 45–55 % (a percent tolerance on a
percent nominal is percentage points). A bound word in front keeps its direction and takes the end
the tolerance allows: `max. 20 kg ± 1 kg` is `at_most` 21 kg.

```sql
SELECT t, q.lo, q.hi, q.unit, q.dimension, q.bound
FROM (VALUES (1, 'max. 38 dB(A)'), (2, 'mindestens 24 cm'), (3, '6–18 V'), (4, '220 N ± 2 %'),
             (5, 'max. 20 kg ± 1 kg'), (6, '37 °C'), (7, '12 furlongs')) v(i, t),
     LATERAL (SELECT parse_quantity(t) AS q) ORDER BY i;
-- => max. 38 dB(A) | 38.0 | 38.0 | dB(A) | sound_level | at_most
-- => mindestens 24 cm | 0.24 | 0.24 | m | length | at_least
-- => 6–18 V | 6.0 | 18.0 | V | voltage | within
-- => 220 N ± 2 % | 215.6 | 224.4 | N | force | equals
-- => max. 20 kg ± 1 kg | 21.0 | 21.0 | kg | mass | at_most
-- => 37 °C | 310.15 | 310.15 | K | temperature | NULL
-- => 12 furlongs | 12.0 | 12.0 | furlongs | NULL | NULL
```

## Scoring per Bound

`value_satisfies` compares an offered interval `[p_lo, p_hi]` with the requirement. An offered
range counts with its favourable end for a limit, and must fit as a whole for a range or a span.

| Bound | Satisfied when | `distance` |
|---|---|---|
| `at_least v` | the offered high end ≥ v | relative miss `(v − p_hi) / \|v\|` |
| `at_most v` | the offered low end ≤ v | relative miss `(p_lo − v) / \|v\|` |
| `within [lo, hi]` | the offered range covers the whole required range (a capability, such as an adjustment travel) | uncovered share |
| `equals v` | the offered value lies within `tol` of v | relative miss |
| `equals [lo, hi]` | the offered value or range lies inside the span, plus `tol` | how far the farther end lies outside, over the nominal |
| point (unqualified) | the offered value is about the same (±1 % linear, ±0.01 decades log) | uncovered share |
| gate | the dimensions differ or are unknown, or the currencies differ | NULL, score 0 |

`within` is a capability, not a band of acceptable values: `22 bis 26 cm` asks for a travel that
spans 22 to 26 cm, and a single 25 cm covers only part of it. A band ("between 4 and 5 kg") is two
limits, `at_least` and `at_most`.

```sql
SELECT r, p, round(v.score, 4) AS score, round(v.distance, 4) AS distance, v.rule
FROM (VALUES (1, 'max. 180 s', '183 s'), (2, 'max. 180 s', '190 s'), (3, 'at least 5 A', '5 V'),
             (4, '22 bis 26 cm', '20 bis 28 cm'), (5, '22 bis 26 cm', '25 cm'),
             (6, '220 N ± 2 %', '223 N'), (7, '220 N ± 2 %', '230 N'), (8, 'max. 43 °C', '45 °C')) v(i, r, p),
     LATERAL (SELECT value_satisfies(parse_quantity(r), parse_quantity(p)) AS v) ORDER BY i;
-- => max. 180 s | 183 s | 1.0 | 0.0167 | at_most
-- => max. 180 s | 190 s | 0.2889 | 0.0556 | at_most
-- => at least 5 A | 5 V | 0.0 | NULL | gate
-- => 22 bis 26 cm | 20 bis 28 cm | 1.0 | 0.0 | within
-- => 22 bis 26 cm | 25 cm | 0.5104 | 0.4896 | within
-- => 220 N ± 2 % | 223 N | 1.0 | 0.0 | equals
-- => 220 N ± 2 % | 230 N | 0.8909 | 0.0255 | equals
-- => max. 43 °C | 45 °C | 0.6 | 2.0 | at_most
```

## Tolerance, Soft Band and Scale

A limit is rarely meant to the last digit, and measured values scatter. Two parameters shape the
score near the limit:

- **`tol`** (default 0.02): a miss up to `tol` still scores 1. 183 s against `max. 180 s` misses by
  1.7 % and passes.
- **`soft_band`** (default 0.05): beyond `tol` the score falls linearly to 0 over this width.
  190 s misses by 5.6 %: (0.0556 − 0.02) / 0.05 of the band is used, so the score is 0.2889. With
  `soft_band` 0 the limit is strict.

On a `±` span, `tol` applies on top of the drawing's tolerance: 230 N misses 224.4 N by 2.5 % of
the nominal and scores 0.89. Pass `tol := 0` and `soft_band := 0` to check the drawing's tolerance
alone.

The **scale** decides what "relative" means. Time, currency, percent and sound level are linear:
2 % is 2 % of the value. Other physical quantities use a log scale, where `tol` is in decades
(0.02 decades ≈ 4.5 %), because they span orders of magnitude and a factor matters more than a
difference:

```sql
SELECT round(value_satisfies(parse_quantity('at least 100 mm'), parse_quantity('96 mm')).score, 4) AS log_scale,
       round(value_satisfies(parse_quantity('at least 100 mm'), parse_quantity('96 mm'), 'linear').score, 4) AS linear_scale;
-- => 1.0 | 0.6
```

**Temperature** is stored in kelvin, where a relative tolerance means nothing (2 % of 310 K is
6 K). It uses an absolute tolerance of 1 K and a soft band of 2.5 K, and its `distance` is in
kelvin: 45 °C against `max. 43 °C` is 2 K over, score 0.6.

## Gates

The gate comes first and is absolute: score 0, rule `gate`, no distance. Volts never satisfy amps,
USD never satisfies EUR, an unknown unit satisfies nothing. A gate is a mismatch, not a weak match:
it says the values are not comparable, so the trace should send you to the data (a unit to add, a
wrong attribute), not to the supplier.

Categories (`IP54`, `PN 16`) have no number and are not scored here: `compat_score` looks them up
in a curated rules table, and equal categories score 1. `IP67` meets `IP65 or better` only through
a rule, never through text similarity.

## Why These Defaults

| Default | Why |
|---|---|
| Dimension gate, no unit guessing | A wrong conversion is worse than a visible gap |
| `tol` 0.02 | Values on spec sheets are rounded; a miss inside the rounding is not a miss |
| `soft_band` 0.05 | A near miss should rank above a far miss; beyond `tol` + `soft_band` (7 % on a linear scale) the score is 0 |
| Log scale for physical quantities | Their values span decades; linear for time, money, percent and sound level, where differences matter |
| Temperature 1 K / 2.5 K absolute | Kelvin ratios are meaningless at room temperature |
| `±` as a span | A drawing tolerance says which offers fit, so the span, not the nominal, is the requirement |
| Unqualified = point | Without a direction, only "about the same" is safe; give a direction with `default_bound` |

The design history is in the [HLD §4.2](../requirements-matching/HLD.md#42-b-quantity-layer-r2);
it records how these rules were decided, not the current contract.
