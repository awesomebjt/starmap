"""
star_names.py - reusable helpers for turning catalog designations into
human-readable star names.

The main entry points are:

    bayer_to_full_name(bayer, con, superscript_style="caret") -> str | None
        'Eps', 'Eri'    -> 'Epsilon Eridani'
        'Omi-2', 'Eri'  -> 'Omicron^2 Eridani'   (caret, the default)
                        -> 'Omicron² Eridani'    (superscript_style="unicode")
                        -> 'Omicron2 Eridani'    (superscript_style="plain")
        'Gam', 'Pav'    -> 'Gamma Pavonis'

    parse_hyg_bf(bf) -> HygDesignation | None
        Splits HYG's combined Bayer/Flamsteed field, e.g. '18Eps Eri',
        '40Omi2Eri', 'Kap1Scl', '61    Cyg', into its parts.

    flamsteed_to_full_name(flam, con) -> str | None
        61, 'Cyg' -> '61 Cygni'

    bayer_abbrev(bayer, con) -> str | None
        'Eps', 'Eri' -> 'eps Eri'  (the short IAU/SIMBAD-style form)

Background for non-astronomers: a *Bayer designation* is a Greek letter plus
the Latin genitive ("of ...") of the constellation, e.g. Epsilon Eridani =
"epsilon of Eridanus". When several nearby stars share one letter they get a
superscript number (Omicron¹ and Omicron² Eridani). A *Flamsteed designation*
uses a number instead of a letter (61 Cygni). Catalogs abbreviate both the
letter ('Eps', 'eps', 'ε') and the constellation ('Eri').

All functions return None for input they cannot interpret; they never raise
on bad data (catalog data is messy and we would rather skip one name than
abort a whole build).
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Literal

SuperscriptStyle = Literal["caret", "unicode", "plain"]

# ---------------------------------------------------------------------------
# The 24 Greek letters, in alphabetical (Greek) order.
# (full English name, lowercase Greek symbol, IAU/SIMBAD 3-letter abbreviation)
# ---------------------------------------------------------------------------
GREEK_LETTERS: list[tuple[str, str, str]] = [
    ("Alpha", "α", "alf"),
    ("Beta", "β", "bet"),
    ("Gamma", "γ", "gam"),
    ("Delta", "δ", "del"),
    ("Epsilon", "ε", "eps"),
    ("Zeta", "ζ", "zet"),
    ("Eta", "η", "eta"),
    ("Theta", "θ", "tet"),
    ("Iota", "ι", "iot"),
    ("Kappa", "κ", "kap"),
    ("Lambda", "λ", "lam"),
    ("Mu", "μ", "mu"),
    ("Nu", "ν", "nu"),
    ("Xi", "ξ", "ksi"),
    ("Omicron", "ο", "omi"),
    ("Pi", "π", "pi"),
    ("Rho", "ρ", "rho"),
    ("Sigma", "σ", "sig"),
    ("Tau", "τ", "tau"),
    ("Upsilon", "υ", "ups"),
    ("Phi", "φ", "phi"),
    ("Chi", "χ", "chi"),
    ("Psi", "ψ", "psi"),
    ("Omega", "ω", "ome"),
]

# Every spelling we accept for a Greek letter, mapped (lowercased) to its index
# in GREEK_LETTERS. HYG uses 'Alp','Bet',...,'The','Omi','Ome'; SIMBAD/IAU use
# 'alf','tet','ksi'; some sources write the full name or the Greek symbol.
_GREEK_ALIASES: dict[str, int] = {}
_HYG_ABBREVS = [
    "Alp", "Bet", "Gam", "Del", "Eps", "Zet", "Eta", "The", "Iot", "Kap", "Lam", "Mu",
    "Nu", "Xi", "Omi", "Pi", "Rho", "Sig", "Tau", "Ups", "Phi", "Chi", "Psi", "Ome",
]
for _i, (_full, _sym, _iau) in enumerate(GREEK_LETTERS):
    for _alias in (_full, _sym, _iau, _HYG_ABBREVS[_i]):
        _GREEK_ALIASES[_alias.lower()] = _i
# A few extra variants seen in the wild.
for _alias, _idx in {"alph": 0, "thet": 7, "tht": 7, "ksi": 13, "ups": 19, "ϑ": 7, "ς": 17,
                     "ϕ": 20, "omic": 14, "omeg": 23, "lamb": 10, "lmb": 10}.items():
    _GREEK_ALIASES[_alias] = _idx

# ---------------------------------------------------------------------------
# The 88 IAU constellations: abbreviation -> (nominative, Latin genitive).
# The genitive is what appears in star names ("Epsilon *Eridani*").
# ---------------------------------------------------------------------------
CONSTELLATIONS: dict[str, tuple[str, str]] = {
    "And": ("Andromeda", "Andromedae"),
    "Ant": ("Antlia", "Antliae"),
    "Aps": ("Apus", "Apodis"),
    "Aqr": ("Aquarius", "Aquarii"),
    "Aql": ("Aquila", "Aquilae"),
    "Ara": ("Ara", "Arae"),
    "Ari": ("Aries", "Arietis"),
    "Aur": ("Auriga", "Aurigae"),
    "Boo": ("Boötes", "Boötis"),
    "Cae": ("Caelum", "Caeli"),
    "Cam": ("Camelopardalis", "Camelopardalis"),
    "Cnc": ("Cancer", "Cancri"),
    "CVn": ("Canes Venatici", "Canum Venaticorum"),
    "CMa": ("Canis Major", "Canis Majoris"),
    "CMi": ("Canis Minor", "Canis Minoris"),
    "Cap": ("Capricornus", "Capricorni"),
    "Car": ("Carina", "Carinae"),
    "Cas": ("Cassiopeia", "Cassiopeiae"),
    "Cen": ("Centaurus", "Centauri"),
    "Cep": ("Cepheus", "Cephei"),
    "Cet": ("Cetus", "Ceti"),
    "Cha": ("Chamaeleon", "Chamaeleontis"),
    "Cir": ("Circinus", "Circini"),
    "Col": ("Columba", "Columbae"),
    "Com": ("Coma Berenices", "Comae Berenices"),
    "CrA": ("Corona Australis", "Coronae Australis"),
    "CrB": ("Corona Borealis", "Coronae Borealis"),
    "Crv": ("Corvus", "Corvi"),
    "Crt": ("Crater", "Crateris"),
    "Cru": ("Crux", "Crucis"),
    "Cyg": ("Cygnus", "Cygni"),
    "Del": ("Delphinus", "Delphini"),
    "Dor": ("Dorado", "Doradus"),
    "Dra": ("Draco", "Draconis"),
    "Equ": ("Equuleus", "Equulei"),
    "Eri": ("Eridanus", "Eridani"),
    "For": ("Fornax", "Fornacis"),
    "Gem": ("Gemini", "Geminorum"),
    "Gru": ("Grus", "Gruis"),
    "Her": ("Hercules", "Herculis"),
    "Hor": ("Horologium", "Horologii"),
    "Hya": ("Hydra", "Hydrae"),
    "Hyi": ("Hydrus", "Hydri"),
    "Ind": ("Indus", "Indi"),
    "Lac": ("Lacerta", "Lacertae"),
    "Leo": ("Leo", "Leonis"),
    "LMi": ("Leo Minor", "Leonis Minoris"),
    "Lep": ("Lepus", "Leporis"),
    "Lib": ("Libra", "Librae"),
    "Lup": ("Lupus", "Lupi"),
    "Lyn": ("Lynx", "Lyncis"),
    "Lyr": ("Lyra", "Lyrae"),
    "Men": ("Mensa", "Mensae"),
    "Mic": ("Microscopium", "Microscopii"),
    "Mon": ("Monoceros", "Monocerotis"),
    "Mus": ("Musca", "Muscae"),
    "Nor": ("Norma", "Normae"),
    "Oct": ("Octans", "Octantis"),
    "Oph": ("Ophiuchus", "Ophiuchi"),
    "Ori": ("Orion", "Orionis"),
    "Pav": ("Pavo", "Pavonis"),
    "Peg": ("Pegasus", "Pegasi"),
    "Per": ("Perseus", "Persei"),
    "Phe": ("Phoenix", "Phoenicis"),
    "Pic": ("Pictor", "Pictoris"),
    "Psc": ("Pisces", "Piscium"),
    "PsA": ("Piscis Austrinus", "Piscis Austrini"),
    "Pup": ("Puppis", "Puppis"),
    "Pyx": ("Pyxis", "Pyxidis"),
    "Ret": ("Reticulum", "Reticuli"),
    "Sge": ("Sagitta", "Sagittae"),
    "Sgr": ("Sagittarius", "Sagittarii"),
    "Sco": ("Scorpius", "Scorpii"),
    "Scl": ("Sculptor", "Sculptoris"),
    "Sct": ("Scutum", "Scuti"),
    "Ser": ("Serpens", "Serpentis"),
    "Sex": ("Sextans", "Sextantis"),
    "Tau": ("Taurus", "Tauri"),
    "Tel": ("Telescopium", "Telescopii"),
    "Tri": ("Triangulum", "Trianguli"),
    "TrA": ("Triangulum Australe", "Trianguli Australis"),
    "Tuc": ("Tucana", "Tucanae"),
    "UMa": ("Ursa Major", "Ursae Majoris"),
    "UMi": ("Ursa Minor", "Ursae Minoris"),
    "Vel": ("Vela", "Velorum"),
    "Vir": ("Virgo", "Virginis"),
    "Vol": ("Volans", "Volantis"),
    "Vul": ("Vulpecula", "Vulpeculae"),
}
assert len(CONSTELLATIONS) == 88
assert len(GREEK_LETTERS) == 24

# Lowercased abbreviations are still unique, so we can match case-insensitively
# ('CMA', 'cma' and 'CMa' all mean Canis Major).
_CON_BY_LOWER: dict[str, str] = {k.lower(): k for k in CONSTELLATIONS}

_SUPERSCRIPT_DIGITS = str.maketrans("0123456789", "⁰¹²³⁴⁵⁶⁷⁸⁹")
_FROM_SUPERSCRIPT = str.maketrans("⁰¹²³⁴⁵⁶⁷⁸⁹", "0123456789")


def _clean(value: object) -> str | None:
    """Return a stripped string, or None for None/NaN/empty/non-strings."""
    if value is None:
        return None
    if isinstance(value, float):
        if value != value:  # NaN (pandas' missing value) is the only float != itself
            return None
        return str(int(value)) if value.is_integer() else str(value)
    s = str(value).strip()
    return s or None


def normalize_constellation(con: object) -> str | None:
    """'eri' / 'ERI' / 'Eri' -> 'Eri' (canonical IAU abbreviation), else None."""
    s = _clean(con)
    if s is None:
        return None
    return _CON_BY_LOWER.get(s.lower())


def constellation_genitive(con: object) -> str | None:
    """'Eri' -> 'Eridani', 'CVn' -> 'Canum Venaticorum'; None if unknown."""
    abbr = normalize_constellation(con)
    return CONSTELLATIONS[abbr][1] if abbr else None


def parse_bayer_letter(bayer: object) -> tuple[int, int | None] | None:
    """
    Split a Bayer letter token into (greek_index, superscript_or_None).

    Accepts 'Eps', 'eps', 'Epsilon', 'ε', 'Omi-2', 'Omi2', 'omi02', 'Alp-1',
    'Omicron^2', 'ο²'. Returns None if the letter isn't recognised.
    """
    s = _clean(bayer)
    if s is None:
        return None
    s = s.translate(_FROM_SUPERSCRIPT)
    m = re.fullmatch(r"([^\W\d_]+)\s*[-^_ ]?\s*(\d{1,2})?", s)
    if not m:
        return None
    idx = _GREEK_ALIASES.get(m.group(1).lower())
    if idx is None:
        return None
    sup = int(m.group(2)) if m.group(2) else None
    if sup == 0:  # 'omi00' isn't a thing; treat as malformed
        return None
    return idx, sup


def _format_superscript(sup: int | None, style: SuperscriptStyle) -> str:
    if sup is None:
        return ""
    if style == "caret":
        return f"^{sup}"
    if style == "unicode":
        return str(sup).translate(_SUPERSCRIPT_DIGITS)
    if style == "plain":
        return str(sup)
    raise ValueError(f"unknown superscript_style {style!r}")


def bayer_to_full_name(
    bayer: object, con: object, superscript_style: SuperscriptStyle = "caret"
) -> str | None:
    """
    Expand a Bayer designation to its full English/Latin form.

    >>> bayer_to_full_name('Eps', 'Eri')
    'Epsilon Eridani'
    >>> bayer_to_full_name('Omi-2', 'Eri')
    'Omicron^2 Eridani'
    >>> bayer_to_full_name('Omi-2', 'Eri', superscript_style='unicode')
    'Omicron² Eridani'
    >>> bayer_to_full_name('Omi-2', 'Eri', superscript_style='plain')
    'Omicron2 Eridani'
    >>> bayer_to_full_name('Gam', 'Pav')
    'Gamma Pavonis'
    >>> bayer_to_full_name('Alp-1', 'Cen')
    'Alpha^1 Centauri'
    >>> bayer_to_full_name('Foo', 'Eri') is None
    True
    """
    if superscript_style not in ("caret", "unicode", "plain"):
        return None
    parsed = parse_bayer_letter(bayer)
    genitive = constellation_genitive(con)
    if parsed is None or genitive is None:
        return None
    idx, sup = parsed
    return f"{GREEK_LETTERS[idx][0]}{_format_superscript(sup, superscript_style)} {genitive}"


def bayer_abbrev(bayer: object, con: object, greek_symbol: bool = False) -> str | None:
    """
    Short form: ('Eps','Eri') -> 'eps Eri'; ('Omi-2','Eri') -> 'omi2 Eri'.
    With greek_symbol=True: 'ε Eri', 'ο² Eri'.
    """
    parsed = parse_bayer_letter(bayer)
    abbr = normalize_constellation(con)
    if parsed is None or abbr is None:
        return None
    idx, sup = parsed
    if greek_symbol:
        return f"{GREEK_LETTERS[idx][1]}{_format_superscript(sup, 'unicode')} {abbr}"
    return f"{GREEK_LETTERS[idx][2]}{sup if sup else ''} {abbr}"


def flamsteed_to_full_name(flam: object, con: object) -> str | None:
    """
    >>> flamsteed_to_full_name(61, 'Cyg')
    '61 Cygni'
    >>> flamsteed_to_full_name('18', 'Eri')
    '18 Eridani'
    """
    s = _clean(flam)
    genitive = constellation_genitive(con)
    if s is None or genitive is None:
        return None
    try:
        num = int(float(s))
    except ValueError:
        return None
    if num <= 0:
        return None
    return f"{num} {genitive}"


@dataclass(frozen=True)
class HygDesignation:
    """Parts of a HYG 'bf' field. Any part may be None."""

    flamsteed: int | None
    bayer: str | None          # letter token as written, e.g. 'Omi'
    superscript: int | None    # e.g. 2 for 'Omi2'
    con: str                   # canonical IAU abbreviation, e.g. 'Eri'

    @property
    def bayer_token(self) -> str | None:
        """Letter + superscript in HYG's 'bayer' column style ('Omi-2')."""
        if self.bayer is None:
            return None
        return f"{self.bayer}-{self.superscript}" if self.superscript else self.bayer


def parse_hyg_bf(bf: object) -> HygDesignation | None:
    """
    Parse HYG's combined Bayer/Flamsteed field.

    Format (from the HYG README): optional Flamsteed number, then optional
    3-letter Bayer abbreviation (2 letters for Mu/Nu/Xi/Pi), optional
    superscript digit, then the 3-letter constellation, with arbitrary spaces.

    >>> parse_hyg_bf('18Eps Eri')
    HygDesignation(flamsteed=18, bayer='Eps', superscript=None, con='Eri')
    >>> parse_hyg_bf('40Omi2Eri')
    HygDesignation(flamsteed=40, bayer='Omi', superscript=2, con='Eri')
    >>> parse_hyg_bf('Kap1Scl')
    HygDesignation(flamsteed=None, bayer='Kap', superscript=1, con='Scl')
    >>> parse_hyg_bf('61    Cyg')
    HygDesignation(flamsteed=61, bayer=None, superscript=None, con='Cyg')
    >>> parse_hyg_bf('') is None
    True
    """
    s = _clean(bf)
    if s is None:
        return None
    s = re.sub(r"\s+", "", s)
    if len(s) < 4:
        return None
    con = normalize_constellation(s[-3:])
    if con is None:
        return None
    m = re.fullmatch(r"(\d*)([A-Za-z]*)(\d?)", s[:-3])
    if not m:
        return None
    flam = int(m.group(1)) if m.group(1) else None
    letter = m.group(2) or None
    sup = int(m.group(3)) if m.group(3) else None
    if letter is not None and letter.lower() not in _GREEK_ALIASES:
        return None
    if letter is None and sup is not None:
        return None
    if flam is None and letter is None:
        return None
    return HygDesignation(flam, letter, sup, con)


def hyg_bayer_full_name(
    bayer: object, con: object, bf: object = None, superscript_style: SuperscriptStyle = "caret"
) -> str | None:
    """
    Convenience wrapper for HYG rows: use the separate 'bayer'/'con' columns,
    falling back to parsing 'bf' when those are blank.
    """
    name = bayer_to_full_name(bayer, con, superscript_style)
    if name is not None:
        return name
    parsed = parse_hyg_bf(bf)
    if parsed is None or parsed.bayer is None:
        return None
    return bayer_to_full_name(parsed.bayer_token, parsed.con, superscript_style)


if __name__ == "__main__":  # pragma: no cover
    import doctest

    doctest.testmod(verbose=False)
    print("doctests done")
