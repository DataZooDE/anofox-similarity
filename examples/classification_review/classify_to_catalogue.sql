-- ============================================================================
-- Pivot classifier: map requirement items onto a catalogue of classes
-- ============================================================================
-- Coverage matching compares items of the same class, so every item needs a
-- class. classify_to_catalogue scores each item against every catalogue class
-- and returns a probability distribution (p), the rank and the margin
-- (p1 - p2). The extension ships no model: bring your own vectors.
--
--   centroid mode      cosine to the class vector, after subtracting the mean
--                      vector of the item's source (removes a maker's sentence
--                      frame; a source with fewer than 10 items is centred
--                      with the batch mean instead)
--   attribute bonus    + β × how well the item's stated values fit the class's
--                      attribute templates (same dimension, similar name)
--   late_interaction   maxsim over token vectors; robust to unseen wording
--
-- Function:
--   classify_to_catalogue(items_table := ..., catalogue_table := ..., mode,
--       center_by_source, attrs_table, templates_table, name_vectors, name_floor, top, ...)
--
-- Data: test/data/coverage (64-dim deterministic vectors, 23 catalogue classes)
--
-- Run: ./build/release/duckdb < examples/classification_review/classify_to_catalogue.sql
-- ============================================================================

.print '>>> 0. Load catalogue, request item vectors and request attributes'
CREATE OR REPLACE TABLE catalogue AS
SELECT class_id, vec::FLOAT[] AS vec, exemplar_tokens::FLOAT[][] AS exemplar_tokens
FROM read_csv('test/data/coverage/catalogue_vectors.csv.gz');

CREATE OR REPLACE TABLE request_items AS
SELECT tree_id, node_id, source, vec::FLOAT[] AS vec, tokens::FLOAT[][] AS tokens
FROM read_csv('test/data/coverage/request_item_vectors.csv.gz');

CREATE OR REPLACE TABLE requests AS SELECT * FROM read_csv('test/data/coverage/requests.csv.gz');
CREATE OR REPLACE TABLE req_attrs AS
SELECT tree_id, node_id, attr_id, name, q.dimension, q.unit, q.lo, q.hi, q.bound, q.category
FROM (SELECT a.*, parse_quantity(a.value_text, r.lang) AS q
      FROM read_csv('test/data/coverage/request_attributes.csv.gz') a
      JOIN requests r ON r.request_id = a.tree_id);

CREATE OR REPLACE TABLE templates AS
SELECT * FROM read_csv('test/data/coverage/catalogue_attributes.csv.gz');
CREATE OR REPLACE TABLE name_vectors AS
SELECT name, vec::FLOAT[] AS vec FROM read_csv('test/data/coverage/attribute_name_vectors.csv.gz');

-- Unrelated attribute names still have a cosine around 0.85: calibrate.
SET VARIABLE name_floor = (SELECT floor FROM cosine_floor('name_vectors', 'vec', sample := 20000));

-- Ground truth, only to measure accuracy.
CREATE OR REPLACE TABLE gold_item AS
SELECT * FROM read_csv('test/data/coverage/gold_item_class.csv.gz');

.print ''
.print '>>> 1. Two items, top 3 classes each'
-- R03.r01 asks for "eine Armlehne mit Schwenkwinkel 20 °": a clear case.
-- R02.r05 is a doubtful one: two classes share the probability mass.
SELECT node_id, class_id, round(p, 3) AS p, rank, round(margin, 3) AS margin
FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                           attrs_table := 'req_attrs', templates_table := 'templates',
                           name_vectors := 'name_vectors', name_floor := getvariable('name_floor'),
                           top := 3)
WHERE node_id IN ('R03.r01', 'R02.r05')
ORDER BY node_id DESC, rank;
-- Expected:
--   R03.r01 | armrest          | 0.999 | 1 | 0.999
--   R03.r01 | headrest_powered | 0.0   | 2 | 0.999
--   R03.r01 | headrest_manual  | 0.0   | 3 | 0.999
--   R02.r05 | recliner         | 0.545 | 1 | 0.126   (low margin: worth a review)
--   R02.r05 | massage          | 0.419 | 2 | 0.126
--   R02.r05 | side_bolster     | 0.016 | 3 | 0.126

.print ''
.print '>>> 2. Top-1 accuracy on 1 520 request items: raw, centred, centred + bonus'
CREATE OR REPLACE TABLE raw AS
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                                    center_by_source := false, top := 1);
CREATE OR REPLACE TABLE centred AS
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                                    top := 1);
CREATE OR REPLACE TABLE with_bonus AS
SELECT * FROM classify_to_catalogue(items_table := 'request_items', catalogue_table := 'catalogue',
                                    attrs_table := 'req_attrs', templates_table := 'templates',
                                    name_vectors := 'name_vectors', name_floor := getvariable('name_floor'),
                                    top := 1);

SELECT setting, count(*) AS items, round(avg((d.class_id = g.class_id)::INT), 3) AS top1_accuracy
FROM (SELECT 'raw cosine' AS setting, * FROM raw
      UNION ALL BY NAME SELECT 'centred by source' AS setting, * FROM centred
      UNION ALL BY NAME SELECT 'centred + attribute bonus' AS setting, * FROM with_bonus) d
JOIN gold_item g USING (tree_id, node_id)
GROUP BY setting ORDER BY top1_accuracy;
-- Expected:
--   raw cosine                | 1520 | 0.593   fooled by each maker's sentence frame
--   centred by source         | 1520 | 0.853   frame removed
--   centred + attribute bonus | 1520 | 0.953   stated values add evidence

.print ''
.print '>>> 3. Unseen wording: centroid vs late interaction on 200 held-out phrasings'
CREATE OR REPLACE TABLE unseen AS
SELECT item_id AS node_id, source, vec::FLOAT[] AS vec, tokens::FLOAT[][] AS tokens
FROM read_csv('test/data/coverage/unseen_vectors.csv.gz');
CREATE OR REPLACE TABLE gold_unseen AS
SELECT item_id AS node_id, class_id FROM read_csv('test/data/coverage/gold_unseen.csv.gz');

SELECT mode, round(avg((d.class_id = g.class_id)::INT), 3) AS top1_accuracy
FROM (SELECT 'centroid' AS mode, * FROM classify_to_catalogue(
          items_table := 'unseen', catalogue_table := 'catalogue', top := 1)
      UNION ALL BY NAME
      SELECT 'late_interaction' AS mode, * FROM classify_to_catalogue(
          items_table := 'unseen', catalogue_table := 'catalogue', mode := 'late_interaction', top := 1)) d
JOIN gold_unseen g USING (node_id)
GROUP BY mode ORDER BY top1_accuracy;
-- Expected: centroid 0.75, late_interaction 0.94
