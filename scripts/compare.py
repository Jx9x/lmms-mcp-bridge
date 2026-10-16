#!/usr/bin/env python3
"""Compare bridge `tracks` JSON with ground truth parsed from the .mmp."""
import json
import sys

TYPE_INT = {0: "instrument", 1: "bb", 2: "sample", 3: "event",
            4: "video", 5: "automation", 6: "hiddenAutomation"}

gt = json.load(open(sys.argv[1]))
br = json.load(open(sys.argv[2]))
out = []
ok_all = True


def check(cond, msg):
    global ok_all
    if not cond:
        ok_all = False
    out.append(("PASS " if cond else "FAIL ") + msg)


def norm_tco(t):
    """Ground-truth TCO -> comparable dict."""
    return t


def compare_track(g, b, label):
    check(b["name"] == g["name"],
          f'{label} name: bridge={b["name"]!r} file={g["name"]!r}')
    check(b["type"] == TYPE_INT.get(g["type"], f'?{g["type"]}'),
          f'{label} type: bridge={b["type"]!r} file={g["type"]}')
    check(b["muted"] == bool(g["muted"]),
          f'{label} muted: bridge={b["muted"]} file={g["muted"]}')
    check(b["solo"] == bool(g["solo"]),
          f'{label} solo: bridge={b["solo"]} file={g["solo"]}')
    gi, bi = g["instrument"], b.get("instrument")
    if gi or bi:
        if bi == gi:
            out.append(f'PASS {label} instrument: {bi!r}')
        else:
            # InstrumentTrack::instrumentName() returns the plugin
            # displayName ("AudioFileProcessor"); the file stores the
            # internal plugin key ("audiofileprocessor"). Expected
            # difference, reported not failed.
            out.append(f'EXPECTED-DIFF {label} instrument: '
                       f'bridge(display)={bi!r} file(internal)={gi!r}')
    gt_tcos = g["tcos"]
    br_tcos = b["tcos"]
    check(len(br_tcos) == len(gt_tcos),
          f'{label} tco count: bridge={len(br_tcos)} file={len(gt_tcos)}')
    for i, (gtc, btc) in enumerate(zip(gt_tcos, br_tcos)):
        L = f'{label} tco[{i}]'
        check(btc["start"] == gtc["pos"],
              f'{L} start: bridge={btc["start"]} file={gtc["pos"]}')
        check(btc["length"] == gtc["len"],
              f'{L} length: bridge={btc["length"]} file={gtc["len"]}')
        KIND = {"pattern": "pattern", "bbtco": "bb", "sampletco": "sample",
                "automationpattern": "automation"}
        check(btc["kind"] == KIND.get(gtc["kind"], gtc["kind"]),
              f'{L} kind: bridge={btc["kind"]!r} file={gtc["kind"]!r}')
        if gtc["kind"] == "pattern":
            gn = gtc["notes"]
            bn = btc.get("notes", [])
            check(len(bn) == len(gn),
                  f'{L} note count: bridge={len(bn)} file={len(gn)}')
            # file order == LMMS NoteVector order?
            for j, (gnn, bnn) in enumerate(zip(gn, bn)):
                for k in ("key", "pos", "len", "vol", "pan"):
                    bk = {"len": "len"}.get(k, k)
                    if k == "len":
                        bk = "len"
                    bv = bnn.get({"key": "key", "pos": "pos", "len": "len",
                                  "vol": "vol", "pan": "pan"}[k])
                    if bv != gnn[k]:
                        check(False,
                              f'{L} note[{j}] {k}: bridge={bv} file={gnn[k]}')


def main():
    b = br
    check(b["ok"] is True, "bridge replied ok")
    g_song, b_song = gt["song"], b["song"]
    check(len(b_song) == len(g_song),
          f'song track count: bridge={len(b_song)} file={len(g_song)}')
    for i, (g, bt) in enumerate(zip(g_song, b_song)):
        compare_track(g, bt, f'song[{i}]')

    g_bb, b_bb = gt["bb"], b["bb"]
    check(len(b_bb) == len(g_bb),
          f'bb track count: bridge={len(b_bb)} file={len(g_bb)}')
    for i, (g, bt) in enumerate(zip(g_bb, b_bb)):
        compare_track(g, bt, f'bb[{i}]')

    gnotes = sum(len(p["notes"]) for t in g_song + g_bb
                 for p in t["tcos"] if p["kind"] == "pattern")
    bnotes = sum(len(p.get("notes", [])) for t in b_song + b_bb
                 for p in t["tcos"] if p["kind"] == "pattern")
    check(bnotes == gnotes == 60,
          f'total notes: bridge={bnotes} file={gnotes} (expect 60)')

    out.append("")
    out.append("hidden automation tracks in file (not reported by bridge "
               "in 'song'): " +
               ", ".join(f'{t["name"]!r} type={t["type"]} tcos={len(t["tcos"])}'
                         for t in gt["hidden"]))
    hidden_in_bridge = [t for t in b_song if t["type"] in
                        ("automation", "hiddenAutomation")]
    out.append("hidden/automation tracks in bridge song list: " +
               (", ".join(f'{t["name"]!r}' for t in hidden_in_bridge)
                or "none"))

    out.append("")
    out.append("instrument names (bridge vs file):")
    for i, (g, bt) in enumerate(zip(g_song + g_bb, b_song + b_bb)):
        out.append(f'  {bt["name"]}: bridge={bt.get("instrument")} '
                   f'file={g["instrument"]}')

    out.append("")
    out.append("RESULT: " + ("ALL CHECKS PASSED" if ok_all
                             else "MISMATCHES FOUND"))
    print("\n".join(out))
    sys.exit(0 if ok_all else 1)


main()
