"""Generate the fictional Rheinwerk alternate-part qualification fixture."""
from __future__ import annotations

import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
DATA = ROOT / 'examples' / 'alternate_part' / 'data'


def write(name: str, columns: list[str], rows: list[tuple[str, ...]]) -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    with (DATA / name).open('w', newline='', encoding='utf-8') as file:
        writer = csv.writer(file, lineterminator='\n')
        writer.writerow(columns)
        writer.writerows(rows)


def main() -> None:
    requirements = [
        ('thread', 'category', 'M8x1,25'),
        ('nominal_diameter', 'category', 'DN50'),
        ('pressure_class', 'category', 'PN 16'),
        ('material', 'category', '1.4404'),
        ('bore', 'quantity', 'at least 19.05 mm'),
        ('runout', 'quantity', '± 0,05 mm'),
        ('certificate', 'category', 'EN 10204 3.1'),
        ('pressure_limit', 'quantity', 'not to exceed 5 bar'),
    ]
    candidates = {
        'ALT-101': ['M8x1,25', 'DN50', 'PN16', '1.4404', '0.75 in', '± 0,02 mm', '3.1 Zeugnis', '4,8 bar'],
        'ALT-102': ['M8x1,25', 'DN 50', 'PN40', '1.4404', '19.05 mm', '± 0,05 mm', 'EN 10204 3.2', '5 bar'],
        'ALT-201': ['M10x1,25', 'DN50', 'PN16', '1.4404', '19.05 mm', '± 0,03 mm', '3.1', '4 bar'],
        'ALT-202': ['M8x1,25', 'DN50', 'PN10', '1.4404', '19.05 mm', '± 0,03 mm', '3.1', '4 bar'],
        'ALT-203': ['M8x1,25', 'DN50', 'PN16', '1.4404', '18.9 mm', '± 0,03 mm', '3.1', '4 bar'],
        'ALT-204': ['M8x1,25', 'DN50', 'PN16', '1.4404', '19.05 mm', '± 0,03 mm', '3.1', '6 bar'],
        'ALT-301': ['M8x1,25', 'DN50', 'PN16', '1.4404', '19.05 mm', '± 0,03 mm', '', '4 bar'],
        'ALT-302': ['M8x1,25', 'DN50', 'PN16', '1.4404', '19.05 mm', '± 0,03 mm', '3.1', '4 bar'],
    }
    write('part_master.csv', ['part_id', 'role', 'supplier', 'designation'],
          sorted([('RW-PUMP-50', 'request', 'Rheinwerk Ersatzteile', 'Pumpensitz M8x1,25 A4-70')] +
          [(part_id, 'candidate', 'Fiktivwerk ' + part_id[-3:], 'Ersatzsitz DN50 A4-70') for part_id in candidates]))
    write('part_characteristics.csv', ['part_id', 'attr_key', 'kind', 'value_text'],
          sorted([('RW-PUMP-50', key, kind, value) for key, kind, value in requirements] +
                 [(part, requirements[i][0], requirements[i][1], value)
                  for part, values in candidates.items() for i, value in enumerate(values)] +
                 [('ALT-302', 'thread', 'category', 'M10x1,25')]))
    rules = [
        ('pressure_class', 'PN16', 'PN40', '1'),
        ('certificate', '3.1', '3.2', '1'),
    ]
    write('compatibility_rules.csv', ['dimension', 'required', 'provided', 'score'], sorted(rules))
    write('gold_verdicts.csv', ['part_id', 'verdict', 'reason'], [
        ('ALT-101', 'ACCEPT', 'all eight attributes meet the request'),
        ('ALT-102', 'ACCEPT', 'approved PN40 and 3.2 upgrades'),
        ('ALT-201', 'REJECT', 'thread mismatch'),
        ('ALT-202', 'REJECT', 'PN10 is not approved for PN16'),
        ('ALT-203', 'REJECT', 'bore mismatch'),
        ('ALT-204', 'REJECT', 'pressure exceeds the hard limit'),
        ('ALT-301', 'REVIEW', 'certificate missing'),
        ('ALT-302', 'REVIEW', 'conflicting duplicate thread key'),
    ])


if __name__ == '__main__':
    main()
