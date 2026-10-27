-- ============================================================================
-- Loader shared by 01..03: raw files -> normalised values -> property trees
-- ============================================================================
-- Read with `.read examples/second_source/sql/load.sql` from the repository root.
-- It prints nothing. The steps:
--   1. raw tables as delivered (drawing extract, capability sheets, vendor master)
--   2. vendor master dedup: one master_id per foundry
--   3. every text row gets a capability class from attribute_vocabulary
--   4. values: numbers through parse_quantity, codes through SQL normalisers
--      (grades, PN, EN 10204 certificates), envelopes split and sorted
--   5. trees: one item per capability class, so classes (not attribute names)
--      decide which values are compared
-- ============================================================================

-- 1. Raw data ---------------------------------------------------------------
CREATE OR REPLACE TABLE parts AS SELECT * FROM read_csv('examples/second_source/data/parts.csv');
CREATE OR REPLACE TABLE part_requirements AS
SELECT * FROM read_csv('examples/second_source/data/part_requirements.csv');
CREATE OR REPLACE TABLE suppliers AS SELECT * FROM read_csv('examples/second_source/data/suppliers.csv');
CREATE OR REPLACE TABLE supplier_capabilities AS
SELECT * FROM read_csv('examples/second_source/data/supplier_capabilities.csv');
-- class_id, kind (numeric | category), dimension, bound (direction of a requirement), ladder
CREATE OR REPLACE TABLE capability_classes AS
SELECT * FROM read_csv('examples/second_source/data/capability_classes.csv');
CREATE OR REPLACE TABLE attribute_vocabulary AS
SELECT * FROM read_csv('examples/second_source/data/attribute_vocabulary.csv');
-- the ordinal ladders written out (9 rows); grades are nominal and have no rules
CREATE OR REPLACE TABLE compatibility_rules AS
SELECT * FROM read_csv('examples/second_source/data/compatibility_rules.csv');

-- 2. Vendor master dedup ----------------------------------------------------
-- "Gießerei Brandt GmbH" and "Giesserei Brandt" are one foundry with two vendor records.
CREATE OR REPLACE MACRO vendor_key(name) AS
    regexp_replace(regexp_replace(replace(lower(name), 'ß', 'ss'),
                   '\b(gmbh|ag|kft|sas|s\.r\.l|s\.r\.o|sp\. z o\.o|uab|a\.ş)\.?', '', 'g'), '[^a-z]', '', 'g');
CREATE OR REPLACE TABLE supplier_master AS
SELECT supplier_id, status, min(supplier_id) OVER (PARTITION BY vendor_key(name)) AS master_id
FROM suppliers;

-- 3./4. Normalisers for codes (never sent through parse_quantity) -----------
-- Grades: EN-GJS-400-15 / GJS-400-15 / GGG-40 (old DIN) -> GJS-400; the elongation suffix is
-- dropped, so EN-GJS-400-18 folds into GJS-400. A supplier range "GJS-400 … GJS-700" or
-- "EN-GJS-400 bis EN-GJS-700" becomes one value per standard grade in the range.
CREATE OR REPLACE MACRO grade_tokens(seg) AS
    CASE WHEN regexp_matches(upper(seg), 'GGG-?\d{2}\b')
         THEN ['GJS-' || (regexp_extract(upper(seg), 'GGG-?(\d{2})', 1)::INT * 10)]
         WHEN regexp_matches(upper(seg), '\bGG-?\d{2}\b')
         THEN ['GJL-' || (regexp_extract(upper(seg), '\bGG-?(\d{2})', 1)::INT * 10)]
         ELSE list_transform(regexp_extract_all(upper(seg), '(GJS|GJL)[- ]?\d{3}'),
                             lambda x: regexp_replace(x, '(GJS|GJL)[- ]?(\d{3})', '\1-\2')) END;
CREATE OR REPLACE MACRO grade_values(seg) AS
    CASE WHEN regexp_matches(seg, '…|\.\.|\bbis\b|\bto\b') AND len(grade_tokens(seg)) = 2
         THEN list_transform(
                list_filter(CASE WHEN grade_tokens(seg)[1] LIKE 'GJS%' THEN [350, 400, 450, 500, 600, 700, 800]
                                 ELSE [150, 200, 250, 300, 350] END,
                            lambda n: n BETWEEN split_part(grade_tokens(seg)[1], '-', 2)::INT
                                       AND split_part(grade_tokens(seg)[2], '-', 2)::INT),
                lambda n: split_part(grade_tokens(seg)[1], '-', 1) || '-' || n)
         ELSE grade_tokens(seg) END;
-- EN 10204 certificates: "Werkszeugnis" is 2.2; "2.2 / 3.1" lists several levels
CREATE OR REPLACE MACRO cert_values(t) AS
    list_transform(CASE WHEN regexp_matches(t, '[23]\.[12]') THEN regexp_extract_all(t, '[23]\.[12]')
                        WHEN t ILIKE '%werkszeugnis%' THEN ['2.2'] ELSE [] END, lambda x: 'EN10204-' || x);
CREATE OR REPLACE MACRO pn_values(t) AS
    CASE WHEN regexp_matches(upper(t), 'PN ?\d+')
         THEN ['PN' || regexp_extract(upper(t), 'PN ?(\d+)', 1)] ELSE [] END;

-- every text row with its class; side, owner and row id in one shape
CREATE OR REPLACE TABLE spec_texts AS
SELECT 'part' AS side, r.part_id AS owner_id, r.req_id AS row_id, r.attr_name AS name, r.value_text, r.lang,
       v.class_id
FROM part_requirements r LEFT JOIN attribute_vocabulary v ON v.wording = r.attr_name AND v.side = 'part'
UNION ALL
SELECT 'supplier', c.supplier_id, c.supplier_id || '-' || row_number() OVER (PARTITION BY c.supplier_id),
       c.capability, c.value_text, c.lang, v.class_id
FROM supplier_capabilities c LEFT JOIN attribute_vocabulary v ON v.wording = c.capability AND v.side = 'supplier';

-- one row per value. Numbers: parse_quantity in the row's language. The direction of a
-- requirement comes from its class, not from the drawing's wording: "max. Stückgewicht 38 kg"
-- says the part weighs at most 38 kg, so the foundry must pour AT LEAST 38 kg.
CREATE OR REPLACE TABLE spec_values AS
WITH numeric_rows AS (
    SELECT t.*, parse_quantity(t.value_text, t.lang) AS q
    FROM spec_texts t WHERE t.class_id IN ('piece_weight', 'lot_size', 'roughness', 'tolerance')
), envelope_rows AS (
    -- "max. 420 x 310 x 180 mm" in any orientation: sorted, so envelope_1 is the longest side
    -- on both sides (a part fits when every sorted side fits; rotations allowed)
    SELECT t.*, t.sides[k] AS mm, k
    FROM (SELECT *, list_reverse_sort(list_transform(regexp_extract_all(replace(value_text, '.', ''), '\d+'),
                                                     lambda x: x::DOUBLE)) AS sides,
                 regexp_extract(value_text, '(mm|cm|m)\s*$', 1) AS unit
          FROM spec_texts WHERE class_id = 'envelope') t,
         LATERAL (SELECT unnest(range(1, len(t.sides) + 1)) AS k)
), category_rows AS (
    SELECT t.*, unnest(CASE t.class_id WHEN 'material_grade' THEN grade_values(seg)
                                       WHEN 'cert_level' THEN cert_values(seg)
                                       ELSE pn_values(seg) END) AS category
    FROM (SELECT *, unnest(string_split(value_text, ';')) AS seg FROM spec_texts
          WHERE class_id IN ('material_grade', 'cert_level', 'pressure_class')) t
)
SELECT side, owner_id, row_id, row_id AS attr_id, name, value_text, class_id,
       q.dimension, q.unit, q.lo, q.hi, q.bound AS stated_bound, NULL::VARCHAR AS category
FROM numeric_rows
UNION ALL
SELECT side, owner_id, row_id, row_id || '.' || k, name, value_text, 'envelope_' || k,
       normalize_quantity(mm, unit).dimension, normalize_quantity(mm, unit).unit,
       normalize_quantity(mm, unit).value, normalize_quantity(mm, unit).value, NULL, NULL
FROM envelope_rows
UNION ALL
SELECT side, owner_id, row_id, row_id || '.' || row_number() OVER (PARTITION BY row_id ORDER BY category),
       name, value_text, class_id, NULL, NULL, NULL, NULL, NULL, category
FROM category_rows;

-- 5. Property trees: a root per part / supplier, one item per capability class ----
-- Attribute dimension and requirement bound come from capability_classes.
CREATE OR REPLACE TABLE part_nodes AS
SELECT part_id AS tree_id, part_id AS node_id, NULL::VARCHAR AS parent_id, 'container' AS node_kind,
       'AND' AS edge_type, description AS label, NULL::VARCHAR AS class_id, 1.0 AS count
FROM parts
UNION ALL
SELECT DISTINCT owner_id, owner_id || ':' || class_id, owner_id, 'item', 'AND', class_id, class_id, 1.0
FROM spec_values WHERE side = 'part';

CREATE OR REPLACE TABLE part_attributes AS
SELECT v.owner_id AS tree_id, v.owner_id || ':' || v.class_id AS node_id, v.attr_id, v.name,
       CASE WHEN c.kind = 'category' THEN c.dimension ELSE v.dimension END AS dimension,
       v.unit, v.lo, v.hi, CASE WHEN c.kind = 'numeric' THEN c.bound END AS bound, v.category
FROM spec_values v JOIN capability_classes c USING (class_id)
WHERE v.side = 'part';

CREATE OR REPLACE TABLE supplier_nodes AS
SELECT supplier_id AS tree_id, supplier_id AS node_id, NULL::VARCHAR AS parent_id, 'container' AS node_kind,
       'AND' AS edge_type, name AS label, NULL::VARCHAR AS class_id, 1.0 AS count
FROM suppliers WHERE supplier_id IN (SELECT DISTINCT owner_id FROM spec_values WHERE side = 'supplier')
UNION ALL
SELECT DISTINCT owner_id, owner_id || ':' || class_id, owner_id, 'item', 'AND', class_id, class_id, 1.0
FROM spec_values WHERE side = 'supplier';

CREATE OR REPLACE TABLE supplier_attributes AS
SELECT v.owner_id AS tree_id, v.owner_id || ':' || v.class_id AS node_id, v.attr_id, v.name,
       CASE WHEN c.kind = 'category' THEN c.dimension ELSE v.dimension END AS dimension,
       v.unit, v.lo, v.hi, NULL::VARCHAR AS bound, v.category
FROM spec_values v JOIN capability_classes c USING (class_id)
WHERE v.side = 'supplier';

-- the candidates a buyer may send an RFQ to: approved, one record per foundry
SET VARIABLE part_ids = (SELECT list(part_id ORDER BY part_id) FROM parts);
SET VARIABLE approved = (SELECT list(supplier_id ORDER BY supplier_id) FROM supplier_master
                         WHERE status = 'approved' AND supplier_id = master_id);
