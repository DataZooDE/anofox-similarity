# Deduplication and LSH Examples

> **Find the same thing written twice, and find candidates without scanning everything.**

Runnable SQL examples for clustering scored pairs into entities (customer master
deduplication) and for weighted MinHash / LSH indexes that speed up covering-entity search on
large archives.

## Functions

| Function | Description |
|----------|-------------|
| `cluster_matches(pairs_table [, threshold, left_col, right_col, score_col])` | Union-find over pairs at or above the threshold: `entity_id, cluster_id, cluster_size` |
| `weighted_minhash(weights, num_hashes [, seed])` | Signature of a weighted set; equal positions estimate weighted Jaccard |
| `minhash_similarity(a, b)` | Share of equal signature positions |
| `lsh_bands(signature, bands)` | One hash per band |
| `minhash_index(feature_table [, num_hashes, seed, partitions, ...])` | Signatures and band hashes per entity, in size partitions |
| `minhash_bands(index_table)` | Band hashes in long form, for `bands_table` |
| `find_covering_entities_lsh(queries_table, index_table [, k, mode, threshold, bands_table])` | Many queries at once, ranked by estimated containment or Jaccard |

## Example Files

| File | Description | Data Source |
|------|-------------|-------------|
| [`cluster_matches.sql`](cluster_matches.sql) | Toy clusters, then a customer-master recipe: normalise, block, score, cluster, check | Inline + `test/data/coverage/customers.csv.gz` |
| [`minhash_lsh.sql`](minhash_lsh.sql) | Signatures, an index over a raw vocabulary, 80 requests against the programme archive | Synthetic (`range()`) + `test/data/coverage/` |

## Quick Start

```bash
# From the repository root
./build/release/duckdb < examples/dedup_and_lsh/cluster_matches.sql
./build/release/duckdb < examples/dedup_and_lsh/minhash_lsh.sql
```

---

## Usage

```sql
-- pairs(a, b, score) from your own scoring; one cluster per entity
SELECT * FROM cluster_matches('pairs', threshold := 0.85);

-- Index once, search many queries (query_id, feature_id, weight)
CREATE TABLE idx AS SELECT * FROM minhash_index(feature_table := 'feature_multiset');
CREATE TABLE bands AS SELECT * FROM minhash_bands('idx');
SELECT * FROM find_covering_entities_lsh('queries', 'idx', k := 20, threshold := 0.9,
                                         bands_table := 'bands');
```

## Key Concepts

- **Scoring is yours.** `cluster_matches` only clusters. Blocking (same tax id or city),
  normalisation (ü → ue, GmbH spellings) and the score formula are plain SQL; the recipe merges
  all 152 planted duplicates and none of the 12 near misses at 0.85.
- **Self-pairs.** Add `(id, id, 1.0)` for every record so singletons appear in the output.
- **cluster_id** is the smallest member id in text order, so it is stable across runs.
- **When LSH pays off.** With a large raw vocabulary (part numbers) LSH scores a small share
  of the entities. With a pivot of about 20 classes the multisets overlap so much that it still
  scores about half of them; use exact `find_covering_entities` there.
- **Store the bands.** A `minhash_bands` table turns the band lookup into a hash join; a band
  table from another index is rejected.
