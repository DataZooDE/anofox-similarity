# Getting Started

> Build the extension, run a first query and learn the table shapes it expects.

**Use this guide to:**
- Build and load anofox-similarity in DuckDB
- Run a first similarity query
- Learn the input tables in one minute: BOMs, movements, feature multisets, property trees
- Pass arguments the way each function type expects them
- Use the extension on read-only databases and switch off telemetry

## Installation

The extension is not in the DuckDB community repository yet. `INSTALL anofox_similarity FROM community`
does not find it. Build it from source:

```bash
git clone --recurse-submodules https://github.com/DataZooDE/anofox-similarity.git
cd anofox-similarity
GEN=ninja make release                       # build the extension and a DuckDB shell
DATAZOO_DISABLE_TELEMETRY=1 make test        # optional: the whole SQL test suite
```

The build produces two things:

| Artifact | Use |
|---|---|
| `build/release/duckdb` | A DuckDB shell with the extension already linked. No `LOAD` needed. |
| `build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension` | The loadable extension for another DuckDB binary of the same version. |

To load the extension into a separate DuckDB binary, allow unsigned extensions:

```bash
duckdb -unsigned
```

```sql
LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
```

The extension supports DuckDB 1.5.6 and 2.0. The DuckDB binary must have the same version as the build.

## Your First Query

```sql
SELECT jaccard_similarity(['A', 'B', 'C'], ['A', 'B', 'D']) AS similarity;
```

| similarity |
|-----------:|
| 0.5        |

Two of four distinct elements are shared, so the Jaccard index is 2 / 4.

Now find similar products from a small bill of materials:

```sql
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
  ('PUMP-A', 'SEAL-001'), ('PUMP-A', 'BEARING-X'), ('PUMP-A', 'MOTOR-M1'),
  ('PUMP-B', 'SEAL-001'), ('PUMP-B', 'BEARING-X'), ('PUMP-B', 'MOTOR-M1'), ('PUMP-B', 'GASKET-Y'),
  ('PUMP-C', 'FILTER-F1'), ('PUMP-C', 'VALVE-V2');

SELECT material_id, similarity, shared_components, total_components
FROM find_similar_materials_jaccard('PUMP-A', 3);
```

| material_id | similarity | shared_components | total_components |
|-------------|-----------:|------------------:|-----------------:|
| PUMP-B      | 0.75       | 3                 | 4                |
| PUMP-C      | 0.0        | 0                 | 5                |

The search reads the table `bom_items` by default. Pass `bom_table := 'my_table'` to read another one.

## The Data Model in One Minute

Every function reads plain tables. You map your ERP data to these shapes with SQL.

| Shape | Columns | Used by |
|---|---|---|
| BOM edges | `bom_items(parent_id, child_id[, quantity])` | Jaccard and WL search, cold start, predecessors |
| Goods movements | `goods_movements(material_id, movement_date, quantity)` | `infer_predecessors`, `cold_start_analogs` |
| Feature multisets | `(entity_id, feature_id, weight)` | `find_covering_entities`, `minhash_index` |
| Property trees | `tree_nodes(tree_id, node_id, parent_id, node_kind, edge_type, label, class_id, count[, source, context])` and `tree_attributes(tree_id, node_id, attr_id, name, dimension, unit, lo, hi[, bound, category])` | `tree_coverage`, `validate_trees`, `classify_to_catalogue` |
| Universal BOM | `bom_header(bom_id, parent_material_id, ...)` and `bom_component(bom_id, child_material_id, quantity_per, ...)` | `bom_explosion_*`, `bom_where_used`, `bom_common_components` |

Notes:

- A BOM table is a list of parent-child edges. One row per component.
- A feature multiset is the general form of a BOM: an entity, a feature and a weight (a count or
  a quantity). Column mapping parameters let you use an existing BOM table as is.
- A property tree describes a specification or an offer: containers group items, items carry
  attributes with values in base units (m, s, A, K, ...).
- The SAP and Dynamics 365 transforms write the universal BOM shape. `bom_to_items()` flattens it to
  `bom_items`.

## Argument Binding

There are two kinds of functions, and they bind arguments differently.

**Table functions** (`SELECT * FROM f(...)`) bind named arguments by name. The leading arguments
(the material id, `k`) are positional:

```sql
SELECT material_id, similarity
FROM find_similar_materials_jaccard('PUMP-A', 3, min_similarity := 0.5, bom_table := 'bom_items');
```

**Scalar functions** (`SELECT f(...)`) bind named arguments by position on DuckDB 1.5.6. The name
is ignored. Pass the arguments in the documented order. DuckDB 2.0 binds them by name.

```sql
SELECT tversky_similarity(['a', 'b'], ['a', 'b', 'c', 'd'], 1.0, 0.0)                 AS positional,
       tversky_similarity(['a', 'b'], ['a', 'b', 'c', 'd'], alpha := 1.0, beta := 0.0) AS in_order,
       tversky_similarity(['a', 'b'], ['a', 'b', 'c', 'd'], beta := 0.0, alpha := 1.0) AS swapped;
```

On DuckDB 1.5.6:

| positional | in_order | swapped |
|-----------:|---------:|--------:|
| 1.0        | 1.0      | 0.5     |

The last call gives `beta = 1.0` and `alpha = 0.0` on 1.5.6. Always write scalar arguments in the
order of the signature, also when you name them.

## Read-Only Databases

The extension loads on a database that is opened read-only. All search, coverage and evaluation
functions work there. Only the embedding tables, which the extension creates on a writable
database, are missing.

```bash
duckdb -readonly -unsigned plant.db
```

```sql
LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';
SELECT * FROM find_similar_materials_jaccard('PUMP-A', 5);
```

Table functions cannot write. Store their results yourself with `CREATE TABLE ... AS SELECT` on a
writable database.

## Telemetry

The extension sends an anonymous usage event when it loads. To switch this off, set an
environment variable before you start DuckDB:

```bash
export DATAZOO_DISABLE_TELEMETRY=1
./build/release/duckdb
```

The setting `SET anofox_telemetry_enabled = false` switches off later events. It cannot suppress
the load event, because the extension is already loaded when the statement runs. Use the
environment variable for a complete opt-out.

## Key Concepts

| Concept | Meaning |
|---|---|
| Similarity | Symmetric: how alike are A and B? Jaccard and the WL kernel. |
| Coverage | Directional: does the offer contain what the request asks for? |
| Positional leading arguments | Material ids, `k` and query ids are always positional. |
| Table names as strings | Tables are passed as names, for example `bom_table := 'my_bom'`. Schema-qualified names work. Names that need quoting (spaces, hyphens) are rejected with `Invalid identifier`; rename such tables or create a view with a plain name. |
| No models shipped | Text classification uses vectors that you compute yourself. |

## Common Pitfalls

| Pitfall | Solution |
|---|---|
| `INSTALL ... FROM community` fails | Build from source and `LOAD` the file. |
| Unsigned extension refused | Start DuckDB with `-unsigned`. |
| Named scalar arguments in the wrong order | On 1.5.6, write them in signature order. |
| `k := 5` on a search function | `k` is positional: `find_similar_materials_jaccard('PUMP-A', 5)`. |
| Function not found with `anofox_similarity.` prefix | Call functions unqualified. |

## Next Steps

- [Material similarity](02-material-similarity.md): Jaccard, WL kernel and BOM traversal
- [Predecessors and cold start](03-predecessors-and-cold-start.md): use movement history
- [Coverage matching](04-coverage-matching.md): match requests against past solutions
- [Conventions](../api/00-conventions.md): naming, argument binding, NULL handling
- [Examples](../../examples/material_similarity): runnable SQL scripts
