#!/usr/bin/env python3
"""dedupe.py — collapse identical recordings across albums into a unique pool.

Stage 2. Reads catalog.json (from catalog_songs.py) and groups tracks that are
the SAME recording even when they carry different filenames/albums, producing:

    unique.json  = the canonical pool (one entry per distinct recording) plus,
                   for each canonical, the list of album/path aliases that map
                   to it. convert_dsp.py turns the pool into dsp/*.dsp and uses
                   the alias map to build albums.json.

Match rule (composition-level = title+content, but content-first):
  two tracks are the SAME recording iff
      |dur_a - dur_b| <= DUR_GATE           (duration gate)
    AND  mel-fingerprint correlation >= CORR_THRESH

Correlation on the zero-mean/unit-norm mel fingerprints is just their dot
product. Chroma was rejected earlier (over-merged distinct tunes); mel bands +
the duration gate are discriminative.

Canonical pick: prefer the earliest release in ALBUM_ORDER, then the longest
take, then the shortest filename — deterministic so reruns are stable.
"""
import os
import json
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))

DUR_GATE = 2.5       # seconds; same recording rarely differs by more
CORR_THRESH = 0.90   # mel fingerprint dot-product threshold

# Preference when choosing which alias is the canonical pool entry.
ALBUM_ORDER = ["Tempest 2000", "Tempest 3000", "TxK", "Space Giraffe", "Tempest 4000"]


def load_catalog():
    with open(os.path.join(HERE, "catalog.json")) as f:
        cat = json.load(f)
    entries = cat["entries"]
    fps = np.array([e["fp"] for e in entries], dtype=np.float32) if entries else np.zeros((0, 1))
    return entries, fps


def canonical_key(e):
    album_rank = ALBUM_ORDER.index(e["album"]) if e["album"] in ALBUM_ORDER else len(ALBUM_ORDER)
    return (album_rank, -e["duration"], len(e["title"]), e["title"])


def main():
    entries, fps = load_catalog()
    n = len(entries)
    if n == 0:
        print("catalog.json empty — run catalog_songs.py first (and put the FLACs back)")
        return

    # Union-find over tracks connected by the match rule.
    parent = list(range(n))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i, j):
        ri, rj = find(i), find(j)
        if ri != rj:
            parent[ri] = rj

    dur = np.array([e["duration"] for e in entries])
    # correlation matrix via one matmul (fingerprints are unit-norm)
    corr = fps @ fps.T
    for i in range(n):
        for j in range(i + 1, n):
            if abs(dur[i] - dur[j]) <= DUR_GATE and corr[i, j] >= CORR_THRESH:
                union(i, j)

    # group -> member indices
    groups = {}
    for i in range(n):
        groups.setdefault(find(i), []).append(i)

    pool = []
    for members in groups.values():
        members.sort(key=lambda i: canonical_key(entries[i]))
        canon = entries[members[0]]
        aliases = [{"album": entries[m]["album"], "path": entries[m]["path"],
                    "title": entries[m]["title"]} for m in members]
        pool.append({
            "title": canon["title"],
            "album": canon["album"],
            "path": canon["path"],
            "duration": canon["duration"],
            "aliases": aliases,
        })

    pool.sort(key=lambda p: (ALBUM_ORDER.index(p["album"]) if p["album"] in ALBUM_ORDER
                             else len(ALBUM_ORDER), p["title"]))

    out = os.path.join(HERE, "unique.json")
    with open(out, "w") as f:
        json.dump({"pool": pool}, f, indent=2)
    dup = n - len(pool)
    print(f"{n} tracks -> {len(pool)} unique ({dup} duplicates collapsed)")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
