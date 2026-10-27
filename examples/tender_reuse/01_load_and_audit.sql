-- ============================================================================
-- Tender reuse 1/3: load the RFQ and audit what the parser made of it
-- ============================================================================
-- Allgäuer Abfülltechnik Demo GmbH (fictional) answers RFQ-2026-014 of Molkerei
-- Bergblick Demo eG (fictional): a yoghurt-cup filling and sealing line with 42
-- clauses (37 must, 5 should). Before any coverage number is trusted, check what
-- the parser read from every clause. A clause read wrongly turns "we misread the
-- spec" into "our line fails the spec", and no function raises an error for it.
--
-- QUICK START: the four inputs and the three settings every script uses
--   rfq_nodes / rfq_attributes        the request, from the extracted JSON (tree_from_json)
--   past_line_nodes / _attributes     12 past lines from the PLM export, values in base units
--   clauses                           sidecar: clause_id, page, must/should per node and attribute
--   vocabulary_overrides              house glossary: wording -> class ("Kompaktaggregat Hygiene")
--   mu := 0.0                         compare values only; attribute names are German, English
--                                     or typo'd in the archive and must not lower a score
--   default_bounds := 'catalogue'     "72 dB(A)" without a bound word takes the direction of
--                                     its catalogue template (at_most) instead of "exactly 72"
--   rules_table := 'compatibility_rules'   IP67 meets "IP65 oder besser", PN10 never meets PN16
--
-- Run from the repository root:
--   DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/tender_reuse/01_load_and_audit.sql
-- ============================================================================

-- The request: one extracted JSON document. "Becher/min" (cups per minute) is not a
-- unit the extension knows, so it is added to the units seed as a frequency.
CREATE OR REPLACE TABLE units AS                             -- the seed plus the RFQ's "Becher/min"
SELECT symbol, alias, dimension, factor, "offset" FROM quantity_units()
UNION ALL SELECT 'Becher/min', ['cups/min'], 'frequency', 1/60, 0.0;
CREATE OR REPLACE TABLE rfq_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content, NULL, 'de', (SELECT list(u) FROM units u))) AS n
  FROM read_text('examples/tender_reuse/data/rfq_2026_014.json'));
CREATE OR REPLACE TABLE rfq_attributes AS SELECT unnest(attributes, recursive := true) FROM rfq_nodes;
CREATE OR REPLACE TABLE clauses AS SELECT * FROM read_csv('examples/tender_reuse/data/clauses.csv');
CREATE OR REPLACE TABLE past_line_nodes AS
SELECT * FROM read_csv('examples/tender_reuse/data/past_line_nodes.csv');
CREATE OR REPLACE TABLE past_line_attributes_raw AS
SELECT * FROM read_csv('examples/tender_reuse/data/past_line_attributes.csv');
CREATE OR REPLACE TABLE vocabulary_overrides AS
SELECT * FROM read_csv('examples/tender_reuse/data/vocabulary_overrides.csv');

.print '>>> 1. What is loaded'
SELECT (SELECT count(*) FROM rfq_nodes WHERE node_kind = 'item') AS rfq_items,
       (SELECT count(*) FROM rfq_attributes) AS rfq_values,
       (SELECT count(*) FROM clauses) AS clauses,
       (SELECT count(*) FROM clauses WHERE must_should = 'must') AS must,
       (SELECT count(DISTINCT tree_id) FROM past_line_nodes) AS past_lines,
       (SELECT count(*) FROM past_line_nodes) AS past_nodes,
       (SELECT count(*) FROM past_line_attributes_raw) AS past_values;
-- Expected:
-- | rfq_items | rfq_values | clauses | must | past_lines | past_nodes | past_values |
-- | 11        | 27         | 42      | 37   | 12         | 278        | 395         |

.print ''
.print '>>> 2. Parse audit: value and manual clauses, and how many the parser could not read'
-- A clause is unread when it has neither a dimension nor a category. Manual clauses
-- are designations and standards; the kernel never checks them. (Item clauses carry
-- their count in the JSON and are not parsed.)
SELECT c.kind, count(*) AS clauses,
       count(*) FILTER (WHERE q.dimension IS NULL AND q.category IS NULL) AS unread,
       count(*) FILTER (WHERE q.bound IS NULL AND q.dimension IS NOT NULL) AS no_bound_word
FROM clauses c
LEFT JOIN rfq_attributes a ON a.node_id = c.node_id AND a.attr_id = c.attr_id,
LATERAL (SELECT CASE WHEN c.kind = 'value' THEN {'dimension': a.dimension, 'category': a.category, 'bound': a.bound}
                     ELSE {'dimension': parse_quantity(c.text, 'de').dimension,
                           'category': parse_quantity(c.text, 'de').category,
                           'bound': parse_quantity(c.text, 'de').bound} END AS q)
WHERE c.kind <> 'item'
GROUP BY c.kind ORDER BY c.kind;
-- Expected:
-- | kind   | clauses | unread | no_bound_word |
-- | manual | 4       | 3      | 1             |  (the 1 is "1.4404", see step 4)
-- | value  | 27      | 0      | 2             |  (C-16 and C-33, see step 3)
-- Without the unit_table above, "mind. 1.400 Becher/min" is unread (dimension NULL).

.print ''
.print '>>> 3. The trap clauses: what the parser read'
SELECT c.clause_id, a.value_text, a.lo, a.unit, a.dimension, a.bound, a.category
FROM clauses c JOIN rfq_attributes a ON a.node_id = c.node_id AND a.attr_id = c.attr_id
WHERE c.clause_id IN ('C-02', 'C-04', 'C-14', 'C-15', 'C-16', 'C-21', 'C-28', 'C-29', 'C-33', 'C-42')
ORDER BY c.clause_id;
-- Expected:
-- | clause_id | value_text             | lo        | unit  | dimension   | bound    | category |
-- | C-02      | mind 60 mm             | 0.06      | m     | length      | at_least |          |  (no period after mind)
-- | C-04      | ± 0,5 g                | 0.0005    | kg    | mass        | at_most  |          |  (a bare tolerance is a limit)
-- | C-14      | höchstens 7,5 A        | 7.5       | A     | current     | at_most  |          |
-- | C-15      | mind. 2.500 h          | 9000000.0 | s     | time        | at_least |          |  (2500 h, not 2.5 h)
-- | C-16      | 72 dB(A)               | 72.0      | dB(A) | sound_level |          |          |  (no bound word: the catalogue says at_most)
-- | C-21      | nicht über 75 dB(A)    | 75.0      | dB(A) | sound_level | at_most  |          |  (negated limit)
-- | C-28      | not to exceed 5 bar    | 500000.0  | Pa    | pressure    | at_most  |          |
-- | C-29      | PN 16                  |           |       |             |          | PN16     |  (a category, no dimension)
-- | C-33      | 400 V                  | 400.0     | V     | voltage     |          |          |  (the catalogue says equals)
-- | C-42      | mind. 1.400 Becher/min | 23.333333333333332 | Hz | frequency | at_least |   |  (1400 per minute)

.print ''
.print '>>> 4. Manual clauses: designations must stay unread'
-- Three of four are refused (all NULL), which is what you want: they go on the
-- deviation list as "confirm by hand". "1.4404" is still read as the number 1.4404.
SELECT c.clause_id, c.text, q.lo, q.dimension
FROM clauses c, LATERAL (SELECT parse_quantity(c.text, 'de') AS q)
WHERE c.kind = 'manual' ORDER BY c.clause_id;
-- Expected:
-- | clause_id | text                                  | lo     | dimension |
-- | C-05      | Abnahmeprüfzeugnis EN 10204 3.1       |        |           |
-- | C-17      | Werkstoff produktberührt 1.4404       | 1.4404 | count     |  (a material number read as a count)
-- | C-22      | Verbindungselemente M8x1,25 aus A4-70 |        |           |
-- | C-30      | Hygienic design to EN 1672-2          |        |           |

.print ''
.print '>>> 5. Validate the trees: a category needs a dimension for the rules'
SELECT tree_id, issue, severity, count(*) AS n
FROM validate_trees('rfq_nodes', 'rfq_attributes') GROUP BY ALL ORDER BY ALL;
-- Expected:
-- | tree_id      | issue                      | severity | n |
-- | RFQ-2026-014 | category_without_dimension | warning  | 3 |
-- Fix: a regex normaliser on the code, as every later script does at load time.
UPDATE rfq_attributes SET dimension = CASE WHEN category LIKE 'IP%' THEN 'protection_class'
                                           WHEN category LIKE 'PN%' THEN 'pressure_class' END
WHERE category IS NOT NULL AND dimension IS NULL;
SELECT count(*) AS warnings_left FROM validate_trees('rfq_nodes', 'rfq_attributes');
-- Expected:
-- | warnings_left |
-- | 0             |

.print ''
.print '>>> 6. Check the past-line export: duplicates and values not in the base unit'
SELECT tree_id, node_id, attr_id, issue, severity
FROM validate_trees('past_line_nodes', 'past_line_attributes_raw') ORDER BY ALL;
-- Expected:
-- | tree_id        | node_id           | attr_id | issue               | severity |
-- | PAST-UNIT-2021 | PAST-UNIT-2021.D2 | power   | duplicate_attribute | error    |
-- validate_trees does not flag a value in mm: the kernel would read 10500 mm as
-- 10500 m and fail "max. 12 m". Find such rows against the units seed:
SELECT a.tree_id, a.node_id, a.attr_id, a.lo, a.unit, u.dimension
FROM past_line_attributes_raw a JOIN quantity_units() u ON u.symbol = a.unit
WHERE u.factor IS NOT NULL AND (u.factor <> 1 OR u."offset" <> 0)
ORDER BY ALL;
-- Expected:
-- | tree_id        | node_id           | attr_id | lo      | unit | dimension |
-- | PAST-UNIT-2021 | PAST-UNIT-2021.L2 | length  | 10500.0 | mm   | length    |
-- Fix: drop exact duplicates and convert to the base unit (every later script does this).
CREATE OR REPLACE TABLE past_line_attributes AS
SELECT DISTINCT * REPLACE (
  CASE WHEN lo IS NULL THEN unit ELSE normalize_quantity(lo, unit).unit END AS unit,
  CASE WHEN lo IS NULL THEN lo ELSE normalize_quantity(lo, unit).value END AS lo,
  CASE WHEN hi IS NULL THEN hi ELSE normalize_quantity(hi, unit).value END AS hi)
FROM past_line_attributes_raw;
SELECT count(*) AS past_values, count(*) FILTER (WHERE unit = 'mm') AS in_mm,
       (SELECT count(*) FROM validate_trees('past_line_nodes', 'past_line_attributes')) AS issues
FROM past_line_attributes;
-- Expected:
-- | past_values | in_mm | issues |
-- | 394         | 0     | 0      |

.print ''
.print '>>> 7. Classes: one RFQ item has no class_id; the house vocabulary resolves it'
SELECT a.node_id, n.label, a.class_id, a.reviewed
FROM tree_anchors('rfq_nodes', overrides_table := 'vocabulary_overrides') a
JOIN rfq_nodes n USING (node_id) WHERE n.class_id IS NULL;
-- Expected:
-- | node_id         | label                   | class_id | reviewed |
-- | RFQ-2026-014.C1 | Kompaktaggregat Hygiene | cip_unit | true     |
-- Without overrides_table the item has no class and the request is not reviewed,
-- so tree_coverage cannot use partition assignment (see 02, step 2).
