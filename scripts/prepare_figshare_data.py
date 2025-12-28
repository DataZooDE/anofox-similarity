#!/usr/bin/env python3
"""
Transform Figshare Consumer Electronics dataset to anofox-similarity format.

Reads the raw Excel files and produces:
- materials.csv: Product catalog
- bom_items.csv: Product-to-material-type relationships
- ground_truth.csv: Expected similarities based on product families
- metadata.json: Dataset metadata

The Figshare dataset contains material composition (mass in grams) for
consumer electronics products. We transform this into a BOM-like structure:
- Products = finished goods (FERT)
- Material types = components (HALB) - e.g., Aluminum, Copper, Plastic
- BOM relationship = product contains material type with quantity = mass

Usage:
    python scripts/prepare_figshare_data.py
    python scripts/prepare_figshare_data.py --input test/data/figshare/raw --output test/data/figshare
"""

import argparse
import csv
import gzip
import io
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import pandas as pd


@dataclass
class Product:
    """A product from the Figshare dataset."""
    product_id: str
    name: str
    category: str
    year: Optional[int] = None


@dataclass
class MaterialComponent:
    """A material type used in products."""
    component_id: str
    name: str
    description: str


@dataclass
class BOMItem:
    """A BOM relationship: product contains material type."""
    parent_id: str
    child_id: str
    quantity: float  # mass in grams


@dataclass
class GroundTruth:
    """Expected similarity between products."""
    material_a: str
    material_b: str
    expected_similarity: float
    relationship_type: str
    notes: str = ""


# Material type columns in the Excel files
MATERIAL_COLUMNS = [
    "Aluminum", "Copper", "Steel", "Plastic", "Li-ion battery",
    "PCB", "Flat panel glass", "CRT glass", "Other glass",
    "Other metals", "Others"
]


def sanitize_id(name: str) -> str:
    """Convert a product name to a valid ID."""
    # Remove special chars, normalize spaces
    clean = re.sub(r'[^\w\s-]', '', name)
    clean = re.sub(r'\s+', '-', clean.strip())
    return clean.upper()[:50]  # Limit length


def extract_year(name: str) -> Optional[int]:
    """Extract year from product name like 'iPhone 3G A1241 (2008)'."""
    match = re.search(r'\((\d{4})\)', name)
    if match:
        return int(match.group(1))
    return None


def extract_product_family(name: str) -> str:
    """Extract product family for grouping (e.g., 'iPhone' from 'iPhone 3G A1241')."""
    # Common patterns
    patterns = [
        (r'^(iPhone)\s', 'iPhone'),
        (r'^(Samsung Galaxy)\s', 'Samsung-Galaxy'),
        (r'^(Samsung)\s', 'Samsung'),
        (r'^(Kindle)\s', 'Kindle'),
        (r'^(PlayStation|PS\d)', 'PlayStation'),
        (r'^(Xbox)\s', 'Xbox'),
        (r'^(MacBook)\s', 'MacBook'),
        (r'^(Dell)\s', 'Dell'),
        (r'^(Motorola)\s', 'Motorola'),
        (r'^(LG)\s', 'LG'),
        (r'^(Blackberry)\s', 'Blackberry'),
        (r'^(Palm)\s', 'Palm'),
        (r'^(Fitbit)\s', 'Fitbit'),
        (r'^(Garmin)\s', 'Garmin'),
    ]

    for pattern, family in patterns:
        if re.search(pattern, name, re.IGNORECASE):
            return family

    # Default: use first word
    first_word = name.split()[0] if name else "Unknown"
    return sanitize_id(first_word)


class FigshareTransformer:
    """Transform Figshare Excel data to anofox-similarity format."""

    def __init__(self, input_dir: Path):
        self.input_dir = input_dir
        self.products: list[Product] = []
        self.components: list[MaterialComponent] = []
        self.bom_items: list[BOMItem] = []
        self.ground_truth: list[GroundTruth] = []

        # Track product IDs to avoid duplicates
        self._product_ids: set[str] = set()

        # Create standard material components
        self._init_material_components()

    def _init_material_components(self):
        """Initialize the standard material type components."""
        for mat in MATERIAL_COLUMNS:
            comp_id = f"MAT-{sanitize_id(mat)}"
            self.components.append(MaterialComponent(
                component_id=comp_id,
                name=mat,
                description=f"Material type: {mat}"
            ))

    def _get_component_id(self, material_name: str) -> str:
        """Get component ID for a material type."""
        return f"MAT-{sanitize_id(material_name)}"

    def _read_product_bom_sheet(self, xl: pd.ExcelFile, sheet_name: str) -> list[tuple[Product, list[BOMItem]]]:
        """Read a single product category sheet from Product Bill of Materials."""
        if sheet_name in ['Summary', 'References']:
            return []

        try:
            df = xl.parse(sheet_name=sheet_name, header=None)
        except Exception as e:
            print(f"  Warning: Could not read sheet '{sheet_name}': {e}")
            return []

        results = []
        category = sheet_name.strip()

        # Find the header row (contains "Product name")
        header_row = None
        for i, row in df.iterrows():
            if row.astype(str).str.contains('Product name', case=False).any():
                header_row = i
                break

        if header_row is None:
            print(f"  Warning: No header found in sheet '{sheet_name}'")
            return []

        # Get column mapping
        headers = df.iloc[header_row].tolist()
        col_map = {}
        for idx, h in enumerate(headers):
            if pd.notna(h):
                h_str = str(h).strip()
                col_map[h_str] = idx

        # Find product name column
        name_col = col_map.get('Product name')
        if name_col is None:
            print(f"  Warning: No 'Product name' column in sheet '{sheet_name}'")
            return []

        # Read products (rows after header, excluding summary rows)
        for i in range(header_row + 1, len(df)):
            row = df.iloc[i]
            name = row.iloc[name_col]

            # Skip empty rows and summary rows
            if pd.isna(name) or not isinstance(name, str):
                continue
            name = name.strip()
            if not name:
                continue
            # Skip summary/aggregate rows
            name_lower = name.lower()
            skip_patterns = [
                'average', 'maximum', 'minimum', 'summary', 'final average',
                'mass %', 'mass%', 'total', 'mean'
            ]
            if any(pattern in name_lower for pattern in skip_patterns):
                continue

            # Create unique product ID
            base_id = sanitize_id(name)
            product_id = base_id
            suffix = 1
            while product_id in self._product_ids:
                product_id = f"{base_id}-{suffix}"
                suffix += 1
            self._product_ids.add(product_id)

            product = Product(
                product_id=product_id,
                name=name,
                category=category,
                year=extract_year(name)
            )

            # Extract material quantities
            bom_items = []
            for mat_name in MATERIAL_COLUMNS:
                if mat_name in col_map:
                    val = row.iloc[col_map[mat_name]]
                    if pd.notna(val) and isinstance(val, (int, float)) and val > 0:
                        bom_items.append(BOMItem(
                            parent_id=product_id,
                            child_id=self._get_component_id(mat_name),
                            quantity=float(val)
                        ))

            if bom_items:
                results.append((product, bom_items))

        return results

    def load_data(self):
        """Load data from Figshare Excel files."""
        bom_file = self.input_dir / "Product Bill of Materials_June2020.xlsx"
        if not bom_file.exists():
            raise FileNotFoundError(f"BOM file not found: {bom_file}")

        print(f"Reading: {bom_file}")
        xl = pd.ExcelFile(bom_file)

        for sheet in xl.sheet_names:
            results = self._read_product_bom_sheet(xl, sheet)
            for product, items in results:
                self.products.append(product)
                self.bom_items.extend(items)
            if results:
                print(f"  {sheet}: {len(results)} products")

        print(f"\nLoaded {len(self.products)} products with {len(self.bom_items)} BOM items")

    def generate_ground_truth(self):
        """Generate expected similarities based on product families and categories."""
        # Group products by category
        by_category: dict[str, list[Product]] = {}
        for p in self.products:
            by_category.setdefault(p.category, []).append(p)

        # Group products by family
        by_family: dict[str, list[Product]] = {}
        for p in self.products:
            family = extract_product_family(p.name)
            by_family.setdefault(family, []).append(p)

        # Same family = very high similarity (within brand/product line)
        # Analysis shows actual mean is 0.92, with 56% having Jaccard = 1.0
        for family, products in by_family.items():
            if len(products) >= 2:
                # Just add representative pairs, not all combinations
                for i in range(min(3, len(products) - 1)):
                    self.ground_truth.append(GroundTruth(
                        material_a=products[i].product_id,
                        material_b=products[i + 1].product_id,
                        expected_similarity=0.9,  # Actual mean: 0.92
                        relationship_type='same_family',
                        notes=f'Same product family: {family}'
                    ))

        # Same category = high similarity
        # Analysis shows actual mean is 0.85
        for category, products in by_category.items():
            if len(products) >= 2:
                # Just a few representative pairs
                for i in range(min(2, len(products) - 1)):
                    self.ground_truth.append(GroundTruth(
                        material_a=products[i].product_id,
                        material_b=products[-1 - i].product_id,
                        expected_similarity=0.8,  # Actual mean: 0.85
                        relationship_type='same_category',
                        notes=f'Same category: {category}'
                    ))

        # Cross-category pairs for lower similarity tests
        # Note: All electronics share 5 core materials (Aluminum, Copper, PCB, Plastic, Steel)
        # so cross-category similarity is still relatively high (mean: 0.68)
        # Pick representative categories that should differ
        category_pairs = [
            ('Smartphone', 'CRT TV'),
            ('Smartphone', 'Printer'),
            ('Laptop', 'Gaming console'),
            ('E-reader', 'LCD TV'),
        ]

        for cat_a, cat_b in category_pairs:
            if cat_a in by_category and cat_b in by_category:
                prod_a = by_category[cat_a][0]
                prod_b = by_category[cat_b][0]
                self.ground_truth.append(GroundTruth(
                    material_a=prod_a.product_id,
                    material_b=prod_b.product_id,
                    expected_similarity=0.65,  # Actual mean: 0.68 (high due to shared core materials)
                    relationship_type='cross_category',
                    notes=f'Different categories: {cat_a} vs {cat_b}'
                ))

        print(f"Generated {len(self.ground_truth)} ground truth pairs")

    def save(self, output_dir: Path):
        """Save transformed data to gzipped CSV files."""
        output_dir.mkdir(parents=True, exist_ok=True)

        def write_gzipped_csv(filepath: Path, header: list, rows: list):
            """Write rows to a gzipped CSV file."""
            buffer = io.StringIO()
            writer = csv.writer(buffer)
            writer.writerow(header)
            writer.writerows(rows)
            with gzip.open(filepath, 'wt', encoding='utf-8') as f:
                f.write(buffer.getvalue())

        # Save materials (products + components)
        material_rows = []
        for p in self.products:
            created_date = f"{p.year}-01-01" if p.year else "2020-01-01"
            material_rows.append([
                p.product_id,
                p.name,
                p.category.upper().replace(' ', '-')[:20],
                'FERT',
                created_date
            ])
        for c in self.components:
            material_rows.append([
                c.component_id,
                c.description,
                'MATERIALS',
                'HALB',
                '2000-01-01'
            ])
        write_gzipped_csv(
            output_dir / "materials.csv.gz",
            ['material_id', 'description', 'material_group', 'material_type', 'created_date'],
            material_rows
        )

        # Save BOM items
        bom_rows = [
            [f"BOM-{item.parent_id}", item.parent_id, item.child_id, item.quantity, 1, i * 10]
            for i, item in enumerate(self.bom_items)
        ]
        write_gzipped_csv(
            output_dir / "bom_items.csv.gz",
            ['bom_id', 'parent_id', 'child_id', 'quantity', 'level', 'position'],
            bom_rows
        )

        # Save ground truth
        gt_rows = [
            [gt.material_a, gt.material_b, round(gt.expected_similarity, 4), gt.relationship_type, gt.notes]
            for gt in self.ground_truth
        ]
        write_gzipped_csv(
            output_dir / "ground_truth.csv.gz",
            ['material_a', 'material_b', 'expected_similarity', 'relationship_type', 'notes'],
            gt_rows
        )

        # Save metadata
        categories = sorted(set(p.category for p in self.products))
        families = sorted(set(extract_product_family(p.name) for p in self.products))

        metadata = {
            "source": "figshare",
            "doi": "10.6084/m9.figshare.11306792.v4",
            "license": "CC0",
            "citation": "Babbitt, C. W., Althaf, S., Cruz Rios, F., & Bilec, M. M. (2020). Material composition of consumer electronics. figshare. Dataset.",
            "n_products": len(self.products),
            "n_components": len(self.components),
            "n_bom_items": len(self.bom_items),
            "n_ground_truth": len(self.ground_truth),
            "categories": categories,
            "families": families,
            "material_types": MATERIAL_COLUMNS,
        }

        with open(output_dir / "metadata.json", 'w') as f:
            json.dump(metadata, f, indent=2)

        print(f"\nSaved to: {output_dir}")
        print(f"  - materials.csv.gz: {len(self.products)} products + {len(self.components)} components")
        print(f"  - bom_items.csv.gz: {len(self.bom_items)} items")
        print(f"  - ground_truth.csv.gz: {len(self.ground_truth)} pairs")
        print(f"  - metadata.json")


def main():
    parser = argparse.ArgumentParser(
        description="Transform Figshare Consumer Electronics dataset"
    )
    parser.add_argument(
        "--input",
        type=str,
        default="test/data/figshare/raw",
        help="Input directory with raw Excel files",
    )
    parser.add_argument(
        "--output",
        type=str,
        default="test/data/figshare",
        help="Output directory for transformed CSVs",
    )
    args = parser.parse_args()

    input_dir = Path(args.input)
    output_dir = Path(args.output)

    if not input_dir.exists():
        print(f"Error: Input directory not found: {input_dir}")
        print("Run: python scripts/download_figshare.py")
        return 1

    print("=" * 60)
    print("Figshare Consumer Electronics Dataset Transformer")
    print("=" * 60)
    print()

    transformer = FigshareTransformer(input_dir)
    transformer.load_data()
    transformer.generate_ground_truth()
    transformer.save(output_dir)

    print("\n" + "=" * 60)
    print("Transformation complete!")
    print("=" * 60)
    return 0


if __name__ == "__main__":
    exit(main())
