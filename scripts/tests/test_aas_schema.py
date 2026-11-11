"""The AAS the extension writes validates against the official AAS JSON schema (GitHub #34).

trees_to_aas / aas_export and aas_coverage_result are run in the built shell over the AAS test
data (generated scenarios and official IDTA packages) and every result is validated against
test/data/aas/schema/aas.json (IDTA-01001-3-2, CC BY 4.0). The shell is ../build/release/duckdb, or
DUCKDB; the tests skip when it is missing.
"""

import json
import os
import subprocess
from pathlib import Path

import pytest
from jsonschema import Draft201909Validator

ROOT = Path(__file__).resolve().parents[2]
SHELL = Path(os.environ.get("DUCKDB", ROOT / "build" / "release" / "duckdb"))
SCHEMA = json.loads((ROOT / "test" / "data" / "aas" / "schema" / "aas.json").read_text(encoding="utf-8"))
VALIDATOR = Draft201909Validator(SCHEMA)

INPUTS = [
    "test/data/aas/scenario/rfq_2026_117.aas.json",
    "test/data/aas/scenario/offers/lenvaro_lx8.aas.json",
    "test/data/aas/scenario/offers/driventa_ds12.aas.xml",
    "test/data/aas/scenario/offers/kessler_ksd10.aasx",
    "test/data/aas/scenario/offers/orbitec_ox6.aas.json",
    "test/data/aas/scenario/sealing/rfq_2026_204.aas.json",
    "test/data/aas/scenario/sealing/capability_taxonomy.aas.json",
    "test/data/aas/scenario/io_profile/ostwald_ios.aas.json",
    "test/data/aas/idta/IDTA_02003_Sample_TechnicalData_2_0_4.aasx",
    "test/data/aas/idta/IDTA_02020_Template_Capability_Description_1_0_2.json",
]

pytestmark = pytest.mark.skipif(not SHELL.exists(), reason=f"no DuckDB shell with the extension at {SHELL}")


def query(sql):
    """Rows of a query in the shell, as JSON (one object per row)."""
    env = dict(os.environ, DATAZOO_DISABLE_TELEMETRY="1")
    env.pop("AI_AGENT", None)
    env.pop("CLAUDECODE", None)
    out = subprocess.run([str(SHELL), "-json", "-c", sql], cwd=ROOT, capture_output=True, text=True, env=env, check=True)
    return json.loads(out.stdout or "[]")


def errors(document):
    return [f"{'/'.join(map(str, e.absolute_path))}: {e.message}" for e in VALIDATOR.iter_errors(document)][:10]


@pytest.mark.parametrize("path", INPUTS)
def test_trees_to_aas_validates(path):
    rows = query(f"SELECT trees_to_aas(aas_to_trees(content)) AS aas FROM read_blob('{path}');")
    assert rows, path
    document = json.loads(rows[0]["aas"])
    assert errors(document) == []


def test_aas_export_and_coverage_result_validate():
    rows = query("""
        CREATE TABLE raw AS
        SELECT unnest(aas_to_trees(content, 'RFQ-2026-117')) AS n FROM read_text('test/data/aas/scenario/rfq_2026_117.aas.json')
        UNION ALL SELECT unnest(aas_to_trees(content)) FROM read_blob('test/data/aas/scenario/offers/*');
        CREATE TABLE tree_nodes AS SELECT n.* EXCLUDE (attributes) FROM raw;
        CREATE TABLE tree_attributes AS SELECT unnest(n.attributes, recursive := true) FROM raw;
        SET VARIABLE offers = (SELECT list(DISTINCT tree_id) FROM tree_nodes WHERE tree_id <> 'RFQ-2026-117');
        CREATE TABLE trace AS SELECT * FROM tree_coverage_trace('RFQ-2026-117', candidates := getvariable('offers'),
                                                                labels := 'hard');
        SELECT 'export' AS kind, aas_json FROM aas_export()
        UNION ALL SELECT 'result', aas_json FROM aas_coverage_result('trace');
    """)
    kinds = {r["kind"] for r in rows}
    assert kinds == {"export", "result"}
    for r in rows:
        document = json.loads(r["aas_json"])
        if r["kind"] == "result":
            document = {"submodels": [document]}
        assert errors(document) == [], r["kind"]


def test_the_schema_check_is_not_vacuous():
    """A broken document (bad idShort, Property without valueType, empty list) must fail."""
    broken = {"submodels": [{"modelType": "Submodel", "id": "urn:x", "idShort": "1bad",
                             "submodelElements": [{"modelType": "Property", "idShort": "p"},
                                                  {"modelType": "SubmodelElementList", "idShort": "L1",
                                                   "typeValueListElement": "Property", "value": []}]}]}
    assert len(errors(broken)) >= 3
