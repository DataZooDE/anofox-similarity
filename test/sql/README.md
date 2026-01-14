# Anofox Similarity SQL Tests

This directory contains SQL tests for the anofox-similarity DuckDB extension. Tests are organized by feature to serve as documentation and usage examples.

## Quick Feature Navigation

| Feature | Primary Test | Description |
|---------|--------------|-------------|
| Extension Loading | `extension/anofox_similarity.test` | Verifies extension loads correctly |
| Jaccard Similarity | `functions/jaccard_similarity.test` | Set-based similarity for BOMs |
| WL Kernel | `algorithms/wl_kernel/wl_kernel.test` | Structural graph similarity |
| Find Similar Materials | `functions/find_similar_materials.test` | Top-K similarity search |
| Cold Start Analogs | `functions/cold_start_analogs.test` | Find analogs using consumption history |
| Predecessor Detection | `functions/infer_predecessors.test` | Detect product successors/predecessors |
| Jaccard Embeddings | `functions/embeddings/compute.test` | Min-hash embedding generation |
| HNSW Search | `functions/embeddings/hnsw_search.test` | Vector similarity search |
| Incremental Updates | `functions/embeddings/refresh.test` | Dirty tracking and refresh |
| Accuracy Metrics | `accuracy/accuracy_functions.test` | P@K, R@K, MRR calculations |

## Directory Structure

```
test/sql/
├── accuracy/           # Accuracy validation (P@K, R@K, MRR)
├── algorithms/         # Core algorithm tests
│   ├── jaccard/        # Jaccard similarity edge cases
│   └── wl_kernel/      # Weisfeiler-Lehman kernel
├── datasets/           # Real-world dataset tests
│   ├── adventureworks/ # AdventureWorks ERP data
│   ├── caterpillar/    # Caterpillar parts data
│   ├── dynamics365/    # Microsoft D365 integration
│   ├── erp_systems/    # Multi-ERP integration
│   ├── figshare/       # Figshare BOM dataset
│   ├── openhardware/   # Open hardware validation
│   └── sap/            # SAP ERP data
├── extension/          # Extension loading tests
├── functions/          # Core function tests
│   └── embeddings/     # Embedding generation/search
├── fusion/             # Multi-method similarity fusion
├── graph/              # Graph database integration
├── integration/        # Cross-system error handling
├── scenarios/          # BOM scenario tests (S1-S12)
└── schema/             # BOM schema validation
```

## BOM Test Scenarios (S1-S12)

The `scenarios/` directory contains controlled test cases for different BOM patterns:

| Scenario | Test File | Description |
|----------|-----------|-------------|
| S1 | `s1_identical_boms.test` | Identical BOMs (similarity = 1.0) |
| S2 | `s2_disjoint_boms.test` | Disjoint BOMs (similarity = 0.0) |
| S3 | `s3_controlled_overlap.test` | Partial overlap with known Jaccard |
| S4 | `s4_variant_clusters.test` | Product variant families |
| S5 | `s5_hierarchical_bom.test` | Shared subassemblies |
| S6 | `s6_predecessor_chain.test` | 2-generation predecessor chain |
| S7 | `s7_anticorrelation.test` | Clear predecessor with anti-correlation |
| S8 | `s8_phaseout.test` | Gradual phase-out patterns |
| S9 | `s9_no_predecessor.test` | Genuinely new product (no predecessor) |
| S12 | `s12_transitive_chain.test` | 6-generation transitive chain |

## Running Tests

```bash
# Run all SQL tests
make test

# Run specific test file
./build/debug/test/unittest "test/sql/functions/jaccard_similarity.test"

# Run tests in a directory
./build/debug/test/unittest "test/sql/scenarios/*"
```

## Test Tiers

- **tier1**: Foundational tests (extension loading, core functions)
- **tier2**: Integration tests (dataset validation, multi-function workflows)
- **tier3**: Advanced tests (accuracy metrics, real-world validation)
