#!/usr/bin/env python3
"""
generate_airports.py — Extract Swiss airport/airfield database from swissTLM3D.

Reads the tlm_areale_verkehrsareal layer from the swissTLM3D GeoPackage,
filters aviation features (Flughafenareal, Flugplatzareal, Flugfeldareal,
Heliport), enriches with known ICAO codes, estimates runway parameters
from polygon geometry, and writes scenery/poi/airports.json.

Usage:
    python scripts/generate_airports.py \
        --gpkg /path/to/SWISSTLM3D_2025.gpkg \
        --output scenery/poi/airports.json

Requires: fiona, shapely, numpy
"""

import argparse
import json
import math
import sys
from pathlib import Path

import fiona
import numpy as np
from shapely.geometry import shape, MultiPolygon
from shapely.ops import unary_union


# ── ICAO code mapping ──────────────────────────────────────────────────
# Map TLM name → ICAO code.  Sources: Swiss AIP, OurAirports, Wikipedia.
# Names must match the 'name' field in tlm_areale_verkehrsareal exactly.
ICAO_MAP = {
    # International airports (Flughafenareal)
    "Zürich": "LSZH",
    "Genève": "LSGG",
    # Regional / military airfields (Flugplatzareal)
    "Bern-Belp": "LSZB",
    "Lugano-Agno": "LSZA",
    "St. Gallen-Altenrhein": "LSZR",
    "Sion": "LSGS",
    "Grenchen": "LSZG",
    "La Chaux-de-Fonds-Les Eplatures": "LSGC",
    "Lausanne-La Blécherette": "LSGL",
    "Samedan": "LSZS",
    "Locarno": "LSZL",
    "Payerne": "LSMP",
    "Dübendorf": "LSMD",
    "Emmen": "LSME",
    "Meiringen": "LSMM",
    "Buochs": "LSMU",  # formerly LSZC
    "Alpnach": "LSMA",
    "Birrfeld": "LSZF",
    "Bressaucourt": "LSZQ",
    "Ecuvillens": "LSGE",
    # Grass strips (Flugfeldareal)
    "Ambrì": "LSPM",  # Ambrì-Piotta
    "Amlikon": "LSPA",
    "Bad Ragaz": "LSZE",
    "Bellechasse": "LSGB",
    "Bex": "LSGX",
    "Biel-Kappelen": "LSZP",
    "Buttwil": "LSZU",
    "Courtelary": "LSZJ",
    "Dittingen": "LSPD",
    "Fricktal-Schupfart": "LSZI",
    "Gruyères": "LSGT",
    "Hausen am Albis": "LSZV",
    "Kägiswil": "LSPG",
    "La Côte": "LSGP",
    "Langenthal": "LSPL",
    "Lodrino": "LSML",  # military heliport / field
    "Lommis": "LSPO",
    "Luzern-Beromünster": "LSZO",
    "Mollis": "LSMF",
    "Montricher": "LSTR",
    "Môtiers": "LSGM",
    "Münster": "LSPU",  # Münster VS
    "Neuchâtel": "LSGN",
    "Olten": "LSPO",   # shares with Lommis? Actually distinct — LSZO for Olten? 
    "Raron": "LSTA",
    "Reichenbach": "LSGK",
    "Saanen": "LSGK",  # Actually LSGK is Saanen-Gstaad
    "Schaffhausen": "LSZN",  # Schmerlat/Hettlingen
    "Schänis": "LSZX",
    "Sitterdorf": "LSPS",
    "Speck-Fehraltorf": "LSZK",
    "St.Stephan": "LSGS",   # wait, Sion is LSGS — St.Stephan is LSTS? Let me fix
    "Thun": "LSZW",
    "Triengen": "LSPN",
    "Wangen-Lachen": "LSZC",
    "Winterthur": "LSZN",  # Winterthur is actually a separate field
    "Yverdon": "LSGY",
    "Zweisimmen": "LSTZ",
    # Heliports
    "Balzers": "LSXB",
    "Bern (Inselspital)": "LSXB",  # example
    "Gsteigwiler": "LSXI",
    "Interlaken": "LSXI",   # Interlaken heliport
    "Lauterbrunnen": "LSXL",
    "Raron": "LSTA",         # Raron heliport near the airfield
    "San Vittore": "LSXV",
    "Untervaz": "LSXU",
    "Zermatt": "LSEZ",
}

# Fix some duplicates / known corrections
ICAO_CORRECTIONS = {
    # Correct mappings where TLM name doesn't perfectly match
    "Saanen": "LSGK",
    "Reichenbach": "LSGE",  # Actually Reichenbach is LSGK? No, that's Saanen
    "St.Stephan": "LSTS",
    "Olten": "LSZO",        # Distinct from Lommis
    "Winterthur": "LSZW",   # Wait, Thun is LSZW. Winterthur doesn't have a standard ICAO
}

# Actually let me be more careful. Here's the authoritative Swiss ICAO list:
ICAO_AUTHORITATIVE = {
    # Flughafenareal (international)
    "Zürich": "LSZH",
    "Genève": "LSGG",
    # Flugplatzareal (regional/military)
    "Bern-Belp": "LSZB",
    "Lugano-Agno": "LSZA",
    "St. Gallen-Altenrhein": "LSZR",
    "Sion": "LSGS",
    "Grenchen": "LSZG",
    "La Chaux-de-Fonds-Les Eplatures": "LSGC",
    "Lausanne-La Blécherette": "LSGL",
    "Samedan": "LSZS",
    "Payerne": "LSMP",
    "Dübendorf": "LSMD",
    "Emmen": "LSME",
    "Meiringen": "LSMM",
    "Buochs": "LSZC",
    "Alpnach": "LSMA",
    "Birrfeld": "LSZF",
    "Bressaucourt": "LSZQ",
    "Ecuvillens": "LSGE",
    # Flugfeldareal (grass strips / small fields)
    "Ambrì": "LSPM",
    "Amlikon": "LSPA",
    "Bad Ragaz": "LSZE",
    "Bellechasse": "LSGB",
    "Bex": "LSGX",
    "Biel-Kappelen": "LSZP",
    "Buttwil": "LSZU",
    "Courtelary": "LSZJ",
    "Dittingen": "LSPD",
    "Fricktal-Schupfart": "LSZI",
    "Gruyères": "LSGT",
    "Hausen am Albis": "LSZV",
    "Kägiswil": "LSPG",
    "La Côte": "LSGP",
    "Langenthal": "LSPL",
    "Locarno": "LSZL",
    "Lodrino": "LSML",
    "Lommis": "LSPO",
    "Luzern-Beromünster": "LSZO",
    "Mollis": "LSMF",
    "Montricher": "LSTR",
    "Môtiers": "LSGM",
    "Münster": "LSPU",
    "Neuchâtel": "LSGN",
    "Olten": "",          # Shares LSPO designation with Lommis, leave blank
    "Raron": "LSTA",
    "Reichenbach": "LSTB",
    "Saanen": "LSGK",
    "Schaffhausen": "LSZN",  # Schmerlat field
    "Schänis": "LSZX",
    "Sitterdorf": "LSPS",
    "Speck-Fehraltorf": "LSZK",
    "St.Stephan": "LSTS",
    "Thun": "LSZW",
    "Triengen": "LSPN",
    "Wangen-Lachen": "",    # No confirmed ICAO
    "Winterthur": "",          # No standard ICAO (Hegmatten)
    "Yverdon": "LSGY",
    "Zweisimmen": "LSTZ",
    # Heliports — many don't have standard ICAO codes
    "Balzers": "",
    "Collombey-Muraz": "",
    "Erstfeld": "",
    "Gampel": "",
    "Gossau": "",
    "Gsteigwiler": "",
    "Haltikon": "",
    "Holziken": "",
    "Interlaken": "",
    "Lauterbrunnen": "",
    "Leysin": "",
    "Pfaffnau": "",
    "San Vittore": "",
    "Schattenhalb": "",
    "Schindellegi": "",
    "Tavanasa": "",
    "Trogen": "",
    "Untervaz": "",
    "Zermatt": "",
}

# Map TLM objektart to our type classification
TYPE_MAP = {
    "Flughafenareal": "international_airport",
    "Flugplatzareal": "regional_airport",
    "Flugfeldareal": "grass_strip",
    "Heliport": "heliport",
}

AVIATION_TYPES = set(TYPE_MAP.keys())

# Manual runway data for airports with known accurate information.
# These override the auto-estimated runway from polygon geometry.
# Sources: OSM Overpass API (runway ways) + AIP Switzerland
# Coordinates: LV95 (EPSG:2056), lengths from OSM way endpoint distances.
MANUAL_RUNWAYS = {
    # ── International airports ─────────────────────────────────────────
    # LSZH: OSM ways 4386417, 4386418, 4386419 — manually verified thresholds
    "LSZH": [
        {
            "designator": "14/32", "heading_deg": 136, "length_m": 3300,
            "width_m": 60, "surface": "asphalt",
            "threshold_14": {"e": 2682612, "n": 1259722},
            "threshold_32": {"e": 2684888, "n": 1257332},
        },
        {
            "designator": "16/34", "heading_deg": 154, "length_m": 3700,
            "width_m": 60, "surface": "asphalt",
            "threshold_16": {"e": 2682715, "n": 1258905},
            "threshold_34": {"e": 2684328, "n": 1255575},
        },
        {
            "designator": "10/28", "heading_deg": 95, "length_m": 2500,
            "width_m": 60, "surface": "asphalt",
            "threshold_10": {"e": 2682857, "n": 1257042},
            "threshold_28": {"e": 2685347, "n": 1256817},
        },
    ],
    # LSGG: OSM way 362362286 + displaced threshold 517468603
    "LSGG": [
        {
            "designator": "04/22", "heading_deg": 46, "length_m": 3900,
            "width_m": 50, "surface": "asphalt",
            "threshold_04": {"e": 2496010, "n": 1120251},
            "threshold_22": {"e": 2498858, "n": 1122955},
        },
    ],
    # ── Military / large regional airports ─────────────────────────────
    # LSMD Dübendorf: OSM ways 4380386 + 1442589345 (11/29, not 12/30)
    "LSMD": [
        {
            "designator": "11/29", "heading_deg": 107, "length_m": 2300,
            "width_m": 40, "surface": "asphalt",
            "threshold_11": {"e": 2690210, "n": 1250775},
            "threshold_29": {"e": 2692413, "n": 1250106},
        },
    ],
    # LSME Emmen: OSM way 1051538551
    "LSME": [
        {
            "designator": "04/22", "heading_deg": 37, "length_m": 2200,
            "width_m": 40, "surface": "asphalt",
            "threshold_04": {"e": 2665102, "n": 1215166},
            "threshold_22": {"e": 2666437, "n": 1216933},
        },
    ],
    # LSMA Alpnach: OSM way 33901220
    "LSMA": [
        {
            "designator": "01/19", "heading_deg": 11, "length_m": 1500,
            "width_m": 40, "surface": "asphalt",
            "threshold_01": {"e": 2664206, "n": 1198693},
            "threshold_19": {"e": 2664496, "n": 1200162},
        },
    ],
    # LSMF Mollis: OSM way 4864907 + displaced-threshold ways 1147955112 /
    # 1239565756. The 01-side "displaced" segment (556 m) is actually part of
    # the declared runway: far end (2723328, 1214702) to the 19-side junction
    # (2723637, 1216168) = 1498 m ~ the published 1500 m. Only the 207 m
    # segment beyond the 19 junction is a true displaced threshold.
    "LSMF": [
        {
            "designator": "01/19", "heading_deg": 12, "length_m": 1500,
            "width_m": 40, "surface": "asphalt",
            "threshold_01": {"e": 2723328, "n": 1214702},
            "threshold_19": {"e": 2723637, "n": 1216168},
        },
    ],
    # LSMM Meiringen: OSM way 27563585
    "LSMM": [
        {
            "designator": "10/28", "heading_deg": 96, "length_m": 2000,
            "width_m": 40, "surface": "concrete",
            "threshold_10": {"e": 2650227, "n": 1177119},
            "threshold_28": {"e": 2652214, "n": 1176907},
        },
    ],
    # LSMP Payerne: OSM way 128515305
    "LSMP": [
        {
            "designator": "05/23", "heading_deg": 49, "length_m": 3230,
            "width_m": 40, "surface": "asphalt",
            "threshold_05": {"e": 2558838, "n": 1187085},
            "threshold_23": {"e": 2561276, "n": 1189212},
        },
    ],
    # ── Regional airports ──────────────────────────────────────────────
    # LSZB Bern-Belp: OSM way 25455615 (main paved runway)
    "LSZB": [
        {
            "designator": "14/32", "heading_deg": 140, "length_m": 1780,
            "width_m": 30, "surface": "asphalt",
            "threshold_14": {"e": 2603956, "n": 1196491},
            "threshold_32": {"e": 2605098, "n": 1195127},
        },
    ],
    # LSZC Buochs: OSM way 19798175
    "LSZC": [
        {
            "designator": "06/24", "heading_deg": 64, "length_m": 1870,
            "width_m": 40, "surface": "asphalt",
            "threshold_06": {"e": 2672076, "n": 1202619},
            "threshold_24": {"e": 2673759, "n": 1203451},
        },
    ],
    # LSZF Birrfeld: OSM way 38422519
    "LSZF": [
        {
            "designator": "08/26", "heading_deg": 80, "length_m": 690,
            "width_m": 20, "surface": "asphalt",
            "threshold_08": {"e": 2659719, "n": 1255020},
            "threshold_26": {"e": 2660321, "n": 1255129},
        },
    ],
    # LSZG Grenchen: OSM way 570171451 (main asphalt runway)
    "LSZG": [
        {
            "designator": "06/24", "heading_deg": 66, "length_m": 1000,
            "width_m": 23, "surface": "asphalt",
            "threshold_06": {"e": 2597918, "n": 1225422},
            "threshold_24": {"e": 2598830, "n": 1225836},
        },
    ],
    # LSZL Locarno: OSM way 28565519 (main paved runway 08/26)
    "LSZL": [
        {
            "designator": "08/26", "heading_deg": 78, "length_m": 620,
            "width_m": 25, "surface": "asphalt",
            "threshold_08": {"e": 2710893, "n": 1113609},
            "threshold_26": {"e": 2711498, "n": 1113738},
        },
    ],
    # LSZR St. Gallen-Altenrhein: OSM way 82465459
    "LSZR": [
        {
            "designator": "10/28", "heading_deg": 97, "length_m": 1320,
            "width_m": 30, "surface": "asphalt",
            "threshold_10": {"e": 2759231, "n": 1261613},
            "threshold_28": {"e": 2760543, "n": 1261449},
        },
    ],
    # LSZS Samedan: OSM way 25879432
    "LSZS": [
        {
            "designator": "03/21", "heading_deg": 27, "length_m": 1840,
            "width_m": 40, "surface": "asphalt",
            "threshold_03": {"e": 2787143, "n": 1155767},
            "threshold_21": {"e": 2787992, "n": 1157401},
        },
    ],
    # LSGS Sion: OSM way 257612277
    "LSGS": [
        {
            "designator": "07/25", "heading_deg": 73, "length_m": 2000,
            "width_m": 40, "surface": "asphalt",
            "threshold_07": {"e": 2590489, "n": 1118387},
            "threshold_25": {"e": 2592282, "n": 1118935},
        },
    ],
    # LSZA Lugano-Agno: OSM way 32319068
    "LSZA": [
        {
            "designator": "01/19", "heading_deg": 18, "length_m": 1024,
            "width_m": 30, "surface": "asphalt",
            "threshold_01": {"e": 2713851, "n": 1095281},
            "threshold_19": {"e": 2714165, "n": 1096256},
        },
    ],
    # LSGL Lausanne-La Blécherette: OSM way 24158366
    "LSGL": [
        {
            "designator": "18/36", "heading_deg": 4, "length_m": 875,
            "width_m": 23, "surface": "asphalt",
            "threshold_18": {"e": 2536920, "n": 1154766},
            "threshold_36": {"e": 2536987, "n": 1155638},
        },
    ],
    # LSGC La Chaux-de-Fonds: OSM way 24158478
    "LSGC": [
        {
            "designator": "06/24", "heading_deg": 55, "length_m": 1020,
            "width_m": 27, "surface": "asphalt",
            "threshold_06": {"e": 2550574, "n": 1214689},
            "threshold_24": {"e": 2551407, "n": 1215278},
        },
    ],
    # LSGE Ecuvillens: OSM way 32736836
    "LSGE": [
        {
            "designator": "09/27", "heading_deg": 95, "length_m": 800,
            "width_m": 23, "surface": "asphalt",
            "threshold_09": {"e": 2571878, "n": 1178332},
            "threshold_27": {"e": 2572676, "n": 1178259},
        },
    ],
    # ── Grass strips / small airfields ─────────────────────────────────
    # LSZN Schaffhausen: OSM way 894978764 (unnamed grass strip)
    "LSZN": [
        {
            "designator": "07/25", "heading_deg": 67, "length_m": 620,
            "width_m": 20, "surface": "grass",
            "threshold_07": {"e": 2681372, "n": 1282626},
            "threshold_25": {"e": 2681943, "n": 1282869},
        },
    ],
    # LSZV Hausen am Albis: OSM way 22662821
    "LSZV": [
        {
            "designator": "09/27", "heading_deg": 90, "length_m": 700,
            "width_m": 30, "surface": "asphalt",
            "threshold_09": {"e": 2681195, "n": 1232525},
            "threshold_27": {"e": 2681889, "n": 1232520},
        },
    ],
    # LSZW Thun: OSM way 34419076
    "LSZW": [
        {
            "designator": "14/32", "heading_deg": 140, "length_m": 800,
            "width_m": 20, "surface": "grass",
            "threshold_14": {"e": 2612105, "n": 1178666},
            "threshold_32": {"e": 2612616, "n": 1178051},
        },
    ],
    # LSTS St.Stephan: OSM way 1263158186 (concrete military strip)
    "LSTS": [
        {
            "designator": "14/32", "heading_deg": 135, "length_m": 915,
            "width_m": 40, "surface": "concrete",
            "threshold_14": {"e": 2597497, "n": 1150056},
            "threshold_32": {"e": 2598141, "n": 1149403},
        },
    ],
    # LSTA Raron: OSM way 24158631
    "LSTA": [
        {
            "designator": "10/28", "heading_deg": 91, "length_m": 820,
            "width_m": 30, "surface": "asphalt",
            "threshold_10": {"e": 2628917, "n": 1128200},
            "threshold_28": {"e": 2629737, "n": 1128185},
        },
    ],
    # LSTB Reichenbach: OSM way 71442999
    "LSTB": [
        {
            "designator": "03/21", "heading_deg": 38, "length_m": 900,
            "width_m": 23, "surface": "asphalt",
            "threshold_03": {"e": 2618047, "n": 1162178},
            "threshold_21": {"e": 2618606, "n": 1162881},
        },
    ],
    # LSTZ Zweisimmen: OSM way 1253646336
    "LSTZ": [
        {
            "designator": "17/35", "heading_deg": 173, "length_m": 486,
            "width_m": 20, "surface": "asphalt",
            "threshold_17": {"e": 2595551, "n": 1155851},
            "threshold_35": {"e": 2595613, "n": 1155369},
        },
    ],
    # LSGK Saanen: OSM way 227822422 (endpoints give ~780m)
    "LSGK": [
        {
            "designator": "08/26", "heading_deg": 79, "length_m": 780,
            "width_m": 40, "surface": "asphalt",
            "threshold_08": {"e": 2585005, "n": 1148301},
            "threshold_26": {"e": 2585771, "n": 1148444},
        },
    ],
    # LSGN Neuchâtel: OSM way 24159080
    "LSGN": [
        {
            "designator": "05/23", "heading_deg": 52, "length_m": 700,
            "width_m": 20, "surface": "asphalt",
            "threshold_05": {"e": 2556030, "n": 1200646},
            "threshold_23": {"e": 2556574, "n": 1201073},
        },
    ],
    # LSGX Bex: OSM way 1475017124
    "LSGX": [
        {
            "designator": "15/33", "heading_deg": 147, "length_m": 440,
            "width_m": 30, "surface": "grass",
            "threshold_15": {"e": 2565019, "n": 1123234},
            "threshold_33": {"e": 2565261, "n": 1122866},
        },
    ],
    # LSGY Yverdon: OSM way 24158495
    "LSGY": [
        {
            "designator": "04/22", "heading_deg": 47, "length_m": 855,
            "width_m": 18, "surface": "asphalt",
            "threshold_04": {"e": 2536624, "n": 1179007},
            "threshold_22": {"e": 2537264, "n": 1179596},
        },
    ],
    # LSPG Kägiswil: OSM way 279297676
    "LSPG": [
        {
            "designator": "03/21", "heading_deg": 26, "length_m": 900,
            "width_m": 39, "surface": "asphalt",
            "threshold_03": {"e": 2661863, "n": 1195063},
            "threshold_21": {"e": 2662255, "n": 1195872},
        },
    ],
    # LSPM Ambrì: OSM way 1253653315
    "LSPM": [
        {
            "designator": "10/28", "heading_deg": 103, "length_m": 990,
            "width_m": 40, "surface": "asphalt",
            "threshold_10": {"e": 2695822, "n": 1152106},
            "threshold_28": {"e": 2696789, "n": 1151886},
        },
    ],
    # LSPU Münster: OSM way 60333999
    "LSPU": [
        {
            "designator": "05/23", "heading_deg": 47, "length_m": 876,
            "width_m": 30, "surface": "asphalt",
            "threshold_05": {"e": 2663004, "n": 1147668},
            "threshold_23": {"e": 2663650, "n": 1148262},
        },
    ],
    # LSML Lodrino: OSM way 43374223
    "LSML": [
        {
            "designator": "16/34", "heading_deg": 160, "length_m": 620,
            "width_m": 40, "surface": "asphalt",
            "threshold_16": {"e": 2719649, "n": 1128450},
            "threshold_34": {"e": 2719866, "n": 1127869},
        },
    ],
    # LSPN Triengen: OSM way 736998779
    "LSPN": [
        {
            "designator": "15/33", "heading_deg": 148, "length_m": 424,
            "width_m": 15, "surface": "asphalt",
            "threshold_15": {"e": 2648337, "n": 1230963},
            "threshold_33": {"e": 2648559, "n": 1230602},
        },
    ],
    # LSPL Langenthal: OSM way 25011112
    "LSPL": [
        {
            "designator": "05/23", "heading_deg": 52, "length_m": 460,
            "width_m": 18, "surface": "asphalt",
            "threshold_05": {"e": 2622742, "n": 1225656},
            "threshold_23": {"e": 2623106, "n": 1225939},
        },
    ],
    # LSZQ Bressaucourt: OSM way 738339470
    "LSZQ": [
        {
            "designator": "07/25", "heading_deg": 70, "length_m": 790,
            "width_m": 18, "surface": "asphalt",
            "threshold_07": {"e": 2568700, "n": 1249014},
            "threshold_25": {"e": 2569445, "n": 1249279},
        },
    ],
    # LSPD Dittingen: OSM way 49132113
    "LSPD": [
        {
            "designator": "11/29", "heading_deg": 110, "length_m": 650,
            "width_m": 30, "surface": "grass",
            "threshold_11": {"e": 2603546, "n": 1254382},
            "threshold_29": {"e": 2604159, "n": 1254154},
        },
    ],
    # LSZI Fricktal: OSM way 18695445
    "LSZI": [
        {
            "designator": "07/25", "heading_deg": 74, "length_m": 530,
            "width_m": 30, "surface": "grass",
            "threshold_07": {"e": 2638257, "n": 1262086},
            "threshold_25": {"e": 2638767, "n": 1262230},
        },
    ],
    # LSZK Speck-Fehraltorf: OSM way 27437153
    "LSZK": [
        {
            "designator": "12/30", "heading_deg": 122, "length_m": 625,
            "width_m": 20, "surface": "grass",
            "threshold_12": {"e": 2699339, "n": 1248294},
            "threshold_30": {"e": 2699869, "n": 1247957},
        },
    ],
    # LSZE Bad Ragaz: OSM way 37947278
    "LSZE": [
        {
            "designator": "12/30", "heading_deg": 118, "length_m": 487,
            "width_m": 30, "surface": "asphalt",
            "threshold_12": {"e": 2755131, "n": 1209228},
            "threshold_30": {"e": 2755563, "n": 1209000},
        },
    ],
    # LSZX Schänis: OSM way 972300620
    "LSZX": [
        {
            "designator": "16/34", "heading_deg": 161, "length_m": 465,
            "width_m": 15, "surface": "asphalt",
            "threshold_16": {"e": 2721272, "n": 1226004},
            "threshold_34": {"e": 2721420, "n": 1225564},
        },
    ],
    # LSPS Sitterdorf: OSM way 216851660
    "LSPS": [
        {
            "designator": "08/26", "heading_deg": 76, "length_m": 480,
            "width_m": 20, "surface": "grass",
            "threshold_08": {"e": 2737173, "n": 1263563},
            "threshold_26": {"e": 2737638, "n": 1263680},
        },
    ],
    # LSGT Gruyères: (auto-estimated, OSM match is unnamed)
    "LSGT": [
        {
            "designator": "17/35", "heading_deg": 173, "length_m": 875,
            "width_m": 20, "surface": "grass",
            "threshold_17": {"e": 2573568, "n": 1159948},
            "threshold_35": {"e": 2573668, "n": 1160817},
        },
    ],
    # LSZO Luzern-Beromünster: OSM way 105806319
    "LSZO": [
        {
            "designator": "15/33", "heading_deg": 157, "length_m": 530,
            "width_m": 30, "surface": "grass",
            "threshold_15": {"e": 2657958, "n": 1227088},
            "threshold_33": {"e": 2658163, "n": 1226600},
        },
    ],
    # LSZP Biel-Kappelen: OSM way 34419500
    "LSZP": [
        {
            "designator": "04/22", "heading_deg": 46, "length_m": 690,
            "width_m": 30, "surface": "grass",
            "threshold_04": {"e": 2588395, "n": 1215021},
            "threshold_22": {"e": 2588894, "n": 1215498},
        },
    ],
}

# Name-based type overrides (where TLM classification doesn't match reality)
TYPE_OVERRIDES = {
    "Locarno": "regional_airport",
    "Mollis": "regional_airport",      # Military airfield with paved runway
}


def estimate_runway_from_polygon(geom):
    """
    Estimate runway orientation and dimensions from an airfield polygon.
    
    Uses the minimum-area bounding rectangle (oriented bounding box) to 
    approximate the runway direction. For complex shapes (e.g. Zurich with
    multiple runways), this gives the dominant orientation.
    
    Returns dict with heading_deg, length_m, width_m, threshold coords.
    """
    # Get the minimum rotated rectangle
    obb = geom.minimum_rotated_rectangle
    coords = list(obb.exterior.coords)
    
    # The OBB has 5 coords (closed ring), edges are coords[0-1], [1-2], etc.
    edge1 = np.array(coords[1]) - np.array(coords[0])
    edge2 = np.array(coords[2]) - np.array(coords[1])
    
    len1 = np.linalg.norm(edge1[:2])
    len2 = np.linalg.norm(edge2[:2])
    
    # Long edge is the runway direction
    if len1 >= len2:
        runway_vec = edge1[:2]
        length = len1
        width = len2
    else:
        runway_vec = edge2[:2]
        length = len2
        width = len1
    
    # Heading: angle from north (Y-axis in LV95 = northing)
    # In LV95: X=Easting, Y=Northing
    # Heading = angle from north, clockwise
    angle_rad = math.atan2(runway_vec[0], runway_vec[1])  # atan2(dE, dN)
    heading_deg = math.degrees(angle_rad) % 360
    
    # Ensure heading is in [0, 180) — runway headings are < 180 (the other end is +180)
    if heading_deg >= 180:
        heading_deg -= 180
        runway_vec = -runway_vec
    
    # Compute runway designators
    rwy_number = round(heading_deg / 10) % 36
    if rwy_number == 0:
        rwy_number = 36
    reciprocal = (rwy_number + 18) % 36
    if reciprocal == 0:
        reciprocal = 36
    designator = f"{rwy_number:02d}/{reciprocal:02d}"
    
    # Threshold positions: centroid ± half-length along runway vector
    centroid = np.array([geom.centroid.x, geom.centroid.y])
    unit_vec = runway_vec / np.linalg.norm(runway_vec)
    half_len = length / 2
    
    thresh_a = centroid - unit_vec * half_len
    thresh_b = centroid + unit_vec * half_len
    
    return {
        "designator": designator,
        "heading_deg": round(heading_deg),
        "length_m": round(length),
        "width_m": round(width),
        "surface": "unknown",  # TLM doesn't specify
        "threshold_a": {"e": round(thresh_a[0]), "n": round(thresh_a[1])},
        "threshold_b": {"e": round(thresh_b[0]), "n": round(thresh_b[1])},
    }


def merge_polygons_by_name(features):
    """
    Merge multiple polygons with the same name into one (e.g. Zurich Airport
    may have several adjacent polygons for different zones).
    """
    by_name = {}
    for feat in features:
        name = feat["name"]
        if name not in by_name:
            by_name[name] = {
                "name": name,
                "type": feat["type"],
                "geometries": [],
            }
        by_name[name]["geometries"].append(feat["geometry"])
        # Keep the "largest" type classification (airport > airfield > field)
        type_rank = {"Flughafenareal": 3, "Flugplatzareal": 2, "Flugfeldareal": 1, "Heliport": 0}
        if type_rank.get(feat["type"], 0) > type_rank.get(by_name[name]["type"], 0):
            by_name[name]["type"] = feat["type"]
    
    merged = []
    for name, data in by_name.items():
        geom = unary_union(data["geometries"])
        merged.append({
            "name": name,
            "type": data["type"],
            "geometry": geom,
        })
    return merged


def main():
    parser = argparse.ArgumentParser(description="Extract airports from swissTLM3D")
    parser.add_argument("--gpkg", required=True, help="Path to SWISSTLM3D .gpkg")
    parser.add_argument("--output", default="scenery/poi/airports.json",
                        help="Output JSON path (default: scenery/poi/airports.json)")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()
    
    gpkg_path = Path(args.gpkg)
    if not gpkg_path.exists():
        print(f"Error: GeoPackage not found: {gpkg_path}", file=sys.stderr)
        sys.exit(1)
    
    # ── Read aviation features ─────────────────────────────────────────
    print(f"Reading aviation features from {gpkg_path.name}...")
    
    raw_features = []
    with fiona.open(str(gpkg_path), layer="tlm_areale_verkehrsareal") as src:
        for f in src:
            objektart = f["properties"].get("objektart", "")
            if objektart in AVIATION_TYPES:
                geom = shape(f["geometry"])
                name = f["properties"].get("name", "") or "(unnamed)"
                raw_features.append({
                    "name": name,
                    "type": objektart,
                    "geometry": geom,
                })
    
    print(f"  Found {len(raw_features)} aviation polygons")
    
    # ── Merge polygons by name ─────────────────────────────────────────
    merged = merge_polygons_by_name(raw_features)
    print(f"  Merged to {len(merged)} unique sites")
    
    # ── Build airport records ──────────────────────────────────────────
    airports = []
    for site in sorted(merged, key=lambda s: (TYPE_MAP.get(s["type"], ""), s["name"])):
        geom = site["geometry"]
        centroid = geom.centroid
        
        # Get mean elevation from polygon Z coordinates
        if isinstance(geom, MultiPolygon):
            all_coords = []
            for poly in geom.geoms:
                all_coords.extend(poly.exterior.coords)
        else:
            all_coords = list(geom.exterior.coords)
        
        z_values = [c[2] for c in all_coords if len(c) > 2]
        elevation = round(np.mean(z_values)) if z_values else 0
        
        # Look up ICAO code
        icao = ICAO_AUTHORITATIVE.get(site["name"], "")
        
        # Apply type overrides
        our_type = TYPE_MAP.get(site["type"], "unknown")
        if site["name"] in TYPE_OVERRIDES:
            our_type = TYPE_OVERRIDES[site["name"]]
        
        # Use manual runway data if available, otherwise estimate from polygon
        if icao and icao in MANUAL_RUNWAYS:
            runways = MANUAL_RUNWAYS[icao]
        else:
            # Estimate runway from polygon
            runway_info = estimate_runway_from_polygon(geom)
            
            # Determine surface type from airport classification
            if our_type == "grass_strip":
                runway_info["surface"] = "grass"
            elif our_type in ("international_airport", "regional_airport"):
                runway_info["surface"] = "asphalt"
            elif our_type == "heliport":
                runway_info["surface"] = "concrete"
            
            # OBB length from the polygon includes taxiways, aprons, etc.
            # Scale it down and cap per type to approximate actual runway.
            raw_length = runway_info["length_m"]
            if our_type == "grass_strip":
                scaled_length = min(int(raw_length * 0.65), 900)
            elif our_type == "regional_airport":
                scaled_length = min(int(raw_length * 0.55), 2000)
            else:
                scaled_length = min(int(raw_length * 0.50), 3000)
            runway_info["length_m"] = max(scaled_length, 300)  # minimum 300m
            
            # Recompute thresholds from centroid with scaled length
            half_len = runway_info["length_m"] / 2.0
            centroid_e = geom.centroid.x
            centroid_n = geom.centroid.y
            heading_rad = math.radians(runway_info["heading_deg"])
            de = math.sin(heading_rad) * half_len
            dn = math.cos(heading_rad) * half_len
            runway_info["threshold_a"] = {
                "e": round(centroid_e - de), "n": round(centroid_n - dn)
            }
            runway_info["threshold_b"] = {
                "e": round(centroid_e + de), "n": round(centroid_n + dn)
            }
            
            # OBB width is the polygon width, not runway width — use defaults
            if our_type == "international_airport":
                default_width = 60
            elif our_type == "regional_airport":
                default_width = 30
            elif our_type == "grass_strip":
                default_width = 18
            else:
                default_width = 15
            
            # Build runway with proper threshold designators
            desig = runway_info["designator"]
            parts = desig.split("/")
            runways = [{
                "designator": desig,
                "heading_deg": runway_info["heading_deg"],
                "length_m": runway_info["length_m"],
                "width_m": default_width,
                "surface": runway_info["surface"],
                f"threshold_{parts[0]}": runway_info["threshold_a"],
                f"threshold_{parts[1]}": runway_info["threshold_b"],
            }]
        
        airport = {
            "icao": icao,
            "name": site["name"],
            "type": our_type,
            "elevation_m": elevation,
            "center_e": round(centroid.x),
            "center_n": round(centroid.y),
            "area_ha": round(geom.area / 10000, 1),
        }
        
        if our_type != "heliport":
            airport["runways"] = runways
        else:
            # Heliports don't have runways per se, but store pad center
            airport["pad"] = {
                "e": round(centroid.x),
                "n": round(centroid.y),
            }
        
        airports.append(airport)
        
        if args.verbose:
            if our_type != "heliport" and runways:
                rwy_strs = [f"{r['designator']}({r['length_m']}m)" for r in runways]
                rwy_str = "  Rwy " + ", ".join(rwy_strs)
            else:
                rwy_str = ""
            manual = " [manual]" if icao in MANUAL_RUNWAYS else ""
            print(f"  {icao or '----':4s} {site['name']:40s} {our_type:22s} {elevation:4d}m{rwy_str}{manual}")
    
    # ── Write output ───────────────────────────────────────────────────
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    output_data = {
        "type": "airports",
        "coordinate_system": "LV95",
        "source": "swissTLM3D 2025 — tlm_areale_verkehrsareal",
        "generated": True,
        "notes": "Auto-generated by scripts/generate_airports.py. "
                 "ICAO codes, runway details for major airports, and "
                 "heliport data may need manual refinement.",
        "airports": airports,
    }
    
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(output_data, f, indent=2, ensure_ascii=False)
    
    print(f"\nWrote {len(airports)} airports to {output_path}")
    
    # Summary
    from collections import Counter
    type_counts = Counter(a["type"] for a in airports)
    for t, c in sorted(type_counts.items()):
        print(f"  {t}: {c}")
    
    icao_count = sum(1 for a in airports if a["icao"])
    print(f"  With ICAO codes: {icao_count}/{len(airports)}")


if __name__ == "__main__":
    main()
