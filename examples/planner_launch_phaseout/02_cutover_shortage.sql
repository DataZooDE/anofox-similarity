-- VoltEdge: investigate a clean cable cut-over and a launch-component shortage.
.print '>>> 1. Load BOM and issue history'
CREATE OR REPLACE TABLE raw_bom AS SELECT * FROM read_csv('examples/planner_launch_phaseout/data/bom_edges.csv.gz');
CREATE OR REPLACE TABLE raw_movements AS SELECT material_id, movement_date::DATE AS movement_date,
  movement_type::VARCHAR AS movement_type, quantity::DOUBLE AS quantity
FROM read_csv('examples/planner_launch_phaseout/data/movements.csv.gz');
CREATE OR REPLACE TABLE bom_items AS SELECT parent_id, child_id, sum(quantity)::DOUBLE AS quantity
FROM raw_bom GROUP BY 1, 2;
CREATE OR REPLACE VIEW issues_only AS SELECT material_id, movement_date, quantity
FROM raw_movements WHERE movement_type = '261' AND quantity > 0;
CREATE OR REPLACE TABLE gold AS SELECT * FROM read_csv('examples/planner_launch_phaseout/data/gold_planner.csv.gz');

.print '>>> 2. The predecessor model misses a clean cut-over'
SELECT count(*) AS inferred FROM infer_predecessors('CABLE-USB2-1M-V2',
  bom_table := 'bom_items', movements_table := 'issues_only');
-- Expected: 0

.print '>>> 3. Pair BOM overlap with the last issue before launch'
-- BRIDGE (proper fix: a cut-over signal in infer_predecessors).
WITH launch AS (SELECT min(movement_date) AS first_issue FROM issues_only WHERE material_id = 'CABLE-USB2-1M-V2'),
  last_issues AS (SELECT material_id, max(movement_date) AS last_issue FROM issues_only GROUP BY 1),
  candidates AS (SELECT s.material_id, round(s.similarity, 4) AS similarity, h.last_issue,
      date_diff('day', h.last_issue, l.first_issue) AS gap_days
    FROM find_similar_materials_jaccard('CABLE-USB2-1M-V2', 20,
      min_similarity := 0.3, bom_table := 'bom_items') s
    JOIN last_issues h USING (material_id) CROSS JOIN launch l
    WHERE h.last_issue < l.first_issue AND h.last_issue >= l.first_issue - INTERVAL 12 WEEK)
SELECT * FROM candidates ORDER BY similarity DESC, material_id;
-- Expected:
-- | material_id      | similarity | last_issue | gap_days |
-- | CABLE-USB2-1M-V1 | 0.4286     | 2024-06-24 | 7        |

.print '>>> 4. Confirm the predecessor candidate against gold'
WITH launch AS (SELECT min(movement_date) AS first_issue FROM issues_only WHERE material_id = 'CABLE-USB2-1M-V2'),
  candidates AS (SELECT s.material_id FROM find_similar_materials_jaccard('CABLE-USB2-1M-V2', 20,
      min_similarity := 0.3, bom_table := 'bom_items') s CROSS JOIN launch l
    JOIN (SELECT material_id, max(movement_date) AS last_issue FROM issues_only GROUP BY 1) h
    ON s.material_id = h.material_id
    WHERE h.last_issue < l.first_issue AND h.last_issue >= l.first_issue - INTERVAL 12 WEEK)
SELECT count(*) AS gold_hits FROM candidates c JOIN gold g ON c.material_id = g.expected_material_id
WHERE g.case_id = 'cutover';
-- Expected: 1

.print '>>> 5. Installed-base impact of a constrained component'
CREATE OR REPLACE TABLE bom_header AS SELECT DISTINCT parent_id AS bom_id, 'VoltEdge' AS source_system,
  parent_id AS parent_material_id FROM bom_items;
CREATE OR REPLACE TABLE bom_component AS SELECT parent_id AS bom_id, child_id AS child_material_id,
  quantity AS quantity_per FROM bom_items;
SELECT count(*) AS affected_parents FROM bom_where_used(child_material_id := 'VE-COMP-000');
-- Expected: 4
SELECT count(*) AS common_cable_components FROM bom_common_components(
  material_1 := 'CABLE-USB2-1M-V1', material_2 := 'CABLE-USB2-1M-V2');
-- Expected: 9
.print '>>> 5b. Components stranded by phase-out'
-- Installed-base where-used is impact evidence, not supersession approval.
WITH old_only AS (SELECT child_id FROM bom_items WHERE parent_id = 'CABLE-USB2-1M-V1'
  EXCEPT SELECT child_id FROM bom_items WHERE parent_id = 'CABLE-USB2-1M-V2')
SELECT count(*) AS stranded_components FROM old_only;
-- Expected: 6
SELECT count(*) AS installed_in_parents FROM bom_where_used(child_material_id := 'VE-COMP-043');
-- Expected: 1

.print '>>> 6. Quantity-aware shortage cover versus set overlap'
-- Both searches run over the whole BOM base (all 60 parents), not a pre-selected family.
-- Only the three other chargers share any component with BLACK, so they are the only
-- candidates either function ranks above zero. find_similar_materials_jaccard also returns
-- materials with no shared component (similarity 0); min_similarity := 0.01 drops them.
CREATE OR REPLACE TABLE component_features AS SELECT parent_id AS entity_id, child_id AS feature_id,
  quantity AS weight FROM bom_items;
SELECT material_id, round(similarity, 4) AS jaccard, shared_components
FROM find_similar_materials_jaccard('CHARGER-65W-EU-BLACK', 5,
  min_similarity := 0.01, bom_table := 'bom_items')
ORDER BY jaccard DESC, material_id;
-- Expected:
-- | material_id           | jaccard | shared_components |
-- | CHARGER-65W-EU-GREY   | 0.875   | 14                |
-- | CHARGER-65W-EU-RETURN | 0.7647  | 13                |
-- | CHARGER-65W-EU-WHITE  | 0.6667  | 12                |
-- Set Jaccard prefers GREY, but GREY uses 0.5 of each component where BLACK needs 1 or 2.
-- find_covering_entities compares quantities: shared_weight sums min(BLACK qty, candidate qty).
SELECT entity_id, round(containment, 4) AS quantity_containment, shared_weight, query_weight
FROM find_covering_entities('CHARGER-65W-EU-BLACK', 5, feature_table := 'component_features')
ORDER BY containment DESC, entity_id;
-- Expected:
-- | entity_id             | quantity_containment | shared_weight | query_weight |
-- | CHARGER-65W-EU-WHITE  | 0.7778               | 14.0          | 18.0         |
-- | CHARGER-65W-EU-RETURN | 0.7222               | 13.0          | 18.0         |
-- | CHARGER-65W-EU-GREY   | 0.3889               | 7.0           | 18.0         |

.print '>>> 7. Confirm the quantity-first alternative against gold'
WITH ranked AS (SELECT entity_id, row_number() OVER (ORDER BY containment DESC, entity_id) AS rank_no
  FROM find_covering_entities('CHARGER-65W-EU-BLACK', 5, feature_table := 'component_features'))
SELECT count(*) AS gold_hits FROM ranked r JOIN gold g ON r.entity_id = g.expected_material_id
WHERE r.rank_no = 1 AND g.case_id = 'shortage';
-- Expected: 1
