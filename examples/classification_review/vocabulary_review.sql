-- ============================================================================
-- Vocabulary review: fix most of the archive with a handful of answers
-- ============================================================================
-- The programme archive uses a closed internal vocabulary of 50 wordings.
-- Four of them mislead ("Klimapaket Heizung" is a VENTILATION module), and
-- the classifier follows their surface form. vocabulary_review_queue ranks
-- the wordings by impact = trees affected × (1 - margin), so a reviewer sees
-- the wordings that touch the most programmes and are least certain first.
-- apply_vocabulary_review merges the answers into vocabulary_overrides, which
-- tree_coverage and classify_to_catalogue then use.
--
-- Functions:
--   classify_to_catalogue(items_table := ..., catalogue_table := ...)
--   vocabulary_review_queue(trees_table := ..., decisions := ... [, overrides_table])
--   apply_vocabulary_review(review_table [, overrides_table])
--
-- Data: test/data/coverage (vocabulary_vectors, programme_nodes, catalogue_vectors)
--
-- Run: ./build/release/duckdb < examples/classification_review/vocabulary_review.sql
-- ============================================================================

CREATE OR REPLACE TABLE catalogue AS
SELECT class_id, vec::FLOAT[] AS vec, exemplar_tokens::FLOAT[][] AS exemplar_tokens
FROM read_csv('test/data/coverage/catalogue_vectors.csv.gz');
CREATE OR REPLACE TABLE prog_nodes AS
SELECT * FROM read_csv('test/data/coverage/programme_nodes.csv.gz');

-- One vector per wording; node_id and label are the wording itself.
CREATE OR REPLACE TABLE wordings AS
SELECT wording AS node_id, wording AS label, source, vec::FLOAT[] AS vec
FROM read_csv('test/data/coverage/vocabulary_vectors.csv.gz');

.print '>>> 1. Classify every wording once'
CREATE OR REPLACE TABLE wording_decisions AS
SELECT * FROM classify_to_catalogue(items_table := 'wordings', catalogue_table := 'catalogue');

SELECT node_id AS wording, class_id, round(p, 3) AS p, round(margin, 3) AS margin
FROM wording_decisions
WHERE rank = 1 AND node_id IN ('Klimapaket Heizung', 'Heizmatte')
ORDER BY wording;
-- Expected:
--   Heizmatte          | seat_heating | 1.0   | 1.0     right
--   Klimapaket Heizung | seat_heating | 0.962 | 0.925   wrong: a ventilation module whose
--                                                        wording points at heating

.print ''
.print '>>> 2. The review queue: highest impact first'
SELECT wording, top1, top2, round(margin, 3) AS margin, n_trees_affected,
       round(impact, 3) AS impact
FROM vocabulary_review_queue(trees_table := 'prog_nodes', decisions := 'wording_decisions')
LIMIT 6;
-- The queue comes sorted by impact, highest first.
-- Expected:
--   Klimapaket Heizung | seat_heating     | seat_ventilation | 0.925 | 23  | 1.723
--   KS-Antrieb         | headrest_powered | seat_motor       | 0.987 | 55  | 0.691
--   LV-Struktur        | seat_track       | seat_frame       | 0.996 | 187 | 0.681
--   Massagelordose     | lumbar_support   | massage          | 0.996 | 38  | 0.148
--   Lüftermodul        | seat_ventilation | trim_cover       | 0.999 | 68  | 0.059
--   Höhenversteller    | height_adjuster  | recliner         | 1.0   | 126 | 0.044
-- The four misleading wordings come first. The classifier is confident but
-- not quite (margin < 1), and each one's runner-up (top2) is its true class,
-- so the reviewer mostly confirms the second choice.

.print ''
.print '>>> 3. The reviewer answers the top 4; merge the answers'
CREATE OR REPLACE TABLE review AS
SELECT * FROM (VALUES
    ('Klimapaket Heizung', 'internal', 'seat_ventilation', 'jr'),
    ('LV-Struktur',        'internal', 'seat_frame',       'jr'),
    ('Massagelordose',     'internal', 'massage',          'jr'),
    ('KS-Antrieb',         'internal', 'seat_motor',       'jr')
) t(wording, source, class_id, reviewed_by);

-- Table functions cannot write: store the merged overrides yourself.
-- overrides_table := NULL starts a fresh vocabulary.
CREATE OR REPLACE TABLE vocabulary_overrides AS
SELECT * FROM apply_vocabulary_review('review', overrides_table := NULL);

SELECT wording, source, class_id, reviewed_by FROM vocabulary_overrides ORDER BY wording;
-- Expected: the 4 answered wordings

.print ''
.print '>>> 4. The queue after the review'
-- Reviewed wordings leave the queue; what is left has lower impact.
SELECT count(*) AS still_open, round(max(impact), 3) AS max_impact
FROM vocabulary_review_queue(trees_table := 'prog_nodes', decisions := 'wording_decisions',
                             overrides_table := 'vocabulary_overrides');
-- Expected: 46 | 0.059  (Lüftermodul now leads, a correct reading)

.print ''
.print '>>> 5. How many programmes the four answers fixed'
SELECT count(DISTINCT n.tree_id) AS programmes_fixed,
       (SELECT count(DISTINCT tree_id) FROM prog_nodes) AS programmes
FROM prog_nodes n JOIN review r ON r.wording = n.label AND r.source = n.source;
-- Expected: 225 | 300  (75 % of the archive fixed by four answers)
