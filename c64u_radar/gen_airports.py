#!/usr/bin/env python3
"""Generate airports.h from airports_cache.json — ICAO code → lat/lon for C64.

Picks the ~200 busiest/best-known airports sorted by ICAO code for binary search.
Lat/lon stored as integer degrees × 1000000 (6 decimal places).
ICAO code stored as uint32 big-endian ASCII.
"""
import json

with open("../executables/server_bundle/airports_cache.json") as f:
    cache = json.load(f)
airports = cache["airports"]

MAJOR = [
    "KATL","KLAX","KORD","KDFW","KDEN","KJFK","KSFO","KSEA","KLAS",
    "KMCO","KMIA","KPHX","KEWR","KMSP","KDTW","KPHL","KBOS","KLGA",
    "KIAD","KDCA","KBWI","KSLC","KTPA","KFLL","KSAN","KPDX","KSTL",
    "KANC","KABQ","KOAK","KSJC","KAUS","KDAL","KHOU","KIAH","KMCI",
    "KIND","KCVG","KCLE","KCMH","KMEM","KBNA","KCLT","KRDU","KJAX",
    "KPBI","KSNA","KBUR","KLGB","KONT","KPSP","KSMF","KRNO","KGEG",
    "KBOI","KELP","KTUS","KTUL","KMSY","KBHM","KGSO","KORF","KSAV",
    "KMDW","CYYZ","CYVR","CYUL","CYYC","CYEG","CYOW","CYHZ","CYWG","CYQB",
    "EGLL","EGKK","EGPH","EGCC","EGBB","EGGD","EGFF","EGSS","EGNX",
    "EGPF","EGAA","EGGW","EGTE","EGMC",
    "LFPG","LFPO","LFMN","LFLL","LFML","LFBO","LFSB",
    "LPPT","LPPR","LEMD","LEBL","LEMG","LEPA","LEAL","LEVC",
    "EDDF","EDDM","EDDK","EDDL","EDDS","EDDH","EDDP","EDDT","EDDV","EDDW","EDDG",
    "EHAM","EHRD","EHEH","LIMC","LIRF","LIPZ","LIRN","LIML","LIPE",
    "LOWW","LOWS","LOWI","LOWG","LROP","LRCL","EPWA","EPKK","EPKT",
    "LKPR","LDZA","LDSP","LDDU","LYBE","LBSF","LGRP","LGIR","LGTS",
    "UUDD","UUEE","ULLI","URSS","UAAA","UTTT",
    "RJAA","RJTT","RJBB","RJCC","RJOO","RJNS","RKSI","RKSS","RKPC",
    "ZSSS","ZSPD","ZBAA","ZGGG","ZGSZ","ZHCC","ZHHH","ZUCK","ZUUU",
    "ZPPP","ZGHA","ZSHC","VHHH","VMMC","VTBS","VVTS","VVNB","WSSS",
    "WIII","WADD","WMKK","RPLL","RPVM","VOBL","VOMM","VABB","VIDP",
    "OMDB","OMAA","OTHH","OKBK","OEDF","OIIE","OIII","LLBG","OLBA","OSTT",
    "FAOR","FACT","HECA","DAAG","DTTA","GMMN","GOOY","DNAA","DNMM",
    "YSSY","YMML","YBBN","YPPH","YPAD","YBCS","YPDN","YSCB",
    "NZAA","NZCH","NZWN",
    "SBGR","SBGL","SBBR","SBCF","SBSP","SBRF","SBSV","SBPA","SBEG",
    "SAEZ","SABE","SCEL","SCIP","SKBO","SKRG","SPJC",
    "SVBC","SVMI","SUMU",
]

seen = set()
table = []
for code in MAJOR:
    if code in airports and code not in seen:
        seen.add(code)
        lat, lon = airports[code]
        lat_i = int(round(lat * 1000000))
        lon_i = int(round(lon * 1000000))
        code_val = (ord(code[0]) << 24) | (ord(code[1]) << 16) | (ord(code[2]) << 8) | ord(code[3])
        table.append((code_val, lat_i, lon_i, code))

table.sort(key=lambda x: x[0])

with open("airports.h", "w") as f:
    f.write("#ifndef AIRPORTS_H\n")
    f.write("#define AIRPORTS_H\n")
    f.write("\n")
    f.write("/* ICAO airport code -> latitude/longitude lookup table */\n")
    f.write("/* Generated from OurAirports data (public domain) */\n")
    f.write("/* Lat/lon stored as integer degrees * 1000000 (6 decimal places) */\n")
    f.write("/* ICAO code packed as uint32 big-endian ASCII */\n")
    f.write(f"/* {len(table)} entries, sorted for binary search */\n")
    f.write("\n")
    f.write("#include <stdint.h>\n")
    f.write("\n")
    f.write("typedef struct { uint32_t code; int32_t lat; int32_t lon; } AirportEntry;\n")
    f.write("\n")
    f.write(f"#define AIRPORT_COUNT {len(table)}\n")
    f.write("\n")
    f.write("static const AirportEntry airports[AIRPORT_COUNT] = {\n")
    for code_val, lat, lon, codestr in table:
        f.write(f"    {{ 0x{code_val:08X}, {lat:+d}, {lon:+d} }},  /* {codestr} */\n")
    f.write("};\n")
    f.write("\n")
    f.write("#endif\n")

print(f"Generated airports.h with {len(table)} entries")
