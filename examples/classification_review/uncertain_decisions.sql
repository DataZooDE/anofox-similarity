-- ============================================================================
-- External review: send the doubtful items out, read the answers back
-- ============================================================================
-- The classifier is least reliable where its margin (p1 - p2) is small.
-- uncertain_decisions selects those items and writes one JSON request per
-- item (text + candidate classes with definitions), ready for an LLM or a
-- colleague. The answers go back into classify_to_catalogue as answers_table:
-- answered items get a one-hot distribution and overridden = true.
--
-- Functions:
--   uncertain_decisions(decisions := ..., share | threshold, top, texts_table, catalogue_table)
--   classify_to_catalogue(..., answers_table := ...)
--
-- Data: 200 unseen requirement phrasings (test/data/coverage/unseen_*.csv.gz);
--       gold_unseen plays the reviewer.
--
-- Run: ./build/release/duckdb < examples/classification_review/uncertain_decisions.sql
-- ============================================================================

CREATE OR REPLACE TABLE catalogue AS
SELECT class_id, vec::FLOAT[] AS vec, exemplar_tokens::FLOAT[][] AS exemplar_tokens
FROM read_csv('test/data/coverage/catalogue_vectors.csv.gz');
CREATE OR REPLACE TABLE classes AS
SELECT * FROM read_csv('test/data/coverage/catalogue_classes.csv.gz');

CREATE OR REPLACE TABLE unseen AS
SELECT item_id AS node_id, source, vec::FLOAT[] AS vec, tokens::FLOAT[][] AS tokens
FROM read_csv('test/data/coverage/unseen_vectors.csv.gz');
-- The texts the reviewer sees: (node_id, text), optionally with tree_id.
CREATE OR REPLACE TABLE unseen_texts AS
SELECT item_id AS node_id, text FROM read_csv('test/data/coverage/unseen_requirements.csv.gz');
-- Ground truth: here it stands in for the reviewer.
CREATE OR REPLACE TABLE gold_unseen AS
SELECT item_id AS node_id, class_id FROM read_csv('test/data/coverage/gold_unseen.csv.gz');

.print '>>> 1. Classify, then export the 15 % with the lowest margin'
CREATE OR REPLACE TABLE decided AS
SELECT * FROM classify_to_catalogue(items_table := 'unseen', catalogue_table := 'catalogue',
                                    mode := 'late_interaction');

CREATE OR REPLACE TABLE export AS
SELECT * FROM uncertain_decisions(decisions := 'decided', share := 0.15, top := 3,
                                  texts_table := 'unseen_texts', catalogue_table := 'classes');

SELECT count(*) AS requests, round(max(margin), 3) AS highest_margin_sent FROM export;
-- Expected: 30 | 0.878   (ceil(0.15 × 200) items, margins up to 0.878)

.print ''
.print '>>> 2. One request as the reviewer receives it'
.mode line
SELECT request FROM export ORDER BY margin, node_id LIMIT 1;
.mode duckbox
-- Expected (one line of JSON text):
--   {"tree_id":null,"node_id":"U194","text":"It is required that the seat has driver
--    profile recall.","margin":0.010096,"candidates":[{"class_id":"trim_cover",
--    "p":0.319385,"label":"trim cover","definition":"Textile or leather cover of
--    cushion and backrest."}, {"class_id":"foam_pad", ...}, {"class_id":"massage", ...}]}
-- "driver profile recall" is a memory function: none of the candidates is
-- right, which is exactly why a human should look at it.

.print ''
.print '>>> 3. The exported items are the doubtful ones'
SELECT round(avg((d.class_id <> g.class_id)::INT), 3) AS error_rate_all,
       round(avg((d.class_id <> g.class_id)::INT)
             FILTER (WHERE d.node_id IN (SELECT node_id FROM export)), 3) AS error_rate_exported
FROM decided d JOIN gold_unseen g USING (node_id)
WHERE d.rank = 1;
-- Expected: 0.06 | 0.367   (6 x the overall error rate)

.print ''
.print '>>> 4. Write the answers back and classify again'
-- answers_table: ([tree_id,] node_id, class_id). Answers beat everything else.
CREATE OR REPLACE TABLE answers AS
SELECT NULL::VARCHAR AS tree_id, e.node_id, g.class_id
FROM export e JOIN gold_unseen g USING (node_id);

CREATE OR REPLACE TABLE redecided AS
SELECT * FROM classify_to_catalogue(items_table := 'unseen', catalogue_table := 'catalogue',
                                    mode := 'late_interaction', answers_table := 'answers');

SELECT 'before review' AS stage, round(avg((d.class_id = g.class_id)::INT), 3) AS top1_accuracy,
       count(*) FILTER (WHERE d.overridden) AS overridden
FROM decided d JOIN gold_unseen g USING (node_id) WHERE d.rank = 1
UNION ALL
SELECT 'after review', round(avg((r.class_id = g.class_id)::INT), 3),
       count(*) FILTER (WHERE r.overridden)
FROM redecided r JOIN gold_unseen g USING (node_id) WHERE r.rank = 1;
-- Expected:
--   before review | 0.94  | 0
--   after review  | 0.995 | 30   (the 30 answered items are now right)
