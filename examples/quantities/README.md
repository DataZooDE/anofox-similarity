# Quantities Examples

> **Numbers need rules, not embeddings.**

Runnable SQL examples for reading specification values from text and checking them with a
direction: "max. 38 dB(A)" is met by 35 dB(A), "at least 5 A" is never met by volts.

## Functions

| Function | Description |
|----------|-------------|
| `quantity_units()` | The built-in units seed: symbol, aliases, dimension, factor, offset, base unit |
| `parse_quantity(text [, lang [, unit_table]])` | Text → `STRUCT(lo, hi, unit, dimension, bound, raw, source_unit, category)` in base units |
| `normalize_quantity(value, unit [, unit_table])` | One value and unit → base unit |
| `value_satisfies(requirement, provided [, scale [, tol, soft_band [, default_bound]]])` | `STRUCT(score, distance, rule)` |
| `compat_score(dimension, required, provided [, rules_table])` | Score of an ordinal class (IP54, F1) from a curated rules table |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`parse_and_normalize.sql`](parse_and_normalize.sql) | Units seed, English and German texts, decimal separators, a whole archive | Inline + `test/data/coverage/request_attributes.csv.gz` |
| [`value_satisfies.sql`](value_satisfies.sql) | Limits, unit gates, ranges, temperatures, prices, template bounds, ordinal classes | Inline + `test/data/coverage/` (templates, rules) |

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/quantities/parse_and_normalize.sql
./build/release/duckdb < examples/quantities/value_satisfies.sql
```

---

## Usage

```sql
SELECT parse_quantity('mindestens 24 cm', 'de');            -- lo 0.24, unit m, bound at_least
SELECT value_satisfies(parse_quantity('max. 38 dB(A)'),
                       parse_quantity('41 dB(A)'));         -- score 0, distance 0.079, at_most
SELECT compat_score('protection_class', 'IP54', 'IP65');    -- 1.0 (needs compatibility_rules)
```

## Rules

| Rule | Satisfied when | `distance` |
|------|----------------|------------|
| `at_least v` | provided high end ≥ v | (v − p_hi) / \|v\| |
| `at_most v` | provided low end ≤ v | (p_lo − v) / \|v\| |
| `within [lo, hi]` | provided range covers the required range | uncovered share |
| `equals v` | \|v − p\| ≤ tol (relative) | \|v − p\| / \|v\| |
| `point` | no bound: provided value within ±tol | uncovered share |
| `gate` | different or unknown dimension, different currency | NULL |

`score` is 1 within `tol` (default 0.02) and falls linearly to 0 across `soft_band` (default
0.05). Temperatures use an absolute tolerance of 1 K and a band of 2.5 K.

## Key Concepts

- **Base units.** Lengths in m, times in s, temperatures in K, currents in A. `°C` and `°F` are
  converted with an offset; currencies are never converted.
- **Language.** `lang` only decides ambiguous separators: `2.500` is 2500 in German, 2.5 in
  English. Bound words of both languages are always recognised.
- **Unknown units are not guessed.** `12 furlongs` keeps its number, `dimension` is NULL, and
  every comparison with it is a gate. Extend the seed with `unit_table`.
- **Unqualified values.** `38 dB(A)` without a bound word is a point. Give it the bound of its
  catalogue template with the `default_bound` argument (or `default_bounds := 'catalogue'` in
  `tree_coverage`).
- **Categories.** `IP54 or better` parses to `category` IP54 with bound `at_least`; compare it
  with `compat_score`, not `value_satisfies`.
