#!/usr/bin/env python3
"""Parser for Caterpillar Tube Pricing Kaggle dataset.

Converts the wide-format BOM (component_id_1..8) to canonical long format.
"""

import argparse
import csv
import gzip
import json
from collections import defaultdict
from pathlib import Path


def parse_caterpillar(input_dir: Path, output_dir: Path) -> None:
    """Parse Caterpillar tube pricing data to canonical format."""

    # Load component types for material groups
    component_types = {}
    type_component_path = input_dir / "type_component.csv"
    if type_component_path.exists():
        with open(type_component_path) as f:
            reader = csv.DictReader(f)
            for row in reader:
                component_types[row["component_type_id"]] = row["name"]

    # Load components master data
    components = {}
    components_path = input_dir / "components.csv"
    print(f"Parsing components from: {components_path}")
    with open(components_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            comp_id = row["component_id"]
            comp_type = row["component_type_id"]
            components[comp_id] = {
                "name": row["name"],
                "type_id": comp_type,
                "type_name": component_types.get(comp_type, "Unknown"),
            }
    print(f"  Found {len(components)} components")

    # Load tube assemblies
    tubes = {}
    tube_path = input_dir / "tube.csv"
    print(f"Parsing tube assemblies from: {tube_path}")
    with open(tube_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            tube_id = row["tube_assembly_id"]
            tubes[tube_id] = {
                "material_id": row.get("material_id", ""),
                "diameter": row.get("diameter", ""),
                "wall": row.get("wall", ""),
                "length": row.get("length", ""),
                "num_bends": row.get("num_bends", "0"),
            }
    print(f"  Found {len(tubes)} tube assemblies")

    # Parse BOM - convert wide format to long format
    bom_path = input_dir / "bill_of_materials.csv"
    print(f"Parsing BOM from: {bom_path}")

    bom_items = []
    tube_components = defaultdict(set)  # Track unique components per tube

    with open(bom_path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            tube_id = row["tube_assembly_id"]

            # Process each component slot (1-8)
            for i in range(1, 9):
                comp_col = f"component_id_{i}"
                qty_col = f"quantity_{i}"

                comp_id = row.get(comp_col, "")
                quantity = row.get(qty_col, "")

                # Skip empty/NA entries
                if not comp_id or comp_id == "NA" or comp_id == "":
                    continue

                try:
                    qty = float(quantity) if quantity and quantity != "NA" else 1.0
                except ValueError:
                    qty = 1.0

                tube_components[tube_id].add(comp_id)

                bom_items.append({
                    "bom_id": f"BOM-{len(bom_items)+1}",
                    "parent_id": tube_id,
                    "child_id": comp_id,
                    "quantity": qty,
                    "level": 1,
                    "position": i,
                })

    print(f"  Created {len(bom_items)} BOM items")

    # Build materials list - include both tubes and components
    materials = []

    # Add tube assemblies as finished goods
    for tube_id, tube_data in tubes.items():
        # Only include tubes that have BOMs
        if tube_id not in tube_components:
            continue

        desc_parts = [f"Tube Assembly"]
        if tube_data["diameter"]:
            desc_parts.append(f"D={tube_data['diameter']}mm")
        if tube_data["length"]:
            desc_parts.append(f"L={tube_data['length']}mm")
        if tube_data["num_bends"] and tube_data["num_bends"] != "0":
            desc_parts.append(f"{tube_data['num_bends']} bends")

        materials.append({
            "material_id": tube_id,
            "description": " ".join(desc_parts),
            "material_group": "TUBE_ASSEMBLY",
            "material_type": "FERT",
            "created_date": "2015-01-01",
        })

    # Add components as raw materials
    used_components = set()
    for comps in tube_components.values():
        used_components.update(comps)

    for comp_id in used_components:
        comp_data = components.get(comp_id, {"name": comp_id, "type_name": "Unknown"})

        materials.append({
            "material_id": comp_id,
            "description": comp_data["name"],
            "material_group": comp_data["type_name"].upper().replace(" ", "_"),
            "material_type": "ROH",
            "created_date": "2015-01-01",
        })

    print(f"  Total materials: {len(materials)} ({len(tubes)} tubes + {len(used_components)} components)")

    # Write output
    output_dir.mkdir(parents=True, exist_ok=True)

    # Write materials
    materials_path = output_dir / "materials.csv.gz"
    with gzip.open(materials_path, "wt", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["material_id", "description", "material_group", "material_type", "created_date"])
        writer.writeheader()
        writer.writerows(materials)
    print(f"Wrote {len(materials)} materials to {materials_path}")

    # Write BOM items
    bom_items_path = output_dir / "bom_items.csv.gz"
    with gzip.open(bom_items_path, "wt", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["bom_id", "parent_id", "child_id", "quantity", "level", "position"])
        writer.writeheader()
        writer.writerows(bom_items)
    print(f"Wrote {len(bom_items)} BOM items to {bom_items_path}")

    # Write metadata
    metadata = {
        "source": "caterpillar_kaggle",
        "description": "Caterpillar Tube Pricing Competition - Industrial tube assemblies with components",
        "license": "Kaggle Competition (research use)",
        "n_materials": len(materials),
        "n_tube_assemblies": len([m for m in materials if m["material_type"] == "FERT"]),
        "n_components": len(used_components),
        "n_bom_items": len(bom_items),
        "max_level": 1,
        "component_types": len(component_types),
    }
    metadata_path = output_dir / "metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)
    print(f"Wrote metadata to {metadata_path}")

    print(f"\nDone! Next step: run compute_ground_truth.py --dataset caterpillar")


def main():
    parser = argparse.ArgumentParser(description="Parse Caterpillar tube pricing data")
    parser.add_argument("--input", "-i", type=Path, required=True, help="Input directory with raw CSV files")
    parser.add_argument("--output", "-o", type=Path, required=True, help="Output directory")
    args = parser.parse_args()

    parse_caterpillar(args.input, args.output)


if __name__ == "__main__":
    main()
