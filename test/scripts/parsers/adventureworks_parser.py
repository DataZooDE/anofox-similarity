#!/usr/bin/env python3
"""
Parse Microsoft AdventureWorks BOM data and convert to canonical format.

AdventureWorks is a bicycle manufacturing company simulation with 2,679 BOM rows
across 7+ hierarchical levels.

Usage:
    python -m parsers.adventureworks_parser --input test/data/adventureworks/raw --output test/data/adventureworks
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


# AdventureWorks BillOfMaterials.csv columns (tab-separated, no header)
# Based on SQL Server schema: Production.BillOfMaterials
BOM_COLUMNS = [
    "BillOfMaterialsID",
    "ProductAssemblyID",
    "ComponentID",
    "StartDate",
    "EndDate",
    "UnitMeasureCode",
    "BOMLevel",
    "PerAssemblyQty",
    "ModifiedDate",
]

# AdventureWorks Product.csv columns (tab-separated, no header)
# Based on SQL Server schema: Production.Product
PRODUCT_COLUMNS = [
    "ProductID",
    "Name",
    "ProductNumber",
    "MakeFlag",
    "FinishedGoodsFlag",
    "Color",
    "SafetyStockLevel",
    "ReorderPoint",
    "StandardCost",
    "ListPrice",
    "Size",
    "SizeUnitMeasureCode",
    "WeightUnitMeasureCode",
    "Weight",
    "DaysToManufacture",
    "ProductLine",
    "Class",
    "Style",
    "ProductSubcategoryID",
    "ProductModelID",
    "SellStartDate",
    "SellEndDate",
    "DiscontinuedDate",
    "rowguid",
    "ModifiedDate",
]


class AdventureWorksParser:
    def __init__(self, input_dir: Path):
        self.input_dir = input_dir
        self.products: Dict[int, dict] = {}  # ProductID -> product data
        self.bom_items: List[BOMItem] = []
        self.materials: Dict[str, Material] = {}

    def parse_products(self, csv_path: Path) -> None:
        """Parse Product.csv (tab-separated, no header)."""
        print(f"Parsing products from: {csv_path}")

        with open(csv_path, "r", encoding="utf-8") as f:
            reader = csv.reader(f, delimiter="\t")
            for row in reader:
                if len(row) < 5:
                    continue

                try:
                    product_id = int(row[0])
                    name = row[1]
                    product_number = row[2]
                    make_flag = row[3] == "1"
                    finished_goods_flag = row[4] == "1"

                    self.products[product_id] = {
                        "id": product_id,
                        "name": name,
                        "number": product_number,
                        "make_flag": make_flag,
                        "finished_goods_flag": finished_goods_flag,
                    }
                except (ValueError, IndexError):
                    continue

        print(f"  Found {len(self.products)} products")

    def parse_bom(self, csv_path: Path) -> None:
        """Parse BillOfMaterials.csv (tab-separated, no header)."""
        print(f"Parsing BOM from: {csv_path}")

        bom_count = 0
        position_counter: Dict[str, int] = defaultdict(int)

        with open(csv_path, "r", encoding="utf-8") as f:
            reader = csv.reader(f, delimiter="\t")
            for row in reader:
                if len(row) < 8:
                    continue

                try:
                    bom_id_raw = int(row[0])
                    product_assembly_id = int(row[1]) if row[1] else None
                    component_id = int(row[2])
                    bom_level = int(row[6])
                    qty = float(row[7])
                except (ValueError, IndexError):
                    continue

                # Skip rows with no parent (these are raw materials)
                if product_assembly_id is None:
                    continue

                # Create material IDs
                parent_id = f"AW-{product_assembly_id}"
                child_id = f"AW-{component_id}"

                # Ensure materials exist
                self._ensure_material(product_assembly_id, parent_id)
                self._ensure_material(component_id, child_id)

                # Generate BOM ID based on parent
                bom_id = f"BOM-{parent_id}"
                position_counter[bom_id] += 10
                position = position_counter[bom_id]

                self.bom_items.append(BOMItem(
                    bom_id=bom_id,
                    parent_id=parent_id,
                    child_id=child_id,
                    quantity=qty,
                    level=bom_level,
                    position=position,
                ))
                bom_count += 1

        print(f"  Found {bom_count} BOM items")

    def _ensure_material(self, product_id: int, material_id: str) -> None:
        """Ensure a material record exists."""
        if material_id in self.materials:
            return

        product = self.products.get(product_id)
        if product:
            name = product["name"]
            # Determine material type
            if product["finished_goods_flag"]:
                mat_type = "FERT"
            elif product["make_flag"]:
                mat_type = "HALB"
            else:
                mat_type = "ROH"
        else:
            name = f"Product {product_id}"
            mat_type = "ROH"

        self.materials[material_id] = Material(
            material_id=material_id,
            description=name,
            material_group="Bicycles",
            material_type=mat_type,
        )

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
            "source": "adventureworks",
            "repository": "https://github.com/Microsoft/sql-server-samples",
            "license": "MIT",
            "description": "Microsoft AdventureWorks bicycle manufacturing sample",
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
        description="Parse AdventureWorks BOM data and convert to canonical format"
    )
    parser.add_argument(
        "--input", "-i",
        type=Path,
        default=Path("test/data/adventureworks/raw"),
        help="Input directory with raw CSV files"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("test/data/adventureworks"),
        help="Output directory for canonical CSV files"
    )

    args = parser.parse_args()

    if not args.input.exists():
        print(f"ERROR: Input directory not found: {args.input}")
        print("Run download_erp_bom_data.py --commercial-only first.")
        return 1

    aw_parser = AdventureWorksParser(args.input)

    # Parse product data
    product_file = args.input / "Product.csv"
    if product_file.exists():
        aw_parser.parse_products(product_file)
    else:
        print(f"Warning: {product_file} not found")

    # Parse BOM data
    bom_file = args.input / "BillOfMaterials.csv"
    if bom_file.exists():
        aw_parser.parse_bom(bom_file)
    else:
        print(f"Warning: {bom_file} not found")
        return 1

    # Write output
    aw_parser.write_output(args.output)

    print("\nDone! Next step: run compute_ground_truth.py --dataset adventureworks")
    return 0


if __name__ == "__main__":
    exit(main())
