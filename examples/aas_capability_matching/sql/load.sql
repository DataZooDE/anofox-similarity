-- Shared loader of the AAS scenario: the RFQ and the four offers as property trees.
-- The licensed unit list (here a stand-in, data/eclass_units.csv) resolves ECLASS unit IRDIs.
CREATE OR REPLACE TABLE eclass_units AS SELECT * FROM read_csv('examples/aas_capability_matching/data/eclass_units.csv');
CREATE OR REPLACE TABLE raw AS
SELECT 'rfq_2026_117.aas.json' AS file, unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n
FROM read_text('examples/aas_capability_matching/data/rfq_2026_117.aas.json')
UNION ALL
SELECT split_part(filename, '/', -1), unnest(aas_to_trees(content, NULL, (SELECT list(u) FROM eclass_units u)))
FROM read_blob('examples/aas_capability_matching/data/offers/*');
CREATE OR REPLACE TABLE tree_nodes AS SELECT file, n.* EXCLUDE (attributes) FROM raw;
CREATE OR REPLACE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;
CREATE OR REPLACE TABLE offers AS SELECT * FROM (VALUES
    ('https://lenvaro.example/aas/lx-8', 'Lenvaro LX-8'), ('https://driventa.example/aas/ds-12', 'Driventa DS-12'),
    ('https://kessler-antriebe.example/aas/ksd-10', 'Kessler KSD-10'), ('https://orbitec.example/aas/ox-6', 'Orbitec OX-6')
) t(candidate_id, offer);
SET VARIABLE offers = (SELECT list(candidate_id ORDER BY candidate_id) FROM offers);
