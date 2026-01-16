#!/usr/bin/env python3
"""
Parse ERPNext BOM test records JSON and convert to canonical BOM format.

Usage:
    python -m parsers.erpnext_parser --input test/data/erpnext/raw --output test/data/erpnext
"""

import argparse
import csv
import gzip
import json
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Set


@dataclass
class Material:
    material_id: str
    description: str
    material_group: str
    material_type: str
    created_date: str = "2024-01-01"


@dataclass
class BOMItem:
    bom_id: str
    parent_id: str
    child_id: str
    quantity: float
    level: int
    position: int


class ERPNextParser:
    def __init__(self, input_dir: Path):
        self.input_dir = input_dir
        self.materials: Dict[str, Material] = {}  # item_code -> Material
        self.bom_headers: Dict[str, dict] = {}  # BOM name -> header info
        self.bom_items: List[BOMItem] = []

    def _clean_id(self, item_code: str) -> str:
        """Convert ERPNext item code to clean material ID."""
        # Remove test prefixes and clean up
        clean = item_code.replace("_Test ", "").replace("_test ", "")
        clean = clean.replace(" ", "-").upper()
        return f"ERPNEXT-{clean}"

    def _determine_material_type(self, item_name: str, is_bom_parent: bool = False) -> str:
        """Determine material type from item name."""
        name_lower = item_name.lower()
        if is_bom_parent:
            return "FERT"  # Finished good
        elif any(x in name_lower for x in ["manufactured", "assembly"]):
            return "HALB"  # Semi-finished
        elif any(x in name_lower for x in ["raw", "material"]):
            return "ROH"  # Raw material
        else:
            return "ROH"

    def parse_items(self, json_path: Path) -> None:
        """Parse item test records from JSON file."""
        print(f"Parsing items from: {json_path}")

        with open(json_path) as f:
            records = json.load(f)

        for record in records:
            if record.get("doctype") == "Item":
                item_code = record.get("item_code", record.get("item_name", ""))
                if not item_code:
                    continue

                material_id = self._clean_id(item_code)
                item_name = record.get("item_name", item_code)
                item_group = record.get("item_group", "Products")

                material = Material(
                    material_id=material_id,
                    description=item_name,
                    material_group=item_group,
                    material_type=self._determine_material_type(item_name),
                )

                self.materials[item_code] = material

        print(f"  Found {len(self.materials)} items")

    def parse_boms(self, json_path: Path) -> None:
        """Parse BOM test records from JSON file."""
        print(f"Parsing BOMs from: {json_path}")

        with open(json_path) as f:
            records = json.load(f)

        bom_count = 0
        for record in records:
            if record.get("doctype") == "BOM":
                bom_name = record.get("name", "")
                parent_item = record.get("item", "")

                if not parent_item:
                    continue

                # Ensure parent material exists
                if parent_item not in self.materials:
                    material_id = self._clean_id(parent_item)
                    self.materials[parent_item] = Material(
                        material_id=material_id,
                        description=parent_item,
                        material_group="Products",
                        material_type="FERT",
                    )
                else:
                    material_id = self.materials[parent_item].material_id

                bom_id = f"BOM-{material_id}"

                self.bom_headers[bom_name] = {
                    "bom_id": bom_id,
                    "parent_id": material_id,
                    "parent_item_code": parent_item,
                }

                # Process BOM items
                items = record.get("items", [])
                for i, item in enumerate(items):
                    child_item = item.get("item_code", "")
                    if not child_item:
                        continue

                    # Ensure child material exists
                    if child_item not in self.materials:
                        child_material_id = self._clean_id(child_item)
                        self.materials[child_item] = Material(
                            material_id=child_material_id,
                            description=child_item,
                            material_group="Components",
                            material_type="ROH",
                        )
                    else:
                        child_material_id = self.materials[child_item].material_id

                    qty = float(item.get("qty", 1.0))
                    position = (i + 1) * 10

                    # Check if this item has a sub-BOM
                    sub_bom_no = item.get("bom_no", "")
                    level = 2 if sub_bom_no else 1

                    self.bom_items.append(BOMItem(
                        bom_id=bom_id,
                        parent_id=material_id,
                        child_id=child_material_id,
                        quantity=qty,
                        level=level,
                        position=position,
                    ))

                bom_count += 1

        print(f"  Found {bom_count} BOMs, {len(self.bom_items)} BOM items")

    def _compute_levels(self) -> None:
        """Compute hierarchical levels for BOM items based on sub-BOM references."""
        # Build parent-child graph
        children: Dict[str, Set[str]] = defaultdict(set)
        for item in self.bom_items:
            children[item.parent_id].add(item.child_id)

        # Find root materials (finished goods with no parent)
        all_children = set()
        all_parents = set()
        for item in self.bom_items:
            all_children.add(item.child_id)
            all_parents.add(item.parent_id)

        roots = all_parents - all_children

        # Compute levels via BFS
        levels: Dict[str, int] = {}
        for root in roots:
            levels[root] = 0

        changed = True
        max_iter = 100
        iter_count = 0
        while changed and iter_count < max_iter:
            changed = False
            iter_count += 1
            for item in self.bom_items:
                if item.parent_id in levels:
                    new_level = levels[item.parent_id] + 1
                    if item.child_id not in levels or levels[item.child_id] < new_level:
                        levels[item.child_id] = new_level
                        changed = True

        # Update BOM items with levels
        for item in self.bom_items:
            parent_level = levels.get(item.parent_id, 0)
            item.level = parent_level + 1

    def write_output(self, output_dir: Path) -> None:
        """Write canonical CSV files."""
        output_dir.mkdir(parents=True, exist_ok=True)

        materials = list(self.materials.values())

        # Write materials.csv.gz
        materials_path = output_dir / "materials.csv.gz"
        with gzip.open(materials_path, "wt", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "material_id", "description", "material_group", "material_type", "created_date"
            ])
            writer.writeheader()
            for mat in materials:
                writer.writerow(asdict(mat))
        print(f"Wrote {len(materials)} materials to {materials_path}")

        # Write bom_items.csv.gz
        bom_path = output_dir / "bom_items.csv.gz"
        with gzip.open(bom_path, "wt", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "bom_id", "parent_id", "child_id", "quantity", "level", "position"
            ])
            writer.writeheader()
            for item in self.bom_items:
                writer.writerow(asdict(item))
        print(f"Wrote {len(self.bom_items)} BOM items to {bom_path}")

        # Write metadata.json
        metadata = {
            "source": "erpnext",
            "branch": "develop",
            "license": "GPL-3.0",
            "repository": "https://github.com/frappe/erpnext",
            "n_materials": len(materials),
            "n_bom_items": len(self.bom_items),
            "n_boms": len(self.bom_headers),
            "max_level": max((item.level for item in self.bom_items), default=0),
        }
        metadata_path = output_dir / "metadata.json"
        with open(metadata_path, "w") as f:
            json.dump(metadata, f, indent=2)
        print(f"Wrote metadata to {metadata_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Parse ERPNext BOM test records and convert to canonical format"
    )
    parser.add_argument(
        "--input", "-i",
        type=Path,
        default=Path("test/data/erpnext/raw"),
        help="Input directory with raw JSON files"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("test/data/erpnext"),
        help="Output directory for canonical CSV files"
    )

    args = parser.parse_args()

    if not args.input.exists():
        print(f"ERROR: Input directory not found: {args.input}")
        print("Run download_erp_bom_data.py first to download the raw files.")
        return 1

    erpnext_parser = ERPNextParser(args.input)

    # Parse item test records
    item_file = args.input / "item_test_records.json"
    if item_file.exists():
        erpnext_parser.parse_items(item_file)
    else:
        print(f"Warning: {item_file} not found")

    # Parse BOM test records
    bom_file = args.input / "bom_test_records.json"
    if bom_file.exists():
        erpnext_parser.parse_boms(bom_file)
    else:
        print(f"Warning: {bom_file} not found")

    # Compute levels and write output
    erpnext_parser._compute_levels()
    erpnext_parser.write_output(args.output)

    print("\nDone! Next step: run compute_ground_truth.py to generate similarity pairs")
    return 0


if __name__ == "__main__":
    exit(main())
