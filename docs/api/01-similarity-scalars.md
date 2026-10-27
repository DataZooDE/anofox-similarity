# Similarity scalars

> Set similarity of two component lists or weighted multisets.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## Core Similarity Algorithms

### Jaccard Similarity
Computes set overlap using Jaccard index. Takes component lists directly.

**Signature:**
```sql
jaccard_similarity(
    list_a LIST,
    list_b LIST
) → DOUBLE
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `list_a` | LIST(ANY) | Required | First set of elements |
| `list_b` | LIST(ANY) | Required | Second set of elements |

**Returns:**
- DOUBLE: Similarity score [0.0, 1.0]
  - 0.0 = Completely disjoint (no common elements), **and by convention two empty (or all-NULL) sets also score 0.0**
  - 1.0 = Identical element sets

**Complexity:** O(n + m) where n, m = list sizes

**Example:**
```sql
-- Direct list comparison
SELECT jaccard_similarity(['A', 'B', 'C'], ['A', 'B', 'D']) AS similarity;
-- Result: 0.5 (2 common out of 4 unique elements)

-- With BOM data (via aggregate_material_components helper)
WITH components AS (
  SELECT material_id, components FROM aggregate_material_components('bom_items')
)
SELECT jaccard_similarity(a.components, b.components) AS similarity
FROM components a, components b
WHERE a.material_id = 'PUMP-A' AND b.material_id = 'PUMP-B';
```

### WL Kernel Similarity
Computes graph-structural similarity using Weisfeiler-Lehman kernel.

**Signature:**
```sql
wl_kernel_similarity(
    material_a VARCHAR,
    material_b VARCHAR,
    iterations := 3,
    bom_table := 'bom_items'
) → DOUBLE
```

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `material_a` | VARCHAR | Required | First material ID |
| `material_b` | VARCHAR | Required | Second material ID |
| `iterations` | INTEGER | 3 | Number of refinement iterations (clamped: values above 5 behave identically to 5) |
| `bom_table` | VARCHAR | 'bom_items' | BOM table name |

**Returns:**
- DOUBLE: Similarity score [0.0, 1.0]

**Complexity:** O(h·m) where h = iterations, m = edges

**Example:**
```sql
-- More sophisticated similarity considering BOM topology
SELECT
  material_1,
  material_2,
  wl_kernel_similarity(material_1, material_2, iterations := 5) AS topology_similarity
FROM cross_product_of_materials;
```

### Directional and weighted set measures (scalars)

| Function | Formula | Empty / NULL |
|---|---|---|
| `tversky_similarity(query LIST, candidate LIST[, alpha DOUBLE, beta DOUBLE])` | `\|q∩c\| / (\|q∩c\| + α\|q\c\| + β\|c\q\|)`; default α = β = 1 (Jaccard) | both empty → 0.0; other zero denominators → NULL |
| `containment(query LIST, candidate LIST)` | `\|q∩c\| / \|q\|` | empty query → NULL |
| `weighted_jaccard(query MAP, candidate MAP)` | `Σ min(q_k, c_k) / Σ max(q_k, c_k)` | both empty → 0.0 |
| `weighted_containment(query MAP, candidate MAP)` | `Σ min(q_k, c_k) / Σ q_k` | empty or all-zero query → NULL |
| `weighted_tversky(query MAP, candidate MAP[, alpha, beta])` | `Σmin / (Σmin + α Σ(q−c)⁺ + β Σ(c−q)⁺)`; default α = β = 1 | as `weighted_jaccard` |

- Lists are sets: duplicates collapse and NULL elements are ignored, as in `jaccard_similarity`.
- Map values are weights (counts, quantities, levels). They must be numeric, finite and
  non-negative; a NULL weight counts as 1, and a zero weight means "absent". Keys may be of
  any type and are compared by their text form, so `1` and `'1'` match (in the scalars and in
  `find_covering_entities_for` against the feature column cast to VARCHAR).
- List elements of different types: `tversky_similarity` and `containment` compare the text
  form, `jaccard_similarity` the typed value. So `jaccard_similarity([1, 2], ['1', '2'])` is 0.0
  while `tversky_similarity([1, 2], ['1', '2'])` is 1.0, and an INTEGER list never matches a
  DOUBLE or DECIMAL list in any of them (`'1'` vs `'1.0'`). Cast both sides to the same type,
  for example `::VARCHAR[]`, before comparing ids from different sources.
- NULL arguments give NULL. `alpha` and `beta` must be non-negative.
- The parameters are called `query` and `candidate` because the measures are directional
  (`jaccard_similarity` keeps `list_a` / `list_b`).

```sql
SELECT containment(['a','b'], ['a','b','c','d']);                          -- 1.0
SELECT containment(['a','b','c','d'], ['a','b']);                          -- 0.5
SELECT weighted_containment(MAP {'massage': 4}, MAP {'massage': 2});      -- 0.5: counts matter
SELECT weighted_tversky(MAP {'a': 3}, MAP {'a': 2, 'b': 3}, 1.0, 0.0);    -- 0.6667
```

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `jaccard_similarity`

*Scalar function*

Computes the Jaccard similarity coefficient between two lists treated as sets. Returns a value in [0.0, 1.0] equal to |intersection| / |union|. Duplicates within each list are deduplicated before comparison. Returns NULL if either argument is NULL; returns 0.0 if both lists are empty.

```sql
jaccard_similarity(list_a ANY[], list_b ANY[]) → DOUBLE
```

**Examples**

```sql
SELECT jaccard_similarity(['A','B','C'], ['B','C','D']); -- 0.5
SELECT jaccard_similarity([1,2,3,4], [3,4,5,6]); -- 0.3333
```

### `wl_kernel_similarity`

*Scalar macro*

Weisfeiler-Lehman kernel similarity between two materials' BOM structures, comparing their depth-expanded component fingerprints. Returns a score in [0, 1] where 1 means structurally identical. Iterations 1-5 are exact; higher values are capped at depth 5.

```sql
wl_kernel_similarity(material_a, material_b, iterations, bom_table)
```

**Examples**

```sql
SELECT wl_kernel_similarity('PUMP-A', 'PUMP-B');
```

### `wl_fingerprint`

*Table macro*

Depth-expanded component fingerprint of one material, as component -> number of distinct depths at which it is reachable. Written as a non-recursive bounded expansion on purpose: DuckDB cannot decorrelate a recursive CTE inside a correlated scalar subquery, which is exactly how wl_kernel_similarity uses it. Iterations 1-5 are exact; higher values are capped at depth 5.

```sql
wl_fingerprint(root, iters, bom_table)
```

**Examples**

```sql
SELECT * FROM wl_fingerprint('PUMP-A', 3, 'bom_items');
```

### `tversky_similarity`

*Scalar function*

Asymmetric Tversky similarity of two lists treated as sets: |q∩c| / (|q∩c| + alpha*|q\c| + beta*|c\q|). alpha = beta = 1 is Jaccard, alpha = 1 and beta = 0 is containment. Pass both alpha and beta, alpha first: DuckDB 1.5.6 binds scalar named arguments by position, so (..., beta := x, alpha := y) silently swaps them there. NULL in gives NULL; two empty lists give 0.0.

```sql
tversky_similarity(query ANY[], candidate ANY[]) → DOUBLE
tversky_similarity(query ANY[], candidate ANY[], alpha DOUBLE, beta DOUBLE) → DOUBLE
```

**Examples**

```sql
SELECT tversky_similarity(['a','b','c'], ['b','c','d']); -- 0.5
SELECT tversky_similarity(['a','b'], ['a','b','c','d'], 1.0, 0.25); -- 0.8
```

### `containment`

*Scalar function*

Share of the query set contained in the candidate set: |q∩c| / |q|. Directional: a richer candidate is not penalised. Duplicates collapse. NULL in gives NULL; an empty query gives NULL.

```sql
containment(query ANY[], candidate ANY[]) → DOUBLE
```

**Examples**

```sql
SELECT containment(['a','b'], ['a','b','c','d']); -- 1.0
SELECT containment(['a','b','c'], ['b','c','d']); -- 0.6667
```

### `weighted_jaccard`

*Scalar function*

Weighted Jaccard similarity of two MAP(key, weight) multisets: Σ min(q_k, c_k) / Σ max(q_k, c_k). Weights must be non-negative; a NULL weight counts as 1. Two empty maps give 0.0.

```sql
weighted_jaccard(query ANY, candidate ANY) → DOUBLE
```

**Examples**

```sql
SELECT weighted_jaccard(MAP {'heating': 2, 'massage': 4}, MAP {'heating': 4, 'ventilation': 2}); -- 0.2
```

### `weighted_containment`

*Scalar function*

Share of a MAP(key, weight) query multiset covered by the candidate: Σ min(q_k, c_k) / Σ q_k. Counts matter: 4 requested zones are half covered by 2. Weights must be non-negative; a NULL weight counts as 1. An empty query gives NULL.

```sql
weighted_containment(query ANY, candidate ANY) → DOUBLE
```

**Examples**

```sql
SELECT weighted_containment(MAP {'a': 3, 'b': 1}, MAP {'a': 2, 'b': 5}); -- 0.75
```

### `weighted_tversky`

*Scalar function*

Weighted Tversky similarity of two MAP(key, weight) multisets: Σmin / (Σmin + alpha*Σ(q-c)+ + beta*Σ(c-q)+). alpha = beta = 1 (the two-argument default) is weighted Jaccard, alpha = 1 and beta = 0 is weighted containment. Pass both alpha and beta, alpha first: DuckDB 1.5.6 binds scalar named arguments by position, so (..., beta := x, alpha := y) silently swaps them there.

```sql
weighted_tversky(query ANY, candidate ANY) → DOUBLE
weighted_tversky(query ANY, candidate ANY, alpha DOUBLE, beta DOUBLE) → DOUBLE
```

**Examples**

```sql
SELECT weighted_tversky(MAP {'a': 3, 'b': 1}, MAP {'a': 2, 'c': 2}); -- 0.3333
SELECT weighted_tversky(MAP {'a': 3, 'b': 1}, MAP {'a': 2, 'c': 2}, 0.5, 0.5); -- 0.5
```

<!-- generated:functions:end -->
