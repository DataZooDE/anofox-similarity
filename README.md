# anofox-similarity

[![License: BSL 1.1](https://img.shields.io/badge/License-BSL%201.1-blue.svg)](https://opensource.org/licenses/BSL-1.0)
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

```sql
-- Install and load the extension
INSTALL anofox_similarity FROM community;
LOAD anofox_similarity;

-- Verify installation
SELECT 1 AS test;  -- Should return 1
```

### Finding Similar Materials

```sql
-- Create sample BOM data
CREATE TABLE bom_items (parent_id VARCHAR, child_id VARCHAR, quantity FLOAT);

INSERT INTO bom_items VALUES
  ('PUMP-A', 'SEAL-001', 2.0),
  ('PUMP-A', 'BEARING-X', 1.0),
  ('PUMP-B', 'SEAL-001', 2.0),
  ('PUMP-B', 'BEARING-X', 1.0),
  ('PUMP-B', 'GASKET-Y', 1.0);

-- Find top 3 most similar materials to PUMP-A
SELECT
  similar_material_id,
  jaccard_similarity AS similarity_score
FROM find_similar_materials(
  'PUMP-A',
  k := 3,
  method := 'structural'
)
ORDER BY similarity_score DESC;
```

### Detecting Predecessors

```sql
-- Create goods movements sample data
CREATE TABLE goods_movements (
  material_id VARCHAR,
  movement_date DATE,
  quantity FLOAT,
  movement_type VARCHAR
);

INSERT INTO goods_movements VALUES
  ('MATERIAL-001', '2024-01-01', 100.0, '261'),
  ('MATERIAL-001', '2024-01-15', 150.0, '261'),
  ('MATERIAL-002', '2024-01-08', 50.0, '261');

-- Find predecessor materials
SELECT
  predecessor_material_id,
  confidence_score
FROM infer_predecessors(
  query_material_id := 'MATERIAL-002',
  lookback_months := 12
)
ORDER BY confidence_score DESC;
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

The extension includes 102 comprehensive tests (2228 assertions):

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
| **SQL (DuckDB)** | ✅ Native | `SELECT find_similar_materials(...)` |
| **Python** | ✅ DuckDB connector | `duckdb.sql("SELECT find_similar_materials(...)")` |
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

- **102 SQL tests** across 11 functional categories
- **2228 assertions** validating core algorithms
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
-- Customize time window for feature extraction
CALL compute_transactional_embeddings(
  time_window_days := 180,      -- Last 6 months only
  min_observations := 5          -- Require 5+ data points
);

-- Customize BOM explosion depth
SELECT * FROM bom_explosion_multilevel(
  'PARENT-001',
  max_depth := 5                 -- Limit to 5 levels
);
```

### HNSW Index Configuration

```sql
-- Create index for faster similarity search
CALL CreateHNSWIndexes();  -- Default: ef=100, M=16

-- Fine-tune for your dataset size
-- Small datasets (< 10K materials): ef=50, M=8
-- Large datasets (> 100K materials): ef=200, M=32
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
