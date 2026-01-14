# BOM Test Scenarios (S1-S12)

This directory contains controlled test scenarios that demonstrate specific BOM (Bill of Materials) patterns and expected similarity behaviors.

## Scenario Overview

Each scenario uses synthetic test data with known component relationships to validate similarity calculations.

### Similarity Scenarios

| Scenario | Pattern | Expected Jaccard |
|----------|---------|------------------|
| **S1** | Identical BOMs | 1.0 (perfect match) |
| **S2** | Disjoint BOMs | 0.0 (no overlap) |
| **S3** | Controlled Overlap | ~0.43 (3/7 components shared) |
| **S4** | Variant Clusters | Gradient from 0.9 to 0.5 |
| **S5** | Hierarchical BOM | Tests shared subassemblies |

### Predecessor Scenarios

| Scenario | Pattern | Detection Confidence |
|----------|---------|---------------------|
| **S6** | 2-Generation Chain | High (direct successor) |
| **S7** | Anti-Correlation | Very High (consumption decline) |
| **S8** | Gradual Phase-Out | Medium (30+ weeks overlap) |
| **S9** | No Predecessor | Zero (genuinely new product) |
| **S12** | 6-Generation Chain | Detects direct, not transitive |

### Structural Scenarios

| Scenario | Pattern | WL Kernel Behavior |
|----------|---------|-------------------|
| **S10** | Same Components, Different Structure | WL < 1.0, Jaccard = 1.0 |
| **S11** | Deep Hierarchy (4 levels) | Captures depth in similarity |

## Detailed Scenario Descriptions

### S1: Identical BOMs
Two products with exactly the same components.
- **Use case**: Detecting duplicate products
- **Expected**: Jaccard = 1.0, WL = 1.0
- **Test file**: `s1_identical_boms.test`

### S2: Disjoint BOMs
Two products with completely different components.
- **Use case**: Validating zero similarity baseline
- **Expected**: Jaccard = 0.0, WL = 0.0
- **Test file**: `s2_disjoint_boms.test`

### S3: Controlled Overlap
Products sharing a known subset of components.
- **Use case**: Validating Jaccard formula correctness
- **Expected**: Jaccard = |intersection| / |union|
- **Test file**: `s3_controlled_overlap.test`

### S4: Variant Clusters
Product families with incremental component changes.
- **Use case**: Product family detection
- **Expected**: VAR-1 > VAR-2 > VAR-3 similarity gradient
- **Test file**: `s4_variant_clusters.test`

### S5: Hierarchical BOM
Products sharing subassemblies at different levels.
- **Use case**: Multi-level BOM analysis
- **Expected**: Captures shared subassemblies
- **Test file**: `s5_hierarchical_bom.test`

### S6: Predecessor Chain
Two-generation product evolution.
- **Use case**: Simple successor detection
- **Expected**: Direct predecessor detected
- **Test file**: `s6_predecessor_chain.test`

### S7: Anti-Correlation
Old product consumption declines as new product rises.
- **Use case**: Clear phase-out detection
- **Expected**: High confidence, negative correlation
- **Test file**: `s7_anticorrelation.test`

### S8: Gradual Phase-Out
Extended overlap period during transition.
- **Use case**: Slow replacement patterns
- **Expected**: 30+ weeks overlapping consumption
- **Test file**: `s8_phaseout.test`

### S9: No Predecessor
Genuinely new product with unique components.
- **Use case**: Avoiding false positives
- **Expected**: Zero predecessors detected
- **Test file**: `s9_no_predecessor.test`

### S12: Transitive Chain
Six-generation product evolution.
- **Use case**: Long product lineage
- **Expected**: Only direct predecessor detected, not distant ancestors
- **Test file**: `s12_transitive_chain.test`

## Test Data Location

All scenarios use synthetic test data from:
```
test/data/synthetic/
├── materials.csv.gz
├── bom_items.csv.gz
├── goods_movements.csv.gz
└── ground_truth.csv.gz
```

## Related Tests

- **Accuracy tests**: `accuracy/synthetic_accuracy.test` validates P@K, R@K, MRR on these scenarios
- **Function tests**: `functions/` directory tests individual functions used in scenarios
- **Algorithm tests**: `algorithms/` directory tests WL kernel and Jaccard implementations
