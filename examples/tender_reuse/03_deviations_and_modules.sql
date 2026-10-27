-- ============================================================================
-- Tender reuse 3/3: the signable deviation list, and modules per station
-- ============================================================================
-- The customer signs a deviation list, not a score: every clause the offered line
-- does not meet, with clause number, page and must/should, plus every clause that
-- has to be confirmed by hand. Then: which module variant covers each station?
--
-- Run from the repository root:
--   DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/tender_reuse/03_deviations_and_modules.sql
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

-- A clause the offer does not meet, and every manual clause, per candidate
CREATE OR REPLACE VIEW deviations AS
SELECT candidate_id, clause_id, page, must_should, 'not met' AS status, required, offered,
       round(strict_score, 4) AS score
FROM clause_check WHERE strict_score < 1
UNION ALL
SELECT DISTINCT k.candidate_id, c.clause_id, c.page, c.must_should, 'confirm by hand', c.text, NULL, NULL
FROM clauses c, (SELECT DISTINCT candidate_id FROM clause_check) k WHERE c.kind = 'manual';

.print '>>> 1. Deviation list for the bid basis PAST-LEAN-2019 (CSV, as sent with the offer)'
.mode csv
.nullvalue ''
SELECT clause_id, page, must_should, status, required, offered, score
FROM deviations WHERE candidate_id = 'PAST-LEAN-2019' ORDER BY clause_id;
.nullvalue NULL
.mode duckbox
-- Expected:
-- | clause_id | page | must_should | status          | required                              | offered         | score |
-- | C-05      | 14   | must        | confirm by hand | Abnahmeprüfzeugnis EN 10204 3.1       |                 |       |
-- | C-17      | 22   | must        | confirm by hand | Werkstoff produktberührt 1.4404       |                 |       |
-- | C-22      | 27   | must        | confirm by hand | Verbindungselemente M8x1,25 aus A4-70 |                 |       |
-- | C-30      | 33   | must        | confirm by hand | Hygienic design to EN 1672-2          |                 |       |
-- | C-42      | 61   | should      | not met         | Ausbringung: mind. 1.400 Becher/min   | 1200 Becher/min | 0.0   |
-- To write the file: COPY (SELECT ... FROM deviations WHERE ...) TO 'RFQ-2026-014_deviations.csv' (HEADER);

.print ''
.print '>>> 2. Near misses: the must clauses that keep a line out of the bid'
SELECT candidate_id, clause_id, page, required, offered, score
FROM deviations
WHERE must_should = 'must' AND status = 'not met'
  AND candidate_id IN (SELECT candidate_id FROM deviations
                       WHERE must_should = 'must' AND status = 'not met'
                       GROUP BY candidate_id HAVING count(*) <= 2)
ORDER BY candidate_id, clause_id;
-- Expected:
-- | candidate_id     | clause_id | page | required                               | offered  | score |
-- | PAST-CUSTOM-2021 | C-01      | 14   | 4 x Dosierzone Joghurt                 | 2 x      | 0.5   |
-- | PAST-NOISY-2020  | C-21      | 27   | Schalldruckpegel: nicht über 75 dB(A)  | 82 dB(A) | 0.0   |
-- | PAST-PN10-2016   | C-27      | 33   | Reinigungstemperatur: mind. 85 °C      | 80 °C    | 0.0   |
-- | PAST-PN10-2016   | C-29      | 33   | Druckstufe: PN 16                      | PN10     | 0.0   |
-- | PAST-SHORT-2017  | C-02      | 14   | Dosierhub: mind 60 mm                  | 50 mm    | 0.0   |
-- | PAST-SHORT-2017  | C-23      | 28   | 2 x Becherentstapler                   | 1 x      | 0.5   |
-- | PAST-TOL-2023    | C-07      | 15   | Antriebsleistung: mind. 90 W           | 87 W     | 0.0   |
-- | PAST-TOL-2023    | C-14      | 22   | Stromaufnahme Heizung: höchstens 7,5 A | 7.6 A    | 0.0   |
-- Offered values are shown in the unit of the clause (80 °C, not 353.15 K).

.print ''
.print '>>> 3. Modules: which variant alone covers a station?'
-- The modules export holds 5 module trees with 10 variants under OR groups. Each
-- variant becomes its own candidate tree, because tree_coverage picks the best
-- variant per requirement item, not per station (see step 4).
CREATE OR REPLACE TABLE module_nodes AS SELECT * FROM read_csv('examples/tender_reuse/data/module_nodes.csv');
CREATE OR REPLACE TABLE module_attributes AS
SELECT * FROM read_csv('examples/tender_reuse/data/module_attributes.csv');
CREATE OR REPLACE TABLE variant_nodes AS
SELECT v.node_id AS tree_id, n.node_id,
       CASE WHEN n.node_id = v.node_id THEN NULL ELSE n.parent_id END AS parent_id, n.node_kind,
       CASE WHEN n.node_id = v.node_id THEN 'AND' ELSE n.edge_type END AS edge_type, n.label, n.class_id, n.count
FROM module_nodes v JOIN module_nodes n ON n.node_id = v.node_id OR n.parent_id = v.node_id
WHERE v.node_kind = 'container';
CREATE OR REPLACE TABLE variant_attributes AS
SELECT v.tree_id, a.* EXCLUDE (tree_id) FROM module_attributes a JOIN variant_nodes v USING (node_id);
-- Cut the request into one tree per station: the station container becomes the root.
CREATE OR REPLACE TABLE station_nodes AS
SELECT s.node_id AS tree_id, n.node_id,
       CASE WHEN n.node_id = s.node_id THEN NULL ELSE n.parent_id END AS parent_id,
       n.node_kind, n.edge_type, n.label, n.class_id, n.count
FROM rfq_nodes s JOIN rfq_nodes n ON n.node_id = s.node_id OR n.parent_id = s.node_id
WHERE s.parent_id = 'RFQ-2026-014';
CREATE OR REPLACE TABLE station_attributes AS
SELECT s.tree_id, a.* EXCLUDE (tree_id) FROM rfq_attributes a JOIN station_nodes s USING (node_id);
CREATE OR REPLACE TABLE gold_station AS SELECT * FROM read_csv('examples/tender_reuse/data/gold_station_cover.csv');
SELECT r.query_id AS station, r.candidate_id AS variant, round(r.coverage, 4) AS coverage, r.n_gaps,
       g.covers AS gold_covers
FROM tree_coverage(['RFQ-2026-014.C', 'RFQ-2026-014.D', 'RFQ-2026-014.F', 'RFQ-2026-014.L', 'RFQ-2026-014.S'],
  k := 10,
  nodes_table := 'station_nodes', attrs_table := 'station_attributes',
  candidate_nodes_table := 'variant_nodes', candidate_attrs_table := 'variant_attributes',
  overrides_table := 'vocabulary_overrides', rules_table := 'compatibility_rules',
  mu := 0.0, default_bounds := 'catalogue') r
JOIN gold_station g ON g.station = r.query_id AND g.variant = r.candidate_id
ORDER BY station, variant;
-- Expected:
-- | station        | variant     | coverage | n_gaps | gold_covers |
-- | RFQ-2026-014.C | MOD-CIP.V1  | 1.0      | 0      | true        |
-- | RFQ-2026-014.D | MOD-DOSE.V1 | 0.8889   | 1      | false       |  (40 mm stroke)
-- | RFQ-2026-014.D | MOD-DOSE.V2 | 0.8889   | 1      | false       |  (50 mm stroke)
-- | RFQ-2026-014.D | MOD-DOSE.V3 | 1.0      | 0      | true        |
-- | RFQ-2026-014.F | MOD-FEED.V1 | 0.9333   | 1      | false       |  (78 dB(A))
-- | RFQ-2026-014.F | MOD-FEED.V2 | 0.5      | 1      | false       |  (500 mm stack)
-- | RFQ-2026-014.L | MOD-LINE.V1 | 0.7917   | 2      | false       |
-- | RFQ-2026-014.L | MOD-LINE.V2 | 0.9167   | 1      | false       |  (only C-42, should)
-- | RFQ-2026-014.S | MOD-SEAL.V1 | 0.7667   | 1      | false       |  (8 A)
-- | RFQ-2026-014.S | MOD-SEAL.V2 | 1.0      | 0      | true        |
-- coverage = 1 agrees with the gold file for all 10 variants. Stations C, D and S can be
-- built from modules; F needs engineering (no single variant covers it).

.print ''
.print '>>> 4. Pitfall: scoring the whole OR tree mixes variants'
-- MOD-FEED.V1 is too loud and MOD-FEED.V2 stacks too low, yet the OR tree "covers"
-- the station: the conveyor comes from V2 and the denester from V1.
SELECT candidate_id, coverage, n_gaps
FROM tree_coverage('RFQ-2026-014.F', candidates := ['MOD-FEED'],
  nodes_table := 'station_nodes', attrs_table := 'station_attributes',
  candidate_nodes_table := 'module_nodes', candidate_attrs_table := 'module_attributes',
  mu := 0.0, default_bounds := 'catalogue');
-- Expected:
-- | candidate_id | coverage | n_gaps |
-- | MOD-FEED     | 1.0      | 0      |
SELECT req_node_id, matched_node_id, attr_id, req_value, prov_value, value_score
FROM tree_coverage_trace('RFQ-2026-014.F', candidates := ['MOD-FEED'],
  nodes_table := 'station_nodes', attrs_table := 'station_attributes',
  candidate_nodes_table := 'module_nodes', candidate_attrs_table := 'module_attributes',
  mu := 0.0, default_bounds := 'catalogue')
ORDER BY req_node_id, attr_id;
-- Expected:
-- | req_node_id     | matched_node_id | attr_id      | req_value        | prov_value | value_score |
-- | RFQ-2026-014.F1 | MOD-FEED.V2.F1  | noise        | at_most 75 dB(A) | 72 dB(A)   | 1.0         |
-- | RFQ-2026-014.F1 | MOD-FEED.V2.F1  | speed        | at_least 0.5 m/s | 0.6 m/s    | 1.0         |
-- | RFQ-2026-014.F1 | MOD-FEED.V2.F1  | width        | at_least 0.3 m   | 0.35 m     | 1.0         |
-- | RFQ-2026-014.F2 | MOD-FEED.V1.F2  | stack_height | at_least 0.6 m   | 0.7 m      | 1.0         |
-- Score variants one by one (step 3) whenever a variant is a whole configuration.
