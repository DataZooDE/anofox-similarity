"""Domain tables for the seating-systems benchmark.

Everything here is fictional: the supplier, the vehicle makers and the
programmes. The catalogue, units and wordings are the closed world the
generator samples from.
"""

from dataclasses import dataclass

# --------------------------------------------------------------------------- units
#
# Base units follow the units seed of the extension (R2): SI where it exists,
# dB(A) for sound level, deg for angles, EUR for currency, "1" for counts.
# Each customer unit maps to the base unit by base = value * factor + offset.

BASE_UNIT = {
    "length": "m",
    "speed": "m/s",
    "force": "N",
    "time": "s",
    "sound_level": "dB(A)",
    "mass": "kg",
    "current": "A",
    "temperature": "K",
    "power": "W",
    "angle": "deg",
    "torque": "N*m",
    "volume_flow": "m3/s",
    "count": "1",
    "currency": "EUR",
}

# symbol -> (dimension, factor, offset)
UNITS = {
    "mm": ("length", 1e-3, 0.0),
    "cm": ("length", 1e-2, 0.0),
    "mm/s": ("speed", 1e-3, 0.0),
    "cm/s": ("speed", 1e-2, 0.0),
    "N": ("force", 1.0, 0.0),
    "kN": ("force", 1e3, 0.0),
    "s": ("time", 1.0, 0.0),
    "ms": ("time", 1e-3, 0.0),
    "min": ("time", 60.0, 0.0),
    "dB(A)": ("sound_level", 1.0, 0.0),
    "kg": ("mass", 1.0, 0.0),
    "g": ("mass", 1e-3, 0.0),
    "A": ("current", 1.0, 0.0),
    "mA": ("current", 1e-3, 0.0),
    "°C": ("temperature", 1.0, 273.15),
    "W": ("power", 1.0, 0.0),
    "°": ("angle", 1.0, 0.0),
    "N·m": ("torque", 1.0, 0.0),
    "Nm": ("torque", 1.0, 0.0),
    "m³/h": ("volume_flow", 1.0 / 3600.0, 0.0),
    "l/min": ("volume_flow", 1e-3 / 60.0, 0.0),
    "cycles": ("count", 1.0, 0.0),
    "Zyklen": ("count", 1.0, 0.0),
    "EUR": ("currency", 1.0, 0.0),
    "€": ("currency", 1.0, 0.0),
    "": ("count", 1.0, 0.0),
}

# Dimensions compared on a linear scale; all others are physical and use log.
LINEAR_DIMENSIONS = {"currency", "time", "sound_level", "temperature"}


def _clean(x: float) -> float:
    """Drop binary noise (0.47000000000000003 -> 0.47) so conversions compare exactly."""
    return float(f"{x:.12g}")


def to_base(value: float, unit: str) -> float:
    _, factor, offset = UNITS[unit]
    return _clean(value * factor + offset)


def from_base(value: float, unit: str) -> float:
    _, factor, offset = UNITS[unit]
    return _clean((value - offset) / factor)


# --------------------------------------------------------------------------- ordinals

ORDINALS = {
    # best last; a provided class satisfies a required one if its rank is >= the required rank
    "flammability_class": ["F3", "F2", "F1", "F0"],
    "protection_class": ["IP40", "IP54", "IP65", "IP67"],
}


# --------------------------------------------------------------------------- catalogue


@dataclass(frozen=True)
class AttrTemplate:
    key: str
    name_en: str
    name_de: str
    dimension: str
    bound: str  # at_least | at_most | within | equals
    natural_unit: str  # unit the value model below is expressed in
    lo: float  # value model range in natural units (ordinal: unused)
    hi: float
    step: float
    customer_units: tuple[str, ...] = ()  # rendering units for requests
    names_en: tuple[str, ...] = ()  # wording variants used in requests
    names_de: tuple[str, ...] = ()

    @property
    def is_ordinal(self) -> bool:
        return self.dimension in ORDINALS

    @property
    def scale(self) -> str:
        return "linear" if self.dimension in LINEAR_DIMENSIONS or self.is_ordinal else "log"


@dataclass(frozen=True)
class SeatClass:
    class_id: str
    label_en: str
    label_de: str
    definition: str
    attrs: tuple[AttrTemplate, ...]
    group: str  # structure | comfort | safety | electrics
    max_count: int = 1  # extensive multiplicity (zones, motors)


def _a(key, en, de, dim, bound, unit, lo, hi, step, cu, ven=(), vde=()):
    return AttrTemplate(key, en, de, dim, bound, unit, lo, hi, step, tuple(cu),
                        tuple(ven) or (en,), tuple(vde) or (de,))


NOISE = lambda lo, hi: _a("noise", "operating noise", "Betriebsgeräusch", "sound_level", "at_most",  # noqa: E731
                          "dB(A)", lo, hi, 1, ["dB(A)"], ["noise", "operating noise", "noise level"],
                          ["Geräusch", "Betriebsgeräusch", "Geräuschpegel"])


def CURRENT(lo, hi, step, cu=("A", "mA"), key="current_draw"):  # noqa: N802
    return _a(key, "current draw", "Stromaufnahme", "current", "at_most", "A", lo, hi, step, cu,
              ["current draw", "current consumption", "supply current"],
              ["Stromaufnahme", "Stromverbrauch", "Strom"])


CLASSES: tuple[SeatClass, ...] = (
    SeatClass("seat_track", "seat track", "Sitzlängsverstellung",
              "Longitudinal adjuster that moves the seat fore and aft on rails.", (
                  _a("travel", "adjustment travel", "Verstellweg", "length", "at_least", "mm", 180, 300, 5,
                     ["mm", "cm"], ["travel", "adjustment travel", "fore-aft travel"],
                     ["Verstellweg", "Längsverstellweg", "Weg"]),
                  _a("adjust_speed", "adjustment speed", "Verstellgeschwindigkeit", "speed", "within",
                     "mm/s", 10, 35, 1, ["mm/s", "cm/s"], ["adjustment speed", "speed"],
                     ["Verstellgeschwindigkeit", "Geschwindigkeit"]),
                  NOISE(32, 45),
                  _a("holding_load", "holding load", "Haltekraft", "force", "at_least", "kN", 10, 25, 0.5,
                     ["kN", "N"], ["holding load", "retention load"], ["Haltekraft", "Haltelast"]),
              ), "structure"),
    SeatClass("height_adjuster", "height adjuster", "Sitzhöhenverstellung",
              "Mechanism that raises and lowers the seat cushion.", (
                  _a("stroke", "height stroke", "Höhenhub", "length", "at_least", "mm", 40, 80, 2,
                     ["mm", "cm"], ["height stroke", "vertical travel"], ["Höhenhub", "Hub"]),
                  _a("adjust_time", "adjustment time", "Verstellzeit", "time", "at_most", "s", 3, 8, 0.5,
                     ["s", "ms"], ["adjustment time", "full-stroke time"], ["Verstellzeit", "Laufzeit"]),
                  _a("holding_load", "holding load", "Haltekraft", "force", "at_least", "kN", 8, 20, 0.5,
                     ["kN", "N"], ["holding load"], ["Haltekraft"]),
              ), "structure"),
    SeatClass("recliner", "recliner", "Lehnenverstellung",
              "Joint that sets the backrest angle.", (
                  _a("angle_range", "recline angle range", "Lehnenwinkelbereich", "angle", "at_least", "°",
                     30, 60, 2, ["°"], ["recline angle", "angle range"], ["Lehnenwinkel", "Winkelbereich"]),
                  _a("strength", "locking strength", "Festigkeit", "torque", "at_least", "N·m", 1500, 3500, 50,
                     ["N·m", "Nm"], ["locking strength", "static strength"], ["Festigkeit", "Haltemoment"]),
                  NOISE(30, 42),
              ), "structure"),
    SeatClass("lumbar_support", "lumbar support", "Lordosenstütze",
              "Adjustable support in the lower backrest.", (
                  _a("travel", "lumbar travel", "Lordosenhub", "length", "at_least", "mm", 20, 50, 2,
                     ["mm", "cm"], ["lumbar travel", "depth"], ["Lordosenhub", "Tiefe"]),
                  _a("ways", "adjustment ways", "Verstellrichtungen", "count", "at_least", "", 2, 4, 2,
                     [""], ["ways", "adjustment ways"], ["Wege", "Verstellrichtungen"]),
                  CURRENT(0.8, 3.0, 0.1),
              ), "comfort"),
    SeatClass("side_bolster", "side bolster adjustment", "Seitenwangenverstellung",
              "Adjustable side bolsters of cushion and backrest.", (
                  _a("travel", "bolster travel", "Wangenhub", "length", "at_least", "mm", 15, 40, 1,
                     ["mm", "cm"], ["bolster travel"], ["Wangenhub", "Verstellweg"]),
                  _a("adjust_time", "adjustment time", "Verstellzeit", "time", "at_most", "s", 2, 6, 0.5,
                     ["s", "ms"], ["adjustment time"], ["Verstellzeit"]),
              ), "comfort"),
    SeatClass("cushion_extension", "cushion length extension", "Sitzkissenverlängerung",
              "Extendable front part of the seat cushion for thigh support.", (
                  _a("travel", "extension travel", "Auszugsweg", "length", "at_least", "mm", 40, 80, 2,
                     ["mm", "cm"], ["extension travel", "extension"], ["Auszugsweg", "Verlängerung"]),
                  _a("adjust_time", "adjustment time", "Verstellzeit", "time", "at_most", "s", 3, 8, 0.5,
                     ["s", "ms"], ["adjustment time"], ["Verstellzeit"]),
              ), "comfort"),
    SeatClass("headrest_manual", "manual headrest", "Kopfstütze manuell",
              "Headrest adjusted by hand in discrete positions.", (
                  _a("height_travel", "height travel", "Höhenverstellweg", "length", "at_least", "mm", 40, 90, 5,
                     ["mm", "cm"], ["height travel"], ["Höhenverstellweg", "Verstellweg"]),
                  _a("positions", "detent positions", "Raststellungen", "count", "at_least", "", 3, 6, 1,
                     [""], ["positions", "detent positions"], ["Raststellungen", "Positionen"]),
              ), "safety"),
    SeatClass("headrest_powered", "powered headrest", "Kopfstütze elektrisch",
              "Headrest moved by an electric drive.", (
                  _a("height_travel", "height travel", "Höhenverstellweg", "length", "at_least", "mm", 40, 90, 5,
                     ["mm", "cm"], ["height travel"], ["Höhenverstellweg"]),
                  # second length attribute: the attribute name has to tell the two apart
                  _a("tilt_travel", "tilt travel", "Neigungsweg", "length", "at_least", "mm", 10, 40, 1,
                     ["mm", "cm"], ["tilt travel", "fore-aft tilt"], ["Neigungsweg", "Neigungsverstellung"]),
                  _a("adjust_speed", "adjustment speed", "Verstellgeschwindigkeit", "speed", "within", "mm/s",
                     8, 30, 1, ["mm/s", "cm/s"], ["adjustment speed"], ["Verstellgeschwindigkeit"]),
                  CURRENT(1.0, 4.0, 0.1),
              ), "comfort"),
    SeatClass("seat_heating", "seat heating", "Sitzheizung",
              "Heating mats in cushion and backrest.", (
                  _a("power", "heating power", "Heizleistung", "power", "within", "W", 30, 120, 5,
                     ["W"], ["heating power", "power"], ["Heizleistung", "Leistung"]),
                  _a("time_to_temp", "time to 37 °C", "Aufheizzeit auf 37 °C", "time", "at_most", "s",
                     120, 300, 10, ["s", "min"], ["time to 37 °C", "heat-up time"],
                     ["Aufheizzeit auf 37 °C", "Aufheizzeit"]),
                  CURRENT(3.0, 10.0, 0.5),
                  _a("surface_temp", "max. surface temperature", "max. Oberflächentemperatur", "temperature",
                     "at_most", "°C", 40, 48, 1, ["°C"], ["surface temperature"], ["Oberflächentemperatur"]),
              ), "comfort", max_count=4),
    SeatClass("seat_ventilation", "seat ventilation", "Sitzbelüftung",
              "Fans that move air through the perforated trim.", (
                  _a("airflow", "air flow", "Luftvolumenstrom", "volume_flow", "within", "m³/h", 8, 40, 1,
                     ["m³/h", "l/min"], ["air flow", "airflow"], ["Luftvolumenstrom", "Luftmenge"]),
                  NOISE(30, 44),
                  CURRENT(1.0, 4.0, 0.1),
              ), "comfort", max_count=2),
    SeatClass("massage", "massage", "Massagefunktion",
              "Pneumatic or mechanical massage elements in the backrest.", (
                  _a("programs", "massage programs", "Massageprogramme", "count", "at_least", "", 3, 8, 1,
                     [""], ["programs", "massage programs"], ["Programme", "Massageprogramme"]),
                  NOISE(35, 48),
                  CURRENT(2.0, 6.0, 0.5),
              ), "comfort", max_count=8),
    SeatClass("memory_module", "seat memory module", "Memory-Steuergerät",
              "Control unit that stores and recalls seat positions.", (
                  _a("ways", "memorised adjustment ways", "gespeicherte Verstellwege", "count", "at_least", "",
                     6, 12, 2, [""], ["ways", "memorised ways"], ["Wege", "Verstellwege"]),
                  _a("standby_current", "standby current", "Ruhestrom", "current", "at_most", "mA", 0.5, 5.0, 0.1,
                     ["mA", "A"], ["standby current", "quiescent current"], ["Ruhestrom"]),
                  _a("response_time", "recall response time", "Reaktionszeit", "time", "at_most", "ms",
                     50, 200, 10, ["ms", "s"], ["response time"], ["Reaktionszeit", "Ansprechzeit"]),
              ), "electrics"),
    SeatClass("seat_motor", "seat motor drive", "Sitzverstellmotor",
              "Electric motor and gearbox driving an adjustment.", (
                  _a("torque", "output torque", "Abtriebsmoment", "torque", "at_least", "N·m", 3, 12, 0.5,
                     ["N·m", "Nm"], ["output torque", "torque"], ["Abtriebsmoment", "Drehmoment"]),
                  CURRENT(3.0, 12.0, 0.5),
                  NOISE(35, 48),
              ), "electrics", max_count=6),
    SeatClass("easy_entry", "easy-entry mechanism", "Easy-Entry-Mechanik",
              "Tilts and slides the seat forward for rear access.", (
                  _a("travel", "forward travel", "Vorschubweg", "length", "at_least", "mm", 80, 200, 5,
                     ["mm", "cm"], ["forward travel"], ["Vorschubweg", "Weg"]),
                  _a("release_force", "release force", "Auslösekraft", "force", "at_most", "N", 30, 80, 1,
                     ["N"], ["release force", "actuation force"], ["Auslösekraft", "Betätigungskraft"]),
              ), "structure"),
    SeatClass("fold_flat", "fold-flat / tumble mechanism", "Klapp-/Tumblemechanik",
              "Folds the second- or third-row seat flat or tumbles it forward.", (
                  _a("fold_time", "folding time", "Klappzeit", "time", "at_most", "s", 3, 10, 0.5,
                     ["s", "ms"], ["folding time"], ["Klappzeit", "Umlegezeit"]),
                  _a("operating_force", "operating force", "Bedienkraft", "force", "at_most", "N", 40, 120, 5,
                     ["N"], ["operating force"], ["Bedienkraft"]),
                  _a("durability", "durability", "Dauerhaltbarkeit", "count", "at_least", "cycles",
                     10000, 50000, 1000, ["cycles", "Zyklen"], ["durability", "endurance"],
                     ["Dauerhaltbarkeit", "Lebensdauer"]),
              ), "structure"),
    SeatClass("belt_presenter", "seat-belt buckle presenter", "Gurtschlossbringer",
              "Raises the belt buckle for easy reach.", (
                  _a("reach", "presenter reach", "Bringerhub", "length", "at_least", "mm", 100, 250, 5,
                     ["mm", "cm"], ["reach", "presenter stroke"], ["Bringerhub", "Hub"]),
                  _a("cycle_time", "cycle time", "Zykluszeit", "time", "at_most", "s", 1, 4, 0.5,
                     ["s", "ms"], ["cycle time"], ["Zykluszeit", "Taktzeit"]),
              ), "safety"),
    SeatClass("occupant_detection", "occupant detection mat", "Sitzbelegungserkennung",
              "Sensor mat that detects an occupant on the cushion.", (
                  _a("detection_threshold", "detection threshold", "Erkennungsschwelle", "mass", "at_most", "kg",
                     5, 30, 1, ["kg", "g"], ["detection threshold"], ["Erkennungsschwelle", "Ansprechschwelle"]),
                  _a("response_time", "response time", "Ansprechzeit", "time", "at_most", "ms", 50, 500, 10,
                     ["ms", "s"], ["response time"], ["Ansprechzeit"]),
              ), "safety"),
    SeatClass("side_airbag", "side airbag integration", "Seitenairbag-Integration",
              "Seat-integrated side airbag with tear seam in the trim.", (
                  _a("deployment_time", "seam opening time", "Nahtöffnungszeit", "time", "at_most", "ms", 5, 15, 0.5,
                     ["ms"], ["seam opening time", "deployment time"], ["Nahtöffnungszeit", "Auslösezeit"]),
                  _a("seam_strength", "seam strength", "Nahtfestigkeit", "force", "at_least", "N", 300, 800, 10,
                     ["N", "kN"], ["seam strength"], ["Nahtfestigkeit"]),
              ), "safety"),
    SeatClass("armrest", "armrest", "Armlehne",
              "Folding armrest at the seat or in the centre.", (
                  _a("vertical_load", "vertical load", "Vertikallast", "force", "at_least", "kN", 0.8, 2.0, 0.1,
                     ["kN", "N"], ["vertical load"], ["Vertikallast", "Belastbarkeit"]),
                  _a("pivot_angle", "pivot angle", "Schwenkwinkel", "angle", "at_least", "°", 10, 90, 5,
                     ["°"], ["pivot angle"], ["Schwenkwinkel"]),
              ), "comfort"),
    SeatClass("trim_cover", "trim cover", "Sitzbezug",
              "Textile or leather cover of cushion and backrest.", (
                  _a("abrasion", "abrasion resistance (Martindale)", "Scheuerbeständigkeit (Martindale)", "count",
                     "at_least", "cycles", 30000, 100000, 5000, ["cycles", "Zyklen"],
                     ["abrasion resistance", "Martindale"], ["Scheuerbeständigkeit", "Martindale"]),
                  _a("flammability", "flammability class", "Brennbarkeitsklasse", "flammability_class",
                     "at_least", "", 0, 0, 0, [], ["flammability class"], ["Brennbarkeitsklasse"]),
                  _a("price", "target price", "Zielpreis", "currency", "at_most", "EUR", 40, 180, 5,
                     ["EUR", "€"], ["target price", "piece price"], ["Zielpreis", "Stückpreis"]),
              ), "structure"),
    SeatClass("foam_pad", "foam pad", "Polsterschaum",
              "Moulded foam of cushion and backrest.", (
                  _a("hardness", "indentation hardness", "Stauchhärte", "force", "equals", "N", 150, 400, 10,
                     ["N"], ["indentation hardness", "hardness"], ["Stauchhärte", "Härte"]),
                  _a("mass", "foam mass", "Schaummasse", "mass", "at_most", "kg", 0.6, 2.0, 0.1,
                     ["kg", "g"], ["mass", "weight"], ["Masse", "Gewicht"]),
              ), "structure"),
    SeatClass("seat_frame", "seat frame / structure", "Sitzstruktur",
              "Load-bearing frame of cushion and backrest.", (
                  _a("mass", "frame mass", "Strukturmasse", "mass", "at_most", "kg", 8, 20, 0.5,
                     ["kg", "g"], ["mass", "weight"], ["Masse", "Gewicht"]),
                  _a("crash_load", "crash load", "Crashlast", "force", "at_least", "kN", 20, 60, 1,
                     ["kN"], ["crash load", "crash strength"], ["Crashlast", "Crashfestigkeit"]),
                  _a("price", "target price", "Zielpreis", "currency", "at_most", "EUR", 60, 250, 5,
                     ["EUR", "€"], ["target price"], ["Zielpreis"]),
              ), "structure"),
    SeatClass("wiring", "wiring and connectors", "Sitzkabelsatz",
              "Wiring harness and connectors of the seat.", (
                  _a("current_rating", "current rating", "Strombelastbarkeit", "current", "at_least", "A", 5, 20, 1,
                     ["A"], ["current rating"], ["Strombelastbarkeit"]),
                  _a("protection", "protection class", "Schutzart", "protection_class", "at_least", "", 0, 0, 0,
                     [], ["protection class"], ["Schutzart"]),
              ), "electrics"),
)

CLASS_BY_ID = {c.class_id: c for c in CLASSES}

GROUP_LABELS = {
    "en": {"structure": "Structure and mechanisms", "comfort": "Comfort functions",
           "safety": "Safety", "electrics": "Electrics and electronics"},
    "de": {"structure": "Struktur und Mechanik", "comfort": "Komfortfunktionen",
           "safety": "Sicherheit", "electrics": "Elektrik und Elektronik"},
}

# --------------------------------------------------------------------------- makers

MAKERS = (
    ("aurivon", "Aurivon Motors", "AUR"),
    ("brennholt", "Brennholt Fahrzeugwerke", "BRH"),
    ("calvento", "Calvento Automotive", "CAL"),
    ("dravik", "Dravik Mobility", "DRV"),
    ("estrelo", "Estrelo Cars", "EST"),
    ("falkmoor", "Falkmoor Automobile", "FKM"),
    ("grovania", "Grovania Motor Company", "GRV"),
    ("hirschtal", "Hirschtal Fahrzeugbau", "HIR"),
)

SUPPLIER = "Sitzwerk Demo GmbH"

# Sentence frames per maker; {x} is the phrasing plus values. These are the
# "source boilerplate" that per-source centring removes.
FRAMES = {
    "aurivon": {"en": "The seat shall provide {x}.", "de": "Der Sitz muss {x} bereitstellen."},
    "brennholt": {"en": "Supplier shall deliver {x}.", "de": "Der Lieferant liefert {x}."},
    "calvento": {"en": "Requirement: {x}.", "de": "Anforderung: {x}."},
    "dravik": {"en": "The system must include {x}.", "de": "Das System muss {x} enthalten."},
    "estrelo": {"en": "Seat assembly to feature {x}.", "de": "Sitzanlage mit {x}."},
    "falkmoor": {"en": "It is required that the seat has {x}.", "de": "Es wird gefordert, dass der Sitz {x} hat."},
    "grovania": {"en": "Scope includes {x}.", "de": "Lieferumfang umfasst {x}."},
    "hirschtal": {"en": "Function: {x}.", "de": "Funktion: {x}."},
}

# --------------------------------------------------------------------------- internal vocabulary
#
# Closed set of wordings used in the supplier's own module library and
# programme archive (source "internal"). Misleading wordings point at the
# wrong class by their surface form; they are the vocabulary review targets.


@dataclass(frozen=True)
class Wording:
    text: str
    class_id: str
    apparent_class_id: str = ""  # set only for misleading wordings

    @property
    def misleading(self) -> bool:
        return bool(self.apparent_class_id)


VOCABULARY: tuple[Wording, ...] = (
    Wording("Längsversteller", "seat_track"), Wording("LV-Schiene", "seat_track"),
    Wording("Höhenversteller", "height_adjuster"), Wording("HV-Kinematik", "height_adjuster"),
    Wording("Lehnenbeschlag", "recliner"), Wording("Recliner-Beschlag", "recliner"),
    Wording("Lordose 4-Wege", "lumbar_support"), Wording("Lordosenmodul", "lumbar_support"),
    Wording("Wangenverstellung", "side_bolster"), Wording("Seitenwange aktiv", "side_bolster"),
    Wording("Kissenauszug", "cushion_extension"), Wording("OS-Auflage verstellbar", "cushion_extension"),
    Wording("KS manuell", "headrest_manual"), Wording("Kopfstütze Raste", "headrest_manual"),
    Wording("KS elektrisch", "headrest_powered"), Wording("Kopfstützenantrieb", "headrest_powered"),
    Wording("Heizmatte", "seat_heating"), Wording("Sitzheizung Carbon", "seat_heating"),
    Wording("Lüftermodul", "seat_ventilation"), Wording("Sitzklima-Modul", "seat_ventilation"),
    Wording("Massagemodul pneumatisch", "massage"), Wording("Massage-Blasenmatte", "massage"),
    Wording("Memory-SG", "memory_module"), Wording("Sitzmemory", "memory_module"),
    Wording("Verstellmotor", "seat_motor"), Wording("Getriebemotor Sitz", "seat_motor"),
    Wording("Easy-Entry-Hebel", "easy_entry"), Wording("Einstiegshilfe 2R", "easy_entry"),
    Wording("Tumble-Beschlag", "fold_flat"), Wording("Umlegemechanik", "fold_flat"),
    Wording("Gurtbringer", "belt_presenter"), Wording("Schlossbringer elektr.", "belt_presenter"),
    Wording("Beifahrer-Belegungsmatte", "occupant_detection"), Wording("SBE-Matte", "occupant_detection"),
    Wording("Airbag-Reißnaht", "side_airbag"), Wording("SAB-Integration", "side_airbag"),
    Wording("Mittelarmlehne", "armrest"), Wording("Armlehne klappbar", "armrest"),
    Wording("Bezug Stoff", "trim_cover"), Wording("Bezug Leder", "trim_cover"),
    Wording("Formschaum", "foam_pad"), Wording("PU-Polster", "foam_pad"),
    Wording("Sitzgestell", "seat_frame"), Wording("Rohbaustruktur Sitz", "seat_frame"),
    Wording("Kabelbaum Sitz", "wiring"), Wording("Steckersatz", "wiring"),
    # misleading: the surface form points at the apparent class
    Wording("Klimapaket Heizung", "seat_ventilation", "seat_heating"),
    Wording("LV-Struktur", "seat_frame", "seat_track"),
    Wording("Massagelordose", "massage", "lumbar_support"),
    Wording("KS-Antrieb", "seat_motor", "headrest_powered"),
)

INTERNAL_SOURCE = "internal"

# Combo modules provide several classes in one application.
COMBOS = (
    ("Komfortpaket Klima", ("seat_heating", "seat_ventilation")),
    ("Komfortpaket Premium", ("seat_heating", "seat_ventilation", "massage")),
    ("Sitzverstellsystem 4-Wege", ("seat_track", "height_adjuster")),
    ("Memory-Antriebspaket", ("memory_module", "seat_motor")),
    ("Kopfstütze Komfort elektr.", ("headrest_powered", "seat_motor")),
)

# --------------------------------------------------------------------------- phrasing pool
#
# 18 phrasings per class: 9 development, 9 held-out test. No test phrasing is a
# substring of a development phrasing, frame, heading or attribute name, so a
# leak check by substring is meaningful. Concept tokens drive late interaction.


@dataclass(frozen=True)
class Phrasing:
    text: str
    lang: str
    concepts: tuple[str, ...]  # concept tokens of the class this phrasing uses


def _p(text, lang, *concepts):
    return Phrasing(text, lang, concepts)


PHRASINGS: dict[str, dict[str, tuple[Phrasing, ...]]] = {
    "seat_track": {
        "dev": (_p("a longitudinal seat adjuster", "en", "longitudinal", "adjust"),
                _p("fore-aft seat rails", "en", "fore-aft", "rail"),
                _p("a seat slide track", "en", "slide", "track"),
                _p("sliding rails for the seat", "en", "slide", "rail"),
                _p("eine Längsverstellung", "de", "längs", "verstell"),
                _p("Sitzschienen für die Längsverstellung", "de", "schiene", "längs", "verstell"),
                _p("ein Schienensystem längs", "de", "schiene", "längs"),
                _p("Sitzlängsführung", "de", "längs", "führung"),
                _p("track adjuster fore/aft", "en", "track", "fore-aft", "adjust")),
        "test": (_p("horizontal seat travel unit", "en", "fore-aft", "track", "travel"),
                 _p("rail-mounted slide mechanism", "en", "rail", "slide"),
                 _p("front-to-back positioning tracks", "en", "fore-aft", "track"),
                 _p("seat runner assembly", "en", "slide", "rail", "runner"),
                 _p("Laufschienenpaar", "de", "schiene", "führung"),
                 _p("Vor-/Zurück-Verstelleinheit", "de", "längs", "verstell"),
                 _p("Gleitschienen-Baugruppe", "de", "schiene", "slide"),
                 _p("Längsführungsschienen", "de", "längs", "führung", "schiene"),
                 _p("rails with fore/aft lock", "en", "rail", "fore-aft", "lock")),
    },
    "height_adjuster": {
        "dev": (_p("a seat height adjuster", "en", "height", "adjust"),
                _p("vertical cushion adjustment", "en", "vertical", "adjust"),
                _p("a seat lifter", "en", "lift", "height"),
                _p("height pumping mechanism", "en", "height", "pump"),
                _p("eine Sitzhöhenverstellung", "de", "höhe", "verstell"),
                _p("Höhenverstellmechanik", "de", "höhe", "verstell", "mechanik"),
                _p("einen Sitzheber", "de", "heber", "höhe"),
                _p("Pumpe für Sitzhöhe", "de", "pump", "höhe"),
                _p("cushion lifting kinematics", "en", "lift", "vertical")),
        "test": (_p("up/down cushion positioning", "en", "vertical", "height"),
                 _p("ratchet height pump", "en", "pump", "height", "ratchet"),
                 _p("elevation linkage", "en", "lift", "vertical", "linkage"),
                 _p("H-point raising unit", "en", "height", "lift"),
                 _p("Hubmechanik Sitzkissen", "de", "heber", "höhe", "mechanik"),
                 _p("Höhenpumpe", "de", "pump", "höhe"),
                 _p("Vertikalverstellung", "de", "vertical", "verstell"),
                 _p("Sitzanhebung", "de", "heber", "höhe"),
                 _p("seat riser linkage", "en", "lift", "linkage")),
    },
    "recliner": {
        "dev": (_p("a backrest recliner", "en", "backrest", "recline"),
                _p("seat back angle adjustment", "en", "backrest", "angle"),
                _p("recliner fittings", "en", "recline", "fitting"),
                _p("a reclining joint", "en", "recline", "joint"),
                _p("eine Lehnenneigungsverstellung", "de", "lehne", "neigung"),
                _p("Lehnenbeschläge", "de", "lehne", "beschlag"),
                _p("Rückenlehnenverstellung", "de", "lehne", "verstell"),
                _p("Neigungsgelenk Lehne", "de", "neigung", "lehne", "joint"),
                _p("backrest tilt fitting", "en", "backrest", "fitting", "tilt")),
        "test": (_p("seatback inclination hinge", "en", "backrest", "joint", "tilt"),
                 _p("rotary recline lock", "en", "recline", "lock"),
                 _p("disc recliner pair", "en", "recline", "fitting"),
                 _p("back angle mechanism", "en", "angle", "backrest"),
                 _p("Lehnengelenk", "de", "lehne", "joint"),
                 _p("Neigungsbeschlag", "de", "neigung", "beschlag"),
                 _p("Lehnenwinkeleinsteller", "de", "lehne", "angle", "verstell"),
                 _p("Taumelbeschlag", "de", "beschlag", "tilt"),
                 _p("continuous recline hinge", "en", "recline", "joint")),
    },
    "lumbar_support": {
        "dev": (_p("a lumbar support", "en", "lumbar", "support"),
                _p("adjustable lower-back support", "en", "lumbar", "back", "support"),
                _p("4-way lumbar", "en", "lumbar", "way"),
                _p("lumbar basket", "en", "lumbar", "basket"),
                _p("eine Lordosenstütze", "de", "lordose", "stütze"),
                _p("Lordosenverstellung", "de", "lordose", "verstell"),
                _p("Kreuzstütze", "de", "kreuz", "stütze"),
                _p("Lendenwirbelstütze", "de", "lende", "stütze"),
                _p("lumbar bladder system", "en", "lumbar", "bladder")),
        "test": (_p("small-of-back support", "en", "back", "support", "lumbar"),
                 _p("spine contour adjuster", "en", "back", "contour"),
                 _p("lower-back cushion pad", "en", "back", "lumbar", "pad"),
                 _p("pneumatic lumbar cells", "en", "lumbar", "bladder"),
                 _p("Lordosenkorb", "de", "lordose", "basket"),
                 _p("Kreuzstützenverstellung", "de", "kreuz", "stütze", "verstell"),
                 _p("Lendenbereichsstütze", "de", "lende", "stütze"),
                 _p("Rückenkonturverstellung", "de", "contour", "back"),
                 _p("lumbar 2-way support", "en", "lumbar", "way", "support")),
    },
    "side_bolster": {
        "dev": (_p("adjustable side bolsters", "en", "bolster", "side"),
                _p("bolster width adjustment", "en", "bolster", "width"),
                _p("lateral support wings", "en", "lateral", "wing"),
                _p("side support adjustment", "en", "side", "lateral"),
                _p("eine Seitenwangenverstellung", "de", "wange", "seite"),
                _p("verstellbare Seitenwangen", "de", "wange", "verstell"),
                _p("Seitenhalt aktiv", "de", "seite", "halt"),
                _p("Wangenbreitenverstellung", "de", "wange", "breite"),
                _p("bolster bladders", "en", "bolster", "bladder")),
        "test": (_p("lateral hold adjusters", "en", "lateral", "halt"),
                 _p("wing support actuation", "en", "wing", "side"),
                 _p("cornering support cushions", "en", "lateral", "side"),
                 _p("dynamic flank support", "en", "side", "wing", "flank"),
                 _p("Flankenstützen", "de", "flank", "seite"),
                 _p("Seitenstützenverstellung", "de", "seite", "verstell", "wange"),
                 _p("Kurvenhalt-Wangen", "de", "wange", "halt"),
                 _p("Lateralstützen", "de", "lateral", "seite"),
                 _p("bolster width actuator", "en", "bolster", "breite")),
    },
    "cushion_extension": {
        "dev": (_p("a cushion length extension", "en", "cushion", "extension"),
                _p("thigh support extension", "en", "thigh", "extension"),
                _p("extendable seat cushion", "en", "cushion", "extend"),
                _p("cushion depth adjustment", "en", "cushion", "depth"),
                _p("eine Sitztiefenverstellung", "de", "sitztiefe", "verstell"),
                _p("Oberschenkelauflage", "de", "oberschenkel", "auflage"),
                _p("Sitzkissenauszug", "de", "kissen", "auszug"),
                _p("ausziehbare Sitzfläche", "de", "auszug", "kissen"),
                _p("thigh rest", "en", "thigh", "rest")),
        "test": (_p("seat depth extender", "en", "depth", "extend"),
                 _p("under-thigh support flap", "en", "thigh", "support", "flap"),
                 _p("front cushion slide-out", "en", "cushion", "extend"),
                 _p("leg rest extension", "en", "extension", "rest"),
                 _p("Schenkelauflage verstellbar", "de", "oberschenkel", "auflage", "verstell"),
                 _p("Sitzkissenvorschub", "de", "kissen", "sitztiefe"),
                 _p("Auszugkissen", "de", "auszug", "kissen"),
                 _p("Sitzflächenverlängerung", "de", "kissen", "extension"),
                 _p("cushion length adjuster", "en", "cushion", "depth")),
    },
    "headrest_manual": {
        "dev": (_p("a manual headrest", "en", "headrest", "manual"),
                _p("height-adjustable head restraint", "en", "head", "restraint", "manual"),
                _p("headrest with detents", "en", "headrest", "detent"),
                _p("mechanical head support", "en", "head", "manual"),
                _p("eine manuelle Kopfstütze", "de", "kopfstütze", "manuell"),
                _p("Kopfstütze mit Rasten", "de", "kopfstütze", "raste"),
                _p("mechanische Kopfstütze", "de", "kopfstütze", "manuell"),
                _p("Kopfstütze handverstellbar", "de", "kopfstütze", "hand"),
                _p("notched head restraint", "en", "restraint", "detent")),
        "test": (_p("hand-set head restraint", "en", "restraint", "hand"),
                 _p("click-stop headrest", "en", "headrest", "detent"),
                 _p("push-button head support", "en", "head", "manual", "button"),
                 _p("non-powered headrest", "en", "headrest", "manual"),
                 _p("Kopfstütze rastend", "de", "kopfstütze", "raste"),
                 _p("Kopfteil manuell", "de", "kopf", "manuell"),
                 _p("Nackenstütze mechanisch", "de", "nacken", "manuell"),
                 _p("Rastkopfstütze", "de", "raste", "kopfstütze"),
                 _p("head restraint with locking stems", "en", "restraint", "head", "lock")),
    },
    "headrest_powered": {
        "dev": (_p("a powered headrest", "en", "headrest", "powered"),
                _p("electric head restraint", "en", "restraint", "electric"),
                _p("motorised headrest", "en", "headrest", "motor"),
                _p("power head support", "en", "head", "powered"),
                _p("eine elektrische Kopfstütze", "de", "kopfstütze", "elektrisch"),
                _p("motorische Kopfstütze", "de", "kopfstütze", "motor"),
                _p("Kopfstütze mit Antrieb", "de", "kopfstütze", "antrieb"),
                _p("elektrisch verstellbare Kopfstütze", "de", "kopfstütze", "elektrisch", "verstell"),
                _p("e-headrest", "en", "headrest", "electric")),
        "test": (_p("motor-driven head restraint", "en", "restraint", "motor"),
                 _p("power-adjust headrest", "en", "headrest", "powered", "adjust"),
                 _p("actuated neck support", "en", "head", "actuator"),
                 _p("electrically raised headrest", "en", "headrest", "electric"),
                 _p("Motorkopfstütze", "de", "motor", "kopfstütze"),
                 _p("Nackenstütze elektrisch", "de", "nacken", "elektrisch"),
                 _p("Kopfteil motorisch", "de", "kopf", "motor"),
                 _p("Kopfstütze E-Antrieb", "de", "kopfstütze", "antrieb", "elektrisch"),
                 _p("servo headrest", "en", "headrest", "actuator")),
    },
    "seat_heating": {
        "dev": (_p("seat heating", "en", "heat", "seat"),
                _p("heated seat cushion and backrest", "en", "heat", "cushion"),
                _p("heater mats", "en", "heat", "mat"),
                _p("seat warmer", "en", "warm", "seat"),
                _p("eine Sitzheizung", "de", "heizung", "sitz"),
                _p("Heizmatten in Kissen und Lehne", "de", "heiz", "matte"),
                _p("beheizte Sitzflächen", "de", "beheizt", "sitz"),
                _p("Sitzwärmer", "de", "wärme", "sitz"),
                _p("cushion heating elements", "en", "heat", "element")),
        "test": (_p("thermal comfort pads", "en", "warm", "mat", "thermal"),
                 _p("carbon-fibre heater wires", "en", "heat", "wire"),
                 _p("warming function for the cushion", "en", "warm", "cushion"),
                 _p("heating foil elements", "en", "heat", "foil", "element"),
                 _p("Heizelemente Sitzfläche", "de", "heiz", "element"),
                 _p("Wärmematten", "de", "wärme", "matte"),
                 _p("Polsterbeheizung", "de", "beheizt", "polster"),
                 _p("Heizdrahtgewebe", "de", "heiz", "wire"),
                 _p("thermo mat set", "en", "thermal", "mat")),
    },
    "seat_ventilation": {
        "dev": (_p("seat ventilation", "en", "ventilat", "seat"),
                _p("ventilated seats", "en", "ventilat", "air"),
                _p("cooling fans in the seat", "en", "fan", "cool"),
                _p("active seat climate airflow", "en", "air", "climate"),
                _p("eine Sitzbelüftung", "de", "belüftung", "sitz"),
                _p("Lüfter im Sitz", "de", "lüfter", "sitz"),
                _p("aktive Sitzklimatisierung", "de", "klima", "luft"),
                _p("belüftete Sitzflächen", "de", "belüftung", "luft"),
                _p("seat cooling blowers", "en", "fan", "cool", "blower")),
        "test": (_p("perforated airflow system", "en", "air", "perforat"),
                 _p("blower-based seat cooling", "en", "blower", "cool"),
                 _p("climate-controlled cushion air", "en", "climate", "air"),
                 _p("through-trim ventilation fans", "en", "ventilat", "fan"),
                 _p("Gebläse im Polster", "de", "lüfter", "blower"),
                 _p("Luftdurchströmung Sitz", "de", "luft", "sitz"),
                 _p("Kühlgebläse Sitzfläche", "de", "blower", "cool"),
                 _p("Sitzlüftungssystem", "de", "belüftung", "lüfter"),
                 _p("air-conditioned seat", "en", "climate", "cool")),
    },
    "massage": {
        "dev": (_p("a massage function", "en", "massage", "function"),
                _p("back massage", "en", "massage", "back"),
                _p("pneumatic massage cells", "en", "massage", "pneumatic"),
                _p("massage programmes in the backrest", "en", "massage", "programme"),
                _p("eine Massagefunktion", "de", "massage", "funktion"),
                _p("Rückenmassage", "de", "massage", "rücken"),
                _p("pneumatische Massage", "de", "massage", "pneumatisch"),
                _p("Massageprogramme", "de", "massage", "programm"),
                _p("kneading massage", "en", "massage", "knead")),
        "test": (_p("relaxation pulsing cells", "en", "pulse", "pneumatic"),
                 _p("wave-pattern back kneading", "en", "knead", "back"),
                 _p("air-bladder massage", "en", "massage", "bladder"),
                 _p("hot-stone style pulsation", "en", "pulse", "relax"),
                 _p("Entspannungsfunktion pulsierend", "de", "pulse", "relax"),
                 _p("Knetmassage", "de", "knead", "massage"),
                 _p("Wellenmassage Lehne", "de", "massage", "welle"),
                 _p("Vitalisierungsprogramm", "de", "programm", "relax"),
                 _p("shiatsu programme", "en", "massage", "programme", "knead")),
    },
    "memory_module": {
        "dev": (_p("a seat memory module", "en", "memory", "module"),
                _p("seat position memory", "en", "memory", "position"),
                _p("memory control unit", "en", "memory", "control"),
                _p("stored seat positions", "en", "store", "position"),
                _p("ein Memory-Steuergerät", "de", "memory", "steuergerät"),
                _p("Sitzpositionsspeicher", "de", "speicher", "position"),
                _p("Memoryfunktion", "de", "memory", "funktion"),
                _p("Speichertasten für Sitzposition", "de", "speicher", "taste"),
                _p("seat memory ECU", "en", "memory", "ecu")),
        "test": (_p("driver profile recall", "en", "recall", "profile"),
                 _p("position recall controller", "en", "recall", "position", "control"),
                 _p("personalisation ECU", "en", "ecu", "profile"),
                 _p("preset seat positions", "en", "store", "position", "preset"),
                 _p("Positionsabruf", "de", "position", "recall"),
                 _p("Speichersteuergerät", "de", "speicher", "steuergerät"),
                 _p("Fahrerprofilspeicher", "de", "speicher", "profile"),
                 _p("Memorytasten", "de", "memory", "taste"),
                 _p("memory recall buttons", "en", "memory", "recall")),
    },
    "seat_motor": {
        "dev": (_p("seat adjustment motors", "en", "motor", "adjust"),
                _p("electric seat drives", "en", "drive", "electric"),
                _p("gear motors", "en", "motor", "gear"),
                _p("motor drive units", "en", "motor", "drive"),
                _p("Sitzverstellmotoren", "de", "motor", "verstell"),
                _p("elektrische Sitzantriebe", "de", "antrieb", "elektrisch"),
                _p("Getriebemotoren", "de", "motor", "getriebe"),
                _p("Antriebseinheiten", "de", "antrieb", "einheit"),
                _p("power seat actuators", "en", "actuator", "drive")),
        "test": (_p("DC spindle drives", "en", "drive", "spindle"),
                 _p("worm-gear actuators", "en", "actuator", "gear"),
                 _p("electromechanical seat actuation", "en", "actuator", "electric"),
                 _p("brushed motor units", "en", "motor", "unit"),
                 _p("Spindelantriebe", "de", "antrieb", "spindle"),
                 _p("Stellmotoren Sitz", "de", "motor", "actuator"),
                 _p("Schneckengetriebeantrieb", "de", "getriebe", "antrieb"),
                 _p("E-Motoren Verstellung", "de", "motor", "verstell", "elektrisch"),
                 _p("drive actuators 12 V", "en", "drive", "actuator")),
    },
    "easy_entry": {
        "dev": (_p("an easy-entry mechanism", "en", "easy-entry", "mechanism"),
                _p("easy entry for rear passengers", "en", "easy-entry", "rear"),
                _p("walk-in function", "en", "walk-in", "rear"),
                _p("entry assist", "en", "entry", "assist"),
                _p("eine Easy-Entry-Funktion", "de", "easy-entry", "funktion"),
                _p("Einstiegshilfe", "de", "einstieg", "hilfe"),
                _p("Easy-Entry-Mechanik", "de", "easy-entry", "mechanik"),
                _p("Durchstiegsfunktion", "de", "einstieg", "rear"),
                _p("walk-in lever", "en", "walk-in", "lever")),
        "test": (_p("rear-access tip-slide", "en", "rear", "entry", "tip"),
                 _p("third-row access mechanism", "en", "entry", "rear", "mechanism"),
                 _p("one-touch walk-in", "en", "walk-in", "touch"),
                 _p("ingress assist slide", "en", "entry", "assist"),
                 _p("Zugangshilfe Fond", "de", "einstieg", "hilfe", "rear"),
                 _p("Einstiegsmechanik", "de", "einstieg", "mechanik"),
                 _p("Kipp-Schiebe-Funktion", "de", "tip", "einstieg"),
                 _p("Fondzugang", "de", "rear", "einstieg"),
                 _p("entry release lever", "en", "entry", "lever")),
    },
    "fold_flat": {
        "dev": (_p("a fold-flat mechanism", "en", "fold", "flat"),
                _p("tumble seat function", "en", "tumble", "seat"),
                _p("folding rear seat", "en", "fold", "rear"),
                _p("flat-folding backrest", "en", "fold", "flat", "backrest"),
                _p("eine Umklappfunktion", "de", "klapp", "funktion"),
                _p("Tumble-Funktion", "de", "tumble", "funktion"),
                _p("umlegbare Rücksitze", "de", "umleg", "rear"),
                _p("Flachklappmechanik", "de", "klapp", "flach"),
                _p("seat tumble mechanism", "en", "tumble", "mechanism")),
        "test": (_p("stow-flat function", "en", "flat", "stow"),
                 _p("cargo-mode fold", "en", "fold", "cargo"),
                 _p("seat flip-forward", "en", "tumble", "flip"),
                 _p("remote fold release", "en", "fold", "release"),
                 _p("Ladeflächenmodus", "de", "flach", "cargo"),
                 _p("Klappsitzmechanik", "de", "klapp", "mechanik"),
                 _p("Vorklappfunktion", "de", "klapp", "flip"),
                 _p("Verstaufunktion Sitz", "de", "stow", "umleg"),
                 _p("fold and tumble unit", "en", "fold", "tumble")),
    },
    "belt_presenter": {
        "dev": (_p("a belt buckle presenter", "en", "buckle", "presenter"),
                _p("seat-belt presenter", "en", "belt", "presenter"),
                _p("buckle lifter", "en", "buckle", "lift-up"),
                _p("belt reach assist", "en", "belt", "reach"),
                _p("einen Gurtschlossbringer", "de", "gurtschloss", "bringer"),
                _p("Gurtbringer", "de", "gurt", "bringer"),
                _p("Gurtschlossanhebung", "de", "gurtschloss", "heben"),
                _p("Gurtzuführung", "de", "gurt", "zuführung"),
                _p("buckle presenter actuator", "en", "buckle", "presenter", "actuator")),
        "test": (_p("powered buckle raise", "en", "buckle", "lift-up"),
                 _p("belt hand-over device", "en", "belt", "handover"),
                 _p("latch presentation unit", "en", "latch", "presenter"),
                 _p("buckle extender motor", "en", "buckle", "reach"),
                 _p("Schlossanhebung elektrisch", "de", "heben", "gurtschloss"),
                 _p("Gurtübergabe", "de", "gurt", "handover"),
                 _p("Gurtschlosszuführung", "de", "gurtschloss", "zuführung"),
                 _p("Schlosspräsenter", "de", "presenter", "gurtschloss"),
                 _p("belt latch lifter", "en", "latch", "lift-up")),
    },
    "occupant_detection": {
        "dev": (_p("occupant detection", "en", "occupant", "detect"),
                _p("seat occupancy sensor", "en", "occupancy", "sensor"),
                _p("passenger presence mat", "en", "presence", "mat"),
                _p("occupant classification mat", "en", "occupant", "classif"),
                _p("eine Sitzbelegungserkennung", "de", "belegung", "erkennung"),
                _p("Belegungssensor", "de", "belegung", "sensor"),
                _p("Insassenerkennung", "de", "insasse", "erkennung"),
                _p("Sensormatte Sitzbelegung", "de", "sensor", "matte", "belegung"),
                _p("occupancy detection pad", "en", "occupancy", "detect")),
        "test": (_p("weight-sensing seat pad", "en", "sensor", "weight"),
                 _p("passenger classification system", "en", "classif", "occupant"),
                 _p("presence detection foil", "en", "presence", "detect", "foil"),
                 _p("capacitive occupant sensing", "en", "occupant", "sensor"),
                 _p("Gewichtserkennung Beifahrer", "de", "weight", "erkennung"),
                 _p("Insassenklassifizierung", "de", "insasse", "classif"),
                 _p("Präsenzsensor Sitz", "de", "presence", "sensor"),
                 _p("Belegterkennungsfolie", "de", "belegung", "foil"),
                 _p("seat sensing mat", "en", "sensor", "mat")),
    },
    "side_airbag": {
        "dev": (_p("side airbag integration", "en", "airbag", "side"),
                _p("seat-mounted side airbag", "en", "airbag", "seat-mounted"),
                _p("airbag tear seam", "en", "airbag", "seam"),
                _p("thorax airbag in the backrest", "en", "airbag", "thorax"),
                _p("eine Seitenairbag-Integration", "de", "airbag", "seite"),
                _p("Airbag-Aufreißnaht", "de", "airbag", "naht"),
                _p("Seitenairbag im Sitz", "de", "airbag", "sitz"),
                _p("Thoraxairbag", "de", "airbag", "thorax"),
                _p("airbag chute", "en", "airbag", "chute")),
        "test": (_p("inflatable side restraint", "en", "inflat", "side"),
                 _p("deployment seam zone", "en", "seam", "deploy"),
                 _p("pelvis-thorax cushion", "en", "thorax", "inflat"),
                 _p("side impact bag module", "en", "airbag", "impact"),
                 _p("Auslösenahtbereich", "de", "naht", "deploy"),
                 _p("Seitenaufprallschutz Sitz", "de", "impact", "seite"),
                 _p("aufblasbarer Seitenschutz", "de", "inflat", "seite"),
                 _p("Brustkorb-Luftsack", "de", "thorax", "inflat"),
                 _p("bag deployment chute", "en", "chute", "deploy")),
    },
    "armrest": {
        "dev": (_p("an armrest", "en", "armrest", "arm"),
                _p("folding centre armrest", "en", "armrest", "fold-arm", "centre"),
                _p("arm support", "en", "arm", "support-arm"),
                _p("elbow rest", "en", "elbow", "rest-arm"),
                _p("eine Armlehne", "de", "armlehne", "arm"),
                _p("Mittelarmlehne klappbar", "de", "armlehne", "mitte"),
                _p("Armauflage", "de", "arm", "auflage-arm"),
                _p("Ellbogenauflage", "de", "elbow", "auflage-arm"),
                _p("seat armrest", "en", "armrest", "seat-arm")),
        "test": (_p("fold-down arm support", "en", "fold-arm", "support-arm"),
                 _p("centre elbow cushion", "en", "elbow", "centre"),
                 _p("integrated arm pad", "en", "arm", "pad-arm"),
                 _p("pivoting arm rest", "en", "arm", "pivot-arm"),
                 _p("Armstütze", "de", "arm", "support-arm"),
                 _p("Ellbogenstütze", "de", "elbow", "support-arm"),
                 _p("Schwenkarmlehne", "de", "armlehne", "pivot-arm"),
                 _p("Mittelauflage Arm", "de", "mitte", "auflage-arm"),
                 _p("armrest with cupholder", "en", "armrest", "cup")),
    },
    "trim_cover": {
        "dev": (_p("a trim cover", "en", "trim", "cover"),
                _p("seat upholstery", "en", "upholster", "seat"),
                _p("fabric seat covers", "en", "fabric", "cover"),
                _p("leather trim", "en", "leather", "trim"),
                _p("einen Sitzbezug", "de", "bezug", "sitz"),
                _p("Stoffbezug", "de", "stoff", "bezug"),
                _p("Lederbezüge", "de", "leder", "bezug"),
                _p("Polsterbezug", "de", "polster", "bezug"),
                _p("trim cover set", "en", "trim", "cover")),
        "test": (_p("textile surface material", "en", "fabric", "surface"),
                 _p("synthetic leather skin", "en", "leather", "skin"),
                 _p("cut-and-sew covers", "en", "cover", "sew"),
                 _p("seat skin assembly", "en", "skin", "upholster"),
                 _p("Kunstlederhaut", "de", "leder", "skin"),
                 _p("Textiloberfläche Sitz", "de", "stoff", "surface"),
                 _p("Nähbezug", "de", "sew", "bezug"),
                 _p("Sitzhaut", "de", "skin", "bezug"),
                 _p("upholstery fabric", "en", "upholster", "fabric")),
    },
    "foam_pad": {
        "dev": (_p("a moulded foam pad", "en", "foam", "mould"),
                _p("cushion foam", "en", "foam", "cushion-pad"),
                _p("PU foam padding", "en", "foam", "pu"),
                _p("seat padding", "en", "padding", "seat-pad"),
                _p("Formschaumteile", "de", "schaum", "form"),
                _p("Polsterschaum", "de", "schaum", "polster-pad"),
                _p("PU-Schaumpolster", "de", "schaum", "pu"),
                _p("Sitzpolsterung", "de", "polsterung", "sitz-pad"),
                _p("foam cushion pad", "en", "foam", "padding")),
        "test": (_p("polyurethane cushion core", "en", "pu", "core"),
                 _p("moulded padding block", "en", "mould", "padding"),
                 _p("resilient seat filling", "en", "filling", "padding"),
                 _p("cold-cure foam part", "en", "foam", "cure"),
                 _p("Kaltschaumformteil", "de", "schaum", "cure"),
                 _p("Polsterkern", "de", "core", "polster-pad"),
                 _p("Schaumformkörper", "de", "schaum", "form"),
                 _p("Sitzfüllung", "de", "filling", "sitz-pad"),
                 _p("foam core block", "en", "foam", "core")),
    },
    "seat_frame": {
        "dev": (_p("a seat frame", "en", "frame", "seat-frame"),
                _p("seat structure", "en", "structure", "seat-frame"),
                _p("backrest and cushion frames", "en", "frame", "backrest-frame"),
                _p("load-bearing seat structure", "en", "structure", "load"),
                _p("ein Sitzgestell", "de", "gestell", "sitz-frame"),
                _p("Sitzstruktur", "de", "struktur", "sitz-frame"),
                _p("Lehnen- und Kissenrahmen", "de", "rahmen", "lehne-frame"),
                _p("tragende Sitzstruktur", "de", "struktur", "load"),
                _p("seat frame structure", "en", "frame", "structure")),
        "test": (_p("pressed-steel carrier", "en", "carrier", "steel"),
                 _p("seat skeleton", "en", "skeleton", "structure"),
                 _p("welded tubular frame", "en", "frame", "tube"),
                 _p("cushion pan and back frame", "en", "frame", "pan"),
                 _p("Stahlträger Sitz", "de", "steel", "carrier"),
                 _p("Sitzskelett", "de", "skeleton", "struktur"),
                 _p("Rohrrahmen", "de", "rahmen", "tube"),
                 _p("Sitzwanne und Lehnenrahmen", "de", "pan", "rahmen"),
                 _p("structural seat carrier", "en", "structure", "carrier")),
    },
    "wiring": {
        "dev": (_p("a seat wiring harness", "en", "wiring", "harness"),
                _p("seat cabling and connectors", "en", "cable", "connector"),
                _p("electrical harness", "en", "harness", "electrical"),
                _p("connector set", "en", "connector", "set"),
                _p("einen Sitzkabelbaum", "de", "kabel", "baum"),
                _p("Kabelsatz Sitz", "de", "kabel", "satz"),
                _p("Steckverbinder", "de", "stecker", "verbinder"),
                _p("Sitzverkabelung", "de", "kabel", "verkabelung"),
                _p("wiring loom", "en", "wiring", "loom")),
        "test": (_p("cable assembly with plugs", "en", "cable", "plug"),
                 _p("loom and terminal set", "en", "loom", "terminal"),
                 _p("electrical distribution lines", "en", "electrical", "line"),
                 _p("plug-in harness", "en", "harness", "plug"),
                 _p("Leitungssatz", "de", "line", "satz"),
                 _p("Steckerleiste", "de", "stecker", "plug"),
                 _p("Kontaktsatz", "de", "terminal", "verbinder"),
                 _p("Bordnetz Sitz", "de", "verkabelung", "line"),
                 _p("harness with connectors", "en", "harness", "connector")),
    },
}
