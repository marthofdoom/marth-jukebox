#!/usr/bin/env python3
"""MarthJukebox.esp generator: an ESL-flagged plugin with K = 24 single-track music
records (MUST), the slot wrappers MJ_SLOT_00 .. MJ_SLOT_23.

Run:  python3 -I tools/make_esp.py [out.esp]      (default: assets/MarthJukebox.esp)

DOCTRINE (same as MFO_GenerateESP.py): never trust format docs, dump a working
record and mirror it. The record layout below is the layout of vanilla
single-track MUST records parsed out of Skyrim.esm (203 of the 258 MUST records
are exactly EDID, CNAM, ANAM). Single tracks carry NO FLTV; FLTV (duration)
exists only on silence (0xA1A9C4D5) and palette (0x23F678C3) tracks, so none is
written here. After writing, the file is re-parsed with the same parser and its
structure is diffed against the vanilla record (when Skyrim.esm is present).

FormIDs: one master (Skyrim.esm) => own-file prefix 0x01; ESL range 0x800-0xFFF.
  0x800 + NN  MJ_SLOT_NN -> Data\\Music\\Harmony\\slot_NN.wav   (NN = 00 .. 23)
native/retarget/SlotAssign.h (kSlotCount, kFirstLocalId) hard-codes K and the first local id. The mod ships
no audio: the slot files are written at runtime by the plugin (docs/INTEGRATION-1.0.md A.1/A.3).
"""

import os
import struct
import sys
import zlib

SKYRIM_ESM = "/mnt/gaming/Steam/steamapps/common/Skyrim Special Edition/Data/Skyrim.esm"
FORM_VERSION = 44          # same as MFO's shipped generator (Skyrim.esm MUST are 40)
OWN = 0x01000000           # Skyrim.esm is master 0 => this file is index 1
TRACK_SINGLE = 0x6ED7E048  # BSIMusicTrack::TrackType::kSingleTrack (CNAM)

# (local id, editor id, ANAM path). The "Data\" prefix is what vanilla uses
# (e.g. MUSExploreDay09 = "Data\Music\Explore\MUS_Explore_Day_09.wav"); 210 other
# vanilla records use "\Data\Music\...". Both exist in Skyrim.esm.
SLOTS = 24
TRACKS = [(0x800 + n, "MJ_SLOT_%02d" % n, "Data\\Music\\Harmony\\slot_%02d.wav" % n) for n in range(SLOTS)]
NEXT_OBJECT_ID = 0x800 + SLOTS


def zstr(s):
    return s.encode("ascii") + b"\x00"   # ASCII only


def subrec(t, d):
    return t.encode("ascii") + struct.pack("<H", len(d)) + d


def record(t, fid, flags, body):
    return (t.encode("ascii") + struct.pack("<III", len(body), flags, fid)
            + struct.pack("<I", 0) + struct.pack("<HH", FORM_VERSION, 0) + body)


def group(label, data):
    return (b"GRUP" + struct.pack("<I", 24 + len(data)) + label.encode("ascii")
            + struct.pack("<iII", 0, 0, 0) + data)


def make_tes4(num_records):
    # flags 0x200 = ESL. Honest only while every local id is in 0x800-0xFFF.
    hedr = struct.pack("<fII", 1.70, num_records, NEXT_OBJECT_ID)
    body = (subrec("HEDR", hedr) + subrec("CNAM", zstr("marth"))
            + subrec("SNAM", zstr("marth's Jukebox slot wrappers (no audio shipped)"))
            + subrec("MAST", zstr("Skyrim.esm")) + subrec("DATA", struct.pack("<Q", 0)))
    return record("TES4", 0, 0x00000200, body)


def make_must(local_id, edid, path):
    body = (subrec("EDID", zstr(edid)) + subrec("CNAM", struct.pack("<I", TRACK_SINGLE))
            + subrec("ANAM", zstr(path)))
    return record("MUST", OWN | local_id, 0, body)


def build():
    must = b"".join(make_must(*t) for t in TRACKS)
    # numRecords counts records + groups after TES4: K MUST + 1 GRUP
    return make_tes4(len(TRACKS) + 1) + group("MUST", must)


# ---- parser (also used on Skyrim.esm for the vanilla reference) ----

def parse_subs(body):
    o, out = 0, []
    while o < len(body):
        t = body[o:o + 4].decode("ascii")
        n = struct.unpack_from("<H", body, o + 4)[0]
        out.append((t, body[o + 6:o + 6 + n]))
        o += 6 + n
    return out


def walk(d, o, end, out):
    """Append (sig, flags, formid, version, raw_header, subrecords) per record."""
    while o < end:
        sig = d[o:o + 4].decode("ascii")
        if sig == "GRUP":
            sz = struct.unpack_from("<I", d, o + 4)[0]
            walk(d, o + 24, o + sz, out)
            o += sz
        else:
            sz, fl, fid = struct.unpack_from("<III", d, o + 4)
            ver = struct.unpack_from("<H", d, o + 20)[0]
            if sig in ("TES4", "MUST"):   # only these are needed; others are skipped
                body = d[o + 24:o + 24 + sz]
                if fl & 0x40000:
                    body = zlib.decompress(body[4:])
                out.append((sig, fl, fid, ver, parse_subs(body)))
            o += 24 + sz


def parse(path):
    d = open(path, "rb").read()
    out = []
    walk(d, 0, len(d), out)
    return out


def shape(subs):
    return [(t, len(v)) for t, v in subs]


def validate(out_path):
    ours = parse(out_path)
    tes4 = ours[0]
    ok = True
    print("--- re-parse of", out_path)
    print("TES4 flags=0x%X subs=%s" % (tes4[1], shape(tes4[4])))
    masters = [v for t, v in tes4[4] if t == "MAST"]
    ok &= tes4[1] == 0x200 and masters == [b"Skyrim.esm\x00"]
    for sig, fl, fid, ver, subs in ours[1:]:
        print("%s id=%08X flags=0x%X ver=%d %s" % (sig, fid, fl, ver, shape(subs)))
        ok &= (fid & 0xFFFFFF) in range(0x800, 0x1000) and fid >> 24 == 1
    if not os.path.exists(SKYRIM_ESM):
        print("Skyrim.esm not found; vanilla comparison skipped")
        return ok
    van = parse(SKYRIM_ESM)
    # the vanilla reference: a plain single-track MUST (EDID, CNAM, ANAM only)
    ref = next(r for r in van if r[0] == "MUST" and r[1] == 0
               and [t for t, _ in r[4]] == ["EDID", "CNAM", "ANAM"]
               and dict(r[4])["CNAM"] == struct.pack("<I", TRACK_SINGLE))
    print("--- vanilla reference MUST %08X %s" % (ref[2], dict(ref[4])["EDID"]))
    print("vanilla  flags=0x%X subs=%s CNAM=%s" % (ref[1], shape(ref[4]), dict(ref[4])["CNAM"].hex()))
    for sig, fl, fid, ver, subs in ours[1:]:
        s_ok = [t for t, _ in subs] == [t for t, _ in ref[4]]
        c_ok = dict(subs)["CNAM"] == dict(ref[4])["CNAM"]
        f_ok = fl == ref[1]
        print("ours %08X: subrecord order/types %s vanilla, CNAM %s, record flags %s"
              % (fid, "==" if s_ok else "!=", "==" if c_ok else "!=", "==" if f_ok else "!="))
        ok &= s_ok and c_ok and f_ok
    return ok


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "assets", "MarthJukebox.esp")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    data = build()
    with open(out_path, "wb") as f:
        f.write(data)
    print("Written: %s (%d bytes)" % (out_path, len(data)))
    for lid, edid, path in TRACKS:
        print("  MUST %08X (local 0x%03X) %s -> %s" % (OWN | lid, lid, edid, path))
    if not validate(out_path):
        print("VALIDATION FAILED")
        sys.exit(1)
    print("VALIDATION OK")


if __name__ == "__main__":
    main()
