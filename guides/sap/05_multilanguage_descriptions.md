# Guide 05: Multi-Language Material Descriptions

## The Problem

> "Our SAP system has material descriptions in multiple languages. How do we work with them for similarity analysis and reporting?"

SAP stores material descriptions in the **MAKT table** with language codes (E=English, D=German, F=French, etc.). This creates challenges:
- Finding materials when you only know the description in one language
- Analyzing description coverage across languages
- Building multi-lingual search capabilities

## The Solution

Use the **language parameter** in transformation functions and query MAKT directly for language analysis.

## Prerequisites

```sql
LOAD anofox_similarity;

-- Load SAP tables
CREATE TABLE sap_mara AS SELECT * FROM read_parquet('test/data/sap/mara.parquet');
CREATE TABLE sap_makt AS SELECT * FROM read_parquet('test/data/sap/makt.parquet');

-- Check data loaded
SELECT 'mara' AS tbl, COUNT(*) AS rows FROM sap_mara
UNION ALL SELECT 'makt', COUNT(*) FROM sap_makt;
```

## Step-by-Step Walkthrough

### Step 1: Understand the Language Distribution

See which languages are available in your MAKT data:

```sql
-- Language distribution
SELECT
    spras AS language_code,
    CASE spras
        WHEN 'E' THEN 'English'
        WHEN 'D' THEN 'German'
        WHEN 'F' THEN 'French'
        WHEN 'S' THEN 'Spanish'
        WHEN 'I' THEN 'Italian'
        WHEN 'J' THEN 'Japanese'
        WHEN 'K' THEN 'Korean'
        WHEN 'Z' THEN 'Chinese'
        ELSE 'Other'
    END AS language_name,
    COUNT(*) AS description_count,
    COUNT(DISTINCT matnr) AS unique_materials
FROM sap_makt
GROUP BY spras
ORDER BY description_count DESC;
```

**Expected Results (example):**

| language_code | language_name | description_count | unique_materials |
|---------------|---------------|-------------------|------------------|
| E | English | 1,023,632 | 1,023,632 |
| D | German | 987,450 | 987,450 |
| F | French | 756,231 | 756,231 |

### Step 2: Extract Materials with English Descriptions

Use the transformation function with language filter:

```sql
-- Extract materials with English descriptions
CREATE TABLE materials_en AS
SELECT * FROM sap_to_materials_with_desc(
    mara_table := 'sap_mara',
    makt_table := 'sap_makt',
    language := 'E'
);

-- Verify
SELECT
    COUNT(*) AS total_materials,
    COUNT(description) AS with_description,
    COUNT(*) - COUNT(description) AS without_description
FROM materials_en;

-- Sample output
SELECT material_id, description, material_type
FROM materials_en
WHERE description IS NOT NULL
LIMIT 5;
```

### Step 3: Extract German Descriptions

Create a parallel dataset with German descriptions:

```sql
-- Extract materials with German descriptions
CREATE TABLE materials_de AS
SELECT * FROM sap_to_materials_with_desc(
    mara_table := 'sap_mara',
    makt_table := 'sap_makt',
    language := 'D'
);

-- Compare coverage
SELECT
    'English' AS language,
    COUNT(*) AS total,
    COUNT(description) AS with_desc
FROM materials_en
UNION ALL
SELECT
    'German' AS language,
    COUNT(*) AS total,
    COUNT(description) AS with_desc
FROM materials_de;
```

### Step 4: Create Multi-Language Material View

Combine descriptions from multiple languages:

```sql
-- Create a view with all available descriptions
CREATE VIEW materials_multilang AS
SELECT
    m.matnr AS material_id,
    m.mtart AS material_type,
    m.matkl AS material_group,
    en.maktx AS description_en,
    de.maktx AS description_de,
    fr.maktx AS description_fr
FROM sap_mara m
LEFT JOIN sap_makt en ON m.matnr = en.matnr AND en.spras = 'E'
LEFT JOIN sap_makt de ON m.matnr = de.matnr AND de.spras = 'D'
LEFT JOIN sap_makt fr ON m.matnr = fr.matnr AND fr.spras = 'F'
WHERE m.lvorm IS NULL OR m.lvorm != 'X';  -- Exclude deleted materials

-- Query the multilingual view
SELECT *
FROM materials_multilang
WHERE description_en IS NOT NULL
  AND description_de IS NOT NULL
LIMIT 10;
```

### Step 5: Find Description Gaps

Identify materials missing translations:

```sql
-- Materials with English but missing German
SELECT
    m.material_id,
    m.description_en,
    m.material_type
FROM materials_multilang m
WHERE m.description_en IS NOT NULL
  AND m.description_de IS NULL
ORDER BY m.material_type, m.material_id
LIMIT 20;

-- Summary of translation coverage
SELECT
    material_type,
    COUNT(*) AS total_materials,
    COUNT(description_en) AS has_english,
    COUNT(description_de) AS has_german,
    COUNT(description_fr) AS has_french,
    ROUND(100.0 * COUNT(description_en) / COUNT(*), 1) AS pct_english,
    ROUND(100.0 * COUNT(description_de) / COUNT(*), 1) AS pct_german
FROM materials_multilang
GROUP BY material_type
ORDER BY total_materials DESC
LIMIT 10;
```

## Understanding the Results

### SAP Language Codes

| Code | Language | ISO 639-1 |
|------|----------|-----------|
| **E** | English | en |
| **D** | German | de |
| **F** | French | fr |
| **S** | Spanish | es |
| **I** | Italian | it |
| **P** | Portuguese | pt |
| **N** | Dutch | nl |
| **J** | Japanese | ja |
| **K** | Korean | ko |
| **Z** / **1** | Chinese | zh |

### Description Handling in Functions

The `sap_to_materials_with_desc()` function:
1. Joins MARA with MAKT using the specified language code
2. Returns NULL for description if no translation exists
3. Does not fall back to other languages automatically

## Variations

### Language Fallback Strategy

Create a description with fallback to other languages:

```sql
-- Priority: English → German → French → Any
SELECT
    m.matnr AS material_id,
    COALESCE(en.maktx, de.maktx, fr.maktx, any_lang.maktx) AS description,
    CASE
        WHEN en.maktx IS NOT NULL THEN 'E'
        WHEN de.maktx IS NOT NULL THEN 'D'
        WHEN fr.maktx IS NOT NULL THEN 'F'
        ELSE any_lang.spras
    END AS source_language
FROM sap_mara m
LEFT JOIN sap_makt en ON m.matnr = en.matnr AND en.spras = 'E'
LEFT JOIN sap_makt de ON m.matnr = de.matnr AND de.spras = 'D'
LEFT JOIN sap_makt fr ON m.matnr = fr.matnr AND fr.spras = 'F'
LEFT JOIN (
    SELECT DISTINCT ON (matnr) matnr, spras, maktx
    FROM sap_makt
    ORDER BY matnr, spras
) any_lang ON m.matnr = any_lang.matnr
WHERE m.lvorm IS NULL OR m.lvorm != 'X'
LIMIT 100;
```

### Search by Description in Any Language

Find materials matching a search term across all languages:

```sql
-- Search for 'pump' in any language
SELECT DISTINCT
    k.matnr AS material_id,
    k.spras AS language,
    k.maktx AS description
FROM sap_makt k
WHERE k.maktx ILIKE '%pump%'
   OR k.maktx ILIKE '%pumpe%'    -- German
   OR k.maktx ILIKE '%pompe%'    -- French
ORDER BY k.matnr
LIMIT 20;

-- More sophisticated: search with full-text pattern
SELECT DISTINCT
    k.matnr AS material_id,
    k.spras AS language,
    k.maktx AS description
FROM sap_makt k
WHERE regexp_matches(k.maktx, '(?i)(pump|pumpe|pompe|bomba)')
LIMIT 20;
```

### Generate Textual Embeddings by Language

Create embeddings for different languages separately:

```sql
-- English embeddings
CREATE TABLE textual_embeddings_en AS
SELECT *
FROM compute_textual_embeddings(
    makt_table := 'sap_makt',
    language := 'E',
    provider := 'gemma-local'
);

-- German embeddings
CREATE TABLE textual_embeddings_de AS
SELECT *
FROM compute_textual_embeddings(
    makt_table := 'sap_makt',
    language := 'D',
    provider := 'gemma-local'
);

-- Compare embedding availability
SELECT
    'English' AS language,
    COUNT(*) AS materials_with_embeddings
FROM textual_embeddings_en
UNION ALL
SELECT
    'German' AS language,
    COUNT(*) AS materials_with_embeddings
FROM textual_embeddings_de;
```

### Cross-Language Similarity

Find similar materials even when they have descriptions in different languages:

```sql
-- Store embeddings with language info
CREATE TABLE material_text_embeddings AS
SELECT
    k.matnr AS material_id,
    k.spras AS language,
    embed_text(k.maktx) AS text_embedding
FROM sap_makt k
WHERE k.maktx IS NOT NULL
  AND k.maktx != ''
  AND k.spras IN ('E', 'D', 'F');  -- Limit to major languages

-- Find similar materials across languages
WITH query_embedding AS (
    SELECT text_embedding
    FROM material_text_embeddings
    WHERE material_id = '000000000010561200' AND language = 'E'
)
SELECT
    e.material_id,
    e.language,
    k.maktx AS description,
    array_cosine_similarity(e.text_embedding, q.text_embedding) AS similarity
FROM material_text_embeddings e
CROSS JOIN query_embedding q
JOIN sap_makt k ON e.material_id = k.matnr AND e.language = k.spras
WHERE e.material_id != '000000000010561200'
ORDER BY similarity DESC
LIMIT 10;
```

## Use Cases

### Translation Quality Check

Compare descriptions across languages to find inconsistencies:

```sql
-- Find materials where English and German descriptions seem unrelated
-- (very different lengths might indicate different content)
SELECT
    m.material_id,
    m.description_en,
    m.description_de,
    LENGTH(m.description_en) AS len_en,
    LENGTH(m.description_de) AS len_de,
    ABS(LENGTH(m.description_en) - LENGTH(m.description_de)) AS length_diff
FROM materials_multilang m
WHERE m.description_en IS NOT NULL
  AND m.description_de IS NOT NULL
  AND ABS(LENGTH(m.description_en) - LENGTH(m.description_de)) > 20
ORDER BY length_diff DESC
LIMIT 20;
```

### Regional Reporting

Generate reports with appropriate language:

```sql
-- Create a parameterized view for regional reports
CREATE OR REPLACE MACRO material_report(lang) AS TABLE
SELECT
    m.matnr AS material_id,
    k.maktx AS description,
    m.mtart AS material_type,
    m.matkl AS material_group
FROM sap_mara m
LEFT JOIN sap_makt k ON m.matnr = k.matnr AND k.spras = lang
WHERE m.lvorm IS NULL OR m.lvorm != 'X';

-- Use for different regions
SELECT * FROM material_report('E') LIMIT 5;  -- English report
SELECT * FROM material_report('D') LIMIT 5;  -- German report
SELECT * FROM material_report('F') LIMIT 5;  -- French report
```

### Export for Translation Service

Generate a list of materials needing translation:

```sql
-- Materials with English but missing German translation
COPY (
    SELECT
        m.matnr AS material_id,
        en.maktx AS english_description,
        '' AS german_translation,
        m.mtart AS material_type
    FROM sap_mara m
    JOIN sap_makt en ON m.matnr = en.matnr AND en.spras = 'E'
    LEFT JOIN sap_makt de ON m.matnr = de.matnr AND de.spras = 'D'
    WHERE de.matnr IS NULL
      AND (m.lvorm IS NULL OR m.lvorm != 'X')
    ORDER BY m.mtart, m.matnr
    LIMIT 1000
) TO 'translation_needed.csv' (HEADER, DELIMITER ',');
```

## Performance Tips

### Index Description Tables

For frequent text searches:

```sql
-- Create index for case-insensitive description search
CREATE INDEX makt_desc_idx ON sap_makt (maktx);

-- Create index for language + material lookups
CREATE INDEX makt_lang_mat_idx ON sap_makt (spras, matnr);
```

### Materialize Common Views

If querying multilingual descriptions frequently:

```sql
-- Materialize instead of using a VIEW
CREATE TABLE materials_multilang_mat AS
SELECT * FROM materials_multilang;

-- Add indexes for common query patterns
CREATE INDEX mm_material_id_idx ON materials_multilang_mat (material_id);
CREATE INDEX mm_type_idx ON materials_multilang_mat (material_type);
```

## Next Steps

- **[Guide 01: Data Transformation](01_data_transformation.md)** - Review ETL basics
- **[Guide 04: Similarity at Scale](04_similarity_at_scale.md)** - Efficient search with embeddings
- **[API Reference](../../docs/API_REFERENCE.md)** - Full function documentation
