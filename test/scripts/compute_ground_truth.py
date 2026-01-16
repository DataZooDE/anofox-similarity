#!/usr/bin/env python3
"""
Compute ground truth similarity pairs for BOM datasets.

Calculates Jaccard similarity between all product pairs and outputs
interesting pairs for testing (identical, variants, hierarchical, etc.).

Usage:
    cd scripts && uv run python ../test/scripts/compute_ground_truth.py --dataset odoo
    cd scripts && uv run python ../test/scripts/compute_ground_truth.py --dataset erpnext
    cd scripts && uv run python ../test/scripts/compute_ground_truth.py --dataset figshare
"""

import argparse
import csv
import gzip
import json
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Set, Tuple


@dataclass
class GroundTruth:
    material_a: str
    material_b: str
    expected_similarity: float
    relationship_type: str
    notes: str = ""


class GroundTruthGenerator:
    def __init__(self, bom_items_path: Path, materials_path: Path):
        self.bom_items_path = bom_items_path
        self.materials_path = materials_path
        self.product_components: Dict[str, Set[str]] = defaultdict(set)
        self.material_types: Dict[str, str] = {}
        self.ground_truth: List[GroundTruth] = []

    def load_data(self) -> None:
        """Load BOM items and materials from CSV files."""
        # Load materials to get types
        print(f"Loading materials from: {self.materials_path}")
        with gzip.open(self.materials_path, "rt") as f:
            reader = csv.DictReader(f)
            for row in reader:
                self.material_types[row["material_id"]] = row["material_type"]

        # Load BOM items and aggregate components per product
        print(f"Loading BOM items from: {self.bom_items_path}")
        all_parents: Set[str] = set()
        all_children: Set[str] = set()

        with gzip.open(self.bom_items_path, "rt") as f:
            reader = csv.DictReader(f)
            for row in reader:
                parent_id = row["parent_id"]
                child_id = row["child_id"]
                all_parents.add(parent_id)
                all_children.add(child_id)
                self.product_components[parent_id].add(child_id)

        # Filter to only top-level products (parents that are not children)
        # or if material_type is FERT
        top_level = all_parents - all_children
        fert_parents = {p for p in all_parents if self.material_types.get(p) == "FERT"}

        # Use top-level parents, or fallback to FERT, or all parents
        if top_level:
            product_ids = top_level
            print(f"  Using {len(product_ids)} top-level products (not used as components)")
        elif fert_parents:
            product_ids = fert_parents
            print(f"  Using {len(product_ids)} FERT products")
        else:
            product_ids = all_parents
            print(f"  Using all {len(product_ids)} parent materials as products")

        # Keep only products that have components
        filtered_components = {p: c for p, c in self.product_components.items() if p in product_ids}
        self.product_components = defaultdict(set, filtered_components)

        print(f"  Loaded {len(self.product_components)} products with BOMs")

    def compute_jaccard(self, set_a: Set[str], set_b: Set[str]) -> float:
        """Compute Jaccard similarity between two sets."""
        if not set_a and not set_b:
            return 0.0
        intersection = len(set_a & set_b)
        union = len(set_a | set_b)
        return intersection / union if union > 0 else 0.0

    def classify_relationship(self, sim: float, shared: int, total_a: int, total_b: int) -> str:
        """Classify the relationship type based on similarity."""
        if sim == 1.0:
            return "identical"
        elif sim == 0.0:
            return "unrelated"
        elif sim >= 0.8:
            return "variant"
        elif sim >= 0.5:
            return "partial_overlap"
        elif sim >= 0.2:
            return "low_overlap"
        else:
            return "minimal_overlap"

    def compute_all_pairs(self, max_pairs: int = 100) -> None:
        """Compute similarity for all product pairs (or sample for large datasets)."""
        import random

        products = list(self.product_components.keys())
        n = len(products)
        total_pairs = n * (n - 1) // 2

        # For large datasets, use sampling instead of computing all pairs
        use_sampling = n > 1000
        if use_sampling:
            # Sample ~50K random pairs to find interesting ones
            sample_size = min(50000, total_pairs)
            print(f"Sampling {sample_size} pairs from {n} products ({total_pairs} total pairs)...")
        else:
            print(f"Computing similarities for {n} products ({total_pairs} pairs)...")

        all_pairs: List[Tuple[str, str, float, str, str]] = []

        if use_sampling:
            # Random sampling approach for large datasets
            seen = set()
            attempts = 0
            max_attempts = sample_size * 3

            while len(all_pairs) < sample_size and attempts < max_attempts:
                attempts += 1
                i = random.randint(0, n - 1)
                j = random.randint(0, n - 1)
                if i == j:
                    continue
                if i > j:
                    i, j = j, i
                if (i, j) in seen:
                    continue
                seen.add((i, j))

                prod_a = products[i]
                prod_b = products[j]

                set_a = self.product_components[prod_a]
                set_b = self.product_components[prod_b]

                sim = self.compute_jaccard(set_a, set_b)
                shared = len(set_a & set_b)
                total = len(set_a | set_b)

                rel_type = self.classify_relationship(sim, shared, len(set_a), len(set_b))
                notes = f"{shared}/{total} components shared"

                all_pairs.append((prod_a, prod_b, sim, rel_type, notes))
        else:
            # Full enumeration for small datasets
            for i in range(n):
                for j in range(i + 1, n):
                    prod_a = products[i]
                    prod_b = products[j]

                    set_a = self.product_components[prod_a]
                    set_b = self.product_components[prod_b]

                    sim = self.compute_jaccard(set_a, set_b)
                    shared = len(set_a & set_b)
                    total = len(set_a | set_b)

                    rel_type = self.classify_relationship(sim, shared, len(set_a), len(set_b))
                    notes = f"{shared}/{total} components shared"

                    all_pairs.append((prod_a, prod_b, sim, rel_type, notes))

        # Sort by similarity (descending) to get most interesting pairs
        all_pairs.sort(key=lambda x: (-x[2], x[0], x[1]))

        # Select diverse set of pairs
        selected = []
        by_type: Dict[str, List] = defaultdict(list)

        for pair in all_pairs:
            by_type[pair[3]].append(pair)

        # Take some from each type
        for rel_type in ["identical", "variant", "partial_overlap", "low_overlap", "unrelated"]:
            pairs = by_type.get(rel_type, [])
            # Take up to 20 from each type
            for pair in pairs[:20]:
                if len(selected) < max_pairs:
                    selected.append(pair)

        # Fill remaining with highest similarity pairs
        for pair in all_pairs:
            if len(selected) >= max_pairs:
                break
            if pair not in selected:
                selected.append(pair)

        # Create ground truth records
        for prod_a, prod_b, sim, rel_type, notes in selected:
            self.ground_truth.append(GroundTruth(
                material_a=prod_a,
                material_b=prod_b,
                expected_similarity=round(sim, 4),
                relationship_type=rel_type,
                notes=notes,
            ))

        print(f"  Selected {len(self.ground_truth)} pairs for ground truth")

        # Print summary by type
        type_counts = defaultdict(int)
        for gt in self.ground_truth:
            type_counts[gt.relationship_type] += 1
        for rel_type, count in sorted(type_counts.items()):
            print(f"    {rel_type}: {count}")

    def write_output(self, output_path: Path) -> None:
        """Write ground truth CSV file."""
        with gzip.open(output_path, "wt", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "material_a", "material_b", "expected_similarity",
                "relationship_type", "notes"
            ])
            writer.writeheader()
            for gt in self.ground_truth:
                writer.writerow(asdict(gt))
        print(f"Wrote {len(self.ground_truth)} ground truth pairs to {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Compute ground truth similarity pairs for BOM datasets"
    )
    parser.add_argument(
        "--dataset", "-d",
        choices=["odoo", "erpnext", "figshare", "figshare_subassembly", "adventureworks", "pdxpert", "neo4j", "openhardware", "caterpillar"],
        required=True,
        help="Dataset to process"
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path("test/data"),
        help="Base data directory"
    )
    parser.add_argument(
        "--max-pairs",
        type=int,
        default=100,
        help="Maximum number of pairs to output"
    )

    args = parser.parse_args()

    # Handle relative paths
    data_dir = args.data_dir
    if not data_dir.exists():
        alt_dir = Path("..") / data_dir
        if alt_dir.exists():
            data_dir = alt_dir

    # Determine file paths based on dataset
    if args.dataset == "figshare_subassembly":
        dataset_dir = data_dir / "figshare"
        bom_path = dataset_dir / "bom_items_subassembly.csv.gz"
        materials_path = dataset_dir / "materials_subassembly.csv.gz"
        output_path = dataset_dir / "ground_truth_subassembly.csv.gz"
    else:
        dataset_dir = data_dir / args.dataset
        bom_path = dataset_dir / "bom_items.csv.gz"
        materials_path = dataset_dir / "materials.csv.gz"
        output_path = dataset_dir / "ground_truth.csv.gz"

    if not bom_path.exists():
        print(f"ERROR: BOM items file not found: {bom_path}")
        print(f"Run the appropriate parser first to generate the data.")
        return 1

    if not materials_path.exists():
        print(f"ERROR: Materials file not found: {materials_path}")
        return 1

    generator = GroundTruthGenerator(bom_path, materials_path)
    generator.load_data()

    if not generator.product_components:
        print("WARNING: No products with BOMs found. Check material_type values.")
        print("  Expected: FERT for finished goods")
        return 1

    generator.compute_all_pairs(max_pairs=args.max_pairs)
    generator.write_output(output_path)

    print("\nDone!")
    return 0


if __name__ == "__main__":
    exit(main())
