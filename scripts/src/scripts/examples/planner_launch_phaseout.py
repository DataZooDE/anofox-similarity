"""Deterministic fictional VoltEdge launch, phase-out and shortage fixture."""

import csv
import gzip
import random
from datetime import date, timedelta
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
OUT = ROOT / "examples/planner_launch_phaseout/data"


def write(name, columns, rows):
    OUT.mkdir(parents=True, exist_ok=True)
    with gzip.GzipFile(filename=str(OUT / name), mode="wb", mtime=0) as stream:
        import io

        with io.TextIOWrapper(stream, encoding="utf-8", newline="") as text:
            writer = csv.writer(text, lineterminator="\n")
            writer.writerow(columns)
            writer.writerows(sorted(rows))


def main():
    rng = random.Random(20261027)
    target = "CHARGER-65W-EU-BLACK"
    dense = "CHARGER-65W-EU-WHITE"
    sparse = "CHARGER-65W-EU-GREY"
    returns = "CHARGER-65W-EU-RETURN"
    old = "CABLE-USB2-1M-V1"
    new = "CABLE-USB2-1M-V2"
    parents = [target, dense, sparse, returns, old, new] + [f"VE-MODULE-{i:03d}" for i in range(54)]
    parts = [f"VE-COMP-{i:03d}" for i in range(240)]
    # Each product has 15 distinct components. Plant 36 repeated BOM positions.
    boms = {}
    base = parts[:15]
    boms[target] = base
    boms[dense] = base[:12] + parts[15:18]
    boms[sparse] = base[:14] + parts[18:19]
    boms[returns] = base[:13] + parts[19:21]
    boms[old] = parts[30:45]
    boms[new] = parts[30:39] + parts[45:51]
    for i, p in enumerate(parents[6:]):
        boms[p] = parts[60 + (i * 3) % 165 : 60 + (i * 3) % 165 + 15]
    edges = []
    for parent_index, (parent, children) in enumerate(boms.items()):
        for j, child in enumerate(children):
            qty = 2 if j % 7 == 0 else 1
            if parent == sparse:
                qty = 0.5
            elif parent == returns:
                qty = 1
            elif parent == dense:
                qty = 3
            if j == 0 and 6 <= parent_index < 42:
                edges.extend([(parent, child, 1), (parent, child, qty - 1)])
            else:
                edges.append((parent, child, qty))
    write("bom_edges.csv.gz", ["parent_id", "child_id", "quantity"], edges)

    start = date(2023, 1, 2)
    movements = []
    def add(material, week, kind, qty):
        movements.append((material, (start + timedelta(weeks=week)).isoformat(), kind, qty))
    for w in range(156):
        add(dense, w, "261", 30 + w % 9)
        if w % 8 == 0:
            add(dense, w, "262", -2)
        if w % 4 == 0:
            add(dense, w, "101", 45)
        if w < 78:
            add(old, w, "261", 100 + (w * 7) % 11 - 5)
        else:
            add(new, w, "261", 100 + (w * 5) % 11 - 5)
    add(sparse, 0, "261", 1)
    add(sparse, 150, "261", 1)
    for w in range(120):
        add(returns, w, "262", -3)
    for i, parent in enumerate(parents[6:]):
        for w in range(140):
            add(parent, w, "261", 5 + rng.randrange(8))
            if w % 17 == 0:
                add(parent, w, "101", 20)
    write("movements.csv.gz", ["material_id", "movement_date", "movement_type", "quantity"], movements)
    write("gold_planner.csv.gz", ["case_id", "expected_material_id", "reason"], [
        ("cold_start", dense, "dense consumption history"),
        ("cutover", old, "last issue immediately before successor"),
        ("shortage", dense, "highest quantity containment; still incomplete"),
    ])
    print(f"parents={len(parents)} edges={len(edges)} movements={len(movements)}")


if __name__ == "__main__":
    main()
