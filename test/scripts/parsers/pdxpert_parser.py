#!/usr/bin/env python3
"""
Parse PDXpert PLM sample data and convert to canonical format.

PDXpert is a Product Lifecycle Management system. The sample package contains
CSV files with item master and BOM data.

Usage:
    python -m parsers.pdxpert_parser --input test/data/pdxpert/raw --output test/data/pdxpert
"""

import argparse
import csv
import gzip
import json
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Optional, Set


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


class PDXpertParser:
    def __init__(self, input_dir: Path):
        self.input_dir = input_dir
        self.items: Dict[str, dict] = {}  # Part number -> item data
        self.bom_items: List[BOMItem] = []
        self.materials: Dict[str, Material] = {}

    def _clean_id(self, part_number: str) -> str:
        """Convert PDXpert part number to clean material ID."""
        clean = part_number.strip().upper().replace(" ", "-")
        return f"PDX-{clean}"

    def parse_item_master(self, csv_path: Path) -> None:
        """Parse ItemMaster CSV file."""
        print(f"Parsing item master from: {csv_path}")

        with open(csv_path, "r", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            for row in reader:
                # PDXpert uses "Number" for part number
                part_number = (
                    row.get("Number") or
                    row.get("Part Number") or
                    row.get("PartNumber") or
                    row.get("Item") or
                    row.get("ItemNumber") or
                    ""
                ).strip()

                if not part_number:
                    continue

                description = (
                    row.get("Description") or
                    row.get("Title") or
                    row.get("Name") or
                    part_number
                )

                item_type = (
                    row.get("Type") or
                    row.get("Class") or
                    row.get("ItemType") or
                    row.get("Part Type") or
                    ""
                )

                self.items[part_number] = {
                    "part_number": part_number,
                    "description": description,
                    "type": item_type,
                }

        print(f"  Found {len(self.items)} items")

    def parse_bom(self, csv_path: Path) -> None:
        """Parse ListBOM CSV file."""
        print(f"Parsing BOM from: {csv_path}")

        bom_count = 0
        position_counter: Dict[str, int] = defaultdict(int)

        with open(csv_path, "r", encoding="utf-8-sig") as f:
            reader = csv.DictReader(f)
            for row in reader:
                # PDXpert uses "Number" for parent and "ChildNumber" for child
                parent = (
                    row.get("Number") or
                    row.get("Parent") or
                    row.get("ParentPartNumber") or
                    row.get("Parent Part Number") or
                    row.get("Assembly") or
                    ""
                ).strip()

                child = (
                    row.get("ChildNumber") or
                    row.get("Child") or
                    row.get("ChildPartNumber") or
                    row.get("Child Part Number") or
                    row.get("Component") or
                    ""
                ).strip()

                if not parent or not child:
                    continue

                # Get quantity
                qty_str = (
                    row.get("Quantity") or
                    row.get("Qty") or
                    row.get("QtyPer") or
                    "1"
                )
                try:
                    qty = float(qty_str)
                except ValueError:
                    qty = 1.0

                # Create material IDs
                parent_id = self._clean_id(parent)
                child_id = self._clean_id(child)

                # Ensure materials exist
                self._ensure_material(parent, parent_id, is_parent=True)
                self._ensure_material(child, child_id, is_parent=False)

                # Generate BOM ID based on parent
                bom_id = f"BOM-{parent_id}"
                position_counter[bom_id] += 10
                position = position_counter[bom_id]

                self.bom_items.append(BOMItem(
                    bom_id=bom_id,
                    parent_id=parent_id,
                    child_id=child_id,
                    quantity=qty,
                    level=1,  # Will be computed later
                    position=position,
                ))
                bom_count += 1

        print(f"  Found {bom_count} BOM items")

    def _ensure_material(self, part_number: str, material_id: str, is_parent: bool = False) -> None:
        """Ensure a material record exists."""
        if material_id in self.materials:
            return

        item = self.items.get(part_number)
        if item:
            description = item["description"]
            item_type = item.get("type", "").lower()

            # Determine material type
            if is_parent:
                mat_type = "FERT"
            elif "assembly" in item_type or "assy" in item_type:
                mat_type = "HALB"
            else:
                mat_type = "ROH"
        else:
            description = part_number
            mat_type = "FERT" if is_parent else "ROH"

        self.materials[material_id] = Material(
            material_id=material_id,
            description=description,
            material_group="PLM",
            material_type=mat_type,
        )

    def _compute_levels(self) -> None:
        """Compute hierarchical levels for BOM items."""
        # Build parent-child graph
        children: Dict[str, Set[str]] = defaultdict(set)
        for item in self.bom_items:
            children[item.parent_id].add(item.child_id)

        # Find all parents and children
        all_children = set()
        all_parents = set()
        for item in self.bom_items:
            all_children.add(item.child_id)
            all_parents.add(item.parent_id)

        # Root materials are parents that are not children
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

        # Compute stats
        max_level = max((item.level for item in self.bom_items), default=0)
        unique_parents = len(set(item.parent_id for item in self.bom_items))

        # Write metadata.json
        metadata = {
            "source": "pdxpert",
            "url": "https://www.buyplm.com/resources/Example-PDXpertImport.zip",
            "license": "Free evaluation",
            "description": "PDXpert PLM sample import package",
            "n_materials": len(materials),
            "n_bom_items": len(self.bom_items),
            "n_assemblies": unique_parents,
            "max_level": max_level,
        }
        metadata_path = output_dir / "metadata.json"
        with open(metadata_path, "w") as f:
            json.dump(metadata, f, indent=2)
        print(f"Wrote metadata to {metadata_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Parse PDXpert PLM sample data and convert to canonical format"
    )
    parser.add_argument(
        "--input", "-i",
        type=Path,
        default=Path("test/data/pdxpert/raw"),
        help="Input directory with raw CSV files"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("test/data/pdxpert"),
        help="Output directory for canonical CSV files"
    )

    args = parser.parse_args()

    if not args.input.exists():
        print(f"ERROR: Input directory not found: {args.input}")
        print("Run download_erp_bom_data.py --commercial-only first.")
        return 1

    pdx_parser = PDXpertParser(args.input)

    # Find and parse item master file
    item_files = list(args.input.glob("*ItemMaster*.csv")) + list(args.input.glob("*Item*.csv"))
    if item_files:
        pdx_parser.parse_item_master(item_files[0])
    else:
        print("Warning: No item master file found")

    # Find and parse BOM file
    bom_files = list(args.input.glob("*ListBOM*.csv")) + list(args.input.glob("*BOM*.csv"))
    if bom_files:
        pdx_parser.parse_bom(bom_files[0])
    else:
        print("Warning: No BOM file found")
        return 1

    # Compute levels and write output
    pdx_parser._compute_levels()
    pdx_parser.write_output(args.output)

    print("\nDone! Next step: run compute_ground_truth.py --dataset pdxpert")
    return 0


if __name__ == "__main__":
    exit(main())
