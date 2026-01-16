#!/usr/bin/env python3
"""
Parse Neo4j BOM Gist Cypher files and convert to canonical format.

The Neo4j BOM Gist contains Cypher CREATE statements with parts, assemblies,
and BELONGS_TO relationships with quantity and cost data.

Usage:
    python -m parsers.neo4j_parser --input test/data/neo4j/raw --output test/data/neo4j
"""

import argparse
import csv
import gzip
import json
import re
from collections import defaultdict
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


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


class Neo4jParser:
    def __init__(self, input_dir: Path):
        self.input_dir = input_dir
        self.nodes: Dict[str, dict] = {}  # Variable name -> node data
        self.relationships: List[dict] = []
        self.materials: Dict[str, Material] = {}
        self.bom_items: List[BOMItem] = []

    def _clean_id(self, part_number: str) -> str:
        """Convert Neo4j part number to clean material ID."""
        clean = part_number.strip().upper().replace(" ", "-").replace(":", "-")
        return f"N4J-{clean}"

    def _parse_node(self, line: str) -> Optional[dict]:
        """Parse a CREATE node statement.

        Examples:
            CREATE (p1:Part {id:"100-001", desc:"MS Bolt, M10x70", cost: 5.30})
            CREATE (a1:Asset {id:'a1'})
            CREATE (f1:Family {id:'f1'})
        """
        # Match CREATE (var:Label {props})
        match = re.search(r"CREATE\s*\((\w+):(\w+)\s*\{([^}]+)\}\)", line)
        if not match:
            return None

        var_name = match.group(1)
        label = match.group(2)
        props_str = match.group(3)

        # Parse properties - handle both 'value' and "value" quoting
        props = {}
        # Match key: 'value' or key: "value" or key: number
        for prop_match in re.finditer(r'(\w+):\s*(?:"([^"]*)"|\'([^\']*)\'|([0-9.]+))', props_str):
            key = prop_match.group(1)
            value = prop_match.group(2) or prop_match.group(3) or prop_match.group(4)
            props[key] = value

        # Handle different property names (id/number, desc/name)
        part_id = props.get("id") or props.get("number") or var_name
        part_name = props.get("desc") or props.get("name") or part_id

        return {
            "var": var_name,
            "label": label,
            "number": part_id,
            "name": part_name,
            "cost": props.get("cost"),
            "unit": props.get("unit"),
        }

    def _parse_relationship(self, line: str) -> Optional[dict]:
        """Parse a CREATE relationship statement.

        Examples:
            CREATE (a1)<-[:BELONGS_TO {qty:3.0, unit:"EA"}]-(p1)
            CREATE (f1)<-[:BELONGS_TO]-(a1)
        """
        # Match CREATE (to)<-[:REL {props}]-(from) format (Neo4j gist style)
        match = re.search(r"CREATE\s*\((\w+)\)<-\[:(\w+)(?:\s*\{([^}]*)\})?\]-\((\w+)\)", line)
        if match:
            to_var = match.group(1)
            rel_type = match.group(2)
            props_str = match.group(3) or ""
            from_var = match.group(4)
        else:
            # Try alternative format: CREATE (from)-[:REL {props}]->(to)
            match = re.search(r"CREATE\s*\((\w+)\)-\[:(\w+)(?:\s*\{([^}]*)\})?\]->\((\w+)\)", line)
            if not match:
                return None
            from_var = match.group(1)
            rel_type = match.group(2)
            props_str = match.group(3) or ""
            to_var = match.group(4)

        # Parse quantity
        qty = 1.0
        qty_match = re.search(r"qty:\s*([0-9.]+)", props_str)
        if qty_match:
            qty = float(qty_match.group(1))

        return {
            "from": from_var,
            "to": to_var,
            "type": rel_type,
            "qty": qty,
        }

    def parse_cypher(self, cypher_path: Path) -> None:
        """Parse a Cypher file for CREATE statements."""
        print(f"Parsing Cypher from: {cypher_path}")

        content = cypher_path.read_text()

        # Parse nodes
        for line in content.split("\n"):
            line = line.strip()
            if not line or line.startswith("//"):
                continue

            node = self._parse_node(line)
            if node:
                self.nodes[node["var"]] = node
                continue

            rel = self._parse_relationship(line)
            if rel:
                self.relationships.append(rel)

        print(f"  Found {len(self.nodes)} nodes, {len(self.relationships)} relationships")

    def _determine_material_type(self, label: str, is_parent: bool = False) -> str:
        """Determine material type from Neo4j label."""
        label_lower = label.lower()
        if label_lower == "family":
            return "FERT"
        elif label_lower == "asset" or label_lower == "assembly":
            return "HALB"
        elif label_lower == "part":
            return "ROH"
        elif is_parent:
            return "FERT"
        else:
            return "ROH"

    def build_bom(self) -> None:
        """Build BOM items from parsed nodes and relationships."""
        print("Building BOM structure...")

        position_counter: Dict[str, int] = defaultdict(int)

        # Create materials from nodes
        for var_name, node in self.nodes.items():
            material_id = self._clean_id(node["number"])

            # Determine if this node is ever a parent (target of BELONGS_TO)
            is_parent = any(rel["to"] == var_name for rel in self.relationships)

            self.materials[material_id] = Material(
                material_id=material_id,
                description=node["name"],
                material_group=node["label"],
                material_type=self._determine_material_type(node["label"], is_parent),
            )

        # Create BOM items from relationships
        # In Neo4j BOM gist, BELONGS_TO goes from child to parent
        # (p1)-[:BELONGS_TO]->(a1) means p1 is a component of a1
        for rel in self.relationships:
            if rel["type"] != "BELONGS_TO":
                continue

            from_node = self.nodes.get(rel["from"])
            to_node = self.nodes.get(rel["to"])

            if not from_node or not to_node:
                continue

            # from_node is child, to_node is parent
            child_id = self._clean_id(from_node["number"])
            parent_id = self._clean_id(to_node["number"])

            bom_id = f"BOM-{parent_id}"
            position_counter[bom_id] += 10
            position = position_counter[bom_id]

            self.bom_items.append(
                BOMItem(
                    bom_id=bom_id,
                    parent_id=parent_id,
                    child_id=child_id,
                    quantity=rel["qty"],
                    level=1,  # Will be computed later
                    position=position,
                )
            )

        print(f"  Created {len(self.bom_items)} BOM items")

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
            writer = csv.DictWriter(
                f, fieldnames=["material_id", "description", "material_group", "material_type", "created_date"]
            )
            writer.writeheader()
            for mat in materials:
                writer.writerow(asdict(mat))
        print(f"Wrote {len(materials)} materials to {materials_path}")

        # Write bom_items.csv.gz
        bom_path = output_dir / "bom_items.csv.gz"
        with gzip.open(bom_path, "wt", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["bom_id", "parent_id", "child_id", "quantity", "level", "position"])
            writer.writeheader()
            for item in self.bom_items:
                writer.writerow(asdict(item))
        print(f"Wrote {len(self.bom_items)} BOM items to {bom_path}")

        # Compute stats
        max_level = max((item.level for item in self.bom_items), default=0)
        unique_parents = len(set(item.parent_id for item in self.bom_items))

        # Write metadata.json
        metadata = {
            "source": "neo4j_gist",
            "gist_url": "https://gist.github.com/maxdemarzi/e77145f0a77b7b5f6c9287bc0a96928f",
            "license": "Open source",
            "description": "Neo4j BOM Gist sample data",
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
    parser = argparse.ArgumentParser(description="Parse Neo4j BOM Gist Cypher files and convert to canonical format")
    parser.add_argument(
        "--input", "-i", type=Path, default=Path("test/data/neo4j/raw"), help="Input directory with raw Cypher files"
    )
    parser.add_argument(
        "--output", "-o", type=Path, default=Path("test/data/neo4j"), help="Output directory for canonical CSV files"
    )

    args = parser.parse_args()

    if not args.input.exists():
        print(f"ERROR: Input directory not found: {args.input}")
        print("Run download_erp_bom_data.py --commercial-only first.")
        return 1

    neo4j_parser = Neo4jParser(args.input)

    # Parse all Cypher files
    cypher_files = list(args.input.glob("*.cypher"))
    if not cypher_files:
        print("Warning: No Cypher files found")
        return 1

    for cypher_file in cypher_files:
        neo4j_parser.parse_cypher(cypher_file)

    # Build BOM structure
    neo4j_parser.build_bom()

    if not neo4j_parser.bom_items:
        print("Warning: No BOM items extracted")
        return 1

    # Compute levels and write output
    neo4j_parser._compute_levels()
    neo4j_parser.write_output(args.output)

    print("\nDone! Next step: run compute_ground_truth.py --dataset neo4j")
    return 0


if __name__ == "__main__":
    exit(main())
