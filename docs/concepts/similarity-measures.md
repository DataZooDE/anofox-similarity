# Similarity Measures

> What the set and BOM measures compute, how they differ, and which one answers which question.
> For the calls, see [guide 02](../guides/02-material-similarity.md) and
> [api/01](../api/01-similarity-scalars.md).

[Concepts](README.md) · [Guides](../guides/README.md) · [API reference](../API_REFERENCE.md)

## Two Kinds of Question

| Question | Kind | Measures |
|---|---|---|
| How alike are A and B? | symmetric | Jaccard, weighted Jaccard, the BOM depth kernel |
| Does B contain what A has? | directional | containment, weighted containment, Tversky |

A symmetric measure punishes both sides for what the other lacks. A directional one takes the first
argument as the query and asks how much of it the candidate covers; extra parts in the candidate
cost nothing. Material search uses the symmetric ones by default; coverage matching
([coverage semantics](coverage-semantics.md)) is directional throughout.

## Sets: Jaccard, Containment, Tversky

For a query set q and a candidate set c (duplicates collapse, NULLs are ignored):

```
jaccard(q, c)     = |q ∩ c| / |q ∪ c|
containment(q, c) = |q ∩ c| / |q|
tversky(q, c)     = |q ∩ c| / (|q ∩ c| + α·|q \ c| + β·|c \ q|)
```

Tversky is the general form: α weighs what the candidate lacks, β what it adds. α = β = 1 is
Jaccard; α = 1, β = 0 is containment. A small β such as 0.25 still prefers the leaner of two
candidates that both contain the query.

A pump with three parts against a variant with the same three and two more:

```sql
SELECT round(jaccard_similarity(['motor','seal','bearing'], ['motor','seal','bearing','sensor','cable']), 4) AS jaccard,
       round(containment(['motor','seal','bearing'], ['motor','seal','bearing','sensor','cable']), 4) AS query_in_candidate,
       round(containment(['motor','seal','bearing','sensor','cable'], ['motor','seal','bearing']), 4) AS candidate_in_query,
       round(tversky_similarity(['motor','seal','bearing'], ['motor','seal','bearing','sensor','cable'], 1.0, 0.25), 4) AS tversky;
-- => 0.6 | 1.0 | 0.6 | 0.8571
```

Jaccard says the two are 60 % alike. Containment says the variant has everything the pump has (1.0),
but not the other way round (0.6). Tversky with β 0.25 sits in between: complete, with a small
penalty for the two extra parts.

## Weighted Sets: Counts and Quantities

When a part appears with a count or a quantity, sets lose information: eight screws and four screws
are the same set. The weighted measures take a MAP of weights and use the elementwise minimum and
maximum:

```
weighted_jaccard(q, c)     = Σ min(q_k, c_k) / Σ max(q_k, c_k)
weighted_containment(q, c) = Σ min(q_k, c_k) / Σ q_k
```

```sql
SELECT round(weighted_jaccard(MAP {'screw': 8, 'motor': 1}, MAP {'screw': 4, 'motor': 1}), 4) AS weighted,
       jaccard_similarity(['screw', 'motor'], ['screw', 'motor']) AS as_sets;
-- => 0.5556 | 1.0
```

A weight of 0 means absent, a NULL weight counts as 1. Weights must be finite and non-negative.

## BOM Structure: the Depth Kernel

Jaccard on direct children misses structure: a product that groups three leaves into a subassembly
shares only three direct children with one that uses all six directly, although both are built from
the same leaves. The BOM kernel (`wl_kernel_similarity`, named after the Weisfeiler-Lehman family it
approximates) compares **depth fingerprints**:

1. For each material, collect every component reachable within `iterations` levels (default 3,
   at most 5). Level 1 is the direct children.
2. Weight each component by the number of distinct levels it appears at.
3. Compare the two fingerprints with weighted Jaccard. A material compared with itself is 1.

```sql
CREATE OR REPLACE TABLE bom_items AS
SELECT parent_id, child_id, quantity FROM read_csv_auto('test/data/synthetic/bom_items.csv.gz');
SELECT i AS iterations, round(wl_kernel_similarity('S10-PROD-LINEAR', 'S10-PROD-STAR', iterations := i), 4) AS similarity
FROM range(1, 4) t(i);
-- => 1 | 0.4286
-- => 2 | 0.8571
-- => 3 | 0.8571
```

With one level the kernel is Jaccard of the direct children: three shared of seven distinct (the
star's six leaves and the linear product's subassembly), 3/7. With two levels the linear product
reaches its other three leaves inside the subassembly: six shared of seven, 6/7. More levels add
nothing here, because the BOM is only two deep. The kernel reads structure as depth only: it does
not see quantities, and it does not distinguish which subassembly a leaf sits in.

## Estimates at Scale: MinHash

Comparing every pair is quadratic. MinHash replaces a set (or a weighted set, `weighted_minhash`)
by a fixed-length signature, chosen so that the share of equal positions in two signatures
(`minhash_similarity`) estimates their (weighted) Jaccard. The error shrinks with the signature
length, roughly as 1/√n:

```sql
SELECT n AS hashes,
       round(minhash_similarity(weighted_minhash(MAP {'a': 3.0, 'b': 1.0, 'c': 2.0}, n),
                                weighted_minhash(MAP {'a': 2.0, 'b': 1.0, 'd': 2.0}, n)), 3) AS estimate,
       round(weighted_jaccard(MAP {'a': 3.0, 'b': 1.0, 'c': 2.0}, MAP {'a': 2.0, 'b': 1.0, 'd': 2.0}), 3) AS exact
FROM (VALUES (16), (128), (1024)) t(n);
-- => 16 | 0.313 | 0.375
-- => 128 | 0.391 | 0.375
-- => 1024 | 0.377 | 0.375
```

Signatures are hashed, not embedded: compare them for equality only, never by cosine or L2. LSH
banding on top of them finds candidate pairs without scanning all pairs (see
[guide 08](../guides/08-scale-and-dedup.md)); the exact measure then re-scores the candidates.

## Which One to Use

| You want | Use | Why |
|---|---|---|
| Products built from mostly the same parts | Jaccard | symmetric, easy to explain |
| Products that contain my parts, extras allowed | containment (`mode := 'containment'`) | directional |
| Contain my parts, prefer lean candidates | Tversky with small β | directional with a penalty for extras |
| Counts or quantities matter | weighted Jaccard / containment, `find_covering_entities` | weights, not sets |
| Subassemblies move between levels | the BOM depth kernel | compares reachable components, not direct children |
| Millions of records | MinHash + LSH, then the exact measure | sub-quadratic candidate search |

## Why These Defaults

| Default | Why |
|---|---|
| Jaccard for material search | Both directions matter when one product replaces another |
| Tversky α = β = 1 | Equals Jaccard, so the directional form is opt-in |
| Two empty sets score 0 | No shared evidence is not similarity |
| Kernel `iterations` 3, capped at 5 | Most ERP BOMs are a few levels deep; deeper levels add cost and noise |
| Zero scores left out of searches | A material sharing nothing is not a candidate; `min_similarity := 0` keeps them |

The design history and the research behind these measures are in
[background/CONCEPT.md](../background/CONCEPT.md); it records how they were chosen, not the
current contract.
