# BOM traversal

> Explode, trace and compare bills of materials.

[API index](../API_REFERENCE.md) · [Guides](../guides/README.md) · [Examples](../../examples/README.md)

## BOM Traversal & Navigation

### BOM Explosion - 1 Level
Returns direct children of a parent material (single-level explosion).

**Signature:**
```sql
bom_explosion_1level(
    query_material_id VARCHAR,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    parent_material_id VARCHAR,
    child_material_id VARCHAR,
    quantity_per FLOAT
)
```

**Complexity:** O(c) where c = number of direct children

**Example:**
```sql
SELECT * FROM bom_explosion_1level('ASSY-001');
```

### BOM Explosion - Multi-Level
Recursive BOM explosion with depth limit.

**Signature:**
```sql
bom_explosion_multilevel(
    query_material_id VARCHAR,
    max_depth := 10,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    parent_material_id VARCHAR,
    child_material_id VARCHAR,
    quantity_per FLOAT
)
```
> [!IMPORTANT]
> `max_depth` is clamped to a **hard ceiling of 64** regardless of the value passed in (this bounds
> traversal cost on cyclic BOMs). A BOM genuinely deeper than 64 levels is silently truncated at 64
> with no error — this is extremely rare in practice, but if you need to detect it, compare the
> returned row count against an independent depth check.

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `query_material_id` | VARCHAR | Required | Root material ID |
| `max_depth` | INTEGER | 10 | Maximum recursion depth |
| `header_table` | VARCHAR | 'bom_header' | Header table name |
| `component_table` | VARCHAR | 'bom_component' | Component table name |

**Complexity:** O(depth × edges) — the traversal is set-based (each edge is expanded at most once
per depth level, even on cyclic or multi-path/diamond BOMs), not exponential in depth.

**Example:**
```sql
SELECT * FROM bom_explosion_multilevel('PRODUCT-001', max_depth := 5);
```

### BOM Where-Used
Reverse BOM: finds all parents that use a given component.

**Signature:**
```sql
bom_where_used(
    child_material_id VARCHAR,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    parent_material_id VARCHAR,
    child_material_id VARCHAR,
    quantity_per FLOAT
)
```

**Example:**
```sql
SELECT parent_material_id
FROM bom_where_used('BEARING-X')
ORDER BY parent_material_id;
```

### Common Components
Finds components shared between two materials.

**Signature:**
```sql
bom_common_components(
    material_1 VARCHAR,
    material_2 VARCHAR,
    header_table := 'bom_header',
    component_table := 'bom_component'
) → TABLE (
    child_material_id_1 VARCHAR,
    child_material_id_2 VARCHAR
)
```

**Example:**
```sql
SELECT COUNT(*) AS common_count
FROM bom_common_components('PUMP-A', 'PUMP-B');
```

---

## Property Graph Integration

### Create BOM Property Graph
Creates DuckPGQ property graph representation.

**Signature:**
```sql
create_bom_property_graph(
    source_system := 'TEST'
) → TABLE (
    graph_name VARCHAR,
    source VARCHAR,
    num_nodes INTEGER,
    num_edges INTEGER
)
```

**Note:** This macro is pure SQL over `bom_header` / `bom_component` and does **not** require the
DuckPGQ extension. (Actual property-graph traversal with DuckPGQ syntax would require it, but this
helper only returns the graph name and node/edge counts.)

**Example:**
```sql
SELECT * FROM create_bom_property_graph(source_system := 'SAP');
```

### DFS Neighborhood Helper
Reusable depth-first traversal for BOM graphs.

**Signature:**
```sql
bom_dfs_neighborhood(
    root_material_id VARCHAR,
    max_depth := 3,
    bom_table := 'bom_items'
) → TABLE (
    component VARCHAR
)
```
> [!IMPORTANT]
> `max_depth` is clamped to a **hard ceiling of 64** (bounds traversal cost on cyclic BOMs); a value
> above that is silently treated as 64. `max_depth := N` returns components reachable within exactly
> N hops of `root_material_id`.

**Example:**
```sql
SELECT COUNT(*) AS total_components
FROM bom_dfs_neighborhood('PUMP-A', max_depth := 5);
```

---

<!-- generated:functions:start -->

## Function reference

*Generated from the extension's function metadata by `scripts/generate_api_reference.py`.*

### `bom_explosion_1level`

*Table macro*

Direct children of one parent material: returns (parent_material_id, child_material_id, quantity_per) for the immediate level only. 'header_table' and 'component_table' name the tables to read, so the macro works on any schema matching the universal BOM shape.

```sql
bom_explosion_1level(query_material_id, header_table, component_table)
```

**Examples**

```sql
SELECT * FROM bom_explosion_1level(query_material_id := 'PUMP-A');
```

### `bom_explosion_multilevel`

*Table macro*

Full multi-level BOM explosion under one parent, to 'max_depth' levels. Uses UNION rather than UNION ALL so a shared subassembly reached by many paths is expanded once -- no exponential blow-up on diamond BOMs -- and the depth gate is clamped to a hard ceiling of 64 so a cyclic BOM cannot iterate unbounded.

```sql
bom_explosion_multilevel(query_material_id, max_depth, header_table, component_table)
```

**Examples**

```sql
SELECT * FROM bom_explosion_multilevel(query_material_id := 'PUMP-A', max_depth := 5);
```

### `bom_where_used`

*Table macro*

Reverse BOM lookup: every parent material that consumes a given child, with the quantity each one uses.

```sql
bom_where_used(child_material_id, header_table, component_table)
```

**Examples**

```sql
SELECT * FROM bom_where_used(child_material_id := 'SEAL-001');
```

### `bom_common_components`

*Table macro*

Components that appear in the BOMs of both given materials -- the overlap used to judge how interchangeable two assemblies are.

```sql
bom_common_components(material_1, material_2, header_table, component_table)
```

**Examples**

```sql
SELECT * FROM bom_common_components(material_1 := 'PUMP-A', material_2 := 'PUMP-B');
```

### `create_bom_property_graph`

*Table macro*

Summarises the BOM graph for one source system as (graph_name, source, num_nodes, num_edges), counting distinct parent materials in bom_header and the components beneath them.

```sql
create_bom_property_graph(source_system)
```

**Examples**

```sql
SELECT * FROM create_bom_property_graph(source_system := 'SAP');
```

### `bom_dfs_neighborhood`

*Table macro*

All descendant components of a root material within a depth limit, as a reusable depth-first BOM traversal over a (parent_id, child_id) edge table.

```sql
bom_dfs_neighborhood(root_material_id, max_depth, bom_table)
```

**Examples**

```sql
SELECT * FROM bom_dfs_neighborhood(root_material_id := 'PUMP-A', max_depth := 3);
```

### `bom_to_items`

*Table macro*

Flattens the universal bom_header + bom_component pair into the flat (parent_id, child_id, quantity) bom_items shape that the similarity algorithms expect.

```sql
bom_to_items(header_table, component_table)
```

**Examples**

```sql
SELECT * FROM bom_to_items(header_table := 'bom_header', component_table := 'bom_component');
```

### `items_to_bom`

*Table macro*

The inverse of bom_to_items: expands a flat bom_items edge list back into the universal bom_header and bom_component shapes, tagging rows with 'source_system'.

```sql
items_to_bom(items_table, source_system)
```

**Examples**

```sql
SELECT * FROM items_to_bom(items_table := 'bom_items', source_system := 'MIGRATED');
```

<!-- generated:functions:end -->
