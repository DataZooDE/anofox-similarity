-- ============================================================================
-- AAS capability matching, step 4: a supplier without semanticIds
-- ============================================================================
-- Nordwerk sends an AAS with German names only, no dictionary ids. The buyer's
-- data steward proposes ids from the company's example phrasings (its own
-- embeddings; no ECLASS content ships with the extension), confirms them once
-- in the vocabulary review, and from then on Nordwerk is matched by key.
-- Run: DATAZOO_DISABLE_TELEMETRY=1 ./build/release/duckdb < examples/aas_capability_matching/04_enrich_uncoded_supplier.sql
-- ============================================================================

.print '>>> 1. The quality report: nothing is coded'
SELECT q.submodel, q.n_values, q.n_with_semantic_id, q.semantic_id_share
FROM (SELECT unnest(aas_quality(content)) AS q FROM read_text('examples/aas_capability_matching/data/enrichment/nordwerk_nd5.aas.json'));
-- Expected:
-- | submodel      | n_values | n_with_semantic_id | semantic_id_share |
-- | TechnicalData | 10       | 0                  | 0.0               |

CREATE OR REPLACE TABLE uncoded_names AS
SELECT node_id, tree_id, label, vec::DOUBLE[] AS vec, tokens::DOUBLE[][] AS tokens
FROM read_csv('examples/aas_capability_matching/data/enrichment/uncoded_names.csv');
CREATE OR REPLACE TABLE dictionary_examples AS
SELECT class_id, label, vec::DOUBLE[] AS vec, exemplar_tokens::DOUBLE[][] AS exemplar_tokens
FROM read_csv('examples/aas_capability_matching/data/enrichment/dictionary_examples.csv');

.print ''
.print '>>> 2. Suggestions as a review queue; what nothing fits comes first'
CREATE OR REPLACE TABLE suggestions AS
SELECT * FROM suggest_semantic_id('uncoded_names', 'dictionary_examples', min_similarity := 0.35);
SELECT s.wording, s.top1, d.label AS suggested, round(s.similarity, 2) AS similarity, s.fits
FROM suggestions s LEFT JOIN dictionary_examples d ON d.class_id = s.top1 ORDER BY s.impact DESC, s.wording;
-- Expected:
-- | wording                  | top1                 | suggested                | similarity | fits  |
-- | Gewicht                  | 0173-1#02-AAZ903#001 | safe torque off (STO)    | 0.22       | false |
-- | Spitzenstrom (3 s)       | 0173-1#02-BAA121#008 | peak output current      | 0.44       | true  |
-- The class probability alone called "Gewicht" (weight) certain; the raw
-- similarity says nothing in the dictionary fits. It stays uncoded.

.print ''
.print '>>> 3. Confirm what fits, attach the ids, query Nordwerk by key'
CREATE OR REPLACE TABLE review AS
SELECT wording, top1 AS class_id, 'data steward' AS reviewed_by FROM suggestions WHERE fits;
CREATE OR REPLACE TABLE attribute_keys AS SELECT * FROM apply_vocabulary_review('review', overrides_table := NULL);
CREATE OR REPLACE TABLE tree_attributes AS
SELECT unnest(n.attributes, recursive := true)
FROM (SELECT unnest(aas_to_trees(content)) AS n FROM read_text('examples/aas_capability_matching/data/enrichment/nordwerk_nd5.aas.json'));
UPDATE tree_attributes SET attr_key = k.class_id FROM attribute_keys k
WHERE tree_attributes.name = k.wording AND tree_attributes.attr_key IS NULL;
SELECT name, value, satisfied
FROM attrs_satisfying('0173-1#02-BAA120#008', parse_quantity('mindestens 7637,1 mA', 'de')) ORDER BY name;
-- Expected:
-- | name              | value | satisfied |
-- | Ausgangsnennstrom | 9 A   | true      |
-- | Nennstrom         | 9 A   | true      |
