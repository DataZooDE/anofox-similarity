-- ============================================================================
-- Direction-aware comparison: does the offered value meet the requirement?
-- ============================================================================
-- Numbers need rules, not embeddings. value_satisfies checks a provided value
-- against a requirement with its direction and returns
--   score    1 inside the tolerance (2 %), falling to 0 across a soft band (5 %)
--   distance how far off: relative for limits (0.079 = 7.9 % over the limit)
--   rule     at_least / at_most / within / equals / point / gate / missing
-- The unit is a hard gate: volts never satisfy amps, USD never satisfies EUR.
-- Ordinal classes (IP54, F1) are compared with compat_score and a curated
-- rules table, never by text similarity.
--
-- Functions:
--   value_satisfies(requirement, provided [, scale [, tol, soft_band [, default_bound]]])
--   compat_score(dimension, required, provided [, rules_table := 'compatibility_rules'])
--
-- Data: inline offers; test/data/coverage/compatibility_rules.csv.gz and
--       catalogue_attributes.csv.gz
--
-- Run: ./build/release/duckdb < examples/quantities/value_satisfies.sql
-- ============================================================================

.print '>>> 1. A noise limit: max. 38 dB(A)'
SELECT offer, v.score, round(v.distance, 3) AS distance, v.rule
FROM (VALUES ('track A', '35 dB(A)'), ('track B', '38,5 dB(A)'), ('track C', '41 dB(A)')) t(offer, noise),
     LATERAL (SELECT value_satisfies(parse_quantity('max. 38 dB(A)'), parse_quantity(noise, 'de')) AS v)
ORDER BY offer;
-- Expected:
--   track A | 1.0 | 0.0   | at_most   below the limit
--   track B | 1.0 | 0.013 | at_most   1.3 % over, inside the 2 % tolerance
--   track C | 0.0 | 0.079 | at_most   7.9 % over, beyond tolerance + soft band

.print ''
.print '>>> 2. The unit is a gate: at least 5 A'
SELECT offer, v.score, v.rule
FROM (VALUES ('O1', '5 V'), ('O2', '10 A'), ('O3', '4 A'), ('O4', '5000 mA'), ('O5', '5 furlongs')) t(offer, rating),
     LATERAL (SELECT value_satisfies(parse_quantity('at least 5 A'), parse_quantity(rating)) AS v)
ORDER BY offer;
-- Expected: O1 0.0 gate (volts), O2 1.0, O3 0.0 at_least, O4 1.0 (5000 mA = 5 A),
--           O5 0.0 gate (unknown unit)

.print ''
.print '>>> 3. Ranges, temperatures and prices'
SELECT label, v.score, round(v.distance, 3) AS distance, v.rule
FROM (VALUES
        -- the offered speed range must cover the required range
        (1, 'speed 15–25 mm/s vs 10–30 mm/s', parse_quantity('15–25 mm/s'), parse_quantity('10–30 mm/s')),
        -- temperatures use an absolute tolerance of 1 K and a band of 2.5 K
        (2, 'surface max. 43 °C vs 45 °C',    parse_quantity('max. 43 °C'), parse_quantity('45 °C')),
        -- currencies are never converted
        (3, 'price max. 120 € vs 126 €',      parse_quantity('höchstens 120 €', 'de'), parse_quantity('126 €')),
        (4, 'price max. 120 € vs 100 USD',    parse_quantity('höchstens 120 €', 'de'), parse_quantity('100 USD'))
     ) t(i, label, req, prov),
     LATERAL (SELECT value_satisfies(req, prov) AS v)
ORDER BY i;
-- Expected:
--   speed   | 1.0 | 0.0  | within    offered range covers the required one
--   surface | 0.6 | 2.0  | at_most   2 K too hot (distance in K)
--   price   | 0.4 | 0.05 | at_most   5 % over, inside the soft band
--   price   | 0.0 | NULL | gate      different currency

.print ''
.print '>>> 4. Unqualified values take the bound of their catalogue template'
-- '38 dB(A)' without a bound word is an exact point; 35 dB(A) is not 38.
-- The catalogue knows that seat-track noise is a lower-is-better limit.
CREATE OR REPLACE TABLE templates AS
SELECT * FROM read_csv('test/data/coverage/catalogue_attributes.csv.gz');

SELECT t.attr_key, t.bound AS template_bound,
       value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)')).rule AS as_point,
       value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)')).score AS point_score,
       value_satisfies(parse_quantity('38 dB(A)'), parse_quantity('35 dB(A)'),
                       NULL, 0.02, 0.05, t.bound).score AS template_score
FROM templates t
WHERE t.class_id = 'seat_track' AND t.attr_key = 'noise';
-- Expected: noise | at_most | point | 0.0 | 1.0

.print ''
.print '>>> 5. Ordinal classes with compat_score'
-- Rules: IP40 < IP54 < IP65 < IP67; one step worse scores 0.5, two steps 0.
CREATE OR REPLACE TABLE compatibility_rules AS
SELECT * FROM read_csv('test/data/coverage/compatibility_rules.csv.gz');

SELECT provided,
       compat_score('protection_class', parse_quantity('IP54 or better').category, provided) AS score
FROM (VALUES ('IP67'), ('IP65'), ('IP54'), ('IP40')) t(provided)
ORDER BY provided DESC;
-- Expected: IP67 1.0, IP65 1.0, IP54 1.0, IP40 0.5

SELECT compat_score('protection_class', 'IP65', 'IP40') AS two_steps_worse,
       compat_score('flammability_class', 'F1', 'F2')   AS one_step_worse;
-- Expected: 0.0 | 0.5
