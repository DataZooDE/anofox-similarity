# Data Structures Architecture for anofox-similarity

Multi-modal product similarity detection in DuckDB requires a carefully designed schema that abstracts ERP-specific BOM representations, leverages native columnar storage for embeddings, and integrates graph structures for structural similarity. This architecture enables **Mittelstand-scale** deployments (10,000-50,000 materials) to perform Jaccard, Weisfeiler-Lehman kernel, and embedding-based similarity computations within a single DuckDB instance.

The design centers on three pillars: a universal BOM abstraction layer accepting data from SAP, Dynamics 365, Oracle, and other ERPs; native DuckDB data structures optimized for **FLOAT[N] embedding storage** with HNSW indexing; and graph representations using DuckPGQ property graphs with CSR-backed traversal. Total memory footprint for 50,000 materials with multi-modal embeddings is approximately **200-250 MB**, well within single-node constraints.

---

## ERP-agnostic BOM abstraction layer

Manufacturing companies run diverse ERP systems with incompatible BOM representations. SAP uses MAST/STPO tables with alternative BOMs and change numbers; Dynamics 365 employs BOMTable/BOMVersion with quantity-based selection; Oracle ERP Cloud stores structures in EGP_STRUCTURES_B with effectivity control. Despite these differences, all systems share common semantics: parent-child relationships, quantities per assembly, validity periods, and component classifications.

The universal schema captures these common denominators while preserving ERP-specific metadata:

```sql
-- Universal BOM Header: One row per BOM definition
CREATE TABLE bom_header (
    bom_id              VARCHAR PRIMARY KEY,
    source_system       VARCHAR NOT NULL,      -- SAP, D365, ORACLE, INFOR, EPICOR, IFS
    source_bom_id       VARCHAR,
    parent_material_id  VARCHAR NOT NULL,
    bom_type            VARCHAR,               -- MANUFACTURING, ENGINEERING, PLANNING
    alternative_number  VARCHAR,
    revision            VARCHAR,
    base_quantity       DECIMAL(18,6) DEFAULT 1,
    base_uom            VARCHAR(10),
    valid_from          DATE,
    valid_to            DATE,
    plant_id            VARCHAR(20),
    is_approved         BOOLEAN DEFAULT FALSE,
    created_at          TIMESTAMP DEFAULT current_timestamp,
    CONSTRAINT fk_parent FOREIGN KEY (parent_material_id) 
        REFERENCES materials(material_id)
);

-- Universal BOM Components: Parent-child relationships with quantities
CREATE TABLE bom_component (
    component_id         VARCHAR PRIMARY KEY,
    bom_id               VARCHAR NOT NULL REFERENCES bom_header(bom_id),
    line_number          INTEGER NOT NULL,
    child_material_id    VARCHAR NOT NULL,
    quantity_per         DECIMAL(18,6) NOT NULL,
    quantity_uom         VARCHAR(10) NOT NULL,
    is_fixed_quantity    BOOLEAN DEFAULT FALSE,
    scrap_percent        DECIMAL(8,4) DEFAULT 0,
    effective_from       DATE,
    effective_to         DATE,
    component_type       VARCHAR(20),          -- STOCK, PHANTOM, NON_STOCK, BULK
    supply_type          VARCHAR(20),          -- PUSH, PULL, ASSEMBLY_PULL
    operation_sequence   INTEGER,
    is_alternative       BOOLEAN DEFAULT FALSE,
    alternative_group    VARCHAR(20),
    created_at           TIMESTAMP DEFAULT current_timestamp,
    CONSTRAINT fk_child FOREIGN KEY (child_material_id) 
        REFERENCES materials(material_id)
);

-- Material Master: Core entity for all products/components
CREATE TABLE materials (
    material_id          VARCHAR PRIMARY KEY,
    material_number      VARCHAR UNIQUE NOT NULL,
    description          VARCHAR(500),
    material_type        VARCHAR(20),          -- RAW, COMPONENT, ASSEMBLY, FINISHED
    material_group       VARCHAR(50),
    procurement_type     VARCHAR(20),          -- MAKE, BUY, BOTH
    base_uom             VARCHAR(10),
    weight               DECIMAL(12,4),
    cost_per_unit        DECIMAL(12,4),
    lead_time_days       INTEGER,
    source_system        VARCHAR(20),
    is_active            BOOLEAN DEFAULT TRUE,
    created_at           TIMESTAMP DEFAULT current_timestamp
);
```

### SAP transformation mapping

SAP's four-table hierarchy (MAST, STKO, STPO, STAS) maps to the universal schema through field-level transformations:

```sql
-- SAP BOM Header Transformation (MAST + STKO → bom_header)
INSERT INTO bom_header (bom_id, source_system, source_bom_id, parent_material_id, 
                        bom_type, alternative_number, base_quantity, base_uom, 
                        valid_from, plant_id, is_approved)
SELECT 
    'SAP_' || STKO.STLNR || '_' || STKO.STLAL AS bom_id,
    'SAP',
    STKO.STLNR,
    MAST.MATNR,
    CASE MAST.STLAN 
        WHEN '1' THEN 'MANUFACTURING'
        WHEN '2' THEN 'ENGINEERING'
        WHEN '5' THEN 'SALES'
        ELSE 'UNIVERSAL'
    END,
    STKO.STLAL,
    STKO.BMENG,
    STKO.BMEIN,
    STKO.DATUV,
    MAST.WERKS,
    STKO.STLST = '1'
FROM sap_mast MAST
JOIN sap_stko STKO ON MAST.STLNR = STKO.STLNR AND MAST.STLAL = STKO.STLAL
WHERE STKO.LOEKZ != 'X';

-- SAP BOM Component Transformation (STPO → bom_component)
INSERT INTO bom_component (component_id, bom_id, line_number, child_material_id,
                           quantity_per, quantity_uom, is_fixed_quantity, 
                           scrap_percent, effective_from, component_type, is_alternative)
SELECT
    'SAP_' || STPO.STLNR || '_' || STPO.STLKN,
    'SAP_' || STPO.STLNR || '_' || STAS.STLAL,
    STPO.STLKN,
    STPO.IDNRK,
    STPO.MENGE,
    STPO.MEINS,
    STPO.FMENG = 'X',
    STPO.AUSCH,
    STPO.DATUV,
    CASE STPO.POSTP
        WHEN 'L' THEN 'STOCK'
        WHEN 'N' THEN 'NON_STOCK'
        WHEN 'T' THEN 'TEXT'
        ELSE 'STOCK'
    END,
    STPO.ALPOS = 'X'
FROM sap_stpo STPO
JOIN sap_stas STAS ON STPO.STLNR = STAS.STLNR AND STPO.STLKN = STAS.STLKN;
```

### Dynamics 365 transformation mapping

Dynamics 365's three-table model (BOMTable, BOMVersion, BOM) uses version-based effectivity:

```sql
-- D365 BOM Header Transformation
INSERT INTO bom_header (bom_id, source_system, source_bom_id, parent_material_id,
                        bom_type, valid_from, valid_to, is_approved)
SELECT
    'D365_' || BOMVersion.BOMId || '_' || BOMVersion.ItemId,
    'D365',
    BOMTable.BOMId,
    BOMVersion.ItemId,
    CASE BOMTable.BOMType WHEN 0 THEN 'MANUFACTURING' WHEN 4 THEN 'FORMULA' END,
    BOMVersion.FromDate,
    BOMVersion.ToDate,
    BOMVersion.Approved
FROM d365_bomversion BOMVersion
JOIN d365_bomtable BOMTable ON BOMVersion.BOMId = BOMTable.BOMId
WHERE BOMVersion.Active = 1;

-- D365 BOM Component Transformation
INSERT INTO bom_component (component_id, bom_id, line_number, child_material_id,
                           quantity_per, quantity_uom, scrap_percent, component_type)
SELECT
    'D365_' || BOM.BOMId || '_' || BOM.LineNum,
    'D365_' || BOMVersion.BOMId || '_' || BOMVersion.ItemId,
    BOM.LineNum,
    BOM.ItemId,
    BOM.BOMQty,
    BOM.UnitId,
    BOM.ScrapVar,
    CASE BOM.BOMType
        WHEN 0 THEN 'STOCK' WHEN 1 THEN 'PHANTOM' WHEN 2 THEN 'COPRODUCT'
    END
FROM d365_bom BOM
JOIN d365_bomversion BOMVersion ON BOM.BOMId = BOMVersion.BOMId;
```

---

## DuckDB native data structures for embeddings

DuckDB's columnar storage excels at fixed-dimension embedding workloads. The **FLOAT[N] ARRAY type** stores embedding vectors with contiguous memory layout, enabling SIMD-accelerated distance computations. For anofox-similarity's three embedding types—structural (256D), textual (384D), and transactional (128D)—this translates to predictable memory usage and excellent cache locality.

### Multi-modal embedding schema

```sql
-- Multi-modal embeddings per material
CREATE TABLE material_embeddings (
    material_id          VARCHAR NOT NULL REFERENCES materials(material_id),
    
    -- Structural embedding: BOM topology via WL kernel (256 dimensions)
    structural_embedding FLOAT[256],
    structural_model     VARCHAR(50),
    structural_updated   TIMESTAMP,
    
    -- Textual embedding: Description/specs similarity (384 dimensions)  
    textual_embedding    FLOAT[384],
    textual_model        VARCHAR(50),
    textual_updated      TIMESTAMP,
    
    -- Transactional embedding: Usage patterns (128 dimensions)
    transactional_embedding FLOAT[128],
    transactional_model  VARCHAR(50),
    transactional_updated TIMESTAMP,
    
    PRIMARY KEY (material_id)
);

-- Alternative: Normalized embedding storage (flexible for new embedding types)
CREATE TABLE material_embeddings_normalized (
    material_id          VARCHAR NOT NULL,
    embedding_type       VARCHAR NOT NULL,    -- 'structural', 'textual', 'transactional'
    embedding            FLOAT[384],          -- Padded to max dimension
    actual_dimensions    INTEGER,
    model_version        VARCHAR(50),
    created_at           TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (material_id, embedding_type)
);
```

### Memory footprint estimates

For **50,000 materials** with all three embedding types:

| Embedding Type | Dimensions | Bytes/Row | 50K Materials | With HNSW Index |
|----------------|------------|-----------|---------------|-----------------|
| Structural     | 256        | 1,024     | ~49 MB        | ~75 MB          |
| Textual        | 384        | 1,536     | ~73 MB        | ~110 MB         |
| Transactional  | 128        | 512       | ~24 MB        | ~36 MB          |
| **Total**      |            |           | **~146 MB**   | **~221 MB**     |

HNSW index overhead is approximately **30-50%** of raw embedding storage, allocated outside DuckDB's memory_limit parameter.

### Configuration for embedding workloads

```sql
-- Production configuration for 50K materials
SET memory_limit = '8GB';
SET threads = 8;
SET temp_directory = './duckdb_temp';

-- Enable VSS persistence (experimental)
INSTALL vss;
LOAD vss;
SET hnsw_enable_experimental_persistence = true;
```

---

## DuckDB VSS extension integration

The VSS extension provides **HNSW (Hierarchical Navigable Small World)** indexes built on the usearch library, enabling sub-linear nearest neighbor search. For manufacturing similarity workloads, HNSW indexes dramatically accelerate embedding-based queries from O(N²) cross-joins to approximately O(log N) per query.

### HNSW index creation with optimal parameters

```sql
-- Structural embedding index: Lower M for lower dimensions
CREATE INDEX structural_hnsw_idx ON material_embeddings 
USING HNSW (structural_embedding)
WITH (
    metric = 'cosine',
    ef_construction = 200,    -- Build quality (higher = better recall)
    ef_search = 100,          -- Search quality (higher = slower but more accurate)
    M = 24                    -- Graph connectivity (24 optimal for 256D)
);

-- Textual embedding index: Higher M for higher dimensions
CREATE INDEX textual_hnsw_idx ON material_embeddings 
USING HNSW (textual_embedding)
WITH (
    metric = 'cosine',
    ef_construction = 256,
    ef_search = 128,
    M = 32                    -- Higher connectivity for 384D
);

-- Transactional embedding index: Lower parameters for lower dimensions
CREATE INDEX transactional_hnsw_idx ON material_embeddings 
USING HNSW (transactional_embedding)
WITH (
    metric = 'l2sq',          -- L2 for pattern similarity
    ef_construction = 128,
    ef_search = 64,
    M = 16                    -- Lower connectivity for 128D
);
```

### Single-modal similarity queries

```sql
-- Find top-10 similar materials by textual embedding
SELECT 
    m.material_number,
    m.description,
    array_cosine_distance(e.textual_embedding, $query_vec::FLOAT[384]) AS distance
FROM materials m
JOIN material_embeddings e ON m.material_id = e.material_id
ORDER BY distance
LIMIT 10;

-- Verify HNSW index usage
EXPLAIN SELECT * FROM material_embeddings
ORDER BY array_cosine_distance(textual_embedding, $query_vec::FLOAT[384])
LIMIT 10;
-- Look for HNSW_INDEX_SCAN in plan
```

### Multi-modal fusion with Reciprocal Rank Fusion (RRF)

```sql
-- Multi-modal similarity: Combine structural, textual, and transactional signals
WITH structural_results AS (
    SELECT material_id,
           ROW_NUMBER() OVER (ORDER BY array_cosine_distance(
               structural_embedding, $struct_query::FLOAT[256])) AS rank
    FROM material_embeddings
    ORDER BY array_cosine_distance(structural_embedding, $struct_query::FLOAT[256])
    LIMIT 100
),
textual_results AS (
    SELECT material_id,
           ROW_NUMBER() OVER (ORDER BY array_cosine_distance(
               textual_embedding, $text_query::FLOAT[384])) AS rank
    FROM material_embeddings
    ORDER BY array_cosine_distance(textual_embedding, $text_query::FLOAT[384])
    LIMIT 100
),
transactional_results AS (
    SELECT material_id,
           ROW_NUMBER() OVER (ORDER BY array_distance(
               transactional_embedding, $trans_query::FLOAT[128])) AS rank
    FROM material_embeddings
    ORDER BY array_distance(transactional_embedding, $trans_query::FLOAT[128])
    LIMIT 100
)
SELECT 
    COALESCE(s.material_id, t.material_id, tr.material_id) AS material_id,
    -- RRF with configurable weights (k=60 is standard)
    (1.0 / (60 + COALESCE(s.rank, 1000))) * 0.4 +   -- 40% structural
    (1.0 / (60 + COALESCE(t.rank, 1000))) * 0.4 +   -- 40% textual  
    (1.0 / (60 + COALESCE(tr.rank, 1000))) * 0.2    -- 20% transactional
    AS rrf_score
FROM structural_results s
FULL OUTER JOIN textual_results t ON s.material_id = t.material_id
FULL OUTER JOIN transactional_results tr ON COALESCE(s.material_id, t.material_id) = tr.material_id
ORDER BY rrf_score DESC
LIMIT 20;
```

### Batch similarity with LATERAL joins

```sql
-- Find top-5 similar materials for each material in a batch
SELECT 
    source.material_id AS source_material,
    list(similar.material_id ORDER BY similar.distance) AS similar_materials
FROM query_materials source, LATERAL (
    SELECT e.material_id, 
           array_cosine_distance(source.textual_embedding, e.textual_embedding) AS distance
    FROM material_embeddings e
    WHERE e.material_id != source.material_id
    ORDER BY distance
    LIMIT 5
) similar
GROUP BY source.material_id;
```

---

## DuckPGQ extension for BOM graph queries

DuckPGQ implements SQL/PGQ (Property Graph Queries) from the SQL:2023 standard, enabling graph pattern matching directly on relational tables. BOMs naturally map to property graphs: materials become vertices, BOM relationships become directed edges with quantity properties.

### Property graph creation

```sql
-- Install DuckPGQ extension
INSTALL duckpgq FROM community;
LOAD duckpgq;

-- Create property graph over BOM tables
CREATE PROPERTY GRAPH bom_graph
VERTEX TABLES (
    materials 
        LABEL Material
        PROPERTIES (material_id, material_number, description, material_type, material_group)
)
EDGE TABLES (
    bom_component
        SOURCE KEY (bom_id) REFERENCES bom_header (bom_id)
        DESTINATION KEY (child_material_id) REFERENCES materials (material_id)
        LABEL contains
        PROPERTIES (quantity_per, component_type, is_alternative)
);

-- Alternative: Direct edge table without BOM header indirection
CREATE TABLE bom_edges AS
SELECT 
    h.parent_material_id AS parent_id,
    c.child_material_id AS child_id,
    c.quantity_per,
    c.component_type,
    c.line_number AS sequence
FROM bom_header h
JOIN bom_component c ON h.bom_id = c.bom_id;

CREATE PROPERTY GRAPH direct_bom_graph
VERTEX TABLES (
    materials LABEL Material
)
EDGE TABLES (
    bom_edges
        SOURCE KEY (parent_id) REFERENCES materials (material_id)
        DESTINATION KEY (child_id) REFERENCES materials (material_id)
        LABEL contains
        PROPERTIES (quantity_per, component_type, sequence)
);
```

### BOM traversal patterns

```sql
-- Single-level BOM explosion: Direct children
FROM GRAPH_TABLE (direct_bom_graph
    MATCH (parent:Material WHERE parent.material_number = 'ASSY-001')
          -[r:contains]->(child:Material)
    COLUMNS (
        parent.material_number AS parent,
        child.material_number AS child,
        child.description,
        r.quantity_per AS quantity
    )
);

-- Multi-level BOM explosion (3 levels)
FROM GRAPH_TABLE (direct_bom_graph
    MATCH (top:Material WHERE top.material_number = 'FINISHED-001')
          -[r1:contains]->(level1:Material)
          -[r2:contains]->(level2:Material)
          -[r3:contains]->(level3:Material)
    COLUMNS (
        top.material_number,
        level1.material_number AS l1,
        level2.material_number AS l2, 
        level3.material_number AS l3,
        r1.quantity_per * r2.quantity_per * r3.quantity_per AS cumulative_qty
    )
);

-- Variable-length path: Find all raw materials reachable from assembly
FROM GRAPH_TABLE (direct_bom_graph
    MATCH p = ANY SHORTEST (assy:Material WHERE assy.material_number = 'ASSY-001')
               -[:contains]->{1,10}
               (raw:Material WHERE raw.material_type = 'RAW')
    COLUMNS (
        assy.material_number AS assembly,
        raw.material_number AS raw_material,
        path_length(p) AS depth
    )
);

-- Where-used query: Find all assemblies using a component
FROM GRAPH_TABLE (direct_bom_graph
    MATCH p = ANY SHORTEST (component:Material WHERE component.material_number = 'COMP-123')
               <-[:contains]-{1,10}
               (parent:Material WHERE parent.material_type = 'ASSEMBLY')
    COLUMNS (
        component.material_number,
        parent.material_number AS used_in,
        path_length(p) AS levels_up
    )
);

-- Common components across assemblies (diamond pattern)
FROM GRAPH_TABLE (direct_bom_graph
    MATCH (a1:Material)-[r1:contains]->(shared:Material),
          (a2:Material)-[r2:contains]->(shared)
    WHERE a1.material_id < a2.material_id  -- Avoid duplicates
      AND a1.material_type = 'ASSEMBLY'
      AND a2.material_type = 'ASSEMBLY'
    COLUMNS (
        a1.material_number AS assembly_1,
        a2.material_number AS assembly_2,
        shared.material_number AS common_component
    )
);
```

### Weisfeiler-Lehman neighborhood extraction

The WL kernel requires extracting k-hop neighborhoods and iteratively refining node labels. DuckPGQ enables this through pattern matching:

```sql
-- Extract 1-hop neighborhood signatures for WL kernel iteration
WITH node_neighborhoods AS (
    FROM GRAPH_TABLE (direct_bom_graph
        MATCH (center:Material)-[e:contains]->(neighbor:Material)
        COLUMNS (
            center.material_id AS node,
            center.material_type AS center_label,
            neighbor.material_type AS neighbor_label,
            e.component_type AS edge_type
        )
    )
)
SELECT 
    node,
    center_label || '||' || 
    STRING_AGG(edge_type || ':' || neighbor_label ORDER BY neighbor_label, ',')
    AS wl_signature
FROM node_neighborhoods
GROUP BY node, center_label;

-- Full WL iteration using recursive aggregation
CREATE TABLE wl_labels_0 AS
SELECT material_id AS node, 
       material_type || '|' || material_group AS label
FROM materials;

-- WL Iteration 1: Aggregate neighbor labels
CREATE TABLE wl_labels_1 AS
WITH neighbor_labels AS (
    FROM GRAPH_TABLE (direct_bom_graph
        MATCH (center:Material)-[:contains]-(neighbor:Material)
        COLUMNS (center.material_id AS node_id, neighbor.material_id AS neighbor_id)
    )
)
SELECT 
    n.node_id AS node,
    MD5(wl.label || '|' || COALESCE(STRING_AGG(nl.label ORDER BY nl.label), ''))
    AS label
FROM wl_labels_0 wl
LEFT JOIN neighbor_labels n ON n.node_id = wl.node
LEFT JOIN wl_labels_0 nl ON n.neighbor_id = nl.node
GROUP BY n.node_id, wl.label;
```

---

## Graph data structures in C++

For the DuckDB extension implementation, **CSR (Compressed Sparse Row)** provides optimal memory efficiency and cache performance for BOM traversal. The structure uses three arrays: row_ptr (vertex offsets), col_idx (edge destinations), and quantities (edge weights).

### CSR implementation

```cpp
#include <vector>
#include <cstdint>
#include <algorithm>
#include <tuple>

// CSR Graph optimized for BOM traversal
struct CSRBOMGraph {
    std::vector<uint32_t> row_ptr;      // Size: num_nodes + 1
    std::vector<uint32_t> col_idx;      // Size: num_edges  
    std::vector<float> quantities;       // Size: num_edges
    uint32_t num_nodes;
    uint32_t num_edges;
    
    // O(1) access to children count
    inline uint32_t out_degree(uint32_t v) const {
        return row_ptr[v + 1] - row_ptr[v];
    }
    
    // Iterate children with quantities
    template<typename Func>
    void for_each_child(uint32_t v, Func&& f) const {
        for (uint32_t i = row_ptr[v]; i < row_ptr[v + 1]; ++i) {
            f(col_idx[i], quantities[i]);
        }
    }
    
    // Build from edge list: O(E log E) total
    static CSRBOMGraph build(
        uint32_t num_nodes,
        std::vector<std::tuple<uint32_t, uint32_t, float>>& edges) 
    {
        std::sort(edges.begin(), edges.end());
        
        CSRBOMGraph graph;
        graph.num_nodes = num_nodes;
        graph.num_edges = edges.size();
        graph.row_ptr.resize(num_nodes + 1, 0);
        graph.col_idx.resize(edges.size());
        graph.quantities.resize(edges.size());
        
        // Count edges per vertex
        for (const auto& [src, dst, qty] : edges) {
            graph.row_ptr[src + 1]++;
        }
        
        // Prefix sum for offsets
        for (uint32_t i = 1; i <= num_nodes; ++i) {
            graph.row_ptr[i] += graph.row_ptr[i - 1];
        }
        
        // Fill arrays
        std::vector<uint32_t> pos = graph.row_ptr;
        for (const auto& [src, dst, qty] : edges) {
            uint32_t idx = pos[src]++;
            graph.col_idx[idx] = dst;
            graph.quantities[idx] = qty;
        }
        
        return graph;
    }
};

// Bidirectional graph for where-used queries
struct BidirectionalBOMGraph {
    CSRBOMGraph forward;   // Parent -> Children
    CSRBOMGraph reverse;   // Child -> Parents (CSC)
    
    static BidirectionalBOMGraph build(
        uint32_t num_nodes,
        std::vector<std::tuple<uint32_t, uint32_t, float>>& edges)
    {
        BidirectionalBOMGraph g;
        g.forward = CSRBOMGraph::build(num_nodes, edges);
        
        // Transpose for reverse
        std::vector<std::tuple<uint32_t, uint32_t, float>> reversed;
        reversed.reserve(edges.size());
        for (const auto& [src, dst, qty] : edges) {
            reversed.emplace_back(dst, src, qty);
        }
        g.reverse = CSRBOMGraph::build(num_nodes, reversed);
        
        return g;
    }
};
```

### Memory footprint for CSR

For **50,000 nodes** and **200,000 edges** (typical Mittelstand BOM):

```
row_ptr:     51,001 × 4 bytes =  204,004 bytes ≈ 199 KB
col_idx:    200,000 × 4 bytes =  800,000 bytes ≈ 781 KB
quantities: 200,000 × 4 bytes =  800,000 bytes ≈ 781 KB
───────────────────────────────────────────────────────
Total CSR:                     1,804,004 bytes ≈ 1.72 MB
Bidirectional (CSR + CSC):                     ≈ 3.44 MB
```

### BOM explosion algorithm

```cpp
#include <queue>
#include <unordered_map>

struct BOMExplosionResult {
    std::vector<uint32_t> materials;
    std::vector<float> cumulative_quantities;
    std::vector<uint32_t> levels;
};

BOMExplosionResult bom_explosion(
    const CSRBOMGraph& bom,
    uint32_t root,
    float initial_qty = 1.0f,
    uint32_t max_depth = 100)
{
    BOMExplosionResult result;
    std::vector<bool> visited(bom.num_nodes, false);
    
    // BFS with quantity propagation
    std::queue<std::tuple<uint32_t, float, uint32_t>> frontier;
    frontier.emplace(root, initial_qty, 0);
    visited[root] = true;
    
    while (!frontier.empty()) {
        auto [node, cum_qty, level] = frontier.front();
        frontier.pop();
        
        if (level > max_depth) continue;
        
        result.materials.push_back(node);
        result.cumulative_quantities.push_back(cum_qty);
        result.levels.push_back(level);
        
        bom.for_each_child(node, [&](uint32_t child, float edge_qty) {
            if (!visited[child]) {
                visited[child] = true;
                frontier.emplace(child, cum_qty * edge_qty, level + 1);
            }
        });
    }
    
    return result;
}
```

---

## Extension interface design

The anofox-similarity extension exposes similarity functions through a consistent SQL interface. All similarity methods return a unified result schema enabling interchangeable use.

### Scalar functions for pairwise similarity

```cpp
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

// Jaccard similarity for component sets
static void JaccardSimilarityFunction(DataChunk &args, ExpressionState &state, 
                                      Vector &result) {
    auto &set1 = args.data[0];
    auto &set2 = args.data[1];
    auto count = args.size();
    
    UnifiedVectorFormat set1_data, set2_data;
    set1.ToUnifiedFormat(count, set1_data);
    set2.ToUnifiedFormat(count, set2_data);
    
    auto result_data = FlatVector::GetData<double>(result);
    
    for (idx_t i = 0; i < count; i++) {
        auto idx1 = set1_data.sel->get_index(i);
        auto idx2 = set2_data.sel->get_index(i);
        
        if (!set1_data.validity.RowIsValid(idx1) || 
            !set2_data.validity.RowIsValid(idx2)) {
            FlatVector::SetNull(result, i, true);
            continue;
        }
        
        // Compute Jaccard: |A ∩ B| / |A ∪ B|
        result_data[i] = ComputeJaccard(
            ListVector::GetEntry(set1),
            ListVector::GetEntry(set2));
    }
}

// Registration
ScalarFunctionSet GetJaccardFunctions() {
    ScalarFunctionSet funcs("jaccard_similarity");
    
    // For LIST[BIGINT] component ID sets
    funcs.AddFunction(ScalarFunction(
        {LogicalType::LIST(LogicalType::BIGINT), 
         LogicalType::LIST(LogicalType::BIGINT)},
        LogicalType::DOUBLE,
        JaccardSimilarityFunction));
    
    // For LIST[VARCHAR] component name sets
    funcs.AddFunction(ScalarFunction(
        {LogicalType::LIST(LogicalType::VARCHAR), 
         LogicalType::LIST(LogicalType::VARCHAR)},
        LogicalType::DOUBLE,
        JaccardTextFunction));
    
    return funcs;
}
```

### Table function for batch similarity

```cpp
// Unified result schema for all similarity methods
struct SimilarityResult {
    int64_t source_id;
    int64_t target_id;
    double score;
    string algorithm;
    string metadata_json;
};

struct SimilarityBindData : public FunctionData {
    string source_table;
    string algorithm;      // 'jaccard', 'wl_kernel', 'embedding', 'combined'
    double threshold;
    int64_t top_k;
    vector<string> columns;
    
    unique_ptr<FunctionData> Copy() const override {
        return make_uniq<SimilarityBindData>(*this);
    }
    bool Equals(const FunctionData &other) const override {
        return source_table == other.Cast<SimilarityBindData>().source_table;
    }
};

static unique_ptr<FunctionData> SimilarityBind(
    ClientContext &context,
    TableFunctionBindInput &input,
    vector<LogicalType> &return_types,
    vector<string> &names) 
{
    auto bind_data = make_uniq<SimilarityBindData>();
    bind_data->source_table = input.inputs[0].GetValue<string>();
    bind_data->algorithm = input.inputs[1].GetValue<string>();
    bind_data->threshold = input.inputs[2].GetValue<double>();
    
    // Unified output schema
    names = {"source_id", "target_id", "similarity_score", "algorithm", "metadata"};
    return_types = {
        LogicalType::BIGINT,
        LogicalType::BIGINT,
        LogicalType::DOUBLE,
        LogicalType::VARCHAR,
        LogicalType::JSON
    };
    
    return std::move(bind_data);
}

// Usage from SQL
// SELECT * FROM find_similar('materials', 'combined', 0.7);
```

### SQL function registration

```sql
-- Scalar functions (registered by extension)
SELECT jaccard_similarity(
    (SELECT list(child_id) FROM bom_edges WHERE parent_id = 1),
    (SELECT list(child_id) FROM bom_edges WHERE parent_id = 2)
);

SELECT wl_kernel_similarity(bom_graph_1, bom_graph_2, iterations := 3);

SELECT embedding_similarity(
    e1.structural_embedding, 
    e2.structural_embedding, 
    metric := 'cosine'
) FROM material_embeddings e1, material_embeddings e2;

-- Table function for Jaccard-based batch processing
SELECT * FROM find_similar_materials_jaccard(
    query_material_id := 'MAT-001',
    k := 20,
    min_similarity := 0.7
)
WHERE similarity > 0.8
ORDER BY similarity DESC;

-- Table function for WL kernel-based batch processing
SELECT * FROM find_similar_materials_wl_kernel(
    query_material_id := 'MAT-001',
    k := 20,
    iterations := 3,
    min_similarity := 0.7
)
WHERE similarity > 0.8
ORDER BY similarity DESC;
```

---

## Complete schema for anofox-similarity

```sql
-- =============================================================================
-- ANOFOX-SIMILARITY: Complete DuckDB Schema
-- =============================================================================

-- Core Material Master
CREATE TABLE materials (
    material_id          VARCHAR PRIMARY KEY,
    material_number      VARCHAR UNIQUE NOT NULL,
    description          VARCHAR(500),
    material_type        VARCHAR(20),
    material_group       VARCHAR(50),
    procurement_type     VARCHAR(20),
    base_uom             VARCHAR(10),
    weight               DECIMAL(12,4),
    cost_per_unit        DECIMAL(12,4),
    source_system        VARCHAR(20),
    is_active            BOOLEAN DEFAULT TRUE,
    created_at           TIMESTAMP DEFAULT current_timestamp
);

-- BOM Header (universal format)
CREATE TABLE bom_header (
    bom_id               VARCHAR PRIMARY KEY,
    source_system        VARCHAR NOT NULL,
    parent_material_id   VARCHAR NOT NULL REFERENCES materials(material_id),
    bom_type             VARCHAR,
    alternative_number   VARCHAR,
    revision             VARCHAR,
    base_quantity        DECIMAL(18,6) DEFAULT 1,
    valid_from           DATE,
    valid_to             DATE,
    plant_id             VARCHAR(20),
    is_approved          BOOLEAN DEFAULT FALSE,
    created_at           TIMESTAMP DEFAULT current_timestamp
);

-- BOM Components (universal format)
CREATE TABLE bom_component (
    component_id         VARCHAR PRIMARY KEY,
    bom_id               VARCHAR NOT NULL REFERENCES bom_header(bom_id),
    line_number          INTEGER NOT NULL,
    child_material_id    VARCHAR NOT NULL REFERENCES materials(material_id),
    quantity_per         DECIMAL(18,6) NOT NULL,
    quantity_uom         VARCHAR(10) NOT NULL,
    is_fixed_quantity    BOOLEAN DEFAULT FALSE,
    scrap_percent        DECIMAL(8,4) DEFAULT 0,
    effective_from       DATE,
    effective_to         DATE,
    component_type       VARCHAR(20),
    is_alternative       BOOLEAN DEFAULT FALSE,
    alternative_group    VARCHAR(20),
    created_at           TIMESTAMP DEFAULT current_timestamp
);

-- Flattened BOM edges for graph queries
CREATE TABLE bom_edges AS
SELECT 
    h.parent_material_id AS parent_id,
    c.child_material_id AS child_id,
    c.quantity_per,
    c.component_type,
    c.line_number AS sequence
FROM bom_header h
JOIN bom_component c ON h.bom_id = c.bom_id;

CREATE INDEX idx_bom_edges_parent ON bom_edges(parent_id);
CREATE INDEX idx_bom_edges_child ON bom_edges(child_id);

-- Multi-modal embeddings
CREATE TABLE material_embeddings (
    material_id              VARCHAR PRIMARY KEY REFERENCES materials(material_id),
    structural_embedding     FLOAT[256],
    textual_embedding        FLOAT[384],
    transactional_embedding  FLOAT[128],
    updated_at               TIMESTAMP DEFAULT current_timestamp
);

-- HNSW indexes for embedding search
CREATE INDEX structural_hnsw ON material_embeddings 
    USING HNSW (structural_embedding) WITH (metric = 'cosine', M = 24);
CREATE INDEX textual_hnsw ON material_embeddings 
    USING HNSW (textual_embedding) WITH (metric = 'cosine', M = 32);
CREATE INDEX transactional_hnsw ON material_embeddings 
    USING HNSW (transactional_embedding) WITH (metric = 'l2sq', M = 16);

-- Property graph for BOM traversal
CREATE PROPERTY GRAPH bom_graph
VERTEX TABLES (
    materials LABEL Material
    PROPERTIES (material_id, material_number, description, material_type, material_group)
)
EDGE TABLES (
    bom_edges
        SOURCE KEY (parent_id) REFERENCES materials (material_id)
        DESTINATION KEY (child_id) REFERENCES materials (material_id)
        LABEL contains
        PROPERTIES (quantity_per, component_type, sequence)
);

-- Similarity results cache
CREATE TABLE similarity_pairs (
    source_id        VARCHAR,
    target_id        VARCHAR,
    algorithm        VARCHAR,
    score            DOUBLE,
    metadata         JSON,
    computed_at      TIMESTAMP DEFAULT current_timestamp,
    PRIMARY KEY (source_id, target_id, algorithm)
);

-- WL kernel label cache (for iterative computation)
CREATE TABLE wl_labels (
    material_id      VARCHAR,
    iteration        INTEGER,
    label_hash       VARCHAR(64),
    PRIMARY KEY (material_id, iteration)
);
```

---

## Performance recommendations

For **Mittelstand scale** (10,000-50,000 materials, 5,000-20,000 BOMs):

| Component | Memory | Notes |
|-----------|--------|-------|
| Materials table | ~5 MB | With all attributes |
| BOM edges | ~3.5 MB | CSR bidirectional |
| Structural embeddings (256D) | ~75 MB | Including HNSW index |
| Textual embeddings (384D) | ~110 MB | Including HNSW index |
| Transactional embeddings (128D) | ~36 MB | Including HNSW index |
| **Total working set** | **~230 MB** | Fits comfortably in RAM |

**Configuration:**
- `memory_limit = '4GB'` minimum, `'8GB'` recommended
- `threads = 8` for parallel similarity computation
- HNSW parameters: `ef_search = 100` balances recall/latency
- Batch similarity queries in chunks of 1,000 materials

**Index maintenance:**
- Build HNSW indexes after bulk loading (faster than incremental)
- Run `PRAGMA hnsw_compact_index('idx_name')` after large deletes
- Rebuild WL label cache when BOM structure changes significantly

This architecture enables sub-second single-material similarity queries and batch processing of 10,000+ pairwise comparisons in under 30 seconds on commodity hardware.
