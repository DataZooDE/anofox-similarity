#!/usr/bin/env python3
"""
Parse Odoo MRP demo data XML files and convert to canonical BOM format.

Usage:
    python -m parsers.odoo_parser --input test/data/odoo/raw --output test/data/odoo
"""

import argparse
import csv
import gzip
import json
import re
import xml.etree.ElementTree as ET
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


@dataclass
class BOMHeader:
    bom_id: str
    parent_id: str
    is_phantom: bool = False


class OdooParser:
    def __init__(self, input_dir: Path):
        self.input_dir = input_dir
        self.products: Dict[str, Material] = {}  # external_id -> Material
        self.bom_headers: Dict[str, BOMHeader] = {}  # external_id -> BOMHeader
        self.bom_lines: List[dict] = []  # Raw BOM line data
        self.external_id_map: Dict[str, str] = {}  # external_id -> material_id

    def _resolve_ref(self, ref: str) -> Optional[str]:
        """Resolve an Odoo external ID reference to material_id."""
        if ref in self.external_id_map:
            return self.external_id_map[ref]
        # Try without module prefix
        if "." in ref:
            short_ref = ref.split(".")[-1]
            if short_ref in self.external_id_map:
                return self.external_id_map[short_ref]
        return None

    def _parse_field(self, record: ET.Element, field_name: str) -> Optional[str]:
        """Get field value from record."""
        for field in record.findall("field"):
            if field.get("name") == field_name:
                # Check for ref attribute
                if field.get("ref"):
                    return field.get("ref")
                # Check for eval attribute (e.g., True, False)
                if field.get("eval"):
                    return field.get("eval")
                # Return text content
                return field.text
        return None

    def _clean_id(self, external_id: str) -> str:
        """Convert Odoo external ID to clean material ID."""
        # Remove module prefix and convert to uppercase
        if "." in external_id:
            external_id = external_id.split(".")[-1]
        # Replace underscores and make readable
        clean = external_id.upper().replace("_", "-")
        return f"ODOO-{clean}"

    def parse_products(self, xml_path: Path) -> None:
        """Parse product.product records from XML file."""
        print(f"Parsing products from: {xml_path}")

        tree = ET.parse(xml_path)
        root = tree.getroot()

        # Find all product.product and product.template records
        for data in root.findall(".//data"):
            for record in data.findall("record"):
                model = record.get("model")
                ext_id = record.get("id")

                if model in ("product.product", "product.template"):
                    name = self._parse_field(record, "name")
                    categ_ref = self._parse_field(record, "categ_id")

                    if not name:
                        name = ext_id

                    # Determine material type based on patterns
                    name_lower = (name or "").lower()
                    if any(x in name_lower for x in ["kit", "combo", "set"]):
                        mat_type = "KIT"
                    elif any(x in name_lower for x in ["desk", "chair", "table", "cabinet"]):
                        mat_type = "FERT"  # Finished good
                    elif any(x in name_lower for x in ["panel", "top", "leg", "drawer"]):
                        mat_type = "HALB"  # Semi-finished
                    else:
                        mat_type = "ROH"  # Raw material

                    material_id = self._clean_id(ext_id)
                    material = Material(
                        material_id=material_id,
                        description=name or "",
                        material_group=categ_ref or "Furniture",
                        material_type=mat_type,
                    )

                    self.products[ext_id] = material
                    self.external_id_map[ext_id] = material_id
                    # Also map short version
                    if "." in ext_id:
                        short_id = ext_id.split(".")[-1]
                        self.external_id_map[short_id] = material_id

        print(f"  Found {len(self.products)} products")

    def parse_boms(self, xml_path: Path) -> None:
        """Parse mrp.bom and mrp.bom.line records from XML file."""
        print(f"Parsing BOMs from: {xml_path}")

        tree = ET.parse(xml_path)
        root = tree.getroot()

        bom_count = 0
        line_count = 0

        for data in root.findall(".//data"):
            for record in data.findall("record"):
                model = record.get("model")
                ext_id = record.get("id")

                if model == "mrp.bom":
                    product_ref = self._parse_field(record, "product_tmpl_id")
                    bom_type = self._parse_field(record, "type")

                    if product_ref:
                        bom_id = self._clean_id(ext_id)
                        parent_id = self._resolve_ref(product_ref)

                        if not parent_id:
                            # Create a material for this product if not found
                            parent_id = self._clean_id(product_ref)

                        self.bom_headers[ext_id] = BOMHeader(
                            bom_id=bom_id,
                            parent_id=parent_id,
                            is_phantom=(bom_type == "phantom"),
                        )
                        self.external_id_map[ext_id] = bom_id
                        bom_count += 1

                elif model == "mrp.bom.line":
                    bom_ref = self._parse_field(record, "bom_id")
                    product_ref = self._parse_field(record, "product_id")
                    qty_str = self._parse_field(record, "product_qty") or "1.0"

                    # Handle eval expressions
                    try:
                        qty = float(qty_str)
                    except (ValueError, TypeError):
                        qty = 1.0

                    self.bom_lines.append({
                        "ext_id": ext_id,
                        "bom_ref": bom_ref,
                        "product_ref": product_ref,
                        "quantity": qty,
                    })
                    line_count += 1

        print(f"  Found {bom_count} BOMs, {line_count} BOM lines")

    def _compute_levels(self, bom_items: List[BOMItem]) -> List[BOMItem]:
        """Compute hierarchical levels for BOM items."""
        # Build parent-child graph
        children: Dict[str, Set[str]] = defaultdict(set)
        for item in bom_items:
            children[item.parent_id].add(item.child_id)

        # Find root materials (finished goods with no parent)
        all_children = set()
        all_parents = set()
        for item in bom_items:
            all_children.add(item.child_id)
            all_parents.add(item.parent_id)

        roots = all_parents - all_children

        # Compute levels via BFS
        levels: Dict[str, int] = {}
        for root in roots:
            levels[root] = 0

        changed = True
        while changed:
            changed = False
            for item in bom_items:
                if item.parent_id in levels:
                    new_level = levels[item.parent_id] + 1
                    if item.child_id not in levels or levels[item.child_id] < new_level:
                        levels[item.child_id] = new_level
                        changed = True

        # Update BOM items with levels
        result = []
        for item in bom_items:
            # Level is relative to parent
            parent_level = levels.get(item.parent_id, 0)
            result.append(BOMItem(
                bom_id=item.bom_id,
                parent_id=item.parent_id,
                child_id=item.child_id,
                quantity=item.quantity,
                level=parent_level + 1,
                position=item.position,
            ))

        return result

    def build_canonical_data(self) -> tuple[List[Material], List[BOMItem]]:
        """Build canonical materials and BOM items from parsed data."""
        materials = list(self.products.values())
        bom_items = []

        position_counter: Dict[str, int] = defaultdict(int)

        for line in self.bom_lines:
            bom_ref = line["bom_ref"]
            product_ref = line["product_ref"]

            if bom_ref not in self.bom_headers:
                continue

            header = self.bom_headers[bom_ref]
            child_id = self._resolve_ref(product_ref)

            if not child_id:
                # Create material for unknown product
                child_id = self._clean_id(product_ref)
                if child_id not in [m.material_id for m in materials]:
                    materials.append(Material(
                        material_id=child_id,
                        description=product_ref,
                        material_group="Components",
                        material_type="ROH",
                    ))

            position_counter[header.bom_id] += 10
            position = position_counter[header.bom_id]

            bom_items.append(BOMItem(
                bom_id=header.bom_id,
                parent_id=header.parent_id,
                child_id=child_id,
                quantity=line["quantity"],
                level=1,  # Will be computed later
                position=position,
            ))

        # Compute hierarchical levels
        bom_items = self._compute_levels(bom_items)

        return materials, bom_items

    def write_output(self, output_dir: Path, materials: List[Material], bom_items: List[BOMItem]) -> None:
        """Write canonical CSV files."""
        output_dir.mkdir(parents=True, exist_ok=True)

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
            for item in bom_items:
                writer.writerow(asdict(item))
        print(f"Wrote {len(bom_items)} BOM items to {bom_path}")

        # Write metadata.json
        metadata = {
            "source": "odoo",
            "version": "18.0",
            "license": "LGPL-3.0",
            "repository": "https://github.com/odoo/odoo",
            "n_materials": len(materials),
            "n_bom_items": len(bom_items),
            "n_boms": len(self.bom_headers),
            "phantom_boms": sum(1 for h in self.bom_headers.values() if h.is_phantom),
            "max_level": max((item.level for item in bom_items), default=0),
        }
        metadata_path = output_dir / "metadata.json"
        with open(metadata_path, "w") as f:
            json.dump(metadata, f, indent=2)
        print(f"Wrote metadata to {metadata_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Parse Odoo MRP demo data and convert to canonical format"
    )
    parser.add_argument(
        "--input", "-i",
        type=Path,
        default=Path("test/data/odoo/raw"),
        help="Input directory with raw XML files"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("test/data/odoo"),
        help="Output directory for canonical CSV files"
    )

    args = parser.parse_args()

    if not args.input.exists():
        print(f"ERROR: Input directory not found: {args.input}")
        print("Run download_erp_bom_data.py first to download the raw files.")
        return 1

    odoo_parser = OdooParser(args.input)

    # Parse product demo data
    product_file = args.input / "product_demo.xml"
    if product_file.exists():
        odoo_parser.parse_products(product_file)
    else:
        print(f"Warning: {product_file} not found")

    # Parse MRP demo data (contains both BOM headers and lines)
    mrp_demo = args.input / "mrp_demo.xml"
    if mrp_demo.exists():
        odoo_parser.parse_boms(mrp_demo)
    else:
        print(f"Warning: {mrp_demo} not found")

    # Parse MRP data (contains additional BOM types)
    mrp_data = args.input / "mrp_data.xml"
    if mrp_data.exists():
        odoo_parser.parse_boms(mrp_data)

    # Build and write canonical data
    materials, bom_items = odoo_parser.build_canonical_data()
    odoo_parser.write_output(args.output, materials, bom_items)

    print("\nDone! Next step: run compute_ground_truth.py to generate similarity pairs")
    return 0


if __name__ == "__main__":
    exit(main())
