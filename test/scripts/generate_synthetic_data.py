#!/usr/bin/env python3
"""
Synthetic BOM test data generator for anofox-similarity TDD.
Generates controlled test scenarios with known ground truth.

Usage:
    python generate_synthetic_data.py --output test/data/synthetic/ --seed 42
"""

import csv
import random
import argparse
from dataclasses import dataclass
from typing import List, Set
from pathlib import Path
import json


@dataclass
class Material:
    id: str
    description: str
    material_group: str
    material_type: str
    created_date: str = "2020-01-01"

    def __hash__(self):
        return hash(self.id)

    def __eq__(self, other):
        if isinstance(other, Material):
            return self.id == other.id
        return False


@dataclass
class BOMItem:
    bom_id: str
    parent_id: str
    child_id: str
    quantity: float
    level: int
    position: int


@dataclass
class GroundTruth:
    material_a: str
    material_b: str
    expected_similarity: float
    relationship_type: str
    notes: str = ""


class SyntheticBOMGenerator:
    def __init__(self, seed: int = 42):
        random.seed(seed)
        self.seed = seed
        self.materials: List[Material] = []
        self.bom_items: List[BOMItem] = []
        self.ground_truth: List[GroundTruth] = []

        self.prefixes = ['Premium', 'Standard', 'Heavy-Duty', 'Precision', 'Industrial']
        self.materials_vocab = ['Steel', 'Aluminum', 'Polymer', 'Copper', 'Composite']
        self.components = ['Housing', 'Bracket', 'Assembly', 'Module', 'Frame']

    def _generate_description(self) -> str:
        return f"{random.choice(self.prefixes)} {random.choice(self.materials_vocab)} {random.choice(self.components)}"

    def _create_material(self, mat_id: str, mat_type: str) -> Material:
        return Material(
            id=mat_id,
            description=self._generate_description(),
            material_group=f"GRP-{random.randint(1, 30):02d}",
            material_type=mat_type
        )

    def generate_baseline_identical(self) -> None:
        """S1: Two products with identical BOMs."""
        components = [self._create_material(f"IDENT-COMP-{i:03d}", "HALB") for i in range(5)]
        self.materials.extend(components)

        prod_a = self._create_material("IDENT-PROD-A", "FERT")
        prod_b = self._create_material("IDENT-PROD-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        for prod in [prod_a, prod_b]:
            for i, comp in enumerate(components):
                self.bom_items.append(BOMItem(
                    bom_id=f"BOM-{prod.id}",
                    parent_id=prod.id,
                    child_id=comp.id,
                    quantity=round(random.uniform(1, 5), 2),
                    level=1,
                    position=i * 10
                ))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=1.0,
            relationship_type='identical',
            notes='Identical component sets'
        ))

    def generate_baseline_disjoint(self) -> None:
        """S2: Two products with no overlap."""
        components_a = [self._create_material(f"DISJ-A-COMP-{i:03d}", "HALB") for i in range(5)]
        components_b = [self._create_material(f"DISJ-B-COMP-{i:03d}", "HALB") for i in range(5)]
        self.materials.extend(components_a + components_b)

        prod_a = self._create_material("DISJ-PROD-A", "FERT")
        prod_b = self._create_material("DISJ-PROD-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        for i, comp in enumerate(components_a):
            self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, comp.id, 1.0, 1, i * 10))
        for i, comp in enumerate(components_b):
            self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, comp.id, 1.0, 1, i * 10))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=0.0,
            relationship_type='unrelated',
            notes='Completely disjoint component sets'
        ))

    def generate_controlled_overlap(self) -> None:
        """S3: Products with known overlap ratio (Jaccard = 3/7)."""
        shared = [self._create_material(f"OVLP-SHARED-{i:03d}", "HALB") for i in range(3)]
        unique_a = [self._create_material(f"OVLP-UNIQ-A-{i:03d}", "HALB") for i in range(2)]
        unique_b = [self._create_material(f"OVLP-UNIQ-B-{i:03d}", "HALB") for i in range(2)]
        self.materials.extend(shared + unique_a + unique_b)

        prod_a = self._create_material("OVLP-PROD-A", "FERT")
        prod_b = self._create_material("OVLP-PROD-B", "FERT")
        self.materials.extend([prod_a, prod_b])

        for i, comp in enumerate(shared + unique_a):
            self.bom_items.append(BOMItem(f"BOM-{prod_a.id}", prod_a.id, comp.id, 1.0, 1, i * 10))
        for i, comp in enumerate(shared + unique_b):
            self.bom_items.append(BOMItem(f"BOM-{prod_b.id}", prod_b.id, comp.id, 1.0, 1, i * 10))

        self.ground_truth.append(GroundTruth(
            material_a=prod_a.id,
            material_b=prod_b.id,
            expected_similarity=3 / 7,
            relationship_type='partial_overlap',
            notes='3 shared of 7 unique components (Jaccard = 3/7)'
        ))

    def generate_variant_cluster(self, base_id: str, n_variants: int = 3, n_components: int = 10) -> None:
        """S4: Product with variants at different similarity levels."""
        base_components = [self._create_material(f"{base_id}-COMP-{i:03d}", "HALB") for i in range(n_components)]
        self.materials.extend(base_components)

        base_product = self._create_material(f"{base_id}-BASE", "FERT")
        self.materials.append(base_product)

        for i, comp in enumerate(base_components):
            self.bom_items.append(BOMItem(f"BOM-{base_product.id}", base_product.id, comp.id, 1.0, 1, i * 10))

        retention_rates = [0.9, 0.7, 0.5]
        for v, retention in enumerate(retention_rates[:n_variants]):
            variant = self._create_material(f"{base_id}-VAR-{v + 1}", "FERT")
            self.materials.append(variant)

            n_keep = int(n_components * retention)
            kept = random.sample(base_components, n_keep)
            n_new = n_components - n_keep
            new_comps = [self._create_material(f"{base_id}-VAR{v + 1}-NEW-{i:03d}", "HALB") for i in range(n_new)]
            self.materials.extend(new_comps)

            for i, comp in enumerate(kept + new_comps):
                self.bom_items.append(BOMItem(f"BOM-{variant.id}", variant.id, comp.id, 1.0, 1, i * 10))

            expected_jaccard = n_keep / (2 * n_components - n_keep)
            self.ground_truth.append(GroundTruth(
                material_a=base_product.id,
                material_b=variant.id,
                expected_similarity=expected_jaccard,
                relationship_type='variant',
                notes=f'{retention * 100:.0f}% component retention'
            ))

    def generate_hierarchical_bom(self) -> None:
        """S5: Multi-level BOM structure with sub-assemblies."""
        # Create leaf components
        leaf_components = [self._create_material(f"HIER-LEAF-{i:03d}", "ROH") for i in range(12)]
        self.materials.extend(leaf_components)

        # Create sub-assemblies (level 2)
        sub_asm_a = self._create_material("HIER-SUBASM-A", "HALB")
        sub_asm_b = self._create_material("HIER-SUBASM-B", "HALB")
        sub_asm_c = self._create_material("HIER-SUBASM-C", "HALB")
        self.materials.extend([sub_asm_a, sub_asm_b, sub_asm_c])

        # Sub-assembly A gets leaf components 0-3
        for i, comp in enumerate(leaf_components[:4]):
            self.bom_items.append(BOMItem(f"BOM-{sub_asm_a.id}", sub_asm_a.id, comp.id, 1.0, 2, i * 10))

        # Sub-assembly B gets leaf components 4-7
        for i, comp in enumerate(leaf_components[4:8]):
            self.bom_items.append(BOMItem(f"BOM-{sub_asm_b.id}", sub_asm_b.id, comp.id, 1.0, 2, i * 10))

        # Sub-assembly C gets leaf components 8-11
        for i, comp in enumerate(leaf_components[8:12]):
            self.bom_items.append(BOMItem(f"BOM-{sub_asm_c.id}", sub_asm_c.id, comp.id, 1.0, 2, i * 10))

        # Create finished products using sub-assemblies
        prod_1 = self._create_material("HIER-PROD-1", "FERT")  # Uses A, B
        prod_2 = self._create_material("HIER-PROD-2", "FERT")  # Uses A, C
        prod_3 = self._create_material("HIER-PROD-3", "FERT")  # Uses B, C
        self.materials.extend([prod_1, prod_2, prod_3])

        # Product 1: sub-assemblies A and B
        self.bom_items.append(BOMItem(f"BOM-{prod_1.id}", prod_1.id, sub_asm_a.id, 1.0, 1, 10))
        self.bom_items.append(BOMItem(f"BOM-{prod_1.id}", prod_1.id, sub_asm_b.id, 1.0, 1, 20))

        # Product 2: sub-assemblies A and C
        self.bom_items.append(BOMItem(f"BOM-{prod_2.id}", prod_2.id, sub_asm_a.id, 1.0, 1, 10))
        self.bom_items.append(BOMItem(f"BOM-{prod_2.id}", prod_2.id, sub_asm_c.id, 1.0, 1, 20))

        # Product 3: sub-assemblies B and C
        self.bom_items.append(BOMItem(f"BOM-{prod_3.id}", prod_3.id, sub_asm_b.id, 1.0, 1, 10))
        self.bom_items.append(BOMItem(f"BOM-{prod_3.id}", prod_3.id, sub_asm_c.id, 1.0, 1, 20))

        # Ground truth: each product pair shares 1 sub-assembly (1/3 Jaccard at level 1)
        # At leaf level: 4 shared of 8 unique = 0.5 Jaccard
        self.ground_truth.append(GroundTruth(
            material_a=prod_1.id,
            material_b=prod_2.id,
            expected_similarity=1 / 3,  # 1 shared sub-assembly of 3 unique
            relationship_type='hierarchical',
            notes='Share sub-assembly A; Jaccard at level 1 = 1/3'
        ))
        self.ground_truth.append(GroundTruth(
            material_a=prod_1.id,
            material_b=prod_3.id,
            expected_similarity=1 / 3,
            relationship_type='hierarchical',
            notes='Share sub-assembly B; Jaccard at level 1 = 1/3'
        ))
        self.ground_truth.append(GroundTruth(
            material_a=prod_2.id,
            material_b=prod_3.id,
            expected_similarity=1 / 3,
            relationship_type='hierarchical',
            notes='Share sub-assembly C; Jaccard at level 1 = 1/3'
        ))

    def generate_predecessor_chain(self, chain_length: int = 4, retention: float = 0.7) -> None:
        """S6: Succession chain for predecessor inference testing."""
        base_components = [self._create_material(f"PRED-CHAIN-COMP-{i:03d}", "HALB") for i in range(10)]
        self.materials.extend(base_components)

        current_components: Set[Material] = set(base_components)
        products: List[Material] = []

        for gen in range(chain_length):
            product = self._create_material(f"PRED-CHAIN-GEN-{gen:02d}", "FERT")
            product.created_date = f"202{gen}-01-01"
            self.materials.append(product)
            products.append(product)

            for i, comp in enumerate(current_components):
                self.bom_items.append(BOMItem(f"BOM-{product.id}", product.id, comp.id, 1.0, 1, i * 10))

            n_keep = int(len(current_components) * retention)
            kept = set(random.sample(list(current_components), n_keep))
            n_new = len(current_components) - n_keep
            new_comps = [self._create_material(f"PRED-CHAIN-GEN{gen + 1}-NEW-{i:03d}", "HALB") for i in range(n_new)]
            self.materials.extend(new_comps)
            current_components = kept | set(new_comps)

        for i in range(1, len(products)):
            # Expected Jaccard for adjacent generations with retention r:
            # intersection = n_keep, union = 2*n - n_keep
            n = 10  # base component count
            n_keep = int(n * retention)
            expected_jaccard = n_keep / (2 * n - n_keep)
            self.ground_truth.append(GroundTruth(
                material_a=products[i].id,
                material_b=products[i - 1].id,
                expected_similarity=expected_jaccard,
                relationship_type='predecessor',
                notes=f'Direct predecessor (Gen {i - 1} -> Gen {i})'
            ))

    def generate_all_scenarios(self) -> None:
        """Generate all test scenarios."""
        print("Generating S1: Identical BOMs...")
        self.generate_baseline_identical()
        print("Generating S2: Disjoint BOMs...")
        self.generate_baseline_disjoint()
        print("Generating S3: Controlled Overlap...")
        self.generate_controlled_overlap()
        print("Generating S4: Variant Clusters...")
        for i in range(5):
            self.generate_variant_cluster(f"FAMILY-{i:02d}")
        print("Generating S5: Hierarchical BOM...")
        self.generate_hierarchical_bom()
        print("Generating S6: Predecessor Chains...")
        self.generate_predecessor_chain(chain_length=4, retention=0.7)

    def save(self, output_dir: str) -> None:
        """Save generated data to CSV files."""
        path = Path(output_dir)
        path.mkdir(parents=True, exist_ok=True)

        with open(path / "materials.csv", 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['material_id', 'description', 'material_group', 'material_type', 'created_date'])
            for m in self.materials:
                writer.writerow([m.id, m.description, m.material_group, m.material_type, m.created_date])

        with open(path / "bom_items.csv", 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['bom_id', 'parent_id', 'child_id', 'quantity', 'level', 'position'])
            for item in self.bom_items:
                writer.writerow([item.bom_id, item.parent_id, item.child_id, item.quantity, item.level, item.position])

        with open(path / "ground_truth.csv", 'w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['material_a', 'material_b', 'expected_similarity', 'relationship_type', 'notes'])
            for gt in self.ground_truth:
                writer.writerow([gt.material_a, gt.material_b, round(gt.expected_similarity, 6), gt.relationship_type, gt.notes])

        metadata = {
            'seed': self.seed,
            'n_materials': len(self.materials),
            'n_bom_items': len(self.bom_items),
            'n_ground_truth': len(self.ground_truth),
            'scenarios': ['S1-identical', 'S2-disjoint', 'S3-overlap', 'S4-variants', 'S5-hierarchical', 'S6-predecessor']
        }
        with open(path / "metadata.json", 'w') as f:
            json.dump(metadata, f, indent=2)

        print(f"\nGenerated: {len(self.materials)} materials, {len(self.bom_items)} BOM items, {len(self.ground_truth)} ground truth pairs")
        print(f"Output written to: {path}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate synthetic BOM test data')
    parser.add_argument('--output', type=str, default='test/data/synthetic')
    parser.add_argument('--seed', type=int, default=42)
    args = parser.parse_args()

    generator = SyntheticBOMGenerator(seed=args.seed)
    generator.generate_all_scenarios()
    generator.save(args.output)
