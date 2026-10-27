# Guide 04: Similarity Search at Scale

## The Problem

> "With hundreds of thousands of BOMs in our SAP system, how do we find similar products for
> many materials at once without comparing every pair?"

`find_similar_materials_jaccard` is exact: for one query it compares the query's components
with every BOM. That is the right tool for one material at a time. For many queries against a
large archive (a whole product line, a duplicate sweep), the extension offers a weighted MinHash
index with locality-sensitive hashing (LSH): it scores only the candidates that share a band
with the query.

> [!NOTE]
> An earlier version of this page described an HNSW index for Jaccard search. The extension
> has no such workflow: `find_similar_materials_jaccard` never uses an index. The steps below use
> the functions that exist. They were verified on the AdventureWorks BOM shipped in
> `test/data/adventureworks/`; with your SAP extract, only the first step differs.

## Step 1: The universal BOM table

```sql
-- Load the extension (built from source, see docs/guides/01-getting-started.md)
LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';

CREATE TABLE sap_mast AS SELECT * FROM read_parquet('test/data/sap/mast.parquet');
CREATE TABLE sap_stko AS SELECT * FROM read_parquet('test/data/sap/stko.parquet');
CREATE TABLE sap_stpo AS SELECT * FROM read_parquet('test/data/sap/stpo.parquet');

CREATE TABLE bom_items AS
SELECT * FROM sap_to_bom_items('sap_mast', 'sap_stko', 'sap_stpo', reference_date := CURRENT_DATE);

-- Without an SAP extract, follow along with the AdventureWorks BOM instead:
-- CREATE TABLE bom_items AS SELECT * FROM read_csv('test/data/adventureworks/bom_items.csv.gz');

SELECT count(*) AS bom_rows, count(DISTINCT parent_id) AS boms FROM bom_items;
```

## Step 2: Exact search for one material

```sql
SELECT material_id, round(similarity, 3) AS similarity, shared_components
FROM find_similar_materials_jaccard('AW-752', 5);
```

Keep using this for interactive, one-material questions. It needs no preparation and its
scores are exact.

## Step 3: Build the MinHash index once

A BOM is a set of components, so it has no weight column: pass `weight_col := NULL` for unit
weights. Store the band table too; it turns the candidate lookup into a hash join.

```sql
CREATE TABLE bom_minhash AS
SELECT * FROM minhash_index(feature_table := 'bom_items', entity_col := 'parent_id',
                            feature_col := 'child_id', weight_col := NULL);
CREATE TABLE bom_bands AS SELECT * FROM minhash_bands('bom_minhash');
```

Rebuild both tables after the BOMs change: a band table from another index is an error.

## Step 4: Search many materials in one call

The queries table holds `(query_id, feature_id)` rows. Queries taken from the archive find
themselves first, so drop the self-match.

```sql
CREATE TABLE queries AS
SELECT parent_id AS query_id, child_id AS feature_id
FROM bom_items
WHERE parent_id IN ('AW-752', 'AW-954');

SELECT query_id, entity_id, round(estimate, 3) AS estimate, n_candidates
FROM find_covering_entities_lsh('queries', 'bom_minhash', k := 6, threshold := 0.5,
                                bands_table := 'bom_bands')
WHERE entity_id <> query_id
ORDER BY query_id, estimate DESC, entity_id;
```

`estimate` is the estimated containment of the query in the candidate (mode `'containment'`;
use `mode := 'jaccard'` for the symmetric measure). `n_candidates` shows how many BOMs were
scored for the query: the saving over the exact search.

## Step 5: Check the approximation

On a sample, compare the LSH result with the exact one before you rely on it:

```sql
SELECT e.material_id, round(e.similarity, 3) AS exact_jaccard
FROM find_similar_materials_jaccard('AW-752', 5) e;
```

Lower `threshold` to trade speed for recall; raise `num_hashes` in `minhash_index` for a more
precise estimate.

## Next Steps

- **[Scale and dedup guide](../../08-scale-and-dedup.md)**: LSH on feature multisets, clustering duplicates
- **[LSH and clustering reference](../../../api/16-lsh-and-clustering.md)**: every parameter
- **[Guide 05: Multi-Language Descriptions](05_multilanguage_descriptions.md)**: work with localized text
