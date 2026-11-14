"""Build the hand-written AASX fixtures of this folder (run once; the outputs are committed).

    python3 test/data/aas/fixtures/make_aasx_fixtures.py

Each package exercises one OPC path of the AASX reader:
  json_spec_relative.aasx  a JSON spec part, named by a relative relationship target
  no_relationships.aasx    no _rels at all: the reader falls back to *.aas.xml parts
  escaping_target.aasx     a relationship target that climbs above the package root
"""
import io
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RELS = ('<?xml version="1.0" encoding="utf-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/'
        'relationships"><Relationship Type="http://admin-shell.io/aasx/relationships/{type}" Target="{target}" '
        'Id="R1" /></Relationships>')
SUBMODEL_JSON = ('{"submodels": [{"modelType": "Submodel", "id": "urn:fx:sm:aasx-json", "idShort": "TechnicalData", '
                 '"submodelElements": [{"modelType": "Property", "idShort": "Axes", "valueType": "xs:int", '
                 '"value": "3"}]}]}')
SUBMODEL_XML = ('<?xml version="1.0" encoding="utf-8"?><environment xmlns="https://admin-shell.io/aas/3/0"><submodels>'
                '<submodel><idShort>TechnicalData</idShort><id>urn:fx:sm:aasx-norels</id><submodelElements>'
                '<property><idShort>Axes</idShort><valueType>xs:int</valueType><value>5</value></property>'
                '</submodelElements></submodel></submodels></environment>')


def package(path, parts):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, text in parts.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 11, 12, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode("utf-8"))
    (HERE / path).write_bytes(buf.getvalue())


package("json_spec_relative.aasx", {
    "_rels/.rels": RELS.format(type="aasx-origin", target="/aasx/aasx-origin"),
    "aasx/aasx-origin": "Intentionally empty",
    "aasx/_rels/aasx-origin.rels": RELS.format(type="aas-spec", target="spec/offer.aas.json"),
    "aasx/spec/offer.aas.json": SUBMODEL_JSON,
})
package("no_relationships.aasx", {"aasx/offer/offer.aas.xml": SUBMODEL_XML})
package("escaping_target.aasx", {
    "_rels/.rels": RELS.format(type="aasx-origin", target="/aasx/aasx-origin"),
    "aasx/aasx-origin": "Intentionally empty",
    "aasx/_rels/aasx-origin.rels": RELS.format(type="aas-spec", target="../../../etc/passwd"),
})
