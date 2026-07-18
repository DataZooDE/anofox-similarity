# anofox-similarity

[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](LICENSE)
[![DuckDB: ≥ v1.0.0](https://img.shields.io/badge/DuckDB-≥%20v1.0.0-blue)](https://duckdb.org)
[![Tests](https://github.com/DataZooDE/anofox-similarity/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/DataZooDE/anofox-similarity/actions)

**Multi-modal product similarity detection for manufacturing supply chain planning**

The `anofox-similarity` extension enables manufacturing companies to identify similar products from enterprise master data (ERP systems like SAP, Microsoft Dynamics 365). Perfect for cold-start forecasting, predecessor detection, and analog-based demand planning.

---

## 📋 Overview

### Why Product Similarity Matters

Mid-sized manufacturers face a persistent planning challenge: *"How do I forecast demand for a new product with no sales history?"* The answer lies in identifying analogous products with established consumption patterns.

**Real-world scenarios:**
- **Cold-start forecasting**: New product launch (X-750) needs consumption forecast → find similar products with history
- **Predecessor detection**: Material AL-7076 recently introduced → identify AL-7075 as its precursor
- **Analog-based planning**: Forecast new product using weighted average of similar products' patterns

### Key Capabilities

| Capability | Use Case | Complexity |
|-----------|----------|-----------|
| **Component Overlap (Jaccard)** | Find products with similar components | Low |
| **Graph Structural Similarity (WL Kernel)** | Detect topological BOM similarities | Medium |
| **Text Embedding Search** | Find similar products by description | Medium |
| **Temporal Analysis** | Identify predecessor-successor relationships | Medium |
| **Multi-modal Fusion** | Combine multiple similarity signals | High |

---

## 🚀 Quick Start (5 minutes)

### Installation

> [!NOTE]
> The community-repository build is not published yet
> (`INSTALL anofox_similarity FROM community` currently returns HTTP 404
> for every platform). Until it lands, build from source (see
> [Development](#%EF%B8%8F-development)) and load the unsigned binary:

```sql
-- After `make release` (requires `duckdb -unsigned` or allow_unsigned_extensions)
LOAD 'build/release/extension/anofox_similarity/anofox_similarity.duckdb_extension';

-- Verify installation
SELECT 1 AS test;  -- Should return 1
```

### Finding Similar Materials

```sql
-- Create sample BOM data (components for each product)
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);

INSERT INTO bom_items VALUES
  ('PUMP-A', 'SEAL-001'),
  ('PUMP-A', 'BEARING-X'),
  ('PUMP-A', 'MOTOR-M1'),
  ('PUMP-B', 'SEAL-001'),
  ('PUMP-B', 'BEARING-X'),
  ('PUMP-B', 'MOTOR-M1'),
  ('PUMP-B', 'GASKET-Y'),
  ('PUMP-C', 'FILTER-F1'),
  ('PUMP-C', 'VALVE-V2');

-- Find top 3 most similar materials to PUMP-A
-- bom_table parameter specifies where to look for BOM data
SELECT material_id, similarity, shared_components, total_components
FROM find_similar_materials_jaccard('PUMP-A', 3, bom_table := 'bom_items');

-- Results:
-- PUMP-B: 3 shared / 4 total = 0.75 (high overlap)
-- PUMP-C: 0 shared / 5 total = 0.0 (disjoint)
```

### Detecting Predecessors

```sql
-- infer_predecessors needs BOTH a bom_items table (to establish structural similarity)
-- AND a goods_movements table (for temporal anti-correlation).
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR);
INSERT INTO bom_items VALUES
  ('OLD-PUMP', 'SEAL-001'), ('OLD-PUMP', 'BEARING-X'), ('OLD-PUMP', 'MOTOR-M1'),
  ('NEW-PUMP', 'SEAL-001'), ('NEW-PUMP', 'BEARING-X'), ('NEW-PUMP', 'MOTOR-M1');

CREATE TABLE goods_movements (
  material_id VARCHAR,
  movement_date DATE,
  quantity DOUBLE
);

-- OLD-PUMP consumption declining, NEW-PUMP rising (anti-correlation), weekly cadence
INSERT INTO goods_movements VALUES
  ('OLD-PUMP', '2023-01-15', 100.0),
  ('OLD-PUMP', '2023-03-15', 80.0),
  ('OLD-PUMP', '2023-06-15', 40.0),
  ('OLD-PUMP', '2023-09-15', 10.0),
  ('NEW-PUMP', '2023-06-01', 20.0),
  ('NEW-PUMP', '2023-09-01', 60.0),
  ('NEW-PUMP', '2023-12-01', 100.0);

-- The material id is the FIRST positional argument (it cannot be passed by name).
SELECT predecessor_id, confidence, correlation, overlapping_weeks
FROM infer_predecessors('NEW-PUMP', min_confidence := 0.5, min_overlapping_weeks := 2)
ORDER BY confidence DESC;
```

> **Note on data density.** Predecessor detection aligns predecessor and successor consumption into
> **weekly** buckets (offset by `lag_weeks`) and requires at least `min_overlapping_weeks` overlapping
> buckets (default 8). The few, months-apart rows above are deliberately sparse and will typically
> return **zero** predecessors — real detection needs dense, overlapping weekly history where the
> predecessor declines as the successor rises (`correlation < 0`). See
> `test/sql/scenarios/s7_anticorrelation.test` for a dataset that produces a confident match.

### Finding Analogs for New Products

```sql
-- Find products similar to NEW-PUMP that have consumption history
-- Useful for cold-start forecasting when a new product has no history
SELECT material_id, similarity, history_months, first_usage, last_usage
FROM cold_start_analogs(
    'NEW-PUMP',
    5,                              -- Return top 5 analogs
    min_history_months := 12,       -- Require at least 12 months of history
    bom_table := 'bom_items',
    movements_table := 'goods_movements'
);

-- Returns similar products with consumption history
-- Use their demand patterns to forecast the new product
```

### Converting ERP Data (SAP)

```sql
-- SAP tables: MARA (materials), MAKT (descriptions)
CREATE TABLE sap_mara AS SELECT * FROM (VALUES
    ('MAT-001', 'FERT', 'GRP01', '20200115', NULL),
    ('MAT-002', 'HALB', 'GRP01', '20210301', NULL)
) AS t(matnr, mtart, matkl, ersda, lvorm);

CREATE TABLE sap_makt AS SELECT * FROM (VALUES
    ('MAT-001', 'E', 'Pump Assembly'),
    ('MAT-002', 'E', 'Motor Component')
) AS t(matnr, spras, maktx);

-- Convert to universal schema
SELECT material_id, material_type, description, created_date
FROM sap_to_materials_with_desc(
    mara_table := 'sap_mara',
    makt_table := 'sap_makt',
    language := 'E'
);

-- Dynamics 365 conversion (see API_REFERENCE.md for full details)
-- SELECT * FROM dynamics365_to_materials('d365_InventTable');
-- SELECT * FROM dynamics365_to_bom_component('d365_BOM');
```

---

## 📚 API Reference

The extension provides 40+ functions organized into 8 functional categories:

### Core Similarity Algorithms
- **Jaccard Similarity** - Component overlap detection (O(n) time)
- **WL Kernel** - Graph structural similarity (O(nm) time)
- **Predecessor Inference** - Temporal anti-correlation analysis

### Data Access & Transformation
- **SAP Transformations** - Convert SAP tables to universal schema
- **Dynamics 365 Transformations** - Convert D365 tables to universal schema
- **BOM Conversion** - Schema normalization and validation

### Advanced Features
- **Textual Embeddings** - Semantic similarity from descriptions
- **Transactional Embeddings** - Time-series feature extraction (98+ features)
- **Multi-modal Fusion** - Combine multiple similarity signals

### Infrastructure
- **Vector Search (HNSW)** - Approximate nearest neighbor search
- **Statistics & Normalization** - Z-score normalization and caching
- **Property Graph Traversal** - DuckPGQ integration for BOM navigation

**Complete API documentation**: [docs/API_REFERENCE.md](docs/API_REFERENCE.md)

---

## 🔧 Building & Development

### Prerequisites

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install build-essential cmake python3 python3-dev
```

**macOS:**
```bash
brew install cmake python3
```

**Windows (via WSL2 or MSVC):**
```bash
# WSL2: Follow Linux instructions
# MSVC: Install Visual Studio 2019+ with C++ support
```

### Build Steps

```bash
# Clone repository with submodules
git clone --recurse-submodules https://github.com/DataZooDE/anofox-similarity.git
cd anofox-similarity

# Set up vcpkg (one-time)
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)/vcpkg

# Build extension
GEN=ninja make

# Run tests
make test

# Load in DuckDB shell
./build/release/duckdb
```

### Test Suite

The extension includes 52 comprehensive tests (1179 assertions):

```bash
# Run all tests
make test

# Run specific test group
./build/release/test/unittest "[similarity]"

# Run with verbose output
./build/release/test/unittest -v "[wl_kernel]"
```

---

## 💻 Language Support

Write once in SQL, use everywhere:

| Language | Support | Example |
|----------|---------|---------|
| **SQL (DuckDB)** | ✅ Native | `SELECT * FROM find_similar_materials_jaccard(...)` |
| **Python** | ✅ DuckDB connector | `duckdb.sql("SELECT * FROM find_similar_materials_jaccard(...)")` |
| **R** | ✅ duckdb package | `duckdb::execute(con, "SELECT...")` |
| **Java** | ✅ JDBC driver | `stmt.execute("SELECT...")` |
| **Node.js** | ✅ duckdb-wasm | `db.query("SELECT...")` |

---

## 📊 Architecture

The extension implements a **tiered similarity approach**:

```
┌─────────────────────────────────────────────────┐
│     Multi-modal Fusion (Highest Accuracy)       │
│    Combines: Structural + Textual + Temporal   │
└─────────────────────────────────────────────────┘
                        ▲
        ┌───────────────┼───────────────┐
        │               │               │
    [Structural]  [Textual]      [Temporal]
    WL Kernel +    Text            Anti-correlation
    Jaccard     Embeddings         Analysis
        │               │               │
        └───────────────┼───────────────┘
                        │
         [Exact Similarity Computation]
         (Brute-force or HNSW Index)
```

**Design Philosophy**: Start simple (Jaccard → 75-85% accuracy), add complexity (WL Kernel, embeddings) only when value justifies cost.

---

## 🧪 Test Coverage

- **52 SQL tests** across 11 functional categories
- **1179 assertions** validating core algorithms
- **Synthetic data tests** with known ground truth
- **Real enterprise data tests** (SAP, Dynamics 365)
- **Edge case tests** (sparse BOMs, circular references, NULL handling)
- **Performance benchmarks** for HNSW indexing
- **Error handling tests** for graceful degradation

---

## 📖 Documentation

| Document | Purpose |
|----------|---------|
| **[API_REFERENCE.md](docs/API_REFERENCE.md)** | Complete function documentation with examples |
| **[CONCEPT.md](docs/CONCEPT.md)** | Algorithmic background and theory |
| **[DATA-STRUCTURES.md](docs/DATA-STRUCTURES.md)** | Schema definitions and data models |
| **[SAP_BOM_Explosion_Technical_Memo.md](docs/SAP_BOM_Explosion_Technical_Memo.md)** | SAP integration specifics |
| **[Microsoft_Dynamics_365_BOM_Explosion_Technical_Memo.md](docs/Microsoft_Dynamics_365_BOM_Explosion_Technical_Memo.md)** | Dynamics 365 integration specifics |

---

## ⚙️ Configuration

### Extension Parameters

All macros support parameterization for different use cases:

```sql
-- Customize time window for feature extraction (requires anofox-forecast)
SELECT * FROM compute_transactional_embeddings(
  movements_table := 'goods_movements',
  time_window_days := 180        -- last 6 months of history
);

-- Customize BOM explosion depth
SELECT * FROM bom_explosion_multilevel(
  'PARENT-001',
  max_depth := 5                 -- Limit to 5 levels
);
```

### HNSW Index Configuration

HNSW indexes over the embedding columns are created automatically on load. There is no
`CreateHNSWIndexes()` function; to build a custom index, use the `vss` extension's DDL directly:

```sql
INSTALL vss; LOAD vss;
SET hnsw_enable_experimental_persistence = true;
CREATE INDEX my_idx ON material_embeddings USING HNSW (combined_embedding) WITH (metric = 'cosine');
```

---

## 📈 Performance

Tested on typical Mittelstand data volumes:

| Operation | Data Size | Latency | Notes |
|-----------|-----------|---------|-------|
| Jaccard Similarity | 10K materials | <1ms | Per pair, no index |
| WL Kernel Similarity | 10K materials | <10ms | Per pair, 3 iterations |
| VSS k-NN Search | 10K materials | <100ms | k=10, HNSW index |
| Brute-force k-NN | 10K materials | <1s | k=10, no index |

---

## 🔐 Data Privacy & Security

The extension processes data locally within DuckDB and never transmits data to external services (unless explicitly configured for embedding providers).

**Optional external services:**
- Textual embeddings can use OpenAI, Anthropic, or local OpenVINO
- No data sharing with Datazoo unless explicitly enabled

---

## 📝 Licensing

Licensed under **Business Source License 1.1 (BSL 1.1)**:
- ✅ Free for development, testing, analytics
- ✅ Free for internal business use
- ⚠️ Requires license for SaaS/commercial redistribution
- 📅 Converts to open source (Apache 2.0) after 4 years

[Full license terms](LICENSE.md)

---

## 🤝 Contributing

We welcome contributions! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for:
- Code of conduct
- Development workflow
- Testing requirements
- Pull request process

---

## 📮 Support & Community

- **GitHub Issues**: [Report bugs and request features](https://github.com/DataZooDE/anofox-similarity/issues)
- **GitHub Discussions**: [Ask questions and share ideas](https://github.com/DataZooDE/anofox-similarity/discussions)
- **Email**: contact@data-zoo.de

---

## 🙏 Acknowledgments

- **DuckDB team** for the excellent extension template and framework
- **Research papers** on WL kernels, BOM similarity, and vector search
- **Manufacturing partners** who validated use cases and provided enterprise data

---

## 📋 Citation

If you use anofox-similarity in research or production, please cite:

```bibtex
@software{datazoo2024anofoxsimilarity,
  title = {anofox-similarity: Multi-modal Product Similarity Detection},
  author = {DataZoo GmbH},
  year = {2024},
  url = {https://github.com/DataZooDE/anofox-similarity}
}
```

---

**Made with ❤️ by [DataZoo](https://data-zoo.de)** for manufacturing excellence.
