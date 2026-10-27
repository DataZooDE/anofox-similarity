"""Generate a small, reproducible C-parts catalogue with reviewed truth."""

import csv
from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
DATA = ROOT / "examples" / "c_parts_harmonisation" / "data"


def write_csv(name, header, rows):
    DATA.mkdir(parents=True, exist_ok=True)
    with (DATA / name).open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        writer.writerows(sorted(rows, key=lambda row: row[0]))


def main():
    parts = []
    gold = []
    suppliers = ("Falken Handel", "Nordtal Technik", "Elster Industrie", "Morgenwerk")
    # Each family has two catalogue lines for the same physical specification.
    families = (("bolt", 20, "DIN 933", "ISO 4017", "Sechskantschraube", "hex bolt"),
                ("washer", 10, "DIN 125", "ISO 7089", "Unterlegscheibe", "plain washer"),
                ("nut", 10, "DIN 934", "ISO 4032", "Sechskantmutter", "hex nut"))
    for kind, count, first_std, second_std, de, en in families:
        for index in range(1, count + 1):
            group = f"{kind.upper()}-{index:02d}"
            diameter = 4 + 2 * index
            length = 15 + 5 * (index % 5) if kind == "bolt" else 0
            grade = "A2-70" if index % 3 else "A4-70"
            base = 0.08 + 0.017 * index + (0.11 if kind == "bolt" else 0)
            for copy in range(2):
                part_id = f"CP-{kind[0].upper()}{index:02d}-{copy + 1}"
                standard = first_std if copy == 0 else second_std
                thread = f"M{diameter}" if copy == 0 else f"M {diameter}"
                if kind == "bolt":
                    length_text = f"{length} mm" if copy == 0 else f"{length / 10:g} cm".replace(".", ",")
                else:
                    length_text = ""
                description = f"{de} {thread}" if copy == 0 else f"{en} {thread}"
                # One catalogue typo is deliberately harmless: the keys carry the decision.
                if kind == "bolt" and index == 7 and copy == 1:
                    description = f"hex blot {thread}"
                price = round(base * (1 if copy == 0 else 1.32 + index % 4 * 0.09), 3)
                parts.append((part_id, suppliers[(index + copy) % 4], description,
                              standard, thread, length_text, grade, price))
                gold.append((part_id, group, "approved"))

    # Distinct lengths must remain distinct even when all other keys agree.
    for index in range(8):
        part_id = f"CP-S{index + 1:02d}"
        parts.append((part_id, suppliers[index % 4], "Sonder-Schraube", "DIN 933",
                      "M8", f"{41 + index} mm", "A2-70", round(0.31 + index * 0.013, 3)))
        gold.append((part_id, part_id, "singleton"))

    # A--B and B--C look plausible when an absent length is treated as a wildcard.
    # A and C conflict; all three require catalogue review, not automatic approval.
    chain = (("CP-X25", "25 mm", "Sechskantschr. M8x25; darf 25 mm nicht überschreiten"),
             ("CP-XUNK", "", "hex bolt M8; length not supplied"),
             ("CP-X30", "30 mm", "hex bolt M8x30; length must not exceed 30 mm"))
    for part_id, length, description in chain:
        parts.append((part_id, "Talberg Supply", description, "DIN 931",
                      "M8", length, "A2-70", 0.42))
        gold.append((part_id, "CHAIN-REVIEW", "review"))

    write_csv("parts.csv", ("part_id", "supplier", "description", "standard_raw",
                            "thread_raw", "length_raw", "grade_raw", "unit_price_eur"), parts)
    write_csv("standard_synonyms.csv", ("standard_raw", "standard_family_norm"),
              (("DIN 933", "ISO 4017"), ("ISO 4017", "ISO 4017"),
               ("DIN 125", "ISO 7089"), ("ISO 7089", "ISO 7089"),
               ("DIN 934", "ISO 4032"), ("ISO 4032", "ISO 4032"),
               ("DIN 931", "ISO 4014"), ("ISO 4014", "ISO 4014")))
    write_csv("gold_clusters.csv", ("part_id", "gold_cluster", "decision"), gold)


if __name__ == "__main__":
    main()
