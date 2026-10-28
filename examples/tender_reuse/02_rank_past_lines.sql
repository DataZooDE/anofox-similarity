-- ============================================================================
-- Tender reuse 2/3: which past line can we offer again?
-- ============================================================================
-- Rank 12 past lines against RFQ-2026-014, then turn the score into what a bid
-- needs: how many MUST clauses each line misses. The headline coverage mixes must
-- and should and gives partial credit, so it is a ranking aid, never compliance.
--
-- Run from the repository root:
--   DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/tender_reuse/02_rank_past_lines.sql
-- ============================================================================

-- Load (the same block opens every script of this folder; 01 explains each fix)
CREATE OR REPLACE TABLE units AS                             -- the seed plus the RFQ's "Becher/min"
SELECT symbol, alias, dimension, factor, "offset" FROM quantity_units()
UNION ALL SELECT 'Becher/min', ['cups/min'], 'frequency', 1/60, 0.0;
CREATE OR REPLACE TABLE rfq_nodes AS
SELECT unnest(n) FROM (SELECT unnest(tree_from_json(content, NULL, 'de', (SELECT list(u) FROM units u))) AS n
  FROM read_text('examples/tender_reuse/data/rfq_2026_014.json'));
CREATE OR REPLACE TABLE rfq_attributes AS                     -- categories get their dimension
SELECT * REPLACE (coalesce(dimension, CASE WHEN category LIKE 'IP%' THEN 'protection_class'
                                           WHEN category LIKE 'PN%' THEN 'pressure_class' END) AS dimension)
FROM (SELECT unnest(attributes, recursive := true) FROM rfq_nodes);
CREATE OR REPLACE TABLE past_line_nodes AS
SELECT * FROM read_csv('examples/tender_reuse/data/past_line_nodes.csv');
CREATE OR REPLACE TABLE past_line_attributes AS               -- no duplicates, base units only
SELECT DISTINCT * REPLACE (
  CASE WHEN lo IS NULL THEN unit ELSE normalize_quantity(lo, unit).unit END AS unit,
  CASE WHEN lo IS NULL THEN lo ELSE normalize_quantity(lo, unit).value END AS lo,
  CASE WHEN hi IS NULL THEN hi ELSE normalize_quantity(hi, unit).value END AS hi)
FROM read_csv('examples/tender_reuse/data/past_line_attributes.csv');
CREATE OR REPLACE TABLE clauses AS SELECT * FROM read_csv('examples/tender_reuse/data/clauses.csv');
CREATE OR REPLACE TABLE catalogue_attributes AS
SELECT * FROM read_csv('examples/tender_reuse/data/catalogue_attributes.csv');
CREATE OR REPLACE TABLE vocabulary_overrides AS
SELECT * FROM read_csv('examples/tender_reuse/data/vocabulary_overrides.csv');
CREATE OR REPLACE TABLE compatibility_rules AS
SELECT * FROM read_csv('examples/tender_reuse/data/compatibility_rules.csv');

-- The kernel's explanation for all 12 lines, one row per required attribute
CREATE OR REPLACE TABLE trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-014', k := 12,
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules',
  mu := 0.0, default_bounds := 'catalogue');

-- One verdict per candidate and clause. The trace marks a whole requirement item as
-- a gap (is_gap) when any of its attributes misses, so verdicts use value_score per
-- attribute and the counts per item. strict_score re-checks each value with no
-- tolerance (tol 0, soft band 0, positional on DuckDB 1.5.6): the kernel scores a
-- value up to 2 % (linear) or 4.5 % (log scale, e.g. power) past a limit as 1.0.
CREATE OR REPLACE TABLE clause_check AS
SELECT t.candidate_id, c.clause_id, c.page, c.must_should, c.kind, c.text AS required,
       CASE WHEN t.matched_node_id IS NULL THEN 'missing' WHEN p.category IS NOT NULL THEN p.category
            ELSE printf('%g %s', round((p.lo - u."offset") / u.factor, 4), u.source_unit) END AS offered,
       t.value_score AS kernel_score,
       CASE WHEN r.category IS NOT NULL OR p.lo IS NULL THEN t.value_score
            ELSE value_satisfies({'lo': r.lo, 'hi': r.hi, 'dimension': r.dimension,
                                  'bound': coalesce(r.bound, tpl.bound)},
                                 {'lo': p.lo, 'hi': p.hi, 'dimension': p.dimension}, NULL, 0.0, 0.0).score
       END AS strict_score
FROM clauses c
JOIN trace t ON t.req_node_id = c.node_id AND t.attr_id = c.attr_id
JOIN rfq_attributes r ON r.node_id = c.node_id AND r.attr_id = c.attr_id
LEFT JOIN past_line_attributes p ON p.node_id = t.matched_node_id AND p.attr_id = t.attr_id
LEFT JOIN (SELECT a.node_id, a.attr_id, q.source_unit, any_value(u.factor) AS factor,
                  any_value(u."offset") AS "offset"           -- offered value in the clause's unit
           FROM rfq_attributes a,
                LATERAL (SELECT parse_quantity(a.value_text, 'de', (SELECT list(u) FROM units u)) AS q)
           JOIN units u ON u.symbol = q.source_unit OR list_contains(u.alias, q.source_unit)
           GROUP BY ALL) u ON u.node_id = c.node_id AND u.attr_id = c.attr_id
LEFT JOIN (SELECT DISTINCT attr_key, dimension, bound FROM catalogue_attributes) tpl
       ON tpl.attr_key = r.attr_id AND tpl.dimension = r.dimension
WHERE c.kind = 'value'
UNION ALL
SELECT DISTINCT t.candidate_id, c.clause_id, c.page, c.must_should, c.kind, c.text,
       coalesce(t.count_available::INT || ' x', 'missing'),
       CASE WHEN t.matched_node_id IS NULL THEN 0.0 ELSE least(1.0, t.count_available / t.count_needed) END,
       CASE WHEN t.matched_node_id IS NULL THEN 0.0 ELSE least(1.0, t.count_available / t.count_needed) END
FROM clauses c JOIN trace t ON t.req_node_id = c.node_id
WHERE c.kind = 'item';

.print '>>> 1. Ranking with must clauses: the bid view'
-- must_missed counts the 33 machine-checkable must clauses a line does not meet
-- (strict); the 4 manual must clauses are on every deviation list (see 03).
SELECT r.candidate_id, round(r.coverage, 4) AS coverage, r.rank,
       count(*) FILTER (WHERE k.must_should = 'must' AND k.strict_score < 1) AS must_missed,
       count(*) FILTER (WHERE k.must_should = 'should' AND k.strict_score < 1) AS should_missed,
       round(avg(k.strict_score) FILTER (WHERE k.must_should = 'must'), 4) AS must_coverage,
       r.assignment_used
FROM tree_coverage('RFQ-2026-014', k := 12,
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules',
  mu := 0.0, default_bounds := 'catalogue') r
JOIN clause_check k USING (candidate_id)
GROUP BY ALL ORDER BY must_missed, should_missed, r.rank;
-- Expected:
-- | candidate_id     | coverage | rank | must_missed | should_missed | must_coverage | assignment_used |
-- | PAST-LEAN-2019   | 0.9697   | 1    | 0           | 1             | 1.0           | partition       |
-- | PAST-SWAP-2018   | 0.9697   | 2    | 0           | 1             | 1.0           | partition       |
-- | PAST-UNIT-2021   | 0.9697   | 3    | 0           | 1             | 1.0           | partition       |
-- | PAST-RICH-2022   | 0.9697   | 4    | 0           | 1             | 1.0           | partition       |
-- | PAST-FAST-2024   | 0.8788   | 11   | 0           | 4             | 1.0           | partition       |
-- | PAST-NOISY-2020  | 0.9394   | 6    | 1           | 1             | 0.9697        | partition       |
-- | PAST-CUSTOM-2021 | 0.9242   | 8    | 1           | 1             | 0.9848        | partition       |
-- | PAST-TOL-2023    | 0.9576   | 5    | 2           | 1             | 0.9394        | partition       |  (kernel: one must gap)
-- | PAST-PN10-2016   | 0.9242   | 7    | 2           | 1             | 0.9394        | partition       |
-- | PAST-SHORT-2017  | 0.8939   | 9    | 2           | 1             | 0.9545        | partition       |
-- | PAST-OLD-2012    | 0.7803   | 12   | 4           | 2             | 0.8788        | partition       |
-- | PAST-NOCIP-2015  | 0.8788   | 10   | 5           | 1             | 0.8485        | partition       |
-- Five lines meet every must clause. No line reaches 1 400 cups/min (C-42, should):
-- the RFQ is honestly not fully coverable, and every bid carries that deviation.
-- PAST-FAST-2024 ranks 11th on coverage only because it lacks the optional labeller.

.print ''
.print '>>> 2. Why the three settings matter: three full-cover lines under each setting'
-- PAST-RICH-2022 offers IP67 and PN25 (better than asked), PAST-SWAP-2018 has
-- English attribute names, and RFQ item C1 has no class_id.
CREATE OR REPLACE TABLE settings AS
SELECT 1 AS step, 'defaults' AS setting, * FROM tree_coverage('RFQ-2026-014',
  candidates := ['PAST-LEAN-2019', 'PAST-RICH-2022', 'PAST-SWAP-2018'],
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes')
UNION ALL
SELECT 2, '+ vocabulary', * FROM tree_coverage('RFQ-2026-014',
  candidates := ['PAST-LEAN-2019', 'PAST-RICH-2022', 'PAST-SWAP-2018'],
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides')
UNION ALL
SELECT 3, '+ rules', * FROM tree_coverage('RFQ-2026-014',
  candidates := ['PAST-LEAN-2019', 'PAST-RICH-2022', 'PAST-SWAP-2018'],
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules')
UNION ALL
SELECT 4, '+ mu 0', * FROM tree_coverage('RFQ-2026-014',
  candidates := ['PAST-LEAN-2019', 'PAST-RICH-2022', 'PAST-SWAP-2018'],
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules', mu := 0.0)
UNION ALL
SELECT 5, '+ catalogue bounds', * FROM tree_coverage('RFQ-2026-014',
  candidates := ['PAST-LEAN-2019', 'PAST-RICH-2022', 'PAST-SWAP-2018'],
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules', mu := 0.0,
  default_bounds := 'catalogue');
SELECT setting,
       round(max(coverage) FILTER (WHERE candidate_id = 'PAST-LEAN-2019'), 4) AS lean_2019,
       round(max(coverage) FILTER (WHERE candidate_id = 'PAST-RICH-2022'), 4) AS rich_2022,
       round(max(coverage) FILTER (WHERE candidate_id = 'PAST-SWAP-2018'), 4) AS swap_2018,
       min(assignment_used) AS assignment_used
FROM settings GROUP BY step, setting ORDER BY step;
-- Expected:
-- | setting            | lean_2019 | rich_2022 | swap_2018 | assignment_used |
-- | defaults           | 0.858     | 0.8125    | 0.7045    | best_match      |  (C1 has no class)
-- | + vocabulary       | 0.9489    | 0.8807    | 0.7773    | partition       |
-- | + rules            | 0.9489    | 0.9489    | 0.7773    | partition       |  (IP67, PN25 now meet the clauses)
-- | + mu 0             | 0.9489    | 0.9489    | 0.9489    | partition       |  (English names no longer cost)
-- | + catalogue bounds | 0.9697    | 0.9697    | 0.9697    | partition       |  (C-16 and C-33 get a direction)
-- The remaining 0.0303 is C-42 (1 400 cups/min), which no past line reaches.

.print ''
.print '>>> 3. PAST-SWAP-2018: same-class items are paired by value, not by node id'
-- Its export lists the 20 W fruit drive as .D2 and the 95 W main drive as .D3.
SELECT req_node_id, attr_id, req_value, matched_node_id, prov_value, value_score
FROM trace WHERE candidate_id = 'PAST-SWAP-2018' AND attr_id = 'power' ORDER BY req_node_id;
-- Expected:
-- | req_node_id     | attr_id | req_value     | matched_node_id   | prov_value | value_score |
-- | RFQ-2026-014.D2 | power   | at_least 90 W | PAST-SWAP-2018.D3 | 95 W       | 1.0         |
-- | RFQ-2026-014.D3 | power   | at_least 10 W | PAST-SWAP-2018.D2 | 20 W       | 1.0         |

.print ''
.print '>>> 4. Hard limits: values the default tolerance passes although they miss the limit'
SELECT candidate_id, clause_id, required, offered, kernel_score, strict_score
FROM clause_check WHERE kernel_score = 1 AND strict_score < 1 ORDER BY ALL;
-- Expected:
-- | candidate_id  | clause_id | required                               | offered | kernel_score | strict_score |
-- | PAST-TOL-2023 | C-14      | Stromaufnahme Heizung: höchstens 7,5 A | 7.6 A   | 1.0          | 0.0          |
-- 7.6 A is 1.3 % over: inside the default tolerance (tol 0.02 plus a soft band). The
-- 87 W against "mind. 90 W" (3.3 % short) is a gap already, because the catalogue
-- template compares power on a linear scale. For a bid that promises "every must clause met", pass tol := 0, soft_band := 0
-- to the kernel itself:
CREATE OR REPLACE TABLE strict_trace AS
SELECT * FROM tree_coverage_trace('RFQ-2026-014', k := 12,
  nodes_table := 'rfq_nodes', attrs_table := 'rfq_attributes',
  candidate_nodes_table := 'past_line_nodes', candidate_attrs_table := 'past_line_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules',
  mu := 0.0, default_bounds := 'catalogue', tol := 0.0, soft_band := 0.0);
SELECT candidate_id, attr_name, req_value, prov_value, value_score
FROM strict_trace WHERE candidate_id = 'PAST-TOL-2023' AND value_score < 1 ORDER BY ALL;
-- Expected:
-- | candidate_id  | attr_name             | req_value           | prov_value | value_score |
-- | PAST-TOL-2023 | Antriebsleistung      | at_least 90 W       | 87 W       | 0.0         |
-- | PAST-TOL-2023 | Ausbringung           | at_least 23.3333 Hz | 20 Hz      | 0.0         |
-- | PAST-TOL-2023 | Stromaufnahme Heizung | at_most 7.5 A       | 7.6 A      | 0.0         |
-- (Ausbringung is C-42, 1 400 cups/min, which no line reaches.)

.print ''
.print '>>> 5. Gold check: deviations found against the ground truth (33 deviations, 12 lines)'
CREATE OR REPLACE TABLE gold AS SELECT * FROM read_csv('examples/tender_reuse/data/gold_deviations.csv');
SELECT check_name,
       count(*) FILTER (WHERE found AND in_gold) AS true_pos,
       count(*) FILTER (WHERE found AND NOT in_gold) AS false_pos,
       count(*) FILTER (WHERE NOT found AND in_gold) AS missed
FROM (SELECT k.candidate_id, k.clause_id, g.clause_id IS NOT NULL AS in_gold,
             unnest(['kernel', 'strict']) AS check_name,
             unnest([k.kernel_score < 1, k.strict_score < 1]) AS found
      FROM clause_check k
      LEFT JOIN gold g USING (candidate_id, clause_id)
      UNION ALL  -- the kernel with tol 0: value clauses from the strict trace, item clauses as above
      SELECT k.candidate_id, k.clause_id, g.clause_id IS NOT NULL, 'kernel, tol 0',
             CASE WHEN k.kind = 'value' THEN coalesce(t.value_score, 0) < 1 ELSE k.kernel_score < 1 END
      FROM clause_check k
      JOIN clauses c ON c.clause_id = k.clause_id
      LEFT JOIN (SELECT DISTINCT candidate_id, req_node_id, attr_id, value_score FROM strict_trace) t
             ON t.candidate_id = k.candidate_id AND t.req_node_id = c.node_id AND t.attr_id = c.attr_id
      LEFT JOIN gold g ON g.candidate_id = k.candidate_id AND g.clause_id = k.clause_id)
GROUP BY check_name ORDER BY check_name;
-- Expected:
-- | check_name | true_pos | false_pos | missed |
-- | kernel        | 32       | 0         | 1      |  (the value of step 4)
-- | kernel, tol 0 | 33       | 0         | 0      |
-- | strict        | 33       | 0         | 0      |
-- With the default tolerance the kernel misses one deviation; with tol := 0 and
-- soft_band := 0 it finds all 33, the same as the per-clause re-check.
