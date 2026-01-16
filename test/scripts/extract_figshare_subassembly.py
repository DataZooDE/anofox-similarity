#!/usr/bin/env python3
"""
Extract sub-assembly level BOM data from Figshare Disassembly Detail workbook.

This creates a two-level BOM hierarchy:
  Level 1: Product -> Components (Display, Main body, etc.)
  Level 2: Component -> Materials (Aluminum, Copper, etc.)

Usage:
    cd scripts && uv run python ../test/scripts/extract_figshare_subassembly.py
"""

import argparse
import csv
import gzip
import json
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Optional, Set
import re


@dataclass
class Material:
    material_id: str
    description: str
    material_group: str
    material_type: str
    created_date: str = "2020-06-01"


@dataclass
class BOMItem:
    bom_id: str
    parent_id: str
    child_id: str
    quantity: float
    level: int
    position: int


class FigshareExtractor:
    # Material columns in the Excel file
    MATERIAL_COLUMNS = [
        "Aluminum", "Copper", "Steel", "Plastic", "Li-ion battery",
        "PCB", "Flat panel glass", "CRT glass", "Other glass",
        "Other metals", "Others"
    ]

    def __init__(self, excel_path: Path):
        self.excel_path = excel_path
        self.materials: Dict[str, Material] = {}
        self.bom_items: List[BOMItem] = []
        self.products_processed = 0

        # Pre-create base material records
        self._create_base_materials()

    def _clean_id(self, name: str, prefix: str = "FIG") -> str:
        """Convert name to clean material ID."""
        # Remove special chars, normalize
        clean = re.sub(r'[^a-zA-Z0-9\s-]', '', name)
        clean = clean.strip().upper().replace(' ', '-')
        # Truncate if too long
        if len(clean) > 40:
            clean = clean[:40]
        return f"{prefix}-{clean}"

    def _create_base_materials(self) -> None:
        """Create material records for base materials (Aluminum, Copper, etc.)."""
        for mat_name in self.MATERIAL_COLUMNS:
            mat_id = self._clean_id(mat_name, "MAT")
            self.materials[mat_id] = Material(
                material_id=mat_id,
                description=mat_name,
                material_group="Raw Materials",
                material_type="ROH",
            )

    def _parse_sheet(self, df, category: str) -> None:
        """Parse a single sheet (product category) from the Excel file."""
        import pandas as pd

        current_product: Optional[str] = None
        current_product_id: Optional[str] = None
        current_component: Optional[str] = None
        current_component_id: Optional[str] = None
        position_counter = 0

        for idx, row in df.iterrows():
            first_col = row.iloc[0] if pd.notna(row.iloc[0]) else None
            second_col = row.iloc[1] if len(row) > 1 and pd.notna(row.iloc[1]) else None

            # Skip header rows
            if first_col and "Product name" in str(first_col):
                continue

            # New product
            if first_col and first_col != current_product:
                current_product = str(first_col).strip()
                current_product_id = self._clean_id(current_product, "PROD")
                position_counter = 0

                # Create product material record
                if current_product_id not in self.materials:
                    self.materials[current_product_id] = Material(
                        material_id=current_product_id,
                        description=current_product,
                        material_group=category,
                        material_type="FERT",
                    )
                    self.products_processed += 1

            # Skip if no product context
            if not current_product_id:
                continue

            # Component row (e.g., "Display", "Main body")
            if second_col and second_col != "Total mass (g)":
                component_name = str(second_col).strip()

                # Skip empty or summary rows
                if not component_name or component_name.lower() in ["nan", "total"]:
                    continue

                current_component = f"{current_product} - {component_name}"
                current_component_id = self._clean_id(current_component, "COMP")

                # Create component material record
                if current_component_id not in self.materials:
                    self.materials[current_component_id] = Material(
                        material_id=current_component_id,
                        description=current_component,
                        material_group=f"{category} - Components",
                        material_type="HALB",
                    )

                # Add Level 1 BOM: Product -> Component
                position_counter += 10
                self.bom_items.append(BOMItem(
                    bom_id=f"BOM-{current_product_id}",
                    parent_id=current_product_id,
                    child_id=current_component_id,
                    quantity=1.0,
                    level=1,
                    position=position_counter,
                ))

                # Add Level 2 BOMs: Component -> Materials
                mat_position = 0
                for i, mat_name in enumerate(self.MATERIAL_COLUMNS):
                    col_idx = i + 2  # Materials start at column 2
                    if col_idx < len(row):
                        value = row.iloc[col_idx]
                        if pd.notna(value):
                            try:
                                float_val = float(value)
                                if float_val > 0:
                                    mat_id = self._clean_id(mat_name, "MAT")
                                    mat_position += 10
                                    self.bom_items.append(BOMItem(
                                        bom_id=f"BOM-{current_component_id}",
                                        parent_id=current_component_id,
                                        child_id=mat_id,
                                        quantity=float_val,
                                        level=2,
                                        position=mat_position,
                                    ))
                            except (ValueError, TypeError):
                                # Skip non-numeric values
                                pass

    def extract(self) -> None:
        """Extract all data from the Excel workbook."""
        import pandas as pd

        print(f"Reading Excel file: {self.excel_path}")
        xl = pd.ExcelFile(self.excel_path)

        print(f"Found {len(xl.sheet_names)} sheets (product categories)")

        for sheet_name in xl.sheet_names:
            print(f"  Processing: {sheet_name}")
            df = pd.read_excel(xl, sheet_name=sheet_name, header=None)
            self._parse_sheet(df, sheet_name.strip())

        print(f"\nExtracted:")
        print(f"  Products: {self.products_processed}")
        print(f"  Total materials: {len(self.materials)}")
        print(f"  BOM items: {len(self.bom_items)}")

    def write_output(self, output_dir: Path) -> None:
        """Write canonical CSV files."""
        output_dir.mkdir(parents=True, exist_ok=True)

        materials = list(self.materials.values())

        # Write materials.csv.gz
        materials_path = output_dir / "materials_subassembly.csv.gz"
        with gzip.open(materials_path, "wt", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "material_id", "description", "material_group", "material_type", "created_date"
            ])
            writer.writeheader()
            for mat in materials:
                writer.writerow(asdict(mat))
        print(f"Wrote {len(materials)} materials to {materials_path}")

        # Write bom_items.csv.gz
        bom_path = output_dir / "bom_items_subassembly.csv.gz"
        with gzip.open(bom_path, "wt", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=[
                "bom_id", "parent_id", "child_id", "quantity", "level", "position"
            ])
            writer.writeheader()
            for item in self.bom_items:
                writer.writerow(asdict(item))
        print(f"Wrote {len(self.bom_items)} BOM items to {bom_path}")

        # Count unique products and components
        level1_items = [i for i in self.bom_items if i.level == 1]
        level2_items = [i for i in self.bom_items if i.level == 2]

        # Write metadata.json
        metadata = {
            "source": "figshare_subassembly",
            "doi": "10.6084/m9.figshare.11306792.v4",
            "license": "CC0",
            "description": "Sub-assembly level BOM extracted from Disassembly Detail workbook",
            "n_materials": len(materials),
            "n_products": self.products_processed,
            "n_components": len(set(i.child_id for i in level1_items)),
            "n_bom_items": len(self.bom_items),
            "n_level1_items": len(level1_items),
            "n_level2_items": len(level2_items),
            "max_level": 2,
        }
        metadata_path = output_dir / "metadata_subassembly.json"
        with open(metadata_path, "w") as f:
            json.dump(metadata, f, indent=2)
        print(f"Wrote metadata to {metadata_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Extract sub-assembly BOM data from Figshare Disassembly Detail"
    )
    parser.add_argument(
        "--input", "-i",
        type=Path,
        default=Path("test/data/figshare/raw/Disassembly Detail_June2020.xlsx"),
        help="Input Excel file"
    )
    parser.add_argument(
        "--output", "-o",
        type=Path,
        default=Path("test/data/figshare"),
        help="Output directory for canonical CSV files"
    )

    args = parser.parse_args()

    # Handle relative paths when run from scripts directory
    if not args.input.exists():
        alt_input = Path("..") / args.input
        if alt_input.exists():
            args.input = alt_input
        else:
            print(f"ERROR: Input file not found: {args.input}")
            return 1

    if not args.output.exists():
        alt_output = Path("..") / args.output
        if alt_output.exists():
            args.output = alt_output

    extractor = FigshareExtractor(args.input)
    extractor.extract()
    extractor.write_output(args.output)

    print("\nDone! Sub-assembly data extracted.")
    print("Note: This creates separate files (*_subassembly.csv.gz) alongside existing data.")
    return 0


if __name__ == "__main__":
    exit(main())
