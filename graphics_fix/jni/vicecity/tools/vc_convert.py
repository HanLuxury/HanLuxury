#!/usr/bin/env python3
"""Turns the SA-MP 0.3.DL Vice City map (github.com/casualmind/samp-vice-city) into the tables the client
compiles in: vicecity/VcMapData.gen.cpp.

    python3 vc_convert.py --repo <checkout of samp-vice-city> --out ../VcMapData.gen.cpp [--report report.txt]

What is taken from where:
  filterscripts/vice_city.pwn   model list (AddVC2SASimpleObject), placements (CreateVCObject), the stream
                                distances per placement type, the table that maps IDE flags to a base model
  models/vice_city/*.dff        bounds: the collision SA-MP embeds in the clump (chunk 0x253F2FF) joined with
                                the geometry itself (a few models have an empty or too small collision box)

Only the file names go into the table: the client reads the models themselves from the device at run time.
Nothing is written into the repository. Standard library only.
"""
import argparse
import os
import re
import struct
import sys

SAMP_COLLISION = 0x253F2FF
TYPES = ["MODEL_TYPE_NONE", "MODEL_TYPE_LANDMASSES", "MODEL_TYPE_BUILDINGS", "MODEL_TYPE_OBJECTS",
         "MODEL_TYPE_VEGETATION", "MODEL_TYPE_INTERIORS", "MODEL_TYPE_2DFX"]

MODEL_RE = re.compile(r'AddVC2SASimpleObject\(\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*'
                      r'(?:,\s*(-?\d+)\s*,\s*(-?\d+)\s*)?\)')
NUM = r'([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)'
OBJ_RE = re.compile(r'CreateVCObject\(\s*(MODEL_TYPE_\w+)\s*,\s*(-?\d+)\s*,\s*' + r'\s*,\s*'.join([NUM] * 6) +
                    r'\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\)')
MAT_RE = re.compile(r'^\s*(\d+)\s*,\s*(-?\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(0x[0-9A-Fa-f]+|-?\d+)\s*\)\s*;')
STREAM_RE = re.compile(r'\{\s*(true|false)\s*,\s*' + NUM + r'\s*,\s*(\d+)\s*\}')


def fail(message):
    sys.stderr.write("vc_convert: " + message + "\n")
    sys.exit(1)


# ---------------------------------------------------------------------------------------------------------
# vice_city.pwn

def int_array(text, name):
    m = re.search(r'new\s+%s\s*\[\s*\d+\s*\]\s*=\s*\{([^}]*)\}' % re.escape(name), text)
    if not m:
        fail("array %s not found in the script" % name)
    return [int(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]


def parse_script(path):
    text = open(path, encoding="latin-1").read()
    lines = text.splitlines()

    base_flags = int_array(text, "modelflags_flags")
    base_ids = int_array(text, "modelflags_index")
    if len(base_flags) != len(base_ids):
        fail("modelflags_flags and modelflags_index differ in length")

    m = re.search(r'new\s+Vice_Stream_Info[^=]*=\s*\{(.*?)\};', text, re.S)
    if not m:
        fail("Vice_Stream_Info not found in the script")
    stream = [(s == "true", float(d), int(p)) for s, d, p in STREAM_RE.findall(m.group(1))]
    if len(stream) != len(TYPES):
        fail("Vice_Stream_Info has %d rows, expected %d" % (len(stream), len(TYPES)))

    models, placements, problems = [], [], []
    where = None
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        if s.startswith("AddVcModels()"):
            where = "models"
        elif s.startswith("CreateVCObjects()"):
            where = "objects"
        elif s.startswith("stock ") or s.startswith("AddVC2SASimpleObject(flags") or s.startswith("CreateVCObject(MODEL_TYPES"):
            where = None
        if where == "models" and "AddVC2SASimpleObject" in line:
            m = MODEL_RE.search(line)
            if not m:
                problems.append("line %d: model line not understood" % (i + 1))
            else:
                flags, mid, dff, txd, on, off = m.groups()
                models.append({"flags": int(flags) & 0xFFFFFFFF, "id": int(mid), "dff": dff, "txd": txd,
                               "on": int(on or 0), "off": int(off or 0), "line": i + 1})
        elif where == "objects" and "CreateVCObject(" in line:
            m = OBJ_RE.search(line)
            if not m:
                problems.append("line %d: placement line not understood" % (i + 1))
            else:
                g = m.groups()
                p = {"type": TYPES.index(g[0]), "model": int(g[1]), "num": list(g[2:8]),
                     "world": int(g[8]), "interior": int(g[9]), "line": i + 1}
                if "SetDynamicObjectMaterial(" in line:
                    m2 = MAT_RE.search(lines[i + 1]) if i + 1 < len(lines) else None
                    if not m2:
                        problems.append("line %d: material line not understood" % (i + 2))
                    else:
                        index, mmodel, mtxd, mtex, colour = m2.groups()
                        p["material"] = {"index": int(index), "model": int(mmodel), "txd": mtxd, "tex": mtex,
                                         "color": int(colour, 0) & 0xFFFFFFFF}
                        i += 1
                placements.append(p)
        i += 1
    return {"models": models, "placements": placements, "problems": problems, "stream": stream,
            "base": dict(zip(base_flags, base_ids))}


# ---------------------------------------------------------------------------------------------------------
# DFF: bounds of the geometry and of the embedded collision

def rw_version(library_id):
    if library_id & 0xFFFF0000:
        return (((library_id >> 14) & 0x3FF00) + 0x30000) | ((library_id >> 16) & 0x3F)
    return library_id << 8


def children(data, start, end):
    """(type, body offset, body size, library id) of the chunks in [start, end)."""
    at = start
    while at + 12 <= end:
        ctype, size, lib = struct.unpack_from("<III", data, at)
        if at + 12 + size > end:
            return
        yield ctype, at + 12, size, lib
        at += 12 + size


def geometry_box(data, body, size, lib):
    """Bounding box of the first morph target, or None."""
    try:
        fmt, ntri, nvert, nmorph = struct.unpack_from("<IIII", data, body)
        p = body + 16
        if rw_version(lib) < 0x34000:
            p += 12
        if fmt & 0x01000000:          # native geometry: no vertices here
            return None
        if fmt & 0x08:
            p += 4 * nvert
        sets = (fmt >> 16) & 0xFF
        if sets == 0:
            sets = 2 if fmt & 0x80 else (1 if fmt & 0x04 else 0)
        p += 8 * nvert * sets + 8 * ntri
        if nmorph < 1 or p + 24 > body + size:
            return None
        has_vertices = struct.unpack_from("<I", data, p + 16)[0]
        p += 24
        if not has_vertices or nvert == 0 or p + 12 * nvert > body + size:
            return None
        v = struct.unpack_from("<%df" % (3 * nvert), data, p)
        xs, ys, zs = v[0::3], v[1::3], v[2::3]
        return [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]
    except struct.error:
        return None


def collision_box(col):
    """Bounding box in a COL file (any version), or None."""
    if len(col) < 0x20 + 40:
        return None
    fourcc = col[:4]
    if fourcc == b"COLL":
        radius, cx, cy, cz, x0, y0, z0, x1, y1, z1 = struct.unpack_from("<10f", col, 0x20)
        return [x0, y0, z0, x1, y1, z1]
    if fourcc in (b"COL2", b"COL3", b"COL4"):
        return list(struct.unpack_from("<6f", col, 0x20))
    return None


def scan_dff(path):
    data = open(path, "rb").read()
    out = {"size": len(data), "geometry": None, "collision": None, "col_kind": "", "col_size": 0, "col_faces": 0}
    if len(data) < 12:
        return out
    ctype, size, lib = struct.unpack_from("<III", data, 0)
    if ctype != 0x10 or 12 + size > len(data):
        return out
    for t, body, sz, lb in children(data, 12, 12 + size):
        if t == 0x1A:                                   # geometry list
            for t2, body2, sz2, lb2 in children(data, body, body + sz):
                if t2 != 0x0F:
                    continue
                for t3, body3, sz3, lb3 in children(data, body2, body2 + sz2):
                    if t3 == 0x01 and out["geometry"] is None:
                        out["geometry"] = geometry_box(data, body3, sz3, lb3)
                    break
        elif t == 0x03:                                 # extension of the clump
            for t2, body2, sz2, lb2 in children(data, body, body + sz):
                if t2 == SAMP_COLLISION:
                    col = data[body2:body2 + sz2]
                    out["collision"] = collision_box(col)
                    out["col_kind"] = col[:4].decode("latin-1", "replace")
                    out["col_size"] = sz2
                    if col[:4] == b"COL3" and len(col) >= 0x78:
                        out["col_faces"] = struct.unpack_from("<H", col, 0x20 + 0x2C)[0]
    return out


def finite(box):
    return box is not None and all(abs(v) < 1.0e6 and v == v for v in box)


def join(a, b):
    if not finite(a):
        a = None
    if not finite(b):
        b = None
    if a is None or all(v == 0.0 for v in a):
        a, b = b, None if a is None else a
        if a is None:
            return None
        if b is not None and all(v == 0.0 for v in b):
            b = None
    if b is None or all(v == 0.0 for v in b):
        return list(a)
    return [min(a[i], b[i]) for i in range(3)] + [max(a[i], b[i]) for i in range(3, 6)]


# ---------------------------------------------------------------------------------------------------------
# output

def c_string(s):
    out = []
    for ch in s:
        if ch in '\\"':
            out.append("\\" + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            fail("file name with a character outside ASCII: %r" % s)
    return '"' + "".join(out) + '"'


def c_float(text_or_value):
    """A float literal. Numbers from the script keep their own digits."""
    if isinstance(text_or_value, str):
        t = text_or_value
        float(t)   # must parse
        if t.startswith("+"):
            t = t[1:]
        if "." not in t and "e" not in t and "E" not in t:
            t += ".0"
        if t.startswith("."):
            t = "0" + t
        if t.startswith("-."):
            t = "-0" + t[1:]
        if t.endswith("."):
            t += "0"
        return t + "f"
    return "%sf" % repr(round(float(text_or_value), 6))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", required=True, help="checkout of github.com/casualmind/samp-vice-city")
    ap.add_argument("--out", required=True, help="VcMapData.gen.cpp to write")
    ap.add_argument("--report", help="text report to write (anomalies, statistics)")
    args = ap.parse_args()

    script_path = os.path.join(args.repo, "filterscripts", "vice_city.pwn")
    model_dir = os.path.join(args.repo, "models", "vice_city")
    if not os.path.isfile(script_path) or not os.path.isdir(model_dir):
        fail("%s is not a samp-vice-city checkout" % args.repo)

    script = parse_script(script_path)
    notes = list(script["problems"])
    on_disk = {name.lower(): name for name in os.listdir(model_dir)}

    # --- TXD table: every name the models use, in order of first use
    txd_index = {}
    txd_files = []
    for m in script["models"]:
        key = m["txd"].lower()
        if key not in txd_index:
            txd_index[key] = len(txd_files)
            txd_files.append(key)
            if key not in on_disk:
                notes.append("TXD %s is not in the repository (first used by model %d)" % (m["txd"], m["id"]))

    # --- models
    type_distance = [d for (_static, d, _prio) in script["stream"]]
    by_id = {}
    for m in script["models"]:
        if m["id"] >= 0:
            fail("line %d: model id %d is not negative" % (m["line"], m["id"]))
        if m["id"] in by_id:
            fail("line %d: model id %d is defined twice" % (m["line"], m["id"]))
        by_id[m["id"]] = m
        m["distance"] = 0.0
        m["placed"] = 0
        key = m["dff"].lower()
        if len(key) > 63 or len(m["txd"]) > 63:
            fail("line %d: file name too long" % m["line"])
        info = scan_dff(os.path.join(model_dir, on_disk[key])) if key in on_disk else None
        if info is None:
            notes.append("DFF %s is not in the repository (model %d)" % (m["dff"], m["id"]))
            box = None
        else:
            box = join(info["collision"], info["geometry"])
            if info["col_kind"] == "":
                notes.append("%s: no embedded collision" % m["dff"])
            elif info["col_kind"] != "COL3":
                notes.append("%s: collision is %s, only its bounds are used" % (m["dff"], info["col_kind"]))
            if info["geometry"] is None:
                notes.append("%s: geometry bounds not readable, collision bounds only" % m["dff"])
        if box is None:
            box = [-1.0, -1.0, -1.0, 1.0, 1.0, 1.0]
            notes.append("%s: no usable bounds, a 2 m box is used" % m["dff"])
        for i in range(3):   # never a flat box: the game divides by its size in places
            if box[i + 3] - box[i] < 0.02:
                box[i] -= 0.01
                box[i + 3] += 0.01
        m["box"] = box
        masked = m["flags"] & 0xFFFFFFDF          # FindModelIDForFlags() in the script
        m["exact"] = masked in script["base"]

    # --- placements
    placements = []
    materials = []
    for p in script["placements"]:
        if p["model"] < 0:
            m = by_id.get(p["model"])
            if m is None:
                # Stays in the table: the client has to know the place to recognise the object a server
                # sends for it (open.mp sends a "?" there). Nothing is ever created for it.
                notes.append("line %d: placement of model %d, which the script never defines: nothing is created there"
                             % (p["line"], p["model"]))
            else:
                m["distance"] = max(m["distance"], type_distance[p["type"]])
                m["placed"] += 1
        if p["world"] != -1 or p["interior"] != -1:
            notes.append("line %d: placement limited to world %d / interior %d: placed everywhere"
                         % (p["line"], p["world"], p["interior"]))
        if "material" in p:
            materials.append((len(placements), p["material"]))
        placements.append(p)
    if len(placements) > 0xFFFF:
        fail("more than 65535 placements")
    for m in script["models"]:
        if m["placed"] == 0:
            notes.append("model %d (%s) is never placed" % (m["id"], m["dff"]))
            m["distance"] = type_distance[TYPES.index("MODEL_TYPE_OBJECTS")]

    # --- write
    w = []
    w.append("// Generated by vicecity/tools/vc_convert.py from github.com/casualmind/samp-vice-city. Do not edit:")
    w.append("// run the converter again instead. Layout of the tables: VcMapData.h.")
    w.append("//")
    w.append("// %d models, %d TXD files, %d placements, %d of them with a material."
             % (len(script["models"]), len(txd_files), len(placements), len(materials)))
    w.append('#include "VcMapData.h"')
    w.append("")
    w.append("namespace vc {")
    w.append("")
    w.append("// drawDistance, priority, isStatic: Vice_Stream_Info in the script, one row per placement type")
    w.append("const TypeInfo kTypes[kTypeCount] = {")
    for (static, distance, priority), name in zip(script["stream"], TYPES):
        w.append("    {%s, %d, %s},   // %s" % (c_float(distance), priority, "true" if static else "false", name))
    w.append("};")
    w.append("")
    w.append("const char* const kTxdFiles[] = {")
    for name in txd_files:
        w.append("    %s," % c_string(name))
    w.append("};")
    w.append("const size_t kTxdCount = sizeof(kTxdFiles) / sizeof(kTxdFiles[0]);")
    w.append("")
    w.append("// sampId, ideFlags, dff, txd, timeOn, timeOff, flagsHaveBase, drawDistance, {min xyz, max xyz}")
    w.append("const ModelDef kModels[] = {")
    for m in script["models"]:
        w.append("    {%d, 0x%X, %s, %d, %d, %d, %d, %s, {%s}}," % (
            m["id"], m["flags"], c_string(m["dff"].lower()), txd_index[m["txd"].lower()], m["on"], m["off"],
            1 if m["exact"] else 0, c_float(m["distance"]), ", ".join(c_float(v) for v in m["box"])))
    w.append("};")
    w.append("const size_t kModelCount = sizeof(kModels) / sizeof(kModels[0]);")
    w.append("")
    w.append("// model (SA-MP id), {x, y, z}, {rx, ry, rz} in degrees, type")
    w.append("const Placement kPlacements[] = {")
    for p in placements:
        n = [c_float(t) for t in p["num"]]
        w.append("    {%d, {%s}, {%s}, %d}," % (p["model"], ", ".join(n[:3]), ", ".join(n[3:]), p["type"]))
    w.append("};")
    w.append("const size_t kPlacementCount = sizeof(kPlacements) / sizeof(kPlacements[0]);")
    w.append("")
    w.append("// placement, material index, model, txd, texture, colour (ARGB as SetObjectMaterial takes it)")
    w.append("const MaterialDef kMaterials[] = {")
    for index, mat in materials:
        w.append("    {%d, %d, %d, %s, %s, 0x%08Xu}," % (index, mat["index"], mat["model"], c_string(mat["txd"]),
                                                     c_string(mat["tex"]), mat["color"]))
    if not materials:
        w.append("    {0, 0, 0, \"\", \"\", 0},")
    w.append("};")
    w.append("const size_t kMaterialCount = %d;" % len(materials))
    w.append("")
    w.append("}  // namespace vc")
    w.append("")
    with open(args.out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(w))

    timed = sum(1 for m in script["models"] if m["on"] or m["off"])
    summary = ["models: %d (%d timed)" % (len(script["models"]), timed),
               "txd files: %d" % len(txd_files),
               "placements: %d (%d with a stock SA model)" % (len(placements), sum(1 for p in placements if p["model"] >= 0)),
               "materials: %d" % len(materials),
               "notes: %d" % len(notes)]
    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(summary) + "\n\n" + "\n".join(notes) + "\n")
    print("\n".join(summary))
    for n in notes[:12]:
        print("  note:", n)
    if len(notes) > 12:
        print("  ... %d more (see --report)" % (len(notes) - 12))


if __name__ == "__main__":
    main()
