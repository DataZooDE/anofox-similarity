"""Generate the data of examples/aas_capability_matching: sourcing servo drives from supplier AAS.

    cd scripts && uv run python -m scripts.examples.aas_capability_matching

A fictional packaging-machine builder (Kröger Verpackungstechnik) needs a servo drive for the
Y axis of a tray sealer. The request is an Asset Administration Shell with IDTA-02020
*Required* capabilities; four suppliers answer with shells that carry *Offered* capabilities
and a TechnicalData submodel. The offers arrive the way they do in practice: AAS JSON, AAS XML
and an AASX package (XML inside, like the official IDTA packages).

Planted traps (each one is a test):
  - Driventa lists only a *peak* current, a different property whose IRDI shares the tokens
    0173, 1 and 02 with the rated current (token overlap must not give credit)
  - Lenvaro states the rated current with an older ECLASS release (#007) and the voltage in the
    ECLASS IRI form; the unit by UN/ECE code (AMP)
  - Kessler's ambient temperature has a semanticId without any ConceptDescription (unknown
    unit) and a cycle time of "n/a" in an xs:double
  - Orbitec gives an unknown unitId with the unit text 'A' as fallback, a decimal comma in an
    xs:double and its fieldbus protocols as a SubmodelElementList (multi-valued)
  - the IRDIs follow ECLASS syntax but are illustrative: no ECLASS content is reproduced

gold_offer_verdicts.csv states per offer, capability and property whether the requirement is
met, computed here from the clean values, independently of the extension.
Same seed, byte-identical files.
"""

import csv
import io
import json
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[4]
OUT_DIRS = [ROOT / "examples" / "aas_capability_matching" / "data", ROOT / "test" / "data" / "aas" / "scenario"]

IEC61360 = "https://admin-shell.io/DataSpecificationTemplates/DataSpecificationIec61360/3/0"
CD = "https://admin-shell.io/idta/CapabilityDescription/"
CPT = "https://admin-shell.io/idta/CapabilityPropertyType/"
CAP_VOCAB = "https://vocab.machinery.example/capability/"
TECHNICAL_DATA = "https://admin-shell.io/ZVEI/TechnicalData/Submodel/1/2"
TECHNICAL_PROPERTIES = "https://admin-shell.io/ZVEI/TechnicalData/TechnicalProperties/1/1"

# Dictionary properties: (IRDI, preferred name, unit symbol, unitId). Illustrative codes in ECLASS
# syntax, not ECLASS content.
RATED_CURRENT = ("0173-1#02-BAA120#008", "rated output current", "A", "0173-1#05-AAA480#003")
PEAK_CURRENT = ("0173-1#02-BAA121#008", "peak output current", "A", "0173-1#05-AAA480#003")
SUPPLY_VOLTAGE = ("0173-1#02-AAB381#005", "supply voltage", "V", "0173-1#05-AAA597#003")
AMBIENT_TEMP = ("0173-1#02-BAA039#012", "max. ambient temperature", "°C", "0173-1#05-AAA567#002")
CYCLE_TIME = ("0173-1#02-AAZ901#002", "minimum bus cycle time", "ms", "0173-1#05-AAA114#003")
FIELDBUS = ("0173-1#02-AAZ902#002", "fieldbus protocol", "", "")
STO = ("0173-1#02-AAZ903#001", "safe torque off (STO)", "", "")
ETHERCAT = "0173-1#07-AAZ001#001"
PROFINET = "0173-1#07-AAZ002#001"

# The company's licensed unit list (stand-in): symbol -> unit IRDI. Attached through unit_table.
ECLASS_UNITS = [("A", "0173-1#05-AAA480#003"), ("V", "0173-1#05-AAA597#003"), ("°C", "0173-1#05-AAA567#002"),
                ("ms", "0173-1#05-AAA114#003")]


# --------------------------------------------------------------------------------------------
# AAS JSON building blocks (IDTA-01001 v3 Part 1 JSON)
# --------------------------------------------------------------------------------------------

def ref(value, key_type="GlobalReference", ref_type="ExternalReference"):
    return {"type": ref_type, "keys": [{"type": key_type, "value": value}]}


def iec61360(name, unit="", unit_id=""):
    content = {"modelType": "DataSpecificationIec61360", "preferredName": [{"language": "en", "text": name}]}
    if unit:
        content["unit"] = unit
    if unit_id:
        content["unitId"] = ref(unit_id)
    return [{"dataSpecification": ref(IEC61360), "dataSpecificationContent": content}]


def prop(id_short, value, value_type="xs:double", semantic=None, value_id=None, spec=None):
    p = {"modelType": "Property", "idShort": id_short, "valueType": value_type}
    if semantic:
        p["semanticId"] = ref(semantic)
    if spec:
        p["embeddedDataSpecifications"] = spec
    if value is not None:
        p["value"] = value
    if value_id:
        p["valueId"] = ref(value_id)
    return p


def rng(id_short, lo, hi, semantic=None, spec=None, supplemental=None):
    r = {"modelType": "Range", "idShort": id_short, "valueType": "xs:double"}
    if semantic:
        r["semanticId"] = ref(semantic)
    if supplemental:
        r["supplementalSemanticIds"] = [ref(supplemental)]
    if spec:
        r["embeddedDataSpecifications"] = spec
    if lo is not None:
        r["min"] = lo
    if hi is not None:
        r["max"] = hi
    return r


def smc(id_short, elements, semantic=None, qualifiers=None):
    c = {"modelType": "SubmodelElementCollection", "idShort": id_short, "value": elements}
    if semantic:
        c["semanticId"] = ref(semantic)
    if qualifiers:
        c["qualifiers"] = qualifiers
    return c


def sml(id_short, elements, semantic=None):
    lst = {"modelType": "SubmodelElementList", "idShort": id_short, "typeValueListElement": "Property",
           "valueTypeListElement": "xs:string", "value": elements}
    if semantic:
        lst["semanticId"] = ref(semantic)
    return lst


def role_qualifiers(role):
    return [{"type": r, "semanticId": ref(f"{CD}CapabilityRoleQualifier/{r}/1/0"), "kind": "ValueQualifier",
             "valueType": "xs:boolean", "value": "true" if r == role else "false"}
            for r in ("Required", "Offered", "NotAssigned")]


def capability_property(container, element):
    """An IDTA-02020 PropertyContainer holding one PropertyRange / PropertyProperty."""
    return smc(container, [element], semantic=f"{CD}PropertyContainer/1/0")


def capability_property_value(dictionary, value=None, lo=None, hi=None, value_type="xs:double", value_id=None,
                              unit_id=None, unit=None, with_spec=True):
    """The property inside a PropertyContainer: template semanticId, dictionary id as supplemental."""
    irdi, name, sym, uid = dictionary
    spec = iec61360(name, unit if unit is not None else sym, unit_id if unit_id is not None else uid) \
        if with_spec else None
    if lo is not None or hi is not None:
        return rng("PropertyRange", lo, hi, semantic=f"{CPT}Range/1/0", spec=spec, supplemental=irdi)
    p = prop("PropertyProperty", value, value_type, semantic=f"{CPT}Property/1/0", value_id=value_id, spec=spec)
    p["supplementalSemanticIds"] = [ref(irdi)]
    return p


def capability_container(id_short, capability_class, role, properties):
    capability = {"modelType": "Capability", "idShort": id_short, "semanticId": ref(f"{CD}Capability/1/0"),
                  "supplementalSemanticIds": [ref(CAP_VOCAB + capability_class)],
                  "qualifiers": role_qualifiers(role)}
    elements = [capability]
    if properties:
        elements.append(smc("PropertySet", properties, semantic=f"{CD}PropertySet/1/0"))
    return smc(id_short + "Container", elements, semantic=f"{CD}CapabilityContainer/1/0")


def capability_submodel(sm_id, containers):
    return {"modelType": "Submodel", "id": sm_id, "idShort": "CapabilityDescription", "kind": "Instance",
            "semanticId": ref("https://admin-shell.io/idta/SubmodelTemplate/CapabilityDescription/1/0"),
            "submodelElements": [smc("CapabilitySet", containers, semantic=f"{CD}CapabilitySet/1/0")]}


def technical_data(sm_id, properties):
    return {"modelType": "Submodel", "id": sm_id, "idShort": "TechnicalData", "kind": "Instance",
            "semanticId": ref(TECHNICAL_DATA),
            "submodelElements": [smc("TechnicalProperties", properties, semantic=TECHNICAL_PROPERTIES)]}


def concept_description(dictionary):
    irdi, name, sym, uid = dictionary
    return {"modelType": "ConceptDescription", "id": irdi, "idShort": name.title().replace(" ", ""),
            "embeddedDataSpecifications": iec61360(name, sym, uid)}


def shell(shell_id, id_short, asset_id, submodels):
    return {"modelType": "AssetAdministrationShell", "id": shell_id, "idShort": id_short,
            "assetInformation": {"assetKind": "Type", "globalAssetId": asset_id},
            "submodels": [ref(sm["id"], "Submodel", "ModelReference") for sm in submodels]}


def environment(shells, submodels, cds):
    return {"assetAdministrationShells": shells, "submodels": submodels, "conceptDescriptions": cds}


# --------------------------------------------------------------------------------------------
# XML serialisation of the same model (IDTA-01001 v3.0 XML, default namespace)
# --------------------------------------------------------------------------------------------

def x_ref(tag, r, out, ind):
    out.append(f"{ind}<{tag}><type>{r['type']}</type><keys>")
    for k in r["keys"]:
        out.append(f"{ind}  <key><type>{k['type']}</type><value>{escape(k['value'])}</value></key>")
    out.append(f"{ind}</keys></{tag}>")


def x_lang(tag, item_tag, strings, out, ind):
    out.append(f"{ind}<{tag}>")
    for s in strings:
        out.append(f"{ind}  <{item_tag}><language>{s['language']}</language><text>{escape(s['text'])}</text></{item_tag}>")
    out.append(f"{ind}</{tag}>")


def x_common(e, out, ind):
    if "idShort" in e:
        out.append(f"{ind}<idShort>{escape(e['idShort'])}</idShort>")
    if "id" in e:
        out.append(f"{ind}<id>{escape(e['id'])}</id>")
    if "kind" in e:
        out.append(f"{ind}<kind>{e['kind']}</kind>")
    if "semanticId" in e:
        x_ref("semanticId", e["semanticId"], out, ind)
    if "supplementalSemanticIds" in e:
        out.append(f"{ind}<supplementalSemanticIds>")
        for r in e["supplementalSemanticIds"]:
            x_ref("reference", r, out, ind + "  ")
        out.append(f"{ind}</supplementalSemanticIds>")
    if "qualifiers" in e:
        out.append(f"{ind}<qualifiers>")
        for q in e["qualifiers"]:
            out.append(f"{ind}  <qualifier>")
            if "semanticId" in q:
                x_ref("semanticId", q["semanticId"], out, ind + "    ")
            out.append(f"{ind}    <kind>{q['kind']}</kind><type>{q['type']}</type>"
                       f"<valueType>{q['valueType']}</valueType><value>{q['value']}</value>")
            out.append(f"{ind}  </qualifier>")
        out.append(f"{ind}</qualifiers>")
    if "embeddedDataSpecifications" in e:
        out.append(f"{ind}<embeddedDataSpecifications>")
        for spec in e["embeddedDataSpecifications"]:
            c = spec["dataSpecificationContent"]
            out.append(f"{ind}  <embeddedDataSpecification>")
            x_ref("dataSpecification", spec["dataSpecification"], out, ind + "    ")
            out.append(f"{ind}    <dataSpecificationContent><dataSpecificationIec61360>")
            x_lang("preferredName", "langStringPreferredNameTypeIec61360", c["preferredName"], out, ind + "      ")
            if "unit" in c:
                out.append(f"{ind}      <unit>{escape(c['unit'])}</unit>")
            if "unitId" in c:
                x_ref("unitId", c["unitId"], out, ind + "      ")
            out.append(f"{ind}    </dataSpecificationIec61360></dataSpecificationContent>")
            out.append(f"{ind}  </embeddedDataSpecification>")
        out.append(f"{ind}</embeddedDataSpecifications>")


TAGS = {"Property": "property", "Range": "range", "SubmodelElementCollection": "submodelElementCollection",
        "SubmodelElementList": "submodelElementList", "Capability": "capability"}


def x_element(e, out, ind):
    tag = TAGS[e["modelType"]]
    out.append(f"{ind}<{tag}>")
    x_common(e, out, ind + "  ")
    if tag == "property":
        out.append(f"{ind}  <valueType>{e['valueType']}</valueType>")
        if "value" in e:
            out.append(f"{ind}  <value>{escape(e['value'])}</value>")
        if "valueId" in e:
            x_ref("valueId", e["valueId"], out, ind + "  ")
    elif tag == "range":
        out.append(f"{ind}  <valueType>{e['valueType']}</valueType>")
        for side in ("min", "max"):
            if side in e:
                out.append(f"{ind}  <{side}>{escape(e[side])}</{side}>")
    elif tag in ("submodelElementCollection", "submodelElementList"):
        if tag == "submodelElementList":
            out.append(f"{ind}  <typeValueListElement>{e['typeValueListElement']}</typeValueListElement>")
            out.append(f"{ind}  <valueTypeListElement>{e['valueTypeListElement']}</valueTypeListElement>")
        out.append(f"{ind}  <value>")
        for c in e["value"]:
            x_element(c, out, ind + "    ")
        out.append(f"{ind}  </value>")
    out.append(f"{ind}</{tag}>")


def to_xml(env):
    out = ['<?xml version="1.0" encoding="utf-8"?>', '<environment xmlns="https://admin-shell.io/aas/3/0">']
    out.append("  <assetAdministrationShells>")
    for s in env["assetAdministrationShells"]:
        out.append("    <assetAdministrationShell>")
        x_common(s, out, "      ")
        a = s["assetInformation"]
        out.append(f"      <assetInformation><assetKind>{a['assetKind']}</assetKind>"
                   f"<globalAssetId>{escape(a['globalAssetId'])}</globalAssetId></assetInformation>")
        out.append("      <submodels>")
        for r in s["submodels"]:
            x_ref("reference", r, out, "        ")
        out.append("      </submodels>")
        out.append("    </assetAdministrationShell>")
    out.append("  </assetAdministrationShells>")
    out.append("  <submodels>")
    for sm in env["submodels"]:
        out.append("    <submodel>")
        x_common(sm, out, "      ")
        out.append("      <submodelElements>")
        for e in sm["submodelElements"]:
            x_element(e, out, "        ")
        out.append("      </submodelElements>")
        out.append("    </submodel>")
    out.append("  </submodels>")
    out.append("  <conceptDescriptions>")
    for cd in env["conceptDescriptions"]:
        out.append("    <conceptDescription>")
        x_common(cd, out, "      ")
        out.append("    </conceptDescription>")
    out.append("  </conceptDescriptions>")
    out.append("</environment>")
    return "\n".join(out) + "\n"


def to_aasx(xml_text, name):
    """An AASX (OPC) package with one XML spec part, written deterministically."""
    parts = {
        "[Content_Types].xml": '<?xml version="1.0" encoding="utf-8"?><Types xmlns="http://schemas.openxmlformats.org/'
                               'package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.'
                               'openxmlformats-package.relationships+xml" /><Default Extension="xml" ContentType="text/'
                               'xml" /><Override PartName="/aasx/aasx-origin" ContentType="text/plain" /></Types>',
        "_rels/.rels": '<?xml version="1.0" encoding="utf-8"?><Relationships xmlns="http://schemas.openxmlformats.org/'
                       'package/2006/relationships"><Relationship Type="http://admin-shell.io/aasx/relationships/'
                       'aasx-origin" Target="/aasx/aasx-origin" Id="R1" /></Relationships>',
        "aasx/aasx-origin": "Intentionally empty",
        "aasx/_rels/aasx-origin.rels": '<?xml version="1.0" encoding="utf-8"?><Relationships xmlns="http://schemas.'
                                       'openxmlformats.org/package/2006/relationships"><Relationship Type="http://'
                                       f'admin-shell.io/aasx/relationships/aas-spec" Target="/aasx/{name}/{name}.aas.'
                                       'xml" Id="R2" /></Relationships>',
        f"aasx/{name}/{name}.aas.xml": xml_text,
    }
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for path, text in parts.items():
            info = zipfile.ZipInfo(path, date_time=(2026, 11, 2, 9, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, text.encode("utf-8"))
    return buf.getvalue()


# --------------------------------------------------------------------------------------------
# The scenario
# --------------------------------------------------------------------------------------------

def request():
    """RFQ-2026-117: Y-axis drive of a tray sealer, IDTA-02020 Required capabilities."""
    motion = capability_container("PositionControlledMotion", "PositionControlledMotion", "Required", [
        capability_property("RatedCurrent", capability_property_value(RATED_CURRENT, lo="7.6")),
        capability_property("SupplyVoltage", capability_property_value(SUPPLY_VOLTAGE, lo="380", hi="480")),
        capability_property("AmbientTemperature", capability_property_value(AMBIENT_TEMP, lo="45")),
        capability_property("SafeTorqueOff", capability_property_value(STO, value="true", value_type="xs:boolean")),
    ])
    fieldbus = capability_container("FieldbusCommunication", "FieldbusCommunication", "Required", [
        capability_property("Protocol", capability_property_value(FIELDBUS, value="EtherCAT", value_type="xs:string",
                                                                  value_id=ETHERCAT)),
        capability_property("CycleTime", capability_property_value(CYCLE_TIME, hi="1")),
    ])
    sm = capability_submodel("https://kroeger-vt.example/aas/rfq-2026-117/sm/capabilities", [motion, fieldbus])
    s = shell("https://kroeger-vt.example/aas/rfq-2026-117", "RFQ_2026_117",
              "https://kroeger-vt.example/asset/tray-sealer-ts40/y-axis", [sm])
    return environment([s], [sm], [])


def offer_lenvaro():
    """Lenvaro LX-8 (JSON). Meets everything; rated current from an older ECLASS release, voltage
    as ECLASS IRI, unit by UN/ECE code."""
    td = technical_data("https://lenvaro.example/aas/lx-8/sm/technical-data", [
        prop("RatedCurrent", "8.0", semantic="0173-1#02-BAA120#007"),
        rng("SupplyVoltage", "380", "480", semantic="https://api.eclass-cdp.com/0173-1-02-AAB381-005"),
        prop("MaxAmbientTemperature", "50", semantic=AMBIENT_TEMP[0]),
    ])
    motion = capability_container("PositionControlledMotion", "PositionControlledMotion", "Offered", [
        capability_property("RatedCurrent",
                            capability_property_value(RATED_CURRENT, value="8.0", unit_id="AMP")),
        capability_property("SupplyVoltage", capability_property_value(SUPPLY_VOLTAGE, lo="380", hi="480")),
        capability_property("AmbientTemperature", capability_property_value(AMBIENT_TEMP, value="50")),
        capability_property("SafeTorqueOff", capability_property_value(STO, value="true", value_type="xs:boolean")),
    ])
    fieldbus = capability_container("FieldbusCommunication", "FieldbusCommunication", "Offered", [
        capability_property("Protocol", capability_property_value(FIELDBUS, value="EtherCAT", value_type="xs:string",
                                                                  value_id=ETHERCAT)),
        capability_property("CycleTime", capability_property_value(CYCLE_TIME, value="0.5")),
    ])
    cap = capability_submodel("https://lenvaro.example/aas/lx-8/sm/capabilities", [motion, fieldbus])
    s = shell("https://lenvaro.example/aas/lx-8", "Lenvaro_LX_8", "https://lenvaro.example/asset/lx-8", [td, cap])
    cds = [concept_description(d) for d in (RATED_CURRENT, SUPPLY_VOLTAGE, AMBIENT_TEMP)]
    cds[0]["id"] = "0173-1#02-BAA120#007"  # the release the offer was written against
    return environment([s], [td, cap], cds)


def offer_driventa():
    """Driventa DS-12 (XML). Only a peak current, a narrower voltage range, 40 °C, no STO,
    PROFINET."""
    motion = capability_container("PositionControlledMotion", "PositionControlledMotion", "Offered", [
        capability_property("PeakCurrent", capability_property_value(PEAK_CURRENT, value="12")),
        capability_property("SupplyVoltage", capability_property_value(SUPPLY_VOLTAGE, lo="400", hi="480")),
        capability_property("AmbientTemperature", capability_property_value(AMBIENT_TEMP, value="40")),
        capability_property("SafeTorqueOff", capability_property_value(STO, value="false", value_type="xs:boolean")),
    ])
    fieldbus = capability_container("FieldbusCommunication", "FieldbusCommunication", "Offered", [
        capability_property("Protocol", capability_property_value(FIELDBUS, value="PROFINET", value_type="xs:string",
                                                                  value_id=PROFINET)),
        capability_property("CycleTime", capability_property_value(CYCLE_TIME, value="1")),
    ])
    cap = capability_submodel("https://driventa.example/aas/ds-12/sm/capabilities", [motion, fieldbus])
    s = shell("https://driventa.example/aas/ds-12", "Driventa_DS_12", "https://driventa.example/asset/ds-12", [cap])
    return environment([s], [cap], [])


def offer_kessler():
    """Kessler KSD-10 (AASX, XML inside). Current by ECLASS unit IRDI; the ambient temperature
    has no dictionary entry; the cycle time is 'n/a'."""
    motion = capability_container("PositionControlledMotion", "PositionControlledMotion", "Offered", [
        capability_property("RatedCurrent", capability_property_value(RATED_CURRENT, value="10")),
        capability_property("SupplyVoltage", capability_property_value(SUPPLY_VOLTAGE, lo="380", hi="480")),
        capability_property("AmbientTemperature",
                            capability_property_value(AMBIENT_TEMP, value="55", with_spec=False)),
        capability_property("SafeTorqueOff", capability_property_value(STO, value="true", value_type="xs:boolean")),
    ])
    fieldbus = capability_container("FieldbusCommunication", "FieldbusCommunication", "Offered", [
        capability_property("Protocol", capability_property_value(FIELDBUS, value="EtherCAT", value_type="xs:string",
                                                                  value_id=ETHERCAT)),
        capability_property("CycleTime", capability_property_value(CYCLE_TIME, value="n/a")),
    ])
    cap = capability_submodel("https://kessler-antriebe.example/aas/ksd-10/sm/capabilities", [motion, fieldbus])
    s = shell("https://kessler-antriebe.example/aas/ksd-10", "Kessler_KSD_10",
              "https://kessler-antriebe.example/asset/ksd-10", [cap])
    return environment([s], [cap], [])


def offer_orbitec():
    """Orbitec OX-6 (JSON). 6 A is too little and 200-240 V the wrong grid; an unknown unitId with
    'A' as fallback; a decimal comma; protocols as a list."""
    td = technical_data("https://orbitec.example/aas/ox-6/sm/technical-data", [
        prop("RatedCurrent", "6,0", semantic=RATED_CURRENT[0]),
    ])
    protocols = sml("Protocols", [prop(None, "EtherCAT", "xs:string", value_id=ETHERCAT),
                                  prop(None, "PROFINET", "xs:string", value_id=PROFINET)])
    for p in protocols["value"]:
        del p["idShort"]
    protocols_container = smc("Protocol", [dict(protocols, idShort="PropertySubmodelList",
                                                supplementalSemanticIds=[ref(FIELDBUS[0])])],
                              semantic=f"{CD}PropertyContainer/1/0")
    motion = capability_container("PositionControlledMotion", "PositionControlledMotion", "Offered", [
        capability_property("RatedCurrent", capability_property_value(RATED_CURRENT, value="6.0",
                                                                      unit_id="urn:orbitec:unit:ampere")),
        capability_property("SupplyVoltage", capability_property_value(SUPPLY_VOLTAGE, lo="200", hi="240")),
        capability_property("AmbientTemperature", capability_property_value(AMBIENT_TEMP, value="55")),
        capability_property("SafeTorqueOff", capability_property_value(STO, value="true", value_type="xs:boolean")),
    ])
    fieldbus = capability_container("FieldbusCommunication", "FieldbusCommunication", "Offered", [
        protocols_container,
        capability_property("CycleTime", capability_property_value(CYCLE_TIME, value="0.25")),
    ])
    cap = capability_submodel("https://orbitec.example/aas/ox-6/sm/capabilities", [motion, fieldbus])
    s = shell("https://orbitec.example/aas/ox-6", "Orbitec_OX_6", "https://orbitec.example/asset/ox-6", [td, cap])
    return environment([s], [td, cap], [concept_description(RATED_CURRENT)])


# Ground truth from the clean values: (offer, capability, property) -> met
REQUIRED = {"RatedCurrent": ("at_least", 7.6), "SupplyVoltage": ("within", (380, 480)),
            "AmbientTemperature": ("at_least", 45), "SafeTorqueOff": ("equals", "true"),
            "Protocol": ("one_of", "EtherCAT"), "CycleTime": ("at_most", 1.0)}
CAPABILITY_OF = {"RatedCurrent": "PositionControlledMotion", "SupplyVoltage": "PositionControlledMotion",
                 "AmbientTemperature": "PositionControlledMotion", "SafeTorqueOff": "PositionControlledMotion",
                 "Protocol": "FieldbusCommunication", "CycleTime": "FieldbusCommunication"}
# what each offer states, None = not stated or not usable (no unit / not a number / other property)
OFFERED = {
    "Lenvaro_LX_8": {"RatedCurrent": 8.0, "SupplyVoltage": (380, 480), "AmbientTemperature": 50,
                     "SafeTorqueOff": "true", "Protocol": ["EtherCAT"], "CycleTime": 0.5},
    "Driventa_DS_12": {"RatedCurrent": None, "SupplyVoltage": (400, 480), "AmbientTemperature": 40,
                       "SafeTorqueOff": "false", "Protocol": ["PROFINET"], "CycleTime": 1.0},
    "Kessler_KSD_10": {"RatedCurrent": 10.0, "SupplyVoltage": (380, 480), "AmbientTemperature": None,
                       "SafeTorqueOff": "true", "Protocol": ["EtherCAT"], "CycleTime": None},
    "Orbitec_OX_6": {"RatedCurrent": 6.0, "SupplyVoltage": (200, 240), "AmbientTemperature": 55,
                     "SafeTorqueOff": "true", "Protocol": ["EtherCAT", "PROFINET"], "CycleTime": 0.25},
}


def met(rule, required, offered):
    if offered is None:
        return False
    if rule == "at_least":
        return offered >= required
    if rule == "at_most":
        return offered <= required
    if rule == "within":
        return offered[0] <= required[0] and offered[1] >= required[1]
    if rule == "equals":
        return offered == required
    return required in offered


def gold_rows():
    rows = []
    for offer, values in OFFERED.items():
        for name, (rule, required) in REQUIRED.items():
            rows.append((offer, CAPABILITY_OF[name], name, rule, str(met(rule, required, values[name])).lower()))
    return rows


def write(out_dir):
    offers = out_dir / "offers"
    offers.mkdir(parents=True, exist_ok=True)

    def dump_json(path, env):
        path.write_text(json.dumps(env, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    dump_json(out_dir / "rfq_2026_117.aas.json", request())
    dump_json(offers / "lenvaro_lx8.aas.json", offer_lenvaro())
    (offers / "driventa_ds12.aas.xml").write_text(to_xml(offer_driventa()), encoding="utf-8")
    (offers / "kessler_ksd10.aasx").write_bytes(to_aasx(to_xml(offer_kessler()), "KesslerKSD10"))
    dump_json(offers / "orbitec_ox6.aas.json", offer_orbitec())
    # the same offer in all three forms, for the format-equivalence test
    same = offer_lenvaro()
    (out_dir / "formats").mkdir(exist_ok=True)
    dump_json(out_dir / "formats" / "lenvaro_lx8.aas.json", same)
    (out_dir / "formats" / "lenvaro_lx8.aas.xml").write_text(to_xml(same), encoding="utf-8")
    (out_dir / "formats" / "lenvaro_lx8.aasx").write_bytes(to_aasx(to_xml(same), "LenvaroLX8"))

    with open(out_dir / "eclass_units.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["symbol", "unit_id"])
        w.writerows(ECLASS_UNITS)
    with open(out_dir / "gold_offer_verdicts.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["offer", "capability", "requirement", "rule", "met"])
        w.writerows(gold_rows())


def main():
    for out_dir in OUT_DIRS:
        write(out_dir)
        print(f"wrote {out_dir.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
