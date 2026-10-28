# Material Similarity

> Find products with similar bills of materials, and walk the BOM structure.

**Use this guide to:**
- Choose between Jaccard (component overlap) and the WL kernel (BOM structure)
- Run `find_similar_materials_jaccard` and `find_similar_materials_wl_kernel`
- Tune `k`, `min_similarity` and the `containment` mode
- Explode a BOM, find where a part is used and list shared components

The examples use the synthetic dataset in `test/data/synthetic`. Run them from the repository root.

## Load the Data

```sql
CREATE TABLE bom_items AS
SELECT parent_id, child_id, quantity
FROM read_csv_auto('test/data/synthetic/bom_items.csv.gz');
```

The table has 426 edges under 58 parents. `FAMILY-00-BASE` has ten components. Its variants keep
90 %, 70 % and 50 % of them and add new parts.

## Component Overlap: Jaccard

Jaccard compares the sets of direct children: shared components divided by all distinct components.

```sql
SELECT material_id, round(similarity, 4) AS similarity, shared_components, total_components
FROM find_similar_materials_jaccard('FAMILY-00-BASE', 5);
```

| material_id     | similarity | shared_components | total_components |
|-----------------|-----------:|------------------:|-----------------:|
| FAMILY-00-VAR-1 | 0.8182     | 9                 | 11               |
| FAMILY-00-VAR-2 | 0.5385     | 7                 | 13               |
| FAMILY-00-VAR-3 | 0.3333     | 5                 | 15               |
| DISJ-PROD-A     | 0.0        | 0                 | 15               |
| DISJ-PROD-B     | 0.0        | 0                 | 15               |

The search fills `k` rows, also with materials that share nothing. Ties are ordered by material id.
Use `min_similarity` to drop weak matches:

```sql
SELECT material_id, round(similarity, 4) AS similarity, shared_components
FROM find_similar_materials_jaccard('FAMILY-00-BASE', 10, min_similarity := 0.5);
```

| material_id     | similarity | shared_components |
|-----------------|-----------:|------------------:|
| FAMILY-00-VAR-1 | 0.8182     | 9                 |
| FAMILY-00-VAR-2 | 0.5385     | 7                 |

### Containment Mode

Jaccard punishes a candidate for extra parts. If you ask "which material contains most of my
parts?", use `mode := 'containment'`. It returns the share of the query's components that the
candidate has.

```sql
SELECT material_id, round(similarity, 4) AS similarity, shared_components
FROM find_similar_materials_jaccard('FAMILY-00-BASE', 5, mode := 'containment', min_similarity := 0.01);
```

| material_id     | similarity | shared_components |
|-----------------|-----------:|------------------:|
| FAMILY-00-VAR-1 | 0.9        | 9                 |
| FAMILY-00-VAR-2 | 0.7        | 7                 |
| FAMILY-00-VAR-3 | 0.5        | 5                 |

Containment counts components, not quantities. For quantities, use `find_covering_entities`
(see [Coverage matching](04-coverage-matching.md)).

## BOM Structure: the WL Kernel

The Weisfeiler-Lehman (WL) kernel looks below the direct children. It compares the components that
a material reaches at each BOM level. `S10-PROD-LINEAR` and `S10-PROD-STAR` use the same six leaves.
The linear product puts three of them into a subassembly. The star product uses all six directly.

```sql
SELECT round(jaccard_similarity(
         (SELECT list(child_id) FROM bom_items WHERE parent_id = 'S10-PROD-LINEAR'),
         (SELECT list(child_id) FROM bom_items WHERE parent_id = 'S10-PROD-STAR')), 4) AS jaccard,
       round(wl_kernel_similarity('S10-PROD-LINEAR', 'S10-PROD-STAR'), 4)             AS wl;
```

| jaccard | wl     |
|--------:|-------:|
| 0.4286  | 0.8571 |

Jaccard sees only three shared direct children. The WL kernel also finds the leaves inside the
subassembly. The search function works the same way:

```sql
SELECT material_id, round(similarity, 4) AS similarity
FROM find_similar_materials_wl_kernel('S10-PROD-LINEAR', 3);
```

| material_id       | similarity |
|-------------------|-----------:|
| S10-PROD-STAR     | 0.8571     |
| S10-SUBASM-LINEAR | 0.4286     |
| DISJ-PROD-A       | 0.0        |

`iterations` (default 3) sets how many levels the kernel looks at. Values above 5 behave like 5.

### Which One to Use

| Situation | Use |
|---|---|
| Flat BOMs, one level | Jaccard. It is fast and easy to explain. |
| Deep BOMs, subassemblies that move between levels | WL kernel |
| "Does B contain my parts?" | Jaccard with `mode := 'containment'` |
| Quantities matter | `find_covering_entities` |
| Many pairs at once | The scalars `jaccard_similarity` and `wl_kernel_similarity` in a join |

## BOM Traversal

The traversal macros read the universal BOM shape: `bom_header(bom_id, parent_material_id)` and
`bom_component(bom_id, child_material_id, quantity_per)`. The ERP transforms write these tables.
For this guide, derive them from `bom_items`:

```sql
CREATE TABLE bom_header AS
SELECT DISTINCT parent_id AS bom_id, parent_id AS parent_material_id FROM bom_items;
CREATE TABLE bom_component AS
SELECT parent_id AS bom_id, child_id AS child_material_id, quantity AS quantity_per FROM bom_items;
```

One level down:

```sql
SELECT * FROM bom_explosion_1level('S11-PROD-DEEP-A');
```

| parent_material_id | child_material_id | quantity_per |
|--------------------|-------------------|-------------:|
| S11-PROD-DEEP-A    | S11-L1-00         | 1.0          |
| S11-PROD-DEEP-A    | S11-L1-01         | 1.0          |

All levels, limited by `max_depth` (default 10, at most 64):

```sql
SELECT * FROM bom_explosion_multilevel('S11-PROD-DEEP-A', max_depth := 2)
ORDER BY parent_material_id, child_material_id;
```

| parent_material_id | child_material_id | quantity_per |
|--------------------|-------------------|-------------:|
| S11-L1-00          | S11-L2-00         | 1.0          |
| S11-L1-00          | S11-L2-01         | 1.0          |
| S11-L1-01          | S11-L2-02         | 1.0          |
| S11-L1-01          | S11-L2-03         | 1.0          |
| S11-PROD-DEEP-A    | S11-L1-00         | 1.0          |
| S11-PROD-DEEP-A    | S11-L1-01         | 1.0          |

Where is a part used, and what do two products share?

```sql
SELECT parent_material_id FROM bom_where_used('S11-L1-00') ORDER BY 1;
SELECT * FROM bom_common_components('S11-PROD-DEEP-A', 'S11-PROD-DEEP-B');
```

The first query returns `S11-PROD-DEEP-A` and `S11-PROD-DEEP-B`. The second returns `S11-L1-00`,
the one direct child that both products use.

The traversal is set-based. Cycles and shared subassemblies do not make it loop or explode.

## Common Patterns

**Score many pairs at once.** The scalar works inside any query:

```sql
WITH sets AS (SELECT parent_id, list(child_id) AS parts FROM bom_items GROUP BY parent_id)
SELECT a.parent_id AS a, b.parent_id AS b, round(jaccard_similarity(a.parts, b.parts), 4) AS jaccard
FROM sets a JOIN sets b ON a.parent_id < b.parent_id
WHERE a.parent_id LIKE 'FAMILY-01%' AND b.parent_id LIKE 'FAMILY-01%'
ORDER BY jaccard DESC, a, b LIMIT 3;
```

| a               | b               | jaccard |
|-----------------|-----------------|--------:|
| FAMILY-01-BASE  | FAMILY-01-VAR-1 | 0.8182  |
| FAMILY-01-BASE  | FAMILY-01-VAR-2 | 0.5385  |
| FAMILY-01-VAR-1 | FAMILY-01-VAR-2 | 0.4286  |

**Use your own BOM table.** Pass its name: `find_similar_materials_jaccard('M-1', 10, bom_table := 'erp.bom')`.
It needs the columns `parent_id` and `child_id`.

## Pitfalls

| Pitfall | Solution |
|---|---|
| Zero-similarity rows at the end | Filter with `min_similarity`. |
| Jaccard misses components inside subassemblies | Use the WL kernel, or flatten with `bom_explosion_multilevel`. |
| `k := 10` raises a binder error | `k` is positional. |
| Unknown or misspelled material id | No error: both searches return no rows. Check the id against your BOM table. |
| Traversal macros fail with "bom_header does not exist" | Create `bom_header` and `bom_component`, or pass `header_table` and `component_table`. |

## Next Steps

- [Predecessors and cold start](03-predecessors-and-cold-start.md): add movement history
- [Coverage matching](04-coverage-matching.md): quantities and directional matching
- API: [Similarity scalars](../api/01-similarity-scalars.md), [Material search](../api/02-material-search.md), [BOM traversal](../api/03-bom-traversal.md)
- [Examples](../../examples/material_similarity)
