# Scale and Deduplication

> Search large archives in sub-linear time, and merge duplicate master data records.

**Use this guide to:**
- Decide when MinHash/LSH pays off over the exact search
- Build a MinHash index with `minhash_index` and `minhash_bands`
- Search many queries in one call with `find_covering_entities_lsh`
- Cluster duplicate records with `cluster_matches`
- Find the DuckPGQ recipe for graph queries over matches

## Exact Search First

`find_covering_entities` is exact. It joins the query's features against the feature table. One
query over 100 000 entities with 15 features each takes about 0.15–0.2 s. Use it as the default.
Many queries at once do not need a loop (a table function cannot take a column of a `LATERAL`
join): the same exact search is one hash join, shown [below](#exact-batch-search).

MinHash/LSH estimates the result. It can pay off when:

- the vocabulary is large (raw BOM components, part numbers), so most entities share nothing, and
- the feature table is too large to join against for every batch of queries.

Measure before you switch: on the archive below (20 000 BOMs, 400 000 rows) and also on 100 000
BOMs (2 million rows), the exact batch join is faster than the LSH search for 100 queries.

It does not pay off with a small pivot vocabulary. With about 20 catalogue classes, the multisets
overlap so much that LSH still scores about half the archive.

## Weighted MinHash

A signature estimates weighted Jaccard. Equal positions in two signatures estimate the similarity:

```sql
SELECT round(weighted_jaccard(MAP {'a': 1.0, 'b': 1.0}, MAP {'a': 1.0, 'b': 3.0}), 3) AS exact,
       round(minhash_similarity(weighted_minhash(MAP {'a': 1.0, 'b': 1.0}, 1024),
                                weighted_minhash(MAP {'a': 1.0, 'b': 3.0}, 1024)), 3) AS estimate;
```

| exact | estimate |
|------:|---------:|
| 0.5   | 0.525    |

The hashing is stable across versions, so stored signatures stay valid.

## Index an Archive

An archive of 20 000 BOMs with 20 parts each, out of 50 000 part numbers:

```sql
CREATE TABLE part_bom AS
SELECT DISTINCT 'A' || lpad(i::VARCHAR, 5, '0') AS entity_id, 'P' || (hash(i, j) % 50000) AS feature_id, 1.0 AS weight
FROM range(20000) t(i), range(20) u(j);

CREATE TABLE part_minhash AS SELECT * FROM minhash_index(feature_table := 'part_bom');
CREATE TABLE part_bands   AS SELECT * FROM minhash_bands('part_minhash');
```

The data comes from DuckDB's `hash()`, which can change between DuckDB versions. The outputs below
are from DuckDB 1.5.6. On a development workstation the index took about 2 s and the band table about 0.7 s. Build both once and rebuild
them when the archive changes. With 128 hashes, `minhash_bands` stores 240 rows per entity:
4.8 million rows for this 20 000-entity archive. Budget memory and storage for these tables.

- `num_hashes` (default 128) sets the precision of the estimate.
- `partitions` (default 4) splits the entities by size. Small queries inside large sets are found
  through the size partitions.
- `entity_col`, `feature_col`, `weight_col` map an existing table, as in `find_covering_entities`.

## Search Many Queries at Once

The queries table has rows `(query_id, feature_id[, weight])`. Here: 100 new BOMs, each with about
three quarters of the parts of an archived one.

```sql
CREATE TABLE new_boms AS
SELECT 'NEW-' || e.entity_id AS query_id, b.feature_id, b.weight
FROM (SELECT entity_id FROM part_minhash ORDER BY entity_id LIMIT 100) e
JOIN part_bom b USING (entity_id)
WHERE hash(b.feature_id) % 4 <> 0;

CREATE TABLE hits AS
SELECT * FROM find_covering_entities_lsh('new_boms', 'part_minhash', k := 3, threshold := 0.8,
                                         bands_table := 'part_bands');

SELECT query_id, entity_id, round(estimate, 3) AS estimate, round(jaccard_estimate, 3) AS jaccard_estimate,
       rank, n_candidates
FROM hits WHERE query_id IN ('NEW-A00000', 'NEW-A00001') ORDER BY query_id, rank;
```

| query_id   | entity_id | estimate | jaccard_estimate | rank | n_candidates |
|------------|-----------|---------:|-----------------:|-----:|-------------:|
| NEW-A00000 | A00000    | 1.0      | 0.766            | 1    | 1            |
| NEW-A00001 | A00001    | 1.0      | 0.656            | 1    | 1            |

```sql
SELECT count(*) FILTER (WHERE 'NEW-' || entity_id = query_id) AS found, avg(n_candidates) AS avg_candidates
FROM hits WHERE rank = 1;
```

| found | avg_candidates |
|------:|---------------:|
| 100   | 1.03           |

All 100 source BOMs are found at rank 1. LSH scores about one candidate per query instead of
20 000. The call takes about 0.2–0.3 s for all 100 queries. One `find_covering_entities_for` call
takes about 0.05–0.08 s, but you do not have to call it 100 times: see the next section.

### Exact Batch Search

The exact containment for all queries is one join over the shared features. It is the reference
to check LSH recall against:

```sql
CREATE TABLE exact_hits AS
WITH shared AS (
    SELECT q.query_id, b.entity_id, sum(least(q.weight, b.weight)) AS shared_weight
    FROM new_boms q JOIN part_bom b USING (feature_id)
    GROUP BY ALL),
query_weight AS (SELECT query_id, sum(weight) AS query_weight FROM new_boms GROUP BY ALL)
SELECT query_id, entity_id, shared_weight / query_weight AS containment,
       row_number() OVER (PARTITION BY query_id ORDER BY shared_weight / query_weight DESC, entity_id) AS rank
FROM shared JOIN query_weight USING (query_id)
QUALIFY rank <= 3;

SELECT count(*) FILTER (WHERE 'NEW-' || entity_id = query_id) AS found FROM exact_hits WHERE rank = 1;
```

| found |
|------:|
| 100   |

This takes about 0.02 s for the 100 queries (DuckDB 1.5.6 and 2.0), about ten times faster than
the LSH search above. With 100 000 BOMs (2 million rows) it takes under 0.1 s, against about 0.5 s
for LSH plus about 5 s to build the index and band table. Because only shared features are joined,
the exact join stays cheap when the vocabulary is large. Use LSH when the join no longer fits your
time or memory budget, and measure its recall against `exact_hits`.

- `estimate` is the containment estimate (mode `'containment'`, default). `mode := 'jaccard'`
  ranks by `jaccard_estimate`.
- `threshold` is the containment you look for. Candidates far below it are not found.
- Without `bands_table`, the index is unnested on every call. Store the band table.
- A band table from another index is an error.

## Duplicate Records: cluster_matches

`cluster_matches` turns scored pairs into clusters. It links every pair at or above the threshold
and returns the connected groups:

```sql
CREATE TABLE pairs AS SELECT * FROM (VALUES
  ('C1', 'C2', 0.95), ('C2', 'C3', 0.90), ('C4', 'C5', 0.60), ('C6', 'C6', 1.0)) t(a, b, score);

SELECT * FROM cluster_matches('pairs', threshold := 0.85) ORDER BY entity_id;
```

| entity_id | cluster_id | cluster_size |
|-----------|------------|-------------:|
| C1        | C1         | 3            |
| C2        | C1         | 3            |
| C3        | C1         | 3            |
| C4        | C4         | 1            |
| C5        | C5         | 1            |
| C6        | C6         | 1            |

Every entity of the pairs table appears. `cluster_id` is the smallest member id. Use `left_col`,
`right_col` and `score_col` for other column names. A self-pair puts a record without candidates
into the output.

### Recipe: Duplicate Customer Plants

The pair scores are your design. The benchmark's customer master (`customers.csv.gz`, 1 910 records)
holds planted duplicates: spelling variants, ü/ue, GmbH/G.m.b.H., moved plants and shared tax ids.
It also holds 12 near misses: other companies at the address of an existing plant. A recipe that
merges all duplicates and none of the near misses:

1. **Normalise.** Lower case, umlauts to ae/oe/ue, `straße` to `str`, punctuation to spaces, legal
   forms to one spelling.
2. **Build an identity per record type.** Company and legal form, company and plant name, the
   contact name.
3. **Block.** Compare only records of the same type that share a tax id or a city.
4. **Score.** `0.6 × jaro_winkler(identity) + 0.3 × same tax id + 0.1 × same address`. For contacts:
   `0.5 × name + 0.3 × same e-mail + 0.2 × same parent`. Plants with different plant numbers score 0.
5. **Cluster** at 0.85 with `cluster_matches`.

The full SQL is in `test/sql/coverage/r5/uc_03_merge_duplicate_customer_plants.test` and in the
[examples](../../examples/dedup_and_lsh). On the benchmark it builds 90 694 candidate pairs and
1 750 clusters. All 152 planted duplicates join their original, and no near miss is merged.

Address agreement alone is not enough: the near misses share the street and house number. The tax
id and the name carry the decision.

## Graph Queries With DuckPGQ

The extension does not call DuckPGQ. To run graph queries over the matches, load it yourself:

```sql
INSTALL duckpgq FROM community; LOAD duckpgq;
CREATE TABLE entities AS SELECT DISTINCT a AS id FROM pairs UNION SELECT DISTINCT b FROM pairs;
CREATE TABLE strong AS SELECT a, b FROM pairs WHERE score >= 0.85;
CREATE PROPERTY GRAPH matches VERTEX TABLES (entities)
    EDGE TABLES (strong SOURCE KEY (a) REFERENCES entities (id) DESTINATION KEY (b) REFERENCES entities (id));
SELECT * FROM weakly_connected_component(matches, entities, strong);
```

On DuckDB 2.0, also run `SET active_grammar_extensions = ['duckpgq'];` per connection. At the time
of writing, `INSTALL duckpgq FROM community` returns HTTP 404 on DuckDB 2.0; the recipe runs on 1.5.6. Check the
DuckPGQ documentation for the syntax of your version. For plain clustering, `cluster_matches` needs
no extra extension.

## Pitfalls

| Pitfall | Solution |
|---|---|
| LSH is slower than the exact search | Small vocabulary or single queries. Use `find_covering_entities`. |
| LSH misses a candidate | Lower `threshold`, or raise `num_hashes`. Exact search stays the reference. |
| Stale index | Rebuild `minhash_index` and `minhash_bands` after archive changes. |
| One wrong pair merges two clusters | Clustering is transitive. Raise the threshold or block more strictly. |
| Records missing from the clusters | Add a self-pair `(id, id, 1.0)` for every record. |

## Next Steps

- [Coverage matching](04-coverage-matching.md): exact covering search and tree coverage
- [Evaluation and tuning](07-evaluation-and-tuning.md): measure recall against the exact search
- API: [LSH and clustering](../api/16-lsh-and-clustering.md), [Infrastructure](../api/06-infrastructure.md)
- [Examples](../../examples/dedup_and_lsh)
