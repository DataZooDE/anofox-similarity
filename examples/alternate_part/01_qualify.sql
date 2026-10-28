-- Rheinwerk Ersatzteile: nightly alternate-part qualification.
-- Run from repository root. Category normalisation is explicit; the quantity parser
-- deliberately refuses to interpret part and standards designations as numbers.
.bail on
.print '>>> 1. Load and audit the flat extracts'
CREATE TABLE part_master AS SELECT * FROM read_csv('examples/alternate_part/data/part_master.csv');
CREATE TABLE part_characteristics AS SELECT * FROM read_csv('examples/alternate_part/data/part_characteristics.csv', all_varchar=true);
CREATE TABLE compatibility_rules AS SELECT * FROM read_csv('examples/alternate_part/data/compatibility_rules.csv');
CREATE TABLE gold_verdicts AS SELECT * FROM read_csv('examples/alternate_part/data/gold_verdicts.csv');
SELECT count(*) AS parts, (SELECT count(*) FROM part_characteristics) AS characteristics,
       (SELECT count(*) FROM compatibility_rules) AS approved_upgrade_rules FROM part_master;
-- Expected: 9 | 73 | 2

CREATE VIEW duplicate_keys AS
SELECT part_id, attr_key, count(*) AS n, count(DISTINCT value_text) AS distinct_values
FROM part_characteristics GROUP BY ALL HAVING count(*) > 1;
SELECT part_id, attr_key, n, distinct_values FROM duplicate_keys;
-- Expected: ALT-302 | thread | 2 | 2

.print '>>> 2. Parse audit: inspect unsafe designations before matching'
SELECT value_text, q.lo, q.dimension, q.category
FROM (SELECT value_text FROM part_characteristics
      WHERE value_text IN ('M8x1,25', 'DN50', 'PN 16', '1.4404', 'EN 10204 3.1', '± 0,05 mm', 'not to exceed 5 bar')
      GROUP BY ALL) t, LATERAL (SELECT parse_quantity(value_text, 'de') q)
ORDER BY value_text;
-- Expected: M8x1,25 and EN 10204 3.1 have no numeric dimension; 1.4404 is misread as count.
-- Expected: ± 0,05 mm | 5e-05 | length
-- Expected: not to exceed 5 bar | 500000.0 | pressure

-- Categories are mapped by known attribute key, never inferred from a code's digits.
CREATE VIEW normalized AS
SELECT part_id, attr_key, kind, value_text,
       CASE WHEN kind = 'category' THEN
         CASE attr_key
           WHEN 'thread' THEN replace(upper(trim(value_text)), ',', '.')
           WHEN 'nominal_diameter' THEN regexp_replace(upper(value_text), '[[:space:]]+', '', 'g')
           WHEN 'pressure_class' THEN regexp_replace(upper(value_text), '[[:space:]]+', '', 'g')
           WHEN 'material' THEN trim(value_text)
           WHEN 'certificate' THEN nullif(regexp_extract(value_text, '3[.][12]'), '')
         END END AS category,
       CASE WHEN kind = 'quantity' AND nullif(value_text, '') IS NOT NULL
            THEN parse_quantity(value_text, 'de') END AS q
FROM part_characteristics;
CREATE VIEW required AS SELECT * FROM normalized WHERE part_id = 'RW-PUMP-50';
CREATE VIEW candidates AS SELECT * FROM normalized WHERE part_id <> 'RW-PUMP-50';

.print '>>> 3. Score every required attribute with strict numeric limits'
-- DuckDB 1.5.6 binds scalar parameters positionally. NULL, 0, 0 means
-- default linear scale, zero tolerance, zero soft band for hard limits.
CREATE VIEW scored AS
SELECT c.part_id, r.attr_key, r.value_text AS required_value,
       c.value_text AS observed_value,
       CASE WHEN c.kind = 'category' THEN compat_score(r.attr_key, r.category, c.category)
            ELSE value_satisfies(r.q, c.q, NULL, 0.0, 0.0).score END AS score
FROM required r JOIN candidates c USING (attr_key);
SELECT part_id, attr_key, required_value, observed_value, score
FROM scored WHERE score IS NULL OR score < 1 ORDER BY part_id, attr_key;
-- Expected: ALT-201 thread 0; ALT-202 pressure_class 0; ALT-203 bore 0;
-- Expected: ALT-204 pressure_limit 0; ALT-301 certificate NULL; ALT-302 thread 0.

.print '>>> 4. Gate on duplicates and missing values, then compare to gold'
CREATE VIEW verdicts AS
WITH summary AS (
 SELECT m.part_id,
        count(s.attr_key) AS scored_rows,
        count(*) FILTER (WHERE s.score IS NULL) AS missing_rows,
        count(*) FILTER (WHERE s.score < 1) AS failed_rows,
        count(d.attr_key) AS duplicate_keys
 FROM part_master m
 LEFT JOIN scored s ON s.part_id = m.part_id
 LEFT JOIN duplicate_keys d ON d.part_id = m.part_id AND d.attr_key = s.attr_key
 WHERE m.role = 'candidate' GROUP BY m.part_id
)
SELECT part_id, scored_rows, missing_rows, failed_rows,
       CASE WHEN duplicate_keys > 0 OR missing_rows > 0 OR scored_rows <> 8 THEN 'REVIEW'
            WHEN failed_rows > 0 THEN 'REJECT' ELSE 'ACCEPT' END AS verdict
FROM summary;
SELECT v.part_id, v.verdict, v.scored_rows, v.missing_rows, v.failed_rows,
       g.verdict AS gold, v.verdict = g.verdict AS matches_gold
FROM verdicts v JOIN gold_verdicts g USING (part_id) ORDER BY v.part_id;
-- Expected: 2 ACCEPT, 4 REJECT, 2 REVIEW; all 8 match gold.
SELECT verdict, count(*) AS n FROM verdicts GROUP BY ALL ORDER BY verdict;
-- Expected: ACCEPT | 2
-- Expected: REJECT | 4
-- Expected: REVIEW | 2
SELECT count(*) AS gold_mismatches FROM verdicts v JOIN gold_verdicts g USING (part_id)
WHERE v.verdict <> g.verdict;
-- Expected: 0
