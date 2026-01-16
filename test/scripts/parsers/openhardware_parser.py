#!/usr/bin/env python3
"""Parser for open hardware BOM files (OpenTrons, OpenCR, CycloneDX)."""

import argparse
import csv
import gzip
import json
import re
from pathlib import Path


def parse_opentrons_pro_bom(xlsx_path: Path) -> tuple[list[dict], list[dict]]:
    """Parse OpenTrons OT-One Pro BOM Excel file."""
    import pandas as pd

    df = pd.read_excel(xlsx_path, sheet_name="Sheet1", header=None)

    materials = []
    bom_items = []

    # Track assembly hierarchy
    current_section = None
    current_type = None
    material_id_counter = 1

    # Create top-level assembly
    assembly_id = "OT-ONE-PRO"
    materials.append(
        {
            "material_id": assembly_id,
            "description": "OpenTrons OT-One Pro Lab Automation Platform",
            "material_group": "ASSEMBLY",
            "material_type": "FERT",
            "created_date": "2016-01-01",
        }
    )

    # Parse rows (skip header row)
    for idx, row in df.iterrows():
        if idx == 0:  # Skip header
            continue

        section = row.iloc[0] if pd.notna(row.iloc[0]) else current_section
        type_name = row.iloc[1] if pd.notna(row.iloc[1]) else current_type
        part_name = row.iloc[2] if pd.notna(row.iloc[2]) else None
        quantity = row.iloc[3] if pd.notna(row.iloc[3]) else 1
        description = row.iloc[5] if len(row) > 5 and pd.notna(row.iloc[5]) else ""

        if section and section != current_section:
            current_section = section
        if type_name and type_name != current_type:
            current_type = type_name

        if part_name and part_name != "Part Name":
            # Create material for part
            part_id = f"OT-{material_id_counter:04d}"
            material_id_counter += 1

            # Determine material group from section
            material_group = "COMPONENT"
            if current_section:
                section_lower = str(current_section).lower()
                if "electronic" in section_lower:
                    material_group = "ELECTRONICS"
                elif "mechanic" in section_lower or "motion" in section_lower:
                    material_group = "MECHANICAL"
                elif "frame" in section_lower or "base" in section_lower:
                    material_group = "FRAME"

            materials.append(
                {
                    "material_id": part_id,
                    "description": str(part_name),
                    "material_group": material_group,
                    "material_type": "ROH",
                    "created_date": "2016-01-01",
                }
            )

            # Create BOM item linking to top-level assembly
            try:
                qty = float(quantity)
            except (ValueError, TypeError):
                qty = 1.0

            bom_items.append(
                {
                    "bom_id": f"BOM-{len(bom_items)+1}",
                    "parent_id": assembly_id,
                    "child_id": part_id,
                    "quantity": qty,
                    "level": 1,
                    "position": len(bom_items) + 1,
                }
            )

    return materials, bom_items


def parse_opencr_bom(xls_path: Path) -> tuple[list[dict], list[dict]]:
    """Parse OpenCR robotics controller BOM."""
    import pandas as pd

    df = pd.read_excel(xls_path, sheet_name="OpenCR", header=None)

    materials = []
    bom_items = []

    # Create top-level assembly
    assembly_id = "OPENCR-1.0"
    materials.append(
        {
            "material_id": assembly_id,
            "description": "OpenCR 1.0 Robotics Controller Board",
            "material_group": "ASSEMBLY",
            "material_type": "FERT",
            "created_date": "2017-01-01",
        }
    )

    material_id_counter = 1

    # Parse component rows (skip header rows)
    for idx, row in df.iterrows():
        if idx < 2:  # Skip header rows
            continue

        part_name = row.iloc[0] if pd.notna(row.iloc[0]) else None
        reference = row.iloc[1] if pd.notna(row.iloc[1]) else ""
        quantity = row.iloc[2] if pd.notna(row.iloc[2]) else 1

        if not part_name or part_name in ["Part Name", "BoardName"]:
            continue

        # Create material
        part_id = f"CR-{material_id_counter:04d}"
        material_id_counter += 1

        # Determine component type from part name
        part_lower = str(part_name).lower()
        if part_lower.startswith("c") and any(c in part_lower for c in ["uf", "pf", "nf"]):
            material_group = "CAPACITOR"
        elif part_lower.startswith("r") or "ohm" in part_lower:
            material_group = "RESISTOR"
        elif "led" in part_lower:
            material_group = "LED"
        elif "ic" in part_lower or "stm" in part_lower or "mpu" in part_lower:
            material_group = "IC"
        elif "conn" in part_lower or "header" in part_lower:
            material_group = "CONNECTOR"
        else:
            material_group = "COMPONENT"

        materials.append(
            {
                "material_id": part_id,
                "description": f"{part_name} ({reference})" if reference else str(part_name),
                "material_group": material_group,
                "material_type": "ROH",
                "created_date": "2017-01-01",
            }
        )

        try:
            qty = float(quantity)
        except (ValueError, TypeError):
            qty = 1.0

        bom_items.append(
            {
                "bom_id": f"BOM-{len(bom_items)+1}",
                "parent_id": assembly_id,
                "child_id": part_id,
                "quantity": qty,
                "level": 1,
                "position": len(bom_items) + 1,
            }
        )

    return materials, bom_items


def parse_cyclonedx_hbom(json_path: Path) -> tuple[list[dict], list[dict]]:
    """Parse CycloneDX Hardware BOM JSON."""
    with open(json_path) as f:
        data = json.load(f)

    materials = []
    bom_items = []

    # Get top-level component from metadata
    metadata = data.get("metadata", {})
    top_component = metadata.get("component", {})
    assembly_id = top_component.get("bom-ref", "CDX-ASSEMBLY")
    assembly_name = top_component.get("name", "CycloneDX Hardware Assembly")

    materials.append(
        {
            "material_id": assembly_id,
            "description": assembly_name,
            "material_group": "ASSEMBLY",
            "material_type": "FERT",
            "created_date": metadata.get("timestamp", "2022-01-01")[:10],
        }
    )

    # Parse components
    for idx, comp in enumerate(data.get("components", [])):
        part_id = comp.get("bom-ref", f"CDX-{idx+1:04d}")
        part_name = comp.get("name", "Unknown")
        description = comp.get("description", part_name)

        # Get quantity from properties
        quantity = 1.0
        for prop in comp.get("properties", []):
            if prop.get("name") == "cdx:device:quantity":
                try:
                    quantity = float(prop.get("value", 1))
                except ValueError:
                    quantity = 1.0
                break

        # Determine material group from component type/function
        material_group = "COMPONENT"
        for prop in comp.get("properties", []):
            if prop.get("name") == "cdx:device:function":
                func = prop.get("value", "").lower()
                if func == "connector":
                    material_group = "CONNECTOR"
                elif func == "jumper":
                    material_group = "JUMPER"
                break

        materials.append(
            {
                "material_id": part_id,
                "description": description,
                "material_group": material_group,
                "material_type": "ROH",
                "created_date": metadata.get("timestamp", "2022-01-01")[:10],
            }
        )

        bom_items.append(
            {
                "bom_id": f"BOM-{len(bom_items)+1}",
                "parent_id": assembly_id,
                "child_id": part_id,
                "quantity": quantity,
                "level": 1,
                "position": len(bom_items) + 1,
            }
        )

    return materials, bom_items


def write_output(materials: list[dict], bom_items: list[dict], output_dir: Path, source: str):
    """Write canonical output files."""
    output_dir.mkdir(parents=True, exist_ok=True)

    # Write materials
    materials_path = output_dir / "materials.csv.gz"
    with gzip.open(materials_path, "wt", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["material_id", "description", "material_group", "material_type", "created_date"]
        )
        writer.writeheader()
        writer.writerows(materials)
    print(f"Wrote {len(materials)} materials to {materials_path}")

    # Write BOM items
    bom_path = output_dir / "bom_items.csv.gz"
    with gzip.open(bom_path, "wt", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["bom_id", "parent_id", "child_id", "quantity", "level", "position"])
        writer.writeheader()
        writer.writerows(bom_items)
    print(f"Wrote {len(bom_items)} BOM items to {bom_path}")

    # Write metadata
    metadata = {
        "source": source,
        "license": "Apache-2.0",
        "n_materials": len(materials),
        "n_bom_items": len(bom_items),
        "max_level": max(b["level"] for b in bom_items) if bom_items else 0,
    }
    metadata_path = output_dir / "metadata.json"
    with open(metadata_path, "w") as f:
        json.dump(metadata, f, indent=2)
    print(f"Wrote metadata to {metadata_path}")


def main():
    parser = argparse.ArgumentParser(description="Parse open hardware BOM files")
    parser.add_argument("--opentrons-dir", type=Path, help="Directory with OpenTrons BOM files")
    parser.add_argument("--opencr-dir", type=Path, help="Directory with OpenCR BOM files")
    parser.add_argument("--cyclonedx-dir", type=Path, help="Directory with CycloneDX BOM files")
    parser.add_argument("--output", "-o", type=Path, required=True, help="Output directory")
    args = parser.parse_args()

    all_materials = []
    all_bom_items = []

    # Parse OpenTrons
    if args.opentrons_dir:
        pro_bom = args.opentrons_dir / "OT-One_Pro_BOM.xlsx"
        if pro_bom.exists():
            print(f"Parsing OpenTrons Pro BOM: {pro_bom}")
            materials, bom_items = parse_opentrons_pro_bom(pro_bom)
            all_materials.extend(materials)
            all_bom_items.extend(bom_items)

    # Parse OpenCR
    if args.opencr_dir:
        opencr_bom = args.opencr_dir / "OpenCR_REVH_BOM.xls"
        if opencr_bom.exists():
            print(f"Parsing OpenCR BOM: {opencr_bom}")
            materials, bom_items = parse_opencr_bom(opencr_bom)
            # Offset BOM IDs to avoid conflicts
            offset = len(all_bom_items)
            for item in bom_items:
                item["bom_id"] = f"BOM-{offset + int(item['bom_id'].split('-')[1])}"
            all_materials.extend(materials)
            all_bom_items.extend(bom_items)

    # Parse CycloneDX
    if args.cyclonedx_dir:
        hbom = args.cyclonedx_dir / "HBOM" / "PCIe-SATA-adapter-board" / "bom.json"
        if hbom.exists():
            print(f"Parsing CycloneDX HBOM: {hbom}")
            materials, bom_items = parse_cyclonedx_hbom(hbom)
            offset = len(all_bom_items)
            for item in bom_items:
                item["bom_id"] = f"BOM-{offset + int(item['bom_id'].split('-')[1])}"
            all_materials.extend(materials)
            all_bom_items.extend(bom_items)

    if all_materials:
        write_output(all_materials, all_bom_items, args.output, "openhardware")
        print(f"\nDone! Total: {len(all_materials)} materials, {len(all_bom_items)} BOM items")
    else:
        print("No BOM files found to parse")


if __name__ == "__main__":
    main()
