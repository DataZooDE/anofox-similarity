# Sub-linear recall and entity clustering

> Weighted MinHash, LSH search and duplicate clustering.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

**Sub-linear recall.** Exact `find_covering_entities` stays the default; these are
optional accelerators.

| Function | Purpose |
|---|---|
| `weighted_minhash(weights MAP, num_hashes [, seed])` | ICWS signature (UBIGINT[]); equal positions estimate weighted Jaccard. Hashing is stable across versions |
| `minhash_similarity(a, b)` | share of equal positions |
| `lsh_bands(signature, bands)` | one hash per band |
| `minhash_index(feature_table [, num_hashes := 128, seed := 7, partitions := 4, entity_col, feature_col, weight_col])` | `entity_id, total_weight, partition, signature, band_hashes, num_hashes, seed` |
| `find_covering_entities_lsh(queries_table, index_table [, k := 20, mode := 'containment', threshold := 0.5, bands_table])` | `query_id, entity_id, estimate, jaccard_estimate, rank, n_candidates` |
| `minhash_bands(index_table)` | the band hashes in long form `(entity_id, part, r, j, h, num_hashes, seed)`, for `bands_table`; a band table from another index is an error |

The index works in the LSH-Ensemble style. Entities are split into size partitions, and for
each query and partition the containment threshold becomes a Jaccard threshold for the
partition's largest set. The rows per band (1, 2, 4 or 8) are chosen so that the LSH S-curve
passes below that threshold. The containment estimate is J (|q| + |c|) / ((1 + J) |q|), capped
at 1. With a pivot vocabulary of about 20 classes the multisets overlap so much that LSH still
scores about half the candidates (examples/dedup_and_lsh). It pays off for large raw vocabularies such as BOM
components. With 20 000 entities over 5 000 features, it scores 80 candidates per query; a
stored `minhash_bands` table turns the band lookup into a hash join, while without it the
index is unnested on every call. Unlike `find_covering_entities`, the queries table takes
`(query_id, feature_id[, weight])` rows, so many requests are searched in one call.

```sql
CREATE TABLE programme_minhash AS SELECT * FROM minhash_index(feature_table := 'programme_class_multiset');
CREATE TABLE programme_bands AS SELECT * FROM minhash_bands('programme_minhash');   -- store once
-- request items need classes (label_table or class_id); unclassified items drop out of the multiset
CREATE TABLE request_ms AS SELECT entity_id AS query_id, feature_id, weight
FROM tree_class_multiset('request_nodes', label_table := 'request_labels');
SELECT * FROM find_covering_entities_lsh('request_ms', 'programme_minhash', k := 20, threshold := 0.9,
                                         bands_table := 'programme_bands');
```

**Entity clustering.** `cluster_matches(pairs_table [, threshold := 0.8, left_col
:= 'a', right_col := 'b', score_col := 'score'])` returns `entity_id, cluster_id,
cluster_size` from a C++ union-find over the pairs at or above the threshold. Every entity of
the pairs table appears (ids as text), and `cluster_id` is the smallest member id in text order. `connected_components(LIST
of STRUCT(a, b))` is the scalar behind it. examples/dedup_and_lsh shows a customer-master recipe: blocking,
normalised names, and tax-id and address agreement. It clusters at 0.85, merges all 152 planted
duplicates and none of the 12 near misses.

**DuckPGQ recipe** (optional, not called by the extension). For graph queries over the matches:

```sql
INSTALL duckpgq FROM community; LOAD duckpgq;
SET active_grammar_extensions = ['duckpgq'];   -- DuckDB 2.0, per connection
CREATE TABLE entities AS SELECT DISTINCT a AS id FROM pairs UNION SELECT DISTINCT b FROM pairs;
CREATE TABLE strong AS SELECT a, b FROM pairs WHERE score >= 0.85;
CREATE PROPERTY GRAPH matches VERTEX TABLES (entities)
    EDGE TABLES (strong SOURCE KEY (a) REFERENCES entities (id) DESTINATION KEY (b) REFERENCES entities (id));
SELECT * FROM weakly_connected_component(matches, entities, strong);
```

Extension macros are registered with the default grammar, so no built-in macro embeds PGQ
syntax. The test suite does not load DuckPGQ; check its documentation for the syntax of
your version.

---

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `weighted_minhash`

*Scalar function*

Weighted MinHash signature of a MAP(key, weight) multiset by improved consistent weighted sampling (ICWS, Ioffe 2010): the share of equal positions of two signatures estimates their weighted Jaccard. num_hashes positions (1..4096), seeded (default 7); hashing is our own (FNV-1a, SplitMix64), independent of DuckDB's hash. A NULL weight counts as 1, zero weights are absent, negative weights are an error; NULL or an empty multiset gives NULL.

```sql
weighted_minhash(weights MAP(VARCHAR, DOUBLE), num_hashes BIGINT) → UBIGINT[]
weighted_minhash(weights MAP(VARCHAR, DOUBLE), num_hashes BIGINT, seed BIGINT) → UBIGINT[]
```

**Examples**

```sql
SELECT weighted_minhash(MAP {'a': 1.0, 'b': 2.0}, 128);
SELECT weighted_minhash(MAP {'a': 1.0, 'b': 2.0}, 128, 7);
```

### `minhash_similarity`

*Scalar function*

Share of equal positions of two MinHash signatures of the same length: the estimate of their (weighted) Jaccard similarity.

```sql
minhash_similarity(signature_a UBIGINT[], signature_b UBIGINT[]) → DOUBLE
```

**Examples**

```sql
SELECT minhash_similarity(weighted_minhash(MAP {'a': 1.0}, 64), weighted_minhash(MAP {'a': 1.0, 'b': 1.0}, 64)); -- about 0.5
```

### `lsh_bands`

*Scalar function*

LSH band hashes of a MinHash signature: one hash per band of len(signature) / bands rows. Two multisets with Jaccard J share at least one band with probability 1 - (1 - J^rows)^bands.

```sql
lsh_bands(signature UBIGINT[], bands BIGINT) → UBIGINT[]
```

**Examples**

```sql
SELECT lsh_bands(weighted_minhash(MAP {'a': 1.0}, 64), 16);
```

### `minhash_index`

*Table function*

Builds a weighted-MinHash index of a feature multiset (feature_table, default 'feature_multiset'; entity_col/feature_col/weight_col as in find_covering_entities; weight_col := NULL for unit weights): one row per entity with total_weight, its size partition (partitions quantiles, default 4), signature (num_hashes, a multiple of 8, default 128; seed default 7), band_hashes for 1, 2, 4 and 8 rows per band (concatenated), num_hashes and seed. Store it as a table; find_covering_entities_lsh searches it.

```sql
minhash_index(feature_table, num_hashes, seed, partitions, entity_col, feature_col, weight_col)
```

**Examples**

```sql
CREATE TABLE programme_index AS SELECT * FROM minhash_index(feature_table := 'programme_class_multiset');
```

### `minhash_bands`

*Table function*

The band hashes of a minhash_index in long form (entity_id, part, r, j, h, num_hashes, seed): one row per entity, rows per band (1, 2, 4, 8) and band. Store it and pass it to find_covering_entities_lsh as bands_table, so the search joins on the band hash instead of unnesting the index.

```sql
minhash_bands(index_table)
```

**Examples**

```sql
CREATE TABLE programme_bands AS SELECT * FROM minhash_bands('programme_index');
```

### `find_covering_entities_lsh`

*Table function*

Sub-linear candidate search over a minhash_index (LSH-Ensemble style). queries_table holds (query_id, feature_id[, weight]) multisets. Per query and size partition, the containment threshold (default 0.5) is turned into the Jaccard threshold for the partition's largest set, and the rows per band are chosen so that the LSH S-curve passes below it; candidates share a band. Ranked by the estimate: containment = J (|q| + |c|) / ((1 + J) |q|) (capped at 1) or J (mode 'jaccard'). Returns query_id, entity_id, estimate, jaccard_estimate, rank (top k, default 20) and n_candidates (scored per query). An approximate accelerator: exact find_covering_entities stays the reference. Without bands_table the index's band hashes are unnested on every call (linear in the index size); store minhash_bands(index) once and pass it as bands_table for a hash-join (sub-linear) search.

```sql
find_covering_entities_lsh(queries_table, index_table, k, mode, threshold, bands_table)
```

**Examples**

```sql
SELECT * FROM find_covering_entities_lsh('request_multisets', 'programme_index', k := 20);
```

### `connected_components`

*Scalar function*

Union-find over a LIST of pairs STRUCT(a, b): returns LIST(STRUCT(entity_id, cluster_id)), one entry per entity sorted by id; cluster_id is the smallest entity id of its connected component. NULL ids are ignored. The building block of cluster_matches.

```sql
connected_components(pairs ANY) → STRUCT(entity_id VARCHAR, cluster_id VARCHAR)[]
```

**Examples**

```sql
SELECT connected_components([{'a': 'x', 'b': 'y'}, {'a': 'y', 'b': 'z'}]);
```

### `cluster_matches`

*Table function*

Entity clustering: a C++ union-find over the pairs whose score (score_col, default 'score') is at or above threshold (default 0.8). Returns entity_id, cluster_id (the smallest member id) and cluster_size for every entity in the pairs table (left_col/right_col, default 'a'/'b'); entities without a pair above the threshold form their own cluster. For a graph view, see the DuckPGQ recipe in the API reference.

```sql
cluster_matches(pairs_table, threshold, left_col, right_col, score_col)
```

**Examples**

```sql
SELECT * FROM cluster_matches('customer_pairs', threshold := 0.85);
```

<!-- generated:functions:end -->
