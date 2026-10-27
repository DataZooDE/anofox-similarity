-- VoltEdge planner: do not borrow demand from a two-posting lookalike or returns.
.print '>>> 1. Load and audit the launch data'
CREATE OR REPLACE TABLE raw_bom AS SELECT * FROM read_csv('examples/planner_launch_phaseout/data/bom_edges.csv.gz');
CREATE OR REPLACE TABLE raw_movements AS SELECT material_id, movement_date::DATE AS movement_date,
  movement_type::VARCHAR AS movement_type, quantity::DOUBLE AS quantity
FROM read_csv('examples/planner_launch_phaseout/data/movements.csv.gz');
CREATE OR REPLACE TABLE bom_items AS SELECT parent_id, child_id, sum(quantity)::DOUBLE AS quantity
FROM raw_bom GROUP BY 1, 2;
SELECT (SELECT count(DISTINCT parent_id) FROM raw_bom) AS parents,
       (SELECT count(*) FROM raw_bom) AS raw_edges,
       (SELECT count(*) FROM bom_items) AS unique_edges,
       (SELECT count(*) FROM raw_movements) AS postings;
-- Expected:
-- | parents | raw_edges | unique_edges | postings |
-- | 60      | 936       | 900          | 8539     |

.print '>>> 2. Inspect movement types and usable history'
SELECT movement_type, count(*) AS postings FROM raw_movements GROUP BY 1 ORDER BY 1;
-- Expected:
-- | movement_type | postings |
-- | 101           | 525      |  (receipts)
-- | 261           | 7874     |  (issues)
-- | 262           | 140      |  (returns)
-- BRIDGE (proper fix: an active_weeks signal in cold_start_analogs).
CREATE OR REPLACE TABLE issue_density AS
SELECT material_id, count(DISTINCT date_trunc('week', movement_date)) AS active_weeks,
  min(movement_date) AS first_issue, max(movement_date) AS last_issue
FROM raw_movements WHERE movement_type = '261' AND quantity > 0 GROUP BY 1;
SELECT material_id, active_weeks FROM issue_density
WHERE material_id LIKE 'CHARGER-65W-EU-%' ORDER BY material_id;
-- Expected:
-- | material_id          | active_weeks |
-- | CHARGER-65W-EU-GREY  | 2            |
-- | CHARGER-65W-EU-WHITE | 156          |

.print '>>> 3. Raw analogs include sparse and returns-only candidates'
SELECT material_id, round(similarity, 4) AS similarity, history_months
FROM cold_start_analogs('CHARGER-65W-EU-BLACK', 5, min_similarity := 0.2,
  bom_table := 'bom_items', movements_table := 'raw_movements')
ORDER BY similarity DESC, material_id;
-- Expected:
-- | material_id           | similarity | history_months |
-- | CHARGER-65W-EU-GREY   | 0.875      | 34             |
-- | CHARGER-65W-EU-RETURN | 0.7647     | 27             |
-- | CHARGER-65W-EU-WHITE  | 0.6667     | 35             |

.print '>>> 4. Filter to dense issues before borrowing history'
-- BRIDGE (proper fix: a movement-type and density filter in the API).
CREATE OR REPLACE VIEW dense_issues AS
SELECT m.material_id, m.movement_date, m.quantity FROM raw_movements m
JOIN issue_density d USING (material_id)
WHERE m.movement_type = '261' AND m.quantity > 0 AND d.active_weeks >= 20;
SELECT material_id, round(similarity, 4) AS similarity, history_months
FROM cold_start_analogs('CHARGER-65W-EU-BLACK', 5, min_history_months := 12,
  min_similarity := 0.2, bom_table := 'bom_items', movements_table := 'dense_issues')
ORDER BY similarity DESC, material_id;
-- Expected:
-- | material_id          | similarity | history_months |
-- | CHARGER-65W-EU-WHITE | 0.6667     | 35             |

.print '>>> 5. Check the planted analog against independent gold'
CREATE OR REPLACE TABLE gold AS SELECT * FROM read_csv('examples/planner_launch_phaseout/data/gold_planner.csv.gz');
WITH result AS (SELECT material_id FROM cold_start_analogs('CHARGER-65W-EU-BLACK', 1,
  min_history_months := 12, min_similarity := 0.2, bom_table := 'bom_items', movements_table := 'dense_issues'))
SELECT count(*) AS gold_hits FROM result JOIN gold ON result.material_id = gold.expected_material_id
WHERE gold.case_id = 'cold_start';
-- Expected: 1
