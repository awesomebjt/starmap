#!/usr/bin/env python3
"""
build_star_catalog.py - compile a SQLite database of every star within
~200 light years of the Sun from several public astronomy catalogs.

    python build_star_catalog.py                     # fetch (cached) + compile -> stars.db
    python build_star_catalog.py --refresh           # ignore the cache, re-download everything
    python build_star_catalog.py --compile-only      # rebuild compiled tables from raw_* tables
    python build_star_catalog.py --skip hypatia      # skip an optional source

Pipeline
--------
1. FETCH  - one function per source; raw downloads are cached under --cache-dir.
            Each source is loaded (almost untouched) into its own raw_* table.
2. COMPILE - cross-match the raw tables into `stars` (one row per physical
            star), `star_names` (many names per star) and `planets`.

Astronomy crib sheet (for the C++ programmer reading this)
---------------------------------------------------------
* Parallax p is the tiny apparent wobble of a star as Earth orbits the Sun.
  Distance in parsecs = 1000 / p[milliarcseconds]. 1 pc = 3.26156 light years.
  200 ly = 61.32 pc  <=>  p >= 16.31 mas.
* parallax_over_error (p / sigma_p) is a signal-to-noise ratio. Low values mean
  the distance is unreliable (1/p is a biased estimator), so we cut on it.
* RA/Dec are sky coordinates on the equatorial system (Earth's equator
  projected onto the sky). Galactic coordinates (l, b) are the same sky but
  with the Milky Way's plane as the equator; l=0 points at the galactic centre.
  We compute Cartesian x/y/z in parsecs in both frames, Sun at the origin.
"""
from __future__ import annotations

import argparse
import gzip
import io
import json
import logging
import math
import re
import socket
import sqlite3
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Iterable

import numpy as np
import pandas as pd
import requests

sys.path.insert(0, str(Path(__file__).resolve().parent))
from star_names import (  # noqa: E402  (local module, see star_names.py)
    bayer_abbrev,
    bayer_to_full_name,
    flamsteed_to_full_name,
    normalize_constellation,
    parse_bayer_letter,
    parse_hyg_bf,
)

log = logging.getLogger("starcatalog")

LY_PER_PC = 3.26156
USER_AGENT = "starcatalog-builder/1.0 (hobby 3D star map; python-requests)"

# --- source endpoints --------------------------------------------------------
HYG_URLS = [
    # HYG moved to Codeberg; files there are Git-LFS, so use the /media/ URL.
    "https://codeberg.org/astronexus/hyg/media/branch/main/data/hyg/CURRENT/hyg_v44.csv.gz",
    # Older mirror on GitHub (v4.1) as a fallback.
    "https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/CURRENT/hygdata_v41.csv",
]
EXO_TAP_SYNC = "https://exoplanetarchive.ipac.caltech.edu/TAP/sync"
GAVO_TAP = "https://dc.g-vo.org/tap"
HYPATIA_API = "https://hypatiacatalog.com/hypatia/api/v2"
IAU_EXOPLA_URL = "https://exopla.net/star-names/modern-iau-star-names/"
IAU_CSN_URL = "https://www.pas.rochester.edu/~emamajek/WGSN/IAU-CSN.txt"

ALL_SOURCES = ["gaia", "hyg", "exoplanets", "hypatia", "iau"]
RAW_TABLES = {"gaia": ["raw_gaia", "raw_gaia_hip_xmatch"], "hyg": ["raw_hyg"], "exoplanets": ["raw_exoplanets"],
              "hypatia": ["raw_hypatia"], "iau": ["raw_iau_names"]}
REQUIRED_SOURCES = {"gaia", "hyg"}


@dataclass
class Config:
    db_path: Path
    cache_dir: Path
    refresh: bool
    max_dist_ly: float
    min_parallax_snr: float
    skip: set[str]
    compile_only: bool
    gaia_mode: str
    gaia_seed: str
    gaia_chunk: int
    gaia_workers: int
    exo_margin_pc: float
    net_timeout: float = 180.0

    @property
    def max_dist_pc(self) -> float:
        return self.max_dist_ly / LY_PER_PC

    @property
    def min_parallax_mas(self) -> float:
        # distance[pc] = 1000 / parallax[mas]  =>  parallax >= 1000 / max_distance
        return 1000.0 / self.max_dist_pc


# =============================================================================
# Small helpers
# =============================================================================
def http_get(url: str, *, params: Any = None, timeout: float = 120, retries: int = 3) -> requests.Response:
    """GET with a few retries and linear back-off."""
    last: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            r = requests.get(url, params=params, timeout=timeout, headers={"User-Agent": USER_AGENT})
            r.raise_for_status()
            return r
        except Exception as exc:  # noqa: BLE001 - network errors come in many types
            last = exc
            log.warning("GET %s failed (attempt %d/%d): %s", url, attempt, retries, exc)
            time.sleep(2 * attempt)
    assert last is not None
    raise last


def cached_download(cfg: Config, name: str, fetch: Callable[[], bytes]) -> bytes:
    """Return cached bytes for `name`, calling `fetch()` only if missing or --refresh."""
    path = cfg.cache_dir / name
    if path.exists() and not cfg.refresh:
        log.info("  using cached %s (%.1f MB)", path, path.stat().st_size / 1e6)
        return path.read_bytes()
    data = fetch()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".part")
    tmp.write_bytes(data)
    tmp.replace(path)
    log.info("  cached %s (%.1f MB)", path, len(data) / 1e6)
    return data


def df_to_sql(conn: sqlite3.Connection, df: pd.DataFrame, table: str) -> None:
    """Replace `table` with the contents of `df` (NaN -> NULL)."""
    df = df.copy()
    for col in df.columns:
        if df[col].dtype == object or str(df[col].dtype) in ("str", "string"):
            df[col] = df[col].astype(object).where(df[col].notna(), None)
    df.to_sql(table, conn, if_exists="replace", index=False, chunksize=5000)
    log.info("  wrote %s: %d rows, %d columns", table, len(df), len(df.columns))


def as_int(v: Any) -> int | None:
    """Coerce catalog IDs like 16537, 16537.0, '16537', 'HIP 16537' to int (else None)."""
    if v is None or v is pd.NA:
        return None
    if isinstance(v, (bool, np.bool_)):
        return None
    if isinstance(v, (int, np.integer)):
        return int(v)
    if isinstance(v, (float, np.floating)):
        return None if not math.isfinite(v) else int(v)
    m = re.search(r"(\d+)\s*$", str(v).strip())
    return int(m.group(1)) if m else None


def none_if_nan(v: Any) -> Any:
    """Normalise pandas/numpy missing values and scalar types to plain Python."""
    if v is None or v is pd.NA or v is pd.NaT:
        return None
    if isinstance(v, (float, np.floating)):
        return float(v) if math.isfinite(float(v)) else None
    if isinstance(v, np.integer):
        return int(v)
    if isinstance(v, str) and not v.strip():
        return None
    return v


def chunks(seq: list[Any], n: int) -> Iterable[list[Any]]:
    for i in range(0, len(seq), n):
        yield seq[i : i + n]


# =============================================================================
# SOURCE 1: HYG database (Hipparcos + Yale Bright Star + Gliese, merged)
# =============================================================================
def fetch_hyg(cfg: Config) -> pd.DataFrame:
    """Download HYG (current version) and keep stars within max distance + Sol."""

    def _fetch() -> bytes:
        for url in HYG_URLS:
            try:
                log.info("  downloading %s", url)
                data = http_get(url, timeout=300).content
                if url.endswith(".csv"):  # store everything gzipped for consistency
                    data = gzip.compress(data)
                if len(data) < 1_000_000:  # a Git-LFS pointer file is ~130 bytes
                    raise ValueError(f"suspiciously small download ({len(data)} bytes)")
                return data
            except Exception as exc:  # noqa: BLE001
                log.warning("  HYG source %s failed: %s", url, exc)
        raise RuntimeError("all HYG URLs failed")

    raw = cached_download(cfg, "hyg.csv.gz", _fetch)
    df = pd.read_csv(io.BytesIO(raw), compression="gzip", low_memory=False)
    # HYG 'dist' is in parsecs; >= 100000 is a sentinel for "unknown/bad parallax".
    keep = (df["dist"] > 0) & (df["dist"] < 100000) & (df["dist"] <= cfg.max_dist_pc)
    keep |= df["id"] == 0  # Sol has dist 0
    out = df[keep].reset_index(drop=True)
    log.info("  HYG: %d rows total, %d within %.2f pc (incl. Sol)", len(df), len(out), cfg.max_dist_pc)
    return out


# =============================================================================
# SOURCE 2: NASA Exoplanet Archive, Planetary Systems Composite table
# =============================================================================
EXO_COLUMNS = [
    "pl_name", "hostname", "gaia_dr3_id", "gaia_dr2_id", "hip_name", "hd_name", "tic_id",
    "sy_dist", "sy_plx", "sy_pnum", "sy_snum", "sy_vmag", "sy_gaiamag",
    "st_spectype", "st_teff", "st_rad", "st_mass", "st_age", "st_met", "st_metratio", "st_lum", "st_logg",
    "ra", "dec", "disc_year", "discoverymethod", "pl_orbper", "pl_orbsmax", "pl_orbeccen",
    "pl_rade", "pl_bmasse", "pl_eqt",
]


def fetch_exoplanets(cfg: Config) -> pd.DataFrame:
    limit = cfg.max_dist_pc + cfg.exo_margin_pc
    query = f"select {', '.join(EXO_COLUMNS)} from pscomppars where sy_dist <= {limit:.3f}"

    def _fetch() -> bytes:
        log.info("  querying NASA Exoplanet Archive TAP (sy_dist <= %.2f pc)", limit)
        return http_get(EXO_TAP_SYNC, params={"query": query, "format": "csv"}, timeout=300).content

    raw = cached_download(cfg, f"exoplanets_pscomppars_{limit:.1f}pc.csv", _fetch)
    df = pd.read_csv(io.BytesIO(raw))
    # "Gaia DR3 5164707970261890560" -> 5164707970261890560 (handy for joins)
    df["gaia_dr3_source_id"] = df["gaia_dr3_id"].map(as_int).astype("Int64")
    log.info("  exoplanets: %d planets around %d hosts", len(df), df["hostname"].nunique())
    return df


# =============================================================================
# SOURCE 3: Gaia DR3 (ESA archive) -- the backbone
# =============================================================================
GAIA_SOURCE_COLS = [
    "source_id", "ra", "dec", "l", "b", "parallax", "parallax_error", "parallax_over_error",
    "pmra", "pmdec", "radial_velocity", "radial_velocity_error", "phot_g_mean_mag",
    "phot_bp_mean_mag", "phot_rp_mean_mag", "bp_rp", "ruwe", "non_single_star",
    "phot_variable_flag", "ref_epoch",
]
GAIA_AP_COLS = [
    "teff_gspphot", "logg_gspphot", "mh_gspphot", "distance_gspphot", "radius_gspphot",
    "radius_flame", "mass_flame", "lum_flame", "age_flame", "evolstage_flame", "flags_flame",
    "teff_gspspec", "mh_gspspec", "teff_esphs", "spectraltype_esphs",
]
GAIA_HIP_COLS = ["original_ext_source_id", "angular_distance", "number_of_neighbours", "xm_flag"]


def _gaia_query(adql: str, mode: str) -> pd.DataFrame:
    """Run one ADQL query on the ESA Gaia archive via astroquery (sync or async job)."""
    from astroquery.gaia import GaiaClass

    gaia = GaiaClass(show_server_messages=False)  # one client per call: thread-safe
    job = gaia.launch_job_async(adql) if mode == "async" else gaia.launch_job(adql)
    df = job.get_results().to_pandas()  # masked values -> NaN
    df.columns = [c.lower() for c in df.columns]
    return df


def _retry(fn: Callable[[], pd.DataFrame], what: str, attempts: int = 4) -> pd.DataFrame:
    for a in range(1, attempts + 1):
        try:
            return fn()
        except Exception as exc:  # noqa: BLE001
            if a == attempts:
                raise
            log.warning("    %s failed (attempt %d/%d): %s", what, a, attempts, str(exc)[:200])
            time.sleep(5 * a)
    raise AssertionError("unreachable")


def _gaia_seed_ids(cfg: Config) -> pd.DataFrame:
    """
    Get the Gaia DR3 source_ids with parallax >= limit and parallax SNR >= cut.

    Why a separate "seed" step? `parallax` is not indexed in the ESA archive, so
    `WHERE parallax >= 16.31` scans all 1.8 billion rows. In testing (Sep 2026)
    that job ran for >5 min and died with HTTP 500, and ESA's async queue was
    stalling. GAVO's Heidelberg TAP mirror of Gaia DR3 (gaia.dr3lite) indexes
    parallax and answers in ~30 s. We take only the IDs from there and fetch all
    real columns (incl. astrophysical_parameters) from ESA by source_id, which
    *is* indexed. With --gaia-seed esa the selection runs directly on ESA.
    """
    snr, plx = cfg.min_parallax_snr, cfg.min_parallax_mas
    if cfg.gaia_seed == "gavo":
        import pyvo

        adql = f"SELECT source_id, parallax, parallax_error FROM gaia.dr3lite WHERE parallax >= {plx:.6f}"
        log.info("  seeding Gaia source_ids from GAVO TAP mirror (async): %s", adql)
        svc = pyvo.dal.TAPService(GAVO_TAP)
        job = svc.submit_job(adql, maxrec=2_000_000)
        job.run()
        job.wait(phases=["COMPLETED", "ERROR", "ABORTED"], timeout=1800)
        if job.phase != "COMPLETED":
            raise RuntimeError(f"GAVO job ended in phase {job.phase}")
        df = job.fetch_result().to_table().to_pandas()
        try:
            job.delete()
        except Exception:  # noqa: BLE001 - cleanup only
            pass
    else:
        adql = f"SELECT source_id, parallax, parallax_error FROM gaiadr3.gaia_source WHERE parallax >= {plx:.6f}"
        log.info("  seeding Gaia source_ids directly on ESA (slow full scan, async): %s", adql)
        df = _gaia_query(adql, "async")
    df.columns = [c.lower() for c in df.columns]
    df = df[(df["parallax"] / df["parallax_error"]) >= snr]
    log.info("  seed: %d sources with parallax >= %.3f mas and parallax/error >= %g", len(df), plx, snr)
    return df[["source_id"]].astype("int64")


def fetch_gaia(cfg: Config, extra_ids: set[int], hyg_hips: set[int]) -> tuple[pd.DataFrame, pd.DataFrame]:
    """
    Returns (raw_gaia, raw_gaia_hip_xmatch).

    raw_gaia = gaia_source LEFT JOIN astrophysical_parameters for (a) the
    parallax/SNR selection and (b) `extra_ids` (Gaia IDs referenced by HYG via
    Hipparcos or by the exoplanet archive), so the compile step can tell "Gaia
    says this star is beyond 200 ly" from "Gaia has no data". Column
    `in_selection` = 1 marks (a).
    """
    tag = f"plx{cfg.min_parallax_mas:.3f}_snr{cfg.min_parallax_snr:g}"
    gdir = cfg.cache_dir / f"gaia_{tag}"
    gdir.mkdir(parents=True, exist_ok=True)
    if cfg.refresh:
        for f in gdir.glob("*.csv.gz"):
            f.unlink()

    # --- step 1: which source_ids? ------------------------------------------
    seed_path = gdir / "seed_ids.csv.gz"
    if seed_path.exists():
        seed = pd.read_csv(seed_path)
        log.info("  using cached seed list %s (%d ids)", seed_path, len(seed))
    else:
        seed = _gaia_seed_ids(cfg)
        seed.to_csv(seed_path, index=False)
    selected = set(int(x) for x in seed["source_id"])

    # --- step 2: Hipparcos->Gaia best-neighbour rows for the HYG stars -------
    # gaiadr3.hipparcos2_best_neighbour is small (~100k rows), so filtering it on
    # original_ext_source_id (= HIP number) is cheap even without an index.
    xm_parts: list[pd.DataFrame] = []
    for i, part in enumerate(chunks(sorted(hyg_hips), cfg.gaia_chunk)):
        p = gdir / f"hipxm_{i:04d}_{part[0]}_{len(part)}.csv.gz"
        if p.exists():
            xm_parts.append(pd.read_csv(p))
            continue
        adql = (
            f"SELECT source_id, {', '.join(GAIA_HIP_COLS)} FROM gaiadr3.hipparcos2_best_neighbour "
            f"WHERE original_ext_source_id IN ({','.join(map(str, part))})"
        )
        df = _retry(lambda: _gaia_query(adql, cfg.gaia_mode), f"hip xmatch chunk {i}")
        df.to_csv(p, index=False)
        xm_parts.append(df)
    xm_hyg = pd.concat(xm_parts, ignore_index=True) if xm_parts else pd.DataFrame(columns=["source_id"])
    log.info("  HIP->Gaia best-neighbour rows for HYG stars: %d", len(xm_hyg))

    all_ids = sorted(selected | set(extra_ids) | set(int(x) for x in xm_hyg["source_id"]))
    log.info("  fetching %d Gaia sources (%d selected + extras) in chunks of %d, %d workers, mode=%s",
             len(all_ids), len(selected), cfg.gaia_chunk, cfg.gaia_workers, cfg.gaia_mode)

    # --- step 3: all columns for every id, chunked (sync jobs cap at 2000 rows)
    cols = (
        [f"g.{c}" for c in GAIA_SOURCE_COLS]
        + [f"ap.{c}" for c in GAIA_AP_COLS]
        + [f"h.{c} AS hip_{c}" for c in GAIA_HIP_COLS]
    )

    def run_chunk(i: int, part: list[int]) -> pd.DataFrame:
        p = gdir / f"chunk_{i:04d}_{part[0]}_{len(part)}.csv.gz"
        if p.exists():
            return pd.read_csv(p)
        adql = (
            f"SELECT {', '.join(cols)} FROM gaiadr3.gaia_source AS g "
            "LEFT JOIN gaiadr3.astrophysical_parameters AS ap ON ap.source_id = g.source_id "
            "LEFT JOIN gaiadr3.hipparcos2_best_neighbour AS h ON h.source_id = g.source_id "
            f"WHERE g.source_id IN ({','.join(map(str, part))})"
        )
        df = _retry(lambda: _gaia_query(adql, cfg.gaia_mode), f"gaia chunk {i}")
        if cfg.gaia_mode == "sync" and len(df) >= 2000:
            raise RuntimeError("sync query hit the 2000-row cap; lower --gaia-chunk")
        df.to_csv(p, index=False)
        return df

    parts: list[pd.DataFrame] = []
    work = list(enumerate(chunks(all_ids, cfg.gaia_chunk)))
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=cfg.gaia_workers) as pool:
        futs = [pool.submit(run_chunk, i, part) for i, part in work]
        for n, fut in enumerate(as_completed(futs), 1):
            parts.append(fut.result())
            if n % 10 == 0 or n == len(work):
                log.info("    gaia chunks %d/%d done (%.0f s)", n, len(work), time.time() - t0)
    gaia = pd.concat(parts, ignore_index=True).drop_duplicates("source_id")
    gaia["in_selection"] = gaia["source_id"].isin(selected).astype(int)

    # raw_gaia_hip_xmatch = xmatch rows joined to our sources + rows found via HYG hips
    xm_sel = gaia.loc[gaia["hip_original_ext_source_id"].notna(),
                      ["source_id"] + [f"hip_{c}" for c in GAIA_HIP_COLS]]
    xm_sel.columns = ["source_id"] + GAIA_HIP_COLS
    xm = pd.concat([xm_sel, xm_hyg], ignore_index=True)
    xm["original_ext_source_id"] = xm["original_ext_source_id"].astype("int64")
    xm = xm.drop_duplicates(["source_id", "original_ext_source_id"])
    gaia = gaia.drop(columns=[f"hip_{c}" for c in GAIA_HIP_COLS])
    log.info("  Gaia: %d rows (%d in selection); HIP xmatch rows: %d",
             len(gaia), int(gaia["in_selection"].sum()), len(xm))
    return gaia.reset_index(drop=True), xm.reset_index(drop=True)


# =============================================================================
# SOURCE 4: Hypatia Catalog (stellar abundances; public REST API, no key)
# =============================================================================
def fetch_hypatia(cfg: Config) -> pd.DataFrame:
    """
    Two API calls (https://www.hypatiacatalog.com/api, v2.2, no key needed):
      1. /data?xaxis1=Fe&filter1_1=dist&... -> median [Fe/H] per star in range
         (Lodders et al. 2009 solar normalisation, Hypatia's default).
      2. /star?name=...  (batched) -> HIP/HD/Gaia identifiers + teff/logg/dist.
    """
    limit = cfg.max_dist_pc + 2.0

    def _fetch_fe() -> bytes:
        params = {"xaxis1": "Fe", "filter1_1": "dist", "filter1_3": 0, "filter1_4": f"{limit:.2f}",
                  "return_nea_name": "true"}
        return http_get(f"{HYPATIA_API}/data/", params=params, timeout=300).content

    fe = json.loads(cached_download(cfg, f"hypatia_fe_{limit:.1f}pc.json", _fetch_fe))
    values = fe.get("values", [])
    log.info("  Hypatia: %d stars with [Fe/H] (reported count: %s)", len(values), fe.get("counts", fe.get("count")))
    names = [v["name"] for v in values]

    def _fetch_stars() -> bytes:
        out: list[dict[str, Any]] = []
        nb = math.ceil(len(names) / 100)
        for i, part in enumerate(chunks(names, 100)):  # much longer URLs get HTTP 400
            out.extend(http_get(f"{HYPATIA_API}/star", params={"name": part}, timeout=300).json())
            if i % 10 == 0 or i == nb - 1:
                log.info("    hypatia star batch %d/%d", i + 1, nb)
        return json.dumps(out).encode()

    stars = json.loads(cached_download(cfg, f"hypatia_stars_{limit:.1f}pc.json", _fetch_stars))
    by_req = {s.get("requested_name"): s for s in stars}
    rows = []
    for v in values:
        s = by_req.get(v["name"], {})
        other = s.get("other_names") or []
        g3 = next((as_int(n) for n in other if n.startswith("Gaia DR3 ")), None)
        g2 = next((as_int(n) for n in other if n.startswith("Gaia DR2 ")), None)
        hd = s.get("hd")
        rows.append({
            "name": v["name"], "fe_h": v.get("xaxis"), "nea_name": v.get("nea_name"),
            "status": s.get("status"), "hip": as_int(s.get("hip")), "hd": hd,
            "hd_num": as_int(hd) if isinstance(hd, str) and re.fullmatch(r"HD\s+\d+", hd) else None,
            "gaia_dr3_source_id": g3, "gaia_dr2_source_id": g2,
            "spec": s.get("spec"), "vmag": s.get("vmag"), "bv": s.get("bv"), "dist": s.get("dist"),
            "ra": s.get("ra"), "dec": s.get("dec"), "teff": s.get("teff"), "logg": s.get("logg"),
            "disk": s.get("disk"), "n_planets": len(s.get("planets") or []),
        })
    df = pd.DataFrame(rows)
    for c in ("hip", "hd_num", "gaia_dr3_source_id", "gaia_dr2_source_id"):
        df[c] = df[c].astype("Int64")
    df = df[df["dist"].isna() | (df["dist"] <= limit)]
    log.info("  Hypatia: %d rows kept, %d with Gaia DR3 id, %d with HIP", len(df),
             int(df["gaia_dr3_source_id"].notna().sum()), int(df["hip"].notna().sum()))
    return df.reset_index(drop=True)


# =============================================================================
# SOURCE 5: IAU WGSN approved star names
# =============================================================================
def fetch_iau_names(cfg: Config) -> pd.DataFrame:
    """
    Primary: the WGSN table published on exopla.net (kept current as names are
    approved). Secondary: E. Mamajek's IAU-CSN.txt (last updated 2022, but it
    has multiple-star component letters, e.g. alf Cen A / B / C).
    """
    frames: list[pd.DataFrame] = []
    try:
        html = cached_download(cfg, "iau_exopla.html", lambda: http_get(IAU_EXOPLA_URL, timeout=120).content)
        frames.append(_parse_exopla(html.decode("utf-8", errors="replace")))
    except Exception as exc:  # noqa: BLE001
        log.warning("  exopla.net IAU table failed: %s", exc)
    try:
        txt = cached_download(cfg, "iau_csn.txt", lambda: http_get(IAU_CSN_URL, timeout=120).content)
        frames.append(_parse_csn(txt.decode("utf-8", errors="replace")))
    except Exception as exc:  # noqa: BLE001
        log.warning("  IAU-CSN.txt failed: %s", exc)
    if not frames:
        raise RuntimeError("no IAU name source worked")
    df = pd.concat(frames, ignore_index=True)
    df["hip"] = df["hip"].map(as_int).astype("Int64")
    df["hd"] = df["hd"].map(as_int).astype("Int64")
    log.info("  IAU names: %s", df.groupby("source").size().to_dict())
    return df


def _parse_exopla(html: str) -> pd.DataFrame:
    from bs4 import BeautifulSoup

    soup = BeautifulSoup(html, "lxml")
    table = soup.find("table", id="table_1")
    if table is None:
        raise ValueError("table_1 not found on exopla.net page (layout changed?)")
    hdr = [th.get_text(strip=True) for th in table.find("thead").find_all("th")]
    rows = [[td.get_text(strip=True) for td in tr.find_all("td")] for tr in table.find_all("tr")]
    df = pd.DataFrame([r for r in rows if len(r) == len(hdr)], columns=hdr)
    desig = df["Designation"].astype(str)
    out = pd.DataFrame({
        "name": df["proper names"],
        "designation": desig,
        "hip": df["HIP"],
        "hd": desig.where(desig.str.match(r"^HD\s*\d+$"), None),
        "bayer_id": df["Bayer ID"],
        "con": df["Constellation"],
        "component": None,
        "ra": pd.to_numeric(df["RA"], errors="coerce"),
        "dec": pd.to_numeric(df["DEC"], errors="coerce"),
        "mag": pd.to_numeric(df["mag"], errors="coerce"),
        "date_adopted": df["Date of Adoption"],
        "source": "exopla.net",
    })
    return out[out["name"].str.len() > 0]


def _parse_csn(txt: str) -> pd.DataFrame:
    rows = []
    for line in txt.splitlines():
        if not line.strip() or line.startswith(("#", "$")):
            continue
        # Fixed-width name/designation columns (they contain spaces), then
        # whitespace-separated fields.
        name, desig, rest = line[0:18].strip(), line[36:49].strip(), line[49:].split()
        if len(rest) < 12:
            continue
        idabbr, _idsym, con, comp, _wds, mag, _band, hip, hd, ra, dec, date = rest[:12]
        rows.append({
            "name": name, "designation": desig,
            "hip": None if hip == "_" else hip, "hd": None if hd in ("_", "999999") else hd,
            "bayer_id": None if idabbr == "_" else idabbr, "con": con,
            "component": None if comp == "_" else comp,
            "ra": float(ra), "dec": float(dec),
            "mag": pd.to_numeric(mag, errors="coerce"), "date_adopted": date, "source": "IAU-CSN",
        })
    return pd.DataFrame(rows)


# =============================================================================
# FETCH ORCHESTRATION
# =============================================================================
def fetch_all(cfg: Config, conn: sqlite3.Connection) -> dict[str, str]:
    status: dict[str, str] = {}

    def run(name: str, fn: Callable[[], Any]) -> Any:
        if name in cfg.skip:
            # --skip means "leave this source out of the build": drop stale raw tables too.
            for t in RAW_TABLES[name]:
                conn.execute(f"DROP TABLE IF EXISTS {t}")
            status[name] = "skipped (--skip)"
            log.info("[%s] skipped by --skip", name)
            return None
        log.info("[%s] fetching ...", name)
        t0 = time.time()
        try:
            res = fn()
            status[name] = f"ok ({time.time() - t0:.0f} s)"
            return res
        except Exception as exc:  # noqa: BLE001
            status[name] = f"FAILED: {exc}"
            if name in REQUIRED_SOURCES:
                raise
            stale = [t for t in RAW_TABLES[name]
                     if conn.execute("SELECT 1 FROM sqlite_master WHERE name=?", (t,)).fetchone()]
            log.error("[%s] optional source failed, continuing without it%s: %s", name,
                      f" (keeping raw tables from a previous run: {stale})" if stale else "", exc)
            return None

    hyg = run("hyg", lambda: fetch_hyg(cfg))
    if hyg is not None:
        df_to_sql(conn, hyg, "raw_hyg")
    exo = run("exoplanets", lambda: fetch_exoplanets(cfg))
    if exo is not None:
        df_to_sql(conn, exo, "raw_exoplanets")

    hyg_hips = set(int(h) for h in hyg["hip"].dropna()) if hyg is not None else set()
    extra = set(int(x) for x in exo["gaia_dr3_source_id"].dropna()) if exo is not None else set()
    res = run("gaia", lambda: fetch_gaia(cfg, extra, hyg_hips))
    if res is not None:
        df_to_sql(conn, res[0], "raw_gaia")
        df_to_sql(conn, res[1], "raw_gaia_hip_xmatch")

    hyp = run("hypatia", lambda: fetch_hypatia(cfg))
    if hyp is not None:
        df_to_sql(conn, hyp, "raw_hypatia")
    iau = run("iau", lambda: fetch_iau_names(cfg))
    if iau is not None:
        df_to_sql(conn, iau, "raw_iau_names")

    meta = pd.DataFrame([{"key": f"source_status.{k}", "value": v} for k, v in status.items()] + [
        {"key": "max_dist_ly", "value": str(cfg.max_dist_ly)},
        {"key": "min_parallax_mas", "value": f"{cfg.min_parallax_mas:.4f}"},
        {"key": "min_parallax_snr", "value": str(cfg.min_parallax_snr)},
        {"key": "fetched_at", "value": time.strftime("%Y-%m-%dT%H:%M:%S%z")},
    ])
    df_to_sql(conn, meta, "raw_meta")
    return status


# =============================================================================
# COMPILE: schema
# =============================================================================
SCHEMA = """
DROP TABLE IF EXISTS planets;
DROP TABLE IF EXISTS star_names;
DROP TABLE IF EXISTS stars;

CREATE TABLE stars (
    star_id              INTEGER PRIMARY KEY,
    display_name         TEXT NOT NULL,   -- chosen primary name (= star_names row with is_primary=1)
    bayer_name           TEXT,            -- e.g. 'Epsilon Eridani', 'Omicron^2 Eridani'; NULL if no Bayer letter
    gaia_source_id       INTEGER UNIQUE,  -- Gaia DR3 source_id
    hip                  INTEGER,         -- Hipparcos catalog number
    hd                   INTEGER,         -- Henry Draper catalog number
    hyg_id               INTEGER,         -- HYG database id
    -- Galactic Cartesian (parsecs, Sun at origin): +x -> galactic centre (l=0,b=0),
    -- +y -> l=90 deg (direction of Galactic rotation), +z -> north galactic pole.
    x                    REAL NOT NULL,
    y                    REAL NOT NULL,
    z                    REAL NOT NULL,
    -- Equatorial ICRS Cartesian (parsecs): +x -> RA 0h Dec 0, +y -> RA 6h Dec 0, +z -> north celestial pole.
    x_eq                 REAL NOT NULL,
    y_eq                 REAL NOT NULL,
    z_eq                 REAL NOT NULL,
    dist_pc              REAL NOT NULL,
    dist_ly              REAL NOT NULL,
    dist_source          TEXT NOT NULL,   -- 'gaia_parallax' | 'hyg' | 'exoplanet_archive' | 'sol'
    ra                   REAL,            -- degrees, ICRS (epoch J2016 for Gaia rows, ~J2000 otherwise)
    dec                  REAL,            -- degrees
    parallax_mas         REAL,            -- Gaia parallax (only when dist_source='gaia_parallax')
    parallax_error_mas   REAL,
    ruwe                 REAL,            -- Gaia astrometric quality; > 1.4 often = unresolved binary
    app_mag              REAL,            -- apparent magnitude: V if known, else Gaia G
    app_mag_band         TEXT,            -- 'V' | 'G'
    abs_mag              REAL,            -- absolute magnitude in app_mag_band
    vmag                 REAL,            -- Johnson V (HYG/Hipparcos, or exoplanet archive)
    gaia_g_mag           REAL,
    bp_rp                REAL,            -- Gaia BP-RP colour index
    ci_bv                REAL,            -- B-V colour index (HYG)
    spectral_type        TEXT,            -- e.g. 'G2V', 'K2V', 'M5.5 V'
    spectral_type_source TEXT,
    spectral_class       TEXT,            -- O B A F G K M, L T Y (brown dwarfs), D (white dwarf), C S W; NULL if unknown
    teff_k               REAL,
    teff_source          TEXT,
    radius_sol           REAL,
    radius_source        TEXT,
    mass_sol             REAL,
    mass_source          TEXT,
    luminosity_sol       REAL,
    luminosity_source    TEXT,
    age_gyr              REAL,
    age_source           TEXT,
    metallicity          REAL,            -- [Fe/H] or [M/H] in dex relative to the Sun (see metallicity_source)
    metallicity_source   TEXT,
    planet_count         INTEGER NOT NULL DEFAULT 0,  -- confirmed planets matched to this star
    system_planet_count  INTEGER,         -- sy_pnum from the exoplanet archive (whole system)
    is_sol               INTEGER NOT NULL DEFAULT 0,
    sources              TEXT NOT NULL    -- comma list of catalogs that contributed to this row
);

CREATE TABLE star_names (
    name_id    INTEGER PRIMARY KEY,
    star_id    INTEGER NOT NULL REFERENCES stars(star_id) ON DELETE CASCADE,
    name       TEXT NOT NULL,
    catalog    TEXT NOT NULL,   -- 'IAU','proper','bayer','bayer_unicode','bayer_plain','bayer_abbrev','bayer_greek',
                                -- 'bayer_component','flamsteed','gliese','HD','HIP','HR','exoplanet_host','Gaia DR3',
                                -- '*_alias' (bare shared designation kept on the primary component)
    is_primary INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE planets (
    planet_id       INTEGER PRIMARY KEY,
    star_id         INTEGER NOT NULL REFERENCES stars(star_id) ON DELETE CASCADE,
    pl_name         TEXT NOT NULL,
    hostname        TEXT,
    disc_year       INTEGER,
    discoverymethod TEXT,
    pl_orbper_days  REAL,
    pl_orbsmax_au   REAL,
    pl_orbeccen     REAL,
    pl_rade         REAL,   -- Earth radii
    pl_bmasse       REAL,   -- Earth masses (best mass: M or M sin i)
    pl_eqt_k        REAL
);
"""

INDEXES = """
CREATE INDEX idx_stars_hip ON stars(hip);
CREATE INDEX idx_stars_hd ON stars(hd);
CREATE INDEX idx_stars_hyg ON stars(hyg_id);
CREATE INDEX idx_stars_dist ON stars(dist_pc);
CREATE INDEX idx_stars_display_name ON stars(display_name);
CREATE INDEX idx_stars_spectral_class ON stars(spectral_class);
CREATE INDEX idx_star_names_star ON star_names(star_id);
CREATE INDEX idx_star_names_name ON star_names(name COLLATE NOCASE);
CREATE UNIQUE INDEX idx_star_names_primary ON star_names(star_id) WHERE is_primary = 1;
CREATE INDEX idx_planets_star ON planets(star_id);
"""

# Display-name priority (lower wins): proper > Bayer > Flamsteed > Gliese > HD > HIP > Gaia DR3.
NAME_PRIORITY = {
    "IAU": 0, "proper": 1, "bayer": 2, "bayer_component": 3, "flamsteed": 4, "gliese": 5,
    "HD": 6, "HIP": 7, "exoplanet_host": 8, "Gaia DR3": 9,
    # aliases: only used if nothing better exists
    "HR": 20, "bayer_unicode": 30, "bayer_plain": 31, "bayer_abbrev": 32, "bayer_greek": 33,
}


def spectral_class_of(sp: Any) -> str | None:
    """'G2V' -> 'G', 'sdM4' -> 'M', 'DA2' -> 'D', 'CSTAR' -> 'C', 'k A2 hA5' -> 'A'."""
    if not isinstance(sp, str) or not sp.strip():
        return None
    s = sp.strip()
    if s.upper() == "CSTAR":  # Gaia ESP-HS label for carbon stars
        return "C"
    if re.match(r"^(D[ABOQZCXG]|WD|D\d)", s):
        return "D"  # white dwarf
    # Gliese-catalog colour classes are lower case: 'm', 'k-m', 'g-k', 'm+', 'm3 V'
    if re.fullmatch(r"[obafgkm](-[obafgkm])?\+?|[obafgkm]\d.*", s):
        return s[0].upper()
    s = re.sub(r"^(esd|usd|sd|d|g|c|k|\(|\s)+", "", s)  # sub-dwarf / dwarf / giant prefixes
    m = re.match(r"([OBAFGKMLTYCSW])", s)
    return m.group(1) if m else None


def teff_from_bv(bv: float | None) -> float | None:
    """Ballesteros (2012) B-V -> Teff estimate; only for -0.4 < B-V < 2.0. Labelled as an estimate."""
    if bv is None or not (-0.4 < bv < 2.0):
        return None
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


@dataclass
class Star:
    """Working record for one physical star while compiling."""

    ra: float | None = None       # degrees
    dec: float | None = None
    dist_pc: float | None = None
    dist_source: str = ""
    gaia: dict[str, Any] | None = None
    hyg: dict[str, Any] | None = None
    exo: list[dict[str, Any]] = field(default_factory=list)
    hyp: dict[str, Any] | None = None
    iau: list[dict[str, Any]] = field(default_factory=list)
    gaia_source_id: int | None = None
    hip: int | None = None
    hd: int | None = None
    is_sol: bool = False


def _read(conn: sqlite3.Connection, table: str) -> list[dict[str, Any]]:
    """
    Read a raw table as a list of dicts.

    Deliberately *not* via pandas: a pandas integer column containing NULLs
    becomes float64, and float64 cannot hold 19-digit Gaia source_ids exactly
    (2441630500517080064 would silently become ...079808).
    """
    if not conn.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)).fetchone():
        log.warning("  table %s missing -> treated as empty", table)
        return []
    cur = conn.execute(f"SELECT * FROM {table}")
    cols = [d[0] for d in cur.description]
    return [{c: none_if_nan(v) for c, v in zip(cols, row)} for row in cur]


def gliese_key(name: Any) -> str | None:
    """'Gl 667C' / 'GJ 667 C' / 'Gliese 667 C' -> 'GJ667C' (for name matching)."""
    if not isinstance(name, str):
        return None
    m = re.fullmatch(r"\s*(?:Gl|GJ|Gliese)\s*(\d+(?:\.\d+)?)\s*([A-Za-z]?)\s*", name)
    return f"GJ{m.group(1)}{m.group(2).upper()}" if m else None


class PositionIndex:
    """
    Minimal positional index for cone searches of a few arcseconds.

    Gaia positions are at epoch J2016.0 while HYG/IAU/exoplanet positions are
    ~J2000. Nearby stars move fast (Barnard's Star ~10"/yr), so Gaia rows are
    wound back 16 years with their proper motion before matching (pmra from Gaia
    already includes the cos(dec) factor, hence the division).
    """

    def __init__(self, stars: list[Star]):
        n = len(stars)
        self.ra = np.full(n, np.nan)
        self.dec = np.full(n, np.nan)
        self.ra16 = np.full(n, np.nan)   # positions as stored (J2016 for Gaia rows)
        self.dec16 = np.full(n, np.nan)
        for i, s in enumerate(stars):
            if s.ra is None or s.dec is None or s.is_sol:
                continue
            ra, dec = s.ra, s.dec
            self.ra16[i], self.dec16[i] = ra, dec
            g = s.gaia
            if g and s.dist_source == "gaia_parallax" and g.get("pmra") is not None and g.get("pmdec") is not None:
                dt = (g.get("ref_epoch") or 2016.0) - 2000.0
                ra -= g["pmra"] * dt / 3.6e6 / max(math.cos(math.radians(dec)), 1e-6)
                dec -= g["pmdec"] * dt / 3.6e6
            self.ra[i], self.dec[i] = ra, dec
        key = np.nan_to_num(self.dec, nan=999.0)
        self.order = np.argsort(key)
        self.sorted_dec = key[self.order]
        key16 = np.nan_to_num(self.dec16, nan=999.0)
        self.order16 = np.argsort(key16)
        self.sorted_dec16 = key16[self.order16]

    def query(self, ra: float, dec: float, radius_arcsec: float, both_epochs: bool = False) -> list[tuple[int, float]]:
        """
        Indices (into the star list) within radius, nearest first, with separation in arcsec.
        both_epochs=True also tries the un-rewound (J2016) Gaia positions, for sources such
        as the exoplanet archive whose coordinates are often copied from Gaia itself.
        """
        hits = self._query(self.ra, self.dec, self.order, self.sorted_dec, ra, dec, radius_arcsec)
        if both_epochs:
            hits += self._query(self.ra16, self.dec16, self.order16, self.sorted_dec16, ra, dec, radius_arcsec)
        best: dict[int, float] = {}
        for i, sep in hits:
            best[i] = min(sep, best.get(i, sep))
        return sorted(best.items(), key=lambda t: t[1])

    @staticmethod
    def _query(ra_arr: np.ndarray, dec_arr: np.ndarray, order: np.ndarray, sorted_dec: np.ndarray,
               ra: float, dec: float, radius_arcsec: float) -> list[tuple[int, float]]:
        self_ra, self_dec = ra_arr, dec_arr
        r = radius_arcsec / 3600.0
        lo, hi = np.searchsorted(sorted_dec, [dec - r, dec + r])
        idx = order[lo:hi]
        if len(idx) == 0:
            return []
        d0, r0 = math.radians(dec), math.radians(ra)
        dd, rr = np.radians(self_dec[idx]), np.radians(self_ra[idx])
        # haversine formula for angular separation
        h = np.sin((dd - d0) / 2) ** 2 + math.cos(d0) * np.cos(dd) * np.sin((rr - r0) / 2) ** 2
        sep = 2 * np.degrees(np.arcsin(np.sqrt(np.clip(h, 0, 1)))) * 3600.0
        hits = [(int(i), float(s)) for i, s in zip(idx, sep) if s <= radius_arcsec]
        return sorted(hits, key=lambda t: t[1])


# =============================================================================
# COMPILE: cross-matching
# =============================================================================
def compile_catalog(cfg: Config, conn: sqlite3.Connection) -> None:
    log.info("[compile] reading raw tables")
    gaia_rows = _read(conn, "raw_gaia")
    xm_rows = _read(conn, "raw_gaia_hip_xmatch")
    hyg_rows = _read(conn, "raw_hyg")
    exo_rows = _read(conn, "raw_exoplanets")
    hyp_rows = _read(conn, "raw_hypatia")
    iau_rows = _read(conn, "raw_iau_names")
    max_pc, min_plx, snr_cut = cfg.max_dist_pc, cfg.min_parallax_mas, cfg.min_parallax_snr

    # stars[0] is Sol, so star_id == list index + 1 throughout.
    sol_hyg = next((h for h in hyg_rows if h["id"] == 0), None)
    stars: list[Star] = [Star(ra=0.0, dec=0.0, dist_pc=0.0, dist_source="sol", hyg=sol_hyg, is_sol=True)]
    by_gaia: dict[int, int] = {}                # gaia source_id -> index in `stars`
    gaia_all: dict[int, dict[str, Any]] = {}    # every Gaia row we have (selected or not)

    # ---- 1. Gaia backbone: one star per selected Gaia source ---------------
    for g in gaia_rows:
        sid = int(g["source_id"])
        gaia_all[sid] = g
        plx = g.get("parallax")
        if not g.get("in_selection") or plx is None or plx < min_plx or (g.get("parallax_over_error") or 0) < snr_cut:
            continue
        by_gaia[sid] = len(stars)
        stars.append(Star(ra=g["ra"], dec=g["dec"], dist_pc=1000.0 / plx, dist_source="gaia_parallax",
                          gaia=g, gaia_source_id=sid))
    n_gaia = len(stars) - 1
    log.info("  %d stars from Gaia DR3", n_gaia)

    hip2sid = {int(r["original_ext_source_id"]): int(r["source_id"]) for r in xm_rows}

    def gaia_says_too_far(sid: int | None) -> bool:
        """True if Gaia has a *good* parallax placing this source beyond the limit."""
        g = gaia_all.get(sid) if sid is not None else None
        return bool(g and g.get("parallax") is not None and g["parallax"] < min_plx
                    and (g.get("parallax_over_error") or 0) >= snr_cut)

    def attach_hyg(st: Star, h: dict[str, Any]) -> None:
        st.hyg, st.hip, st.hd = h, as_int(h.get("hip")), as_int(h.get("hd"))

    # ---- 2. HYG: attach to Gaia via HIP, else by position, else standalone ----
    orphans: list[dict[str, Any]] = []
    n_hip = n_pos = n_drop = 0
    for h in hyg_rows:
        if h["id"] == 0:
            continue
        hip = as_int(h.get("hip"))
        sid = hip2sid.get(hip) if hip else None
        if sid is not None and sid in by_gaia and stars[by_gaia[sid]].hyg is None:
            attach_hyg(stars[by_gaia[sid]], h)
            n_hip += 1
        elif gaia_says_too_far(sid):
            n_drop += 1  # Gaia's (better) parallax puts it beyond the limit
        else:
            h["_gaia_sid"] = sid
            orphans.append(h)

    # Positional fallback. Two kinds of orphans end up here:
    #  * Gliese-catalog stars with no HIP number (positions often good only to
    #    ~10-30", and photometric distances that can be off by 2x);
    #  * HIP stars whose Gaia "best neighbour" is a component without a parallax
    #    (common in close binaries) while the other component, with a parallax,
    #    sits < 1" away and is already in our Gaia list.
    # Only the ~90k Gaia stars within the distance limit are candidates (about
    # 2 per square degree), so a chance alignment within 60" is ~0.5% likely.
    pidx = PositionIndex(stars)
    standalone: list[dict[str, Any]] = []
    for h in orphans:
        best = None
        if h.get("ra") is not None:
            has_hip = as_int(h.get("hip")) is not None
            radius, (rlo, rhi) = (20.0, (0.6, 1.6)) if has_hip else (60.0, (0.4, 3.0))
            # HYG RA is in hours. HYG positions are nominally J2000, but some rows
            # agree better with Gaia's J2016 positions, so try both epochs.
            for k, _sep in pidx.query(h["ra"] * 15.0, h["dec"], radius, both_epochs=True):
                st = stars[k]
                if st.hyg is not None or st.dist_source != "gaia_parallax":
                    continue
                gmag, vmag = (st.gaia or {}).get("phot_g_mean_mag"), h.get("mag")
                # G is never much fainter than V; red dwarfs can be ~3 mag brighter in G.
                ok_mag = gmag is None or vmag is None or -3.5 <= gmag - vmag <= 1.0
                ok_dist = bool(h.get("dist")) and rlo <= st.dist_pc / h["dist"] <= rhi
                if ok_mag and ok_dist:
                    best = k
                    break
        if best is not None:
            attach_hyg(stars[best], h)
            n_pos += 1
        else:
            standalone.append(h)
    for h in standalone:
        # Standalone HYG star (Sirius, Vega, ... are too bright for Gaia DR3 astrometry)
        sid = h.get("_gaia_sid")
        st = Star(ra=h["ra"] * 15.0, dec=h["dec"], dist_pc=h["dist"], dist_source="hyg")
        attach_hyg(st, h)
        if sid is not None and sid not in by_gaia:
            st.gaia_source_id, st.gaia = sid, gaia_all.get(sid)
            by_gaia[sid] = len(stars)
        stars.append(st)
    log.info("  HYG: %d matched via HIP, %d by position, %d standalone, %d dropped (Gaia: beyond limit)",
             n_hip, n_pos, len(standalone), n_drop)

    def lookups() -> tuple[dict[int, int], dict[int, int]]:
        return ({s.hip: i for i, s in enumerate(stars) if s.hip},
                {s.hd: i for i, s in enumerate(stars) if s.hd})

    by_hip, by_hd = lookups()

    # ---- 3. exoplanet hosts: Gaia id > HIP > HD > position > new star -------
    hosts: dict[str, list[dict[str, Any]]] = {}
    for p in exo_rows:
        hosts.setdefault(p["hostname"], []).append(p)
    pidx = PositionIndex(stars)
    how = {"gaia": 0, "hip": 0, "hd": 0, "gliese_name": 0, "position": 0, "new": 0, "dropped": 0}
    by_gl: dict[str, int] = {}
    for i, s in enumerate(stars):
        key = gliese_key((s.hyg or {}).get("gl"))
        if key:
            by_gl.setdefault(key, i)
    for hostname, planets in hosts.items():
        p0 = planets[0]
        sid = as_int(p0.get("gaia_dr3_source_id"))
        hip = as_int(p0.get("hip_name"))
        # 'HD 41004 B' is the B component, not HD 41004 (A); don't HD-match those
        hd = as_int(p0.get("hd_name")) if re.fullmatch(r"HD \d+", str(p0.get("hd_name") or "")) else None
        k, m = None, ""
        if sid is not None and sid in by_gaia:
            k, m = by_gaia[sid], "gaia"
        elif hip is not None and hip in by_hip and not re.search(r" [B-E]$", hostname):
            k, m = by_hip[hip], "hip"
        elif hd is not None and hd in by_hd and not re.search(r" [B-E]$", hostname):
            k, m = by_hd[hd], "hd"
        elif gliese_key(hostname) in by_gl:
            k, m = by_gl[gliese_key(hostname)], "gliese_name"
        elif p0.get("ra") is not None:
            for kk, _sep in pidx.query(p0["ra"], p0["dec"], 5.0, both_epochs=True):
                d = stars[kk].dist_pc
                if d and p0.get("sy_dist") and 0.7 <= d / p0["sy_dist"] <= 1.4:
                    k, m = kk, "position"
                    break
        if k is None:
            if gaia_says_too_far(sid) or (p0.get("sy_dist") or 1e9) > max_pc:
                how["dropped"] += 1
                continue
            # A host like 'GJ 667 C' is a companion: the HIP/HD number belongs to the primary.
            component_host = re.search(r" [B-E]$", hostname) is not None
            st = Star(ra=p0["ra"], dec=p0["dec"], dist_pc=p0["sy_dist"], dist_source="exoplanet_archive",
                      hip=None if component_host or hip in by_hip else hip,
                      hd=None if component_host or hd in by_hd else hd)
            if sid is not None:
                st.gaia_source_id, st.gaia = sid, gaia_all.get(sid)
                by_gaia[sid] = len(stars)
            stars.append(st)
            k, m = len(stars) - 1, "new"
        how[m] += 1
        stars[k].exo.extend(planets)
    log.info("  exoplanet hosts: %s", how)
    by_hip, by_hd = lookups()

    # ---- 4. Hypatia [Fe/H]: Gaia id > HIP > HD ------------------------------
    n_hyp = 0
    for r in hyp_rows:
        sid, hip, hd = as_int(r.get("gaia_dr3_source_id")), as_int(r.get("hip")), as_int(r.get("hd_num"))
        k = by_gaia.get(sid) if sid is not None else None
        if k is None and hip is not None:
            k = by_hip.get(hip)
        if k is None and hd is not None:
            k = by_hd.get(hd)
        if k is not None and stars[k].hyp is None and r.get("fe_h") is not None:
            stars[k].hyp = r
            n_hyp += 1
    log.info("  Hypatia [Fe/H] matched to %d stars", n_hyp)

    # ---- 5. IAU names: HIP > HD > position (+ magnitude sanity check) -------
    # exopla.net and IAU-CSN list most names twice; group by name so that one
    # name lands on exactly one star (HIP from either source wins).
    by_name: dict[str, list[dict[str, Any]]] = {}
    for r in iau_rows:
        by_name.setdefault(str(r["name"]).strip(), []).append(r)
    pidx = PositionIndex(stars)
    n_iau = 0
    for name, rows in by_name.items():
        hip = next((as_int(r["hip"]) for r in rows if as_int(r.get("hip"))), None)
        hd = next((as_int(r["hd"]) for r in rows if as_int(r.get("hd"))), None)
        k = by_hip.get(hip) if hip is not None else None
        if k is None and hd is not None:
            k = by_hd.get(hd)
        r0 = next((r for r in rows if r.get("ra") is not None and r.get("dec") is not None), None)
        if k is None and hip is None and r0 is not None:
            for kk, _sep in pidx.query(r0["ra"], r0["dec"], 10.0, both_epochs=True):
                s = stars[kk]
                mag = (s.hyg or {}).get("mag") or (s.gaia or {}).get("phot_g_mean_mag")
                if mag is None or r0.get("mag") is None or abs(mag - r0["mag"]) < 2.0:
                    k = kk
                    break
        if k is not None:
            stars[k].iau.extend(rows)
            n_iau += 1
    log.info("  IAU names matched: %d of %d distinct names (most IAU-named stars are farther than the limit)",
             n_iau, len(by_name))

    _write_compiled(conn, stars)


# =============================================================================
# COMPILE: derive final fields, names, write tables
# =============================================================================
def _pick(*cands: tuple[Any, str]) -> tuple[Any, str | None]:
    """Return the first (value, source) whose value is not missing."""
    for v, src in cands:
        v = none_if_nan(v)
        if v is not None:
            return v, src
    return None, None


def _star_values(s: Star) -> dict[str, tuple[Any, str | None]]:
    """Physical parameters with a source tag, in order of preference."""
    if s.is_sol:
        return dict(spectral_type=("G2V", "sol"), teff=(5772.0, "sol"), radius=(1.0, "sol"),
                    mass=(1.0, "sol"), lum=(1.0, "sol"), age=(4.6, "sol"), met=(0.0, "sol"))
    g, h, hy = s.gaia or {}, s.hyg or {}, s.hyp or {}
    e = s.exo[0] if s.exo else {}
    st_lum = e.get("st_lum")  # log10(L/Lsun) in the archive
    # Gaia's ESP-HS module is trained for hot O/B/A stars and labels many nearby
    # white dwarfs 'B' or 'O'. An O/B/A star can't have absolute G fainter than ~7
    # (A-type main sequence is ~0.5-2.5), so drop those labels rather than lie.
    esphs, teff_esphs = g.get("spectraltype_esphs"), g.get("teff_esphs")
    gmag = g.get("phot_g_mean_mag")
    abs_g = gmag - 5 * math.log10(s.dist_pc / 10.0) if gmag is not None and s.dist_pc else None
    if esphs == "unknown" or (esphs in ("O", "B", "A") and (abs_g is None or abs_g > 7.0)):
        esphs, teff_esphs = None, None
    return dict(
        spectral_type=_pick((h.get("spect"), "hyg"), (e.get("st_spectype"), "exoplanet_archive"),
                            (hy.get("spec"), "hypatia"), (esphs, "gaia_esphs")),
        teff=_pick((e.get("st_teff"), "exoplanet_archive"), (hy.get("teff"), "hypatia"),
                   (g.get("teff_gspspec"), "gaia_gspspec"), (g.get("teff_gspphot"), "gaia_gspphot"),
                   (teff_esphs, "gaia_esphs"), (teff_from_bv(h.get("ci")), "estimated_from_hyg_bv")),
        radius=_pick((e.get("st_rad"), "exoplanet_archive"), (g.get("radius_flame"), "gaia_flame"),
                     (g.get("radius_gspphot"), "gaia_gspphot")),
        mass=_pick((e.get("st_mass"), "exoplanet_archive"), (g.get("mass_flame"), "gaia_flame")),
        lum=_pick((10 ** st_lum if st_lum is not None else None, "exoplanet_archive"),
                  (g.get("lum_flame"), "gaia_flame"), (h.get("lum"), "hyg_visual")),
        age=_pick((e.get("st_age"), "exoplanet_archive"), (g.get("age_flame"), "gaia_flame")),
        met=_pick((hy.get("fe_h"), "hypatia[Fe/H]"),
                  (e.get("st_met"), "exoplanet_archive" + str(e.get("st_metratio") or "")),
                  (g.get("mh_gspspec"), "gaia_gspspec[M/H]"), (g.get("mh_gspphot"), "gaia_gspphot[M/H]")),
    )


def _write_compiled(conn: sqlite3.Connection, stars: list[Star]) -> None:
    from astropy import units as u
    from astropy.coordinates import SkyCoord

    # Cartesian coordinates for all stars in one vectorised astropy call.
    ra = np.array([s.ra or 0.0 for s in stars])
    dec = np.array([s.dec or 0.0 for s in stars])
    dist = np.array([s.dist_pc or 0.0 for s in stars])
    c = SkyCoord(ra=ra * u.deg, dec=dec * u.deg, distance=np.maximum(dist, 1e-12) * u.pc, frame="icrs")
    # Equatorial: x = d cos(dec) cos(ra), y = d cos(dec) sin(ra), z = d sin(dec)
    eq = c.cartesian
    # Galactic: the same vectors rotated so x points to the galactic centre and z to
    # the north galactic pole (astropy applies the standard ICRS->Galactic matrix).
    gal = c.galactic.cartesian
    xe, ye, ze = (eq.x.to_value(u.pc), eq.y.to_value(u.pc), eq.z.to_value(u.pc))
    xg, yg, zg = (gal.x.to_value(u.pc), gal.y.to_value(u.pc), gal.z.to_value(u.pc))

    star_rows: list[dict[str, Any]] = []
    name_rows: list[list[Any]] = []  # [star_id, name, catalog]
    planet_rows: list[tuple[Any, ...]] = []
    for i, s in enumerate(stars):
        star_id = i + 1
        g, h = s.gaia or {}, s.hyg or {}
        e = s.exo[0] if s.exo else {}
        vals = _star_values(s)
        if s.is_sol:
            row: dict[str, Any] = dict(
                gaia_source_id=None, hip=None, hd=None, hyg_id=0 if s.hyg else None,
                x=0.0, y=0.0, z=0.0, x_eq=0.0, y_eq=0.0, z_eq=0.0, dist_pc=0.0, dist_ly=0.0, dist_source="sol",
                ra=None, dec=None, parallax_mas=None, parallax_error_mas=None, ruwe=None,
                app_mag=-26.74, app_mag_band="V", abs_mag=4.83, vmag=-26.74, gaia_g_mag=None,
                bp_rp=0.82, ci_bv=0.653, planet_count=8, system_planet_count=8, is_sol=1,
                sources="sol_reference" + (",hyg" if s.hyg else ""),
            )
        else:
            from_gaia = s.dist_source == "gaia_parallax"
            vmag = h.get("mag") if h else e.get("sy_vmag")
            gmag = g.get("phot_g_mean_mag")
            app, band = _pick((vmag, "V"), (gmag, "G"))
            # distance modulus: M = m - 5 log10(d / 10 pc)
            absm = app - 5 * math.log10(s.dist_pc / 10.0) if app is not None and s.dist_pc else None
            srcs = [n for n, ok in (("gaia", bool(s.gaia)), ("hyg", bool(s.hyg)), ("exoplanet_archive", bool(s.exo)),
                                    ("hypatia", bool(s.hyp)), ("iau", bool(s.iau))) if ok]
            row = dict(
                gaia_source_id=s.gaia_source_id, hip=s.hip, hd=s.hd, hyg_id=as_int(h.get("id")) if h else None,
                x=xg[i], y=yg[i], z=zg[i], x_eq=xe[i], y_eq=ye[i], z_eq=ze[i],
                dist_pc=s.dist_pc, dist_ly=s.dist_pc * LY_PER_PC, dist_source=s.dist_source,
                ra=s.ra, dec=s.dec,
                parallax_mas=g.get("parallax") if from_gaia else None,
                parallax_error_mas=g.get("parallax_error") if from_gaia else None,
                ruwe=g.get("ruwe"), app_mag=app, app_mag_band=band, abs_mag=absm, vmag=vmag, gaia_g_mag=gmag,
                bp_rp=g.get("bp_rp"), ci_bv=h.get("ci"),
                planet_count=len({p["pl_name"] for p in s.exo}),
                system_planet_count=as_int(e.get("sy_pnum")) if e else None,
                is_sol=0, sources=",".join(srcs),
            )
        sp, sp_src = vals["spectral_type"]
        row.update(
            spectral_type=sp, spectral_type_source=sp_src, spectral_class=spectral_class_of(sp),
            teff_k=vals["teff"][0], teff_source=vals["teff"][1],
            radius_sol=vals["radius"][0], radius_source=vals["radius"][1],
            mass_sol=vals["mass"][0], mass_source=vals["mass"][1],
            luminosity_sol=vals["lum"][0], luminosity_source=vals["lum"][1],
            age_gyr=vals["age"][0], age_source=vals["age"][1],
            metallicity=vals["met"][0], metallicity_source=vals["met"][1],
        )
        names = _names_for(s)
        if s.is_sol:
            names = [("Sol", "proper"), ("Sun", "proper")] + names
        for n, cat in names:
            name_rows.append([star_id, n, cat])
        star_rows.append(row)
        for p in s.exo:
            planet_rows.append((star_id, p["pl_name"], p.get("hostname"), as_int(p.get("disc_year")),
                                p.get("discoverymethod"), p.get("pl_orbper"), p.get("pl_orbsmax"),
                                p.get("pl_orbeccen"), p.get("pl_rade"), p.get("pl_bmasse"), p.get("pl_eqt")))

    # If HYG gives a star a proper name that the IAU assigned to a *different*
    # star (e.g. HYG and IAU disagree on which component of mu Dra is 'Alrakis'),
    # the IAU wins and the HYG copy is dropped.
    iau_owner = {n.lower(): sid for sid, n, cat in name_rows if cat == "IAU"}
    name_rows = [r for r in name_rows
                 if not (r[2] == "proper" and iau_owner.get(r[1].lower(), r[0]) != r[0])]
    _disambiguate_shared_names(stars, name_rows)

    # bayer_name column (caret style) and the display name (best-priority name)
    best: dict[int, tuple[int, int]] = {}
    for idx, (sid, n, cat) in enumerate(name_rows):
        if cat == "bayer":
            star_rows[sid - 1]["bayer_name"] = n
        pr = NAME_PRIORITY.get(cat, 50)
        if sid not in best or pr < best[sid][0]:
            best[sid] = (pr, idx)
    primary_idx = {idx for _pr, idx in best.values()}
    for r in star_rows:
        r.setdefault("bayer_name", None)
        r["display_name"] = None
    for sid, (_pr, idx) in best.items():
        star_rows[sid - 1]["display_name"] = name_rows[idx][1]
    for r in star_rows:
        if not r["display_name"]:  # cannot happen (every star has an id), but keep NOT NULL honest
            r["display_name"] = f"unnamed {r['hyg_id'] or r['gaia_source_id']}"

    conn.executescript(SCHEMA)
    cols = list(star_rows[0].keys())
    conn.executemany(
        f"INSERT INTO stars (star_id, {', '.join(cols)}) VALUES ({', '.join('?' * (len(cols) + 1))})",
        [(i + 1, *[none_if_nan(r[c]) for c in cols]) for i, r in enumerate(star_rows)],
    )
    conn.executemany(
        "INSERT INTO star_names (star_id, name, catalog, is_primary) VALUES (?, ?, ?, ?)",
        [(sid, n, cat, int(i in primary_idx)) for i, (sid, n, cat) in enumerate(name_rows)],
    )
    conn.executemany(
        "INSERT INTO planets (star_id, pl_name, hostname, disc_year, discoverymethod, pl_orbper_days, "
        "pl_orbsmax_au, pl_orbeccen, pl_rade, pl_bmasse, pl_eqt_k) VALUES (?,?,?,?,?,?,?,?,?,?,?)",
        [tuple(none_if_nan(v) for v in r) for r in planet_rows],
    )
    conn.executescript(INDEXES)
    conn.commit()
    log.info("  wrote stars=%d star_names=%d planets=%d", len(star_rows), len(name_rows), len(planet_rows))


def _names_for(s: Star) -> list[tuple[str, str]]:
    """All names for one star as (name, catalog), de-duplicated, best first."""
    out: list[tuple[str, str]] = []
    seen: set[str] = set()

    def add(name: Any, cat: str) -> None:
        name = none_if_nan(name)
        if name is None:
            return
        name = re.sub(r"\s+", " ", str(name)).strip()
        if name and name.lower() not in seen:
            seen.add(name.lower())
            out.append((name, cat))

    h = s.hyg or {}
    for r in s.iau:
        add(r.get("name"), "IAU")
    add(h.get("proper"), "proper")

    # Bayer: HYG's separate bayer/con columns, falling back to parsing 'bf'.
    bayer_tok, con = h.get("bayer"), h.get("con")
    if parse_bayer_letter(bayer_tok) is None or normalize_constellation(con) is None:
        d = parse_hyg_bf(h.get("bf"))
        bayer_tok, con = (d.bayer_token, d.con) if d and d.bayer else (None, None)
    full = bayer_to_full_name(bayer_tok, con)  # caret style, e.g. 'Omicron^2 Eridani'
    if full:
        add(full, "bayer")
        add(bayer_to_full_name(bayer_tok, con, "unicode"), "bayer_unicode")  # 'Omicron² Eridani'
        add(bayer_to_full_name(bayer_tok, con, "plain"), "bayer_plain")      # 'Omicron2 Eridani'
        add(bayer_abbrev(bayer_tok, con), "bayer_abbrev")                    # 'omi2 Eri'
        add(bayer_abbrev(bayer_tok, con, greek_symbol=True), "bayer_greek")  # 'ο² Eri'
    # IAU-CSN has component letters: 'alf' 'Cen' 'A' -> 'Alpha Centauri A'
    for r in s.iau:
        if r.get("source") == "IAU-CSN" and r.get("bayer_id") and re.fullmatch(r"[A-E]", str(r.get("component"))):
            base = bayer_to_full_name(r["bayer_id"], r.get("con"))  # keeps superscripts: 'Omicron^2 Eridani'
            if base:
                add(f"{base} {r['component']}", "bayer_component")

    flam, fcon = h.get("flam"), h.get("con")
    if not flamsteed_to_full_name(flam, fcon):
        d = parse_hyg_bf(h.get("bf"))
        flam, fcon = (d.flamsteed, d.con) if d and d.flamsteed else (None, None)
    add(flamsteed_to_full_name(flam, fcon), "flamsteed")
    add(h.get("gl"), "gliese")
    if s.hd:
        add(f"HD {s.hd}", "HD")
    if s.hip:
        add(f"HIP {s.hip}", "HIP")
    if as_int(h.get("hr")):
        add(f"HR {as_int(h['hr'])}", "HR")
    if s.exo:
        add(s.exo[0].get("hostname"), "exoplanet_host")
    if s.gaia_source_id:
        add(f"Gaia DR3 {s.gaia_source_id}", "Gaia DR3")
    return out


def _disambiguate_shared_names(stars: list[Star], name_rows: list[list[Any]]) -> None:
    """
    HYG lists both components of some binaries under one Bayer/Flamsteed/HD
    designation or proper name (61 Cygni A and B are both '61 Cyg'). Give each a component
    suffix (from the Gliese number, e.g. 'Gl 820A', else HYG 'comp' order) so
    display names stay unique. The primary component also keeps the bare name
    as a low-priority '<catalog>_alias' row so searching '61 Cygni' still works.
    """
    fams = ("IAU", "proper", "bayer", "bayer_unicode", "bayer_plain", "bayer_abbrev", "bayer_greek",
            "flamsteed", "gliese", "HD")
    users: dict[tuple[str, str], list[int]] = {}
    for idx, (_sid, n, cat) in enumerate(name_rows):
        if cat in fams:
            users.setdefault((cat, n), []).append(idx)
    extra: list[list[Any]] = []
    for (cat, n), idxs in users.items():
        if len({name_rows[i][0] for i in idxs}) < 2:
            continue
        info = []
        for i in idxs:
            h = stars[name_rows[i][0] - 1].hyg or {}
            m = re.search(r"\d\s*([A-E])$", str(h.get("gl") or ""))
            info.append((i, m.group(1) if m else None, as_int(h.get("comp")) or 99,
                         h.get("mag") if h.get("mag") is not None else 99.0))
        info.sort(key=lambda t: (t[1] or "Z", t[2], t[3]))
        taken: set[str] = set()
        for i, letter, _comp, _mag in info:
            if letter is None or letter in taken:
                letter = next(ch for ch in "ABCDEFGHIJ" if ch not in taken)
            taken.add(letter)
            if letter == "A":
                extra.append([name_rows[i][0], n, cat + "_alias"])
            name_rows[i][1] = f"{n} {letter}"
    name_rows.extend(extra)


# =============================================================================
# VERIFY
# =============================================================================
CHECK_STARS = [
    # (label, a name to look up in star_names, expected distance range in ly)
    ("Sol", "Sol", (0.0, 0.0)),
    ("Proxima Centauri", "Proxima Centauri", (4.1, 4.4)),
    ("Alpha Centauri A", "Rigil Kentaurus", (4.2, 4.5)),
    ("Alpha Centauri B", "Toliman", (4.2, 4.5)),
    ("Barnard's Star", "Barnard's Star", (5.8, 6.1)),
    ("Sirius", "Sirius", (8.4, 8.8)),
    ("Epsilon Eridani", "Epsilon Eridani", (10.3, 10.7)),
    ("61 Cygni A", "61 Cygni A", (11.2, 11.6)),
    ("61 Cygni B", "61 Cygni B", (11.2, 11.6)),
    ("Tau Ceti", "Tau Ceti", (11.7, 12.1)),
    ("Omicron^2 Eridani (40 Eri)", "Omicron^2 Eridani", (16.0, 16.5)),
    ("Vega", "Vega", (24.5, 25.5)),
    ("Gamma Pavonis", "Gamma Pavonis", (29.5, 31.0)),
]

EXPECTED_BAYER = {"Epsilon Eridani": "Epsilon Eridani", "Omicron^2 Eridani": "Omicron^2 Eridani",
                  "Gamma Pavonis": "Gamma Pavonis", "Tau Ceti": "Tau Ceti"}


def verify(conn: sqlite3.Connection) -> bool:
    ok = True
    log.info("[verify] row counts")
    for t in ("raw_gaia", "raw_gaia_hip_xmatch", "raw_hyg", "raw_exoplanets", "raw_hypatia", "raw_iau_names",
              "stars", "star_names", "planets"):
        try:
            n: Any = conn.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]
        except sqlite3.OperationalError:
            n = "missing"
        log.info("  %-22s %s", t, n)
    total = conn.execute("SELECT COUNT(*) FROM stars").fetchone()[0]
    log.info("[verify] coverage")
    for label, expr in [
        ("teff_k", "teff_k IS NOT NULL"), ("radius_sol", "radius_sol IS NOT NULL"),
        ("mass_sol", "mass_sol IS NOT NULL"), ("luminosity_sol", "luminosity_sol IS NOT NULL"),
        ("age_gyr", "age_gyr IS NOT NULL"), ("metallicity", "metallicity IS NOT NULL"),
        ("spectral_type", "spectral_type IS NOT NULL"), ("spectral_class", "spectral_class IS NOT NULL"),
        ("bayer_name", "bayer_name IS NOT NULL"), ("planet_count>0", "planet_count > 0"),
    ]:
        n = conn.execute(f"SELECT COUNT(*) FROM stars WHERE {expr}").fetchone()[0]
        log.info("  %-15s %6d / %d = %5.1f%%", label, n, total, 100.0 * n / total)

    log.info("[verify] named stars")
    log.info("  %-27s %-6s %-18s %7s %-19s %-3s %-9s %s", "check", "id", "display_name", "dist_ly",
             "bayer_name", "pl", "spectral", "names matched")
    for label, name, (lo, hi) in CHECK_STARS:
        rows = conn.execute(
            "SELECT DISTINCT s.star_id, s.display_name, s.dist_ly, s.bayer_name, s.planet_count, s.spectral_type "
            "FROM stars s JOIN star_names n ON n.star_id = s.star_id WHERE n.name = ? COLLATE NOCASE", (name,)
        ).fetchall()
        if len(rows) != 1:
            ok = False
            log.error("  %-27s expected exactly 1 star, got %d: %s", label, len(rows), rows)
            continue
        sid, disp, dly, bay, pc, sp = rows[0]
        good = (lo <= dly <= hi) if hi else dly == 0.0
        if name in EXPECTED_BAYER and bay != EXPECTED_BAYER[name]:
            good = False
        ok &= good
        log.info("  %-27s %-6d %-18s %7.2f %-19s %-3d %-9s %s", label, sid, disp, dly, bay or "-", pc, sp or "-",
                 "OK" if good else "<-- FAILED")
    ee = conn.execute("SELECT s.planet_count FROM stars s JOIN star_names n USING(star_id) "
                      "WHERE n.name = 'Epsilon Eridani'").fetchone()
    if not ee or ee[0] < 1:
        ok = False
        log.error("  Epsilon Eridani should have planet_count >= 1: %s", ee)
    sol = conn.execute("SELECT star_id, x, y, z, teff_k, planet_count, display_name FROM stars WHERE is_sol=1").fetchall()
    log.info("  Sol row: %s", sol)
    ok &= len(sol) == 1 and tuple(sol[0][1:4]) == (0.0, 0.0, 0.0) and sol[0][0] == 1

    log.info("[verify] integrity")
    fk = conn.execute("PRAGMA foreign_key_check").fetchall()
    q = lambda sql: conn.execute(sql).fetchone()[0]  # noqa: E731
    dup_hip = q("SELECT COUNT(*) FROM (SELECT hip FROM stars WHERE hip IS NOT NULL GROUP BY hip HAVING COUNT(*)>1)")
    dup_hyg = q("SELECT COUNT(*) FROM (SELECT hyg_id FROM stars WHERE hyg_id IS NOT NULL GROUP BY hyg_id HAVING COUNT(*)>1)")
    multi_p = q("SELECT COUNT(*) FROM (SELECT star_id FROM star_names WHERE is_primary=1 GROUP BY star_id HAVING COUNT(*)>1)")
    no_p = q("SELECT COUNT(*) FROM stars WHERE star_id NOT IN (SELECT star_id FROM star_names WHERE is_primary=1)")
    dup_disp = q("SELECT COUNT(*) FROM (SELECT display_name FROM stars GROUP BY display_name HAVING COUNT(*)>1)")
    far = q("SELECT COUNT(*) FROM stars WHERE dist_ly > (SELECT CAST(value AS REAL) FROM raw_meta "
            "WHERE key='max_dist_ly') + 1e-6")
    log.info("  foreign_key_check violations=%d, duplicate hip=%d, duplicate hyg_id=%d, stars with >1 primary=%d, "
             "without primary=%d, duplicated display names=%d, beyond max distance=%d",
             len(fk), dup_hip, dup_hyg, multi_p, no_p, dup_disp, far)
    ok &= not fk and dup_hip == 0 and dup_hyg == 0 and multi_p == 0 and no_p == 0 and far == 0
    log.info("  VERIFY %s", "PASSED" if ok else "FAILED")
    return ok


# =============================================================================
# CLI
# =============================================================================
def parse_args(argv: list[str] | None = None) -> tuple[Config, bool]:
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", type=Path, default=Path("stars.db"), help="output SQLite file (default: stars.db)")
    ap.add_argument("--cache-dir", type=Path, default=Path("cache"), help="raw download cache (default: ./cache)")
    ap.add_argument("--refresh", action="store_true", help="ignore cached downloads and fetch again")
    ap.add_argument("--max-dist-ly", type=float, default=200.0, help="distance limit in light years (default 200)")
    ap.add_argument("--min-parallax-snr", type=float, default=10.0,
                    help="minimum Gaia parallax_over_error (default 10; 5 keeps more, noisier sources)")
    ap.add_argument("--skip", action="append", default=[], choices=ALL_SOURCES,
                    help="skip a source (repeatable)")
    ap.add_argument("--compile-only", action="store_true", help="no fetching; rebuild compiled tables from raw_*")
    ap.add_argument("--no-verify", action="store_true", help="skip the verification report")
    ap.add_argument("--gaia-mode", choices=["sync", "async"], default="sync",
                    help="ESA TAP job type for the chunked Gaia queries (default sync: async jobs were stalling)")
    ap.add_argument("--gaia-seed", choices=["gavo", "esa"], default="gavo",
                    help="where the parallax selection runs (gavo = indexed mirror, fast; esa = full scan, slow)")
    ap.add_argument("--gaia-chunk", type=int, default=1500, help="source_ids per Gaia query (< 2000 for sync)")
    ap.add_argument("--gaia-workers", type=int, default=4, help="parallel Gaia queries (default 4; be polite)")
    ap.add_argument("--exo-margin-pc", type=float, default=4.0,
                    help="extra parsecs when querying the exoplanet archive, since distances differ between catalogs")
    ap.add_argument("--net-timeout", type=float, default=180.0,
                    help="socket timeout in seconds for each network read (default 180)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(message)s", datefmt="%H:%M:%S")
    for noisy in ("urllib3",):  # (never pre-create the "astropy" logger: it breaks astropy's own logger setup)
        logging.getLogger(noisy).setLevel(logging.WARNING)
    cfg = Config(db_path=a.db, cache_dir=a.cache_dir, refresh=a.refresh, max_dist_ly=a.max_dist_ly,
                 min_parallax_snr=a.min_parallax_snr, skip=set(a.skip), compile_only=a.compile_only,
                 gaia_mode=a.gaia_mode, gaia_seed=a.gaia_seed, gaia_chunk=a.gaia_chunk,
                 gaia_workers=a.gaia_workers, exo_margin_pc=a.exo_margin_pc,
                 net_timeout=a.net_timeout)
    return cfg, a.no_verify


def main(argv: list[str] | None = None) -> int:
    cfg, no_verify = parse_args(argv)
    # astroquery's TAP client uses http.client without a timeout, so a stalled ESA
    # request would hang forever; a global socket timeout turns that into a retry.
    socket.setdefaulttimeout(cfg.net_timeout)
    t0 = time.time()
    cfg.cache_dir.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(cfg.db_path)
    conn.execute("PRAGMA foreign_keys = ON")
    try:
        if not cfg.compile_only:
            status = fetch_all(cfg, conn)
            log.info("source status: %s", status)
        compile_catalog(cfg, conn)
        conn.execute("ANALYZE")
        conn.commit()
        ok = True if no_verify else verify(conn)
    finally:
        conn.close()
    log.info("done in %.1f s; %s is %.1f MB", time.time() - t0, cfg.db_path, cfg.db_path.stat().st_size / 1e6)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
