-- ============================================================================
-- Quantities: units, parsing (EN/DE) and normalisation
-- ============================================================================
-- Specification values arrive as text: 'max. 38 dB(A)', 'mindestens 24 cm',
-- '15–25 mm/s', 'IP54 or better'. parse_quantity turns them into a struct with
-- the value range in BASE units (m, s, A, K, kg, ...), the dimension, and the
-- direction of the limit (bound), so they can be compared with rules.
--
-- Functions:
--   quantity_units()                              the built-in units seed
--   parse_quantity(text [, lang [, unit_table]])  -> STRUCT(lo, hi, unit, dimension,
--                                                   bound, raw, source_unit, category)
--   normalize_quantity(value, unit [, unit_table]) -> STRUCT(value, unit, dimension)
--
-- Data: inline texts, then test/data/coverage/request_attributes.csv.gz
--
-- Run: ./build/release/duckdb < examples/quantities/parse_and_normalize.sql
-- ============================================================================

.print '>>> 1. The units seed: base = value × factor + offset'
SELECT symbol, dimension, factor, "offset", base_unit
FROM quantity_units()
WHERE symbol IN ('mm', 'kN', 'mA', 'min', '°C', 'dB(A)', 'EUR')
ORDER BY dimension, symbol;
-- Expected: mm -> m × 0.001, kN -> N × 1000, mA -> A × 0.001, min -> s × 60,
-- °C -> K + 273.15, dB(A) stays dB(A), EUR has factor NULL (never converted).

.print ''
.print '>>> 2. parse_quantity: bound words, ranges, tolerances, categories'
SELECT txt, q.lo, q.hi, q.unit, q.dimension, q.bound, q.category
FROM (VALUES (1, 'max. 38 dB(A)'), (2, 'at least 5 A'), (3, '6–18 V'), (4, '220 N ± 2 %'),
             (5, '37 °C'), (6, 'IP54 or better'), (7, '12 furlongs')) t(i, txt),
     LATERAL (SELECT parse_quantity(txt) AS q)
ORDER BY i;
-- Expected:
--   max. 38 dB(A)  | 38     | 38     | dB(A)    | sound_level | at_most
--   at least 5 A   | 5      | 5      | A        | current     | at_least
--   6–18 V         | 6      | 18     | V        | voltage     | within
--   220 N ± 2 %    | 220    | 220    | N        | force       | equals
--   37 °C          | 310.15 | 310.15 | K        | temperature | NULL (unqualified)
--   IP54 or better | NULL   | NULL   | NULL     | NULL        | at_least | IP54
--   12 furlongs    | 12     | 12     | furlongs | NULL (unknown unit: never guessed)

.print ''
.print '>>> 3. German texts: bound words and the decimal separator'
-- lang decides ambiguous separators only: '2.500' is 2500 in German, 2.5 in English.
SELECT txt, q.lo, q.hi, q.unit, q.bound
FROM (VALUES (1, 'mindestens 24 cm'), (2, 'höchstens 120 €'), (3, 'zwischen 15 und 25 mm/s'),
             (4, 'Nennwert 220 N'), (5, '65.000 Zyklen'), (6, '2.500 N')) t(i, txt),
     LATERAL (SELECT parse_quantity(txt, 'de') AS q)
ORDER BY i;
-- Expected:
--   mindestens 24 cm        | 0.24  | 0.24  | m   | at_least
--   höchstens 120 €         | 120   | 120   | EUR | at_most
--   zwischen 15 und 25 mm/s | 0.015 | 0.025 | m/s | within
--   Nennwert 220 N          | 220   | 220   | N   | equals
--   65.000 Zyklen           | 65000 | 65000 | 1   | NULL
--   2.500 N                 | 2500  | 2500  | N   | NULL

SELECT parse_quantity('2.500 N', 'en').lo AS english, parse_quantity('2.500 N', 'de').lo AS german;
-- Expected: 2.5 | 2500.0

.print ''
.print '>>> 4. normalize_quantity: one value and unit to the base unit'
SELECT normalize_quantity(250, 'mm')  AS travel,
       normalize_quantity(1.5, 'min') AS duration,
       normalize_quantity(12, 'parsec-ish') AS unknown;
-- Expected: {0.25, m, length} | {90.0, s, time} | {12.0, parsec-ish, NULL}

.print ''
.print '>>> 5. Parse a whole specification archive'
-- 2 736 extracted values from 80 requests, each in its request's language.
CREATE OR REPLACE TABLE requests AS
SELECT * FROM read_csv('test/data/coverage/requests.csv.gz');
CREATE OR REPLACE TABLE parsed AS
SELECT a.tree_id, a.name, a.value_text, parse_quantity(a.value_text, r.lang) AS q
FROM read_csv('test/data/coverage/request_attributes.csv.gz') a
JOIN requests r ON r.request_id = a.tree_id;

SELECT coalesce(q.bound, 'unqualified') AS bound, count(*) AS n
FROM parsed GROUP BY ALL ORDER BY n DESC;
-- Expected: at_least 985, at_most 867, unqualified 651, within 201, equals 32
-- About a quarter of the values carry no bound word; tree_coverage gives them
-- the direction of their catalogue template (default_bounds := 'catalogue').

-- Customer units and decimal commas, all in base units now:
SELECT value_text, q.lo, q.unit, q.bound
FROM parsed
WHERE value_text IN ('8,5 Nm', 'minimal 8 N·m', '2,2 min', '8,5 ms')
GROUP BY ALL ORDER BY value_text;
-- Expected:
--   2,2 min       | 132.0  | s   | NULL
--   8,5 Nm        | 8.5    | N*m | NULL
--   8,5 ms        | 0.0085 | s   | NULL
--   minimal 8 N·m | 8.0    | N*m | at_least
