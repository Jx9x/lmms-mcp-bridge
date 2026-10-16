#!/usr/bin/env python3
"""Parse an .mmp project into ground-truth JSON for bridge comparison.

Rules per spec: head attrs; song tracks in document order under
/lmms-project/song/trackcontainer[@type='song']; BB tracks nested in the
first track's bbtrack/trackcontainer[@type='bbtrackcontainer']; hidden
automation track = /lmms-project/song/track[@type='6'] recorded
separately. Missing attributes -> null (no defaults).
"""
import json
import sys
import xml.etree.ElementTree as ET


def attr(el, name):
    v = el.get(name)
    return int(v) if v is not None else None


def parse_note(el):
    return {
        "key": attr(el, "key"),
        "pos": attr(el, "pos"),
        "len": attr(el, "len"),
        "vol": attr(el, "vol"),
        "pan": attr(el, "pan"),
    }


def parse_tco(el):
    tag = el.tag
    if tag == "pattern":
        t = {
            "kind": "pattern",
            "pos": attr(el, "pos"),
            "len": attr(el, "len"),
            "type": attr(el, "type"),
            "steps": attr(el, "steps"),
            "name": el.get("name"),
            "muted": attr(el, "muted"),
            "notes": [parse_note(n) for n in el.findall("note")],
        }
        return t
    if tag == "bbtco":
        return {"kind": "bbtco", "pos": attr(el, "pos"),
                "len": attr(el, "len")}
    if tag == "sampletco":
        return {"kind": "sampletco", "pos": attr(el, "pos"),
                "len": attr(el, "len"), "name": el.get("name"),
                "muted": attr(el, "muted")}
    if tag == "automationpattern":
        return {"kind": "automationpattern", "pos": attr(el, "pos"),
                "len": attr(el, "len"), "name": el.get("name"),
                "muted": attr(el, "muted")}
    return {"kind": "other:" + tag, "pos": attr(el, "pos"),
            "len": attr(el, "len")}


TCO_TAGS = ("pattern", "bbtco", "sampletco", "automationpattern")


def parse_track(el):
    instr = None
    it = el.find("instrumenttrack")
    if it is not None:
        ins = it.find("instrument")
        if ins is not None:
            instr = ins.get("name")
    tcos = [parse_tco(c) for c in el if c.tag in TCO_TAGS]
    return {
        "name": el.get("name"),
        "type": attr(el, "type"),
        "muted": attr(el, "muted"),
        "solo": attr(el, "solo"),
        "instrument": instr,
        "tcos": tcos,
    }


def main():
    src, dst = sys.argv[1], sys.argv[2]
    tree = ET.parse(src)
    root = tree.getroot()

    head = root.find("head")
    out = {
        "head": {
            "bpm": attr(head, "bpm"),
            "timesig_numerator": attr(head, "timesig_numerator"),
            "timesig_denominator": attr(head, "timesig_denominator"),
            "mastervol": attr(head, "mastervol"),
            "masterpitch": attr(head, "masterpitch"),
        },
        "song": [],
        "bb": [],
        "hidden": [],
    }

    song = root.find("song")
    song_tc = song.find("trackcontainer[@type='song']")
    song_tracks = song_tc.findall("track")

    for tr in song_tracks:
        out["song"].append(parse_track(tr))
        bb_tc = tr.find("bbtrack/trackcontainer[@type='bbtrackcontainer']")
        if bb_tc is not None and not out["bb"]:
            for btr in bb_tc.findall("track"):
                out["bb"].append(parse_track(btr))

    for tr in song.findall("track[@type='6']"):
        out["hidden"].append(parse_track(tr))

    with open(dst, "w") as f:
        json.dump(out, f, indent=1)
    nnotes = sum(len(p["notes"])
                 for t in out["song"] + out["bb"]
                 for p in t["tcos"] if p["kind"] == "pattern")
    print("song tracks:", len(out["song"]),
          "| bb tracks:", len(out["bb"]),
          "| hidden:", len(out["hidden"]),
          "| notes:", nnotes)


if __name__ == "__main__":
    main()
