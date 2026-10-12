#!/usr/bin/env python3
"""download.py — one-shot soundtrack fetch + organize + convert.

Runs the whole audio pipeline in one invocation:

    download.py  ->  <album folders>/*.flac
                 ->  catalog_songs.py  ->  catalog.json
                 ->  dedupe.py         ->  unique.json
                 ->  convert_dsp.py    ->  dsp/*.dsp + albums.json + manifest.json

Sources are the KHInsider rips that the production `dsp/albums.json` was built
from (verified by track-list match, 2026-09-27):

    Tempest 2000    tempest-2000-the-soundtrack-1995   (12 tracks)
    Tempest 3000    tempest-3000-nuon-gamerip-2000     (19 tracks)
    Tempest 4000    tempest-4000-gamerip               (144 rip tracks -> deduped)
    TxK             txk-ps-vita-gamerip-2014           (21 tracks)
    Space Giraffe   space-giraffe-windows-gamerip-2009  (4 tracks)

The Tempest 2000 MOD album (`mod/*.mod`, streamed natively, never converted) is
NOT on KHInsider — keep those files local; convert_dsp.py stages them into dsp/.

Usage:
    python3 download.py                      # every album (fetch + organize only)
    python3 download.py TxK Space_Giraffe    # subset (folder / display / slug)
    python3 download.py --convert            # ...then run the SUPERSEDED chain
    python3 download.py --status             # what is present / missing / hash-bad
    python3 download.py -f mp3               # prefer mp3 over flac
    python3 download.py --url URL --into DIR # ad-hoc extra album

Every fetched and cached file that `track_map.json` pins is checked by SHA-256,
not by size. A cached file that does not match its pin (truncated download, HTML
error page saved as `.flac`, re-rip) is re-fetched; a freshly fetched file that
does not match is deleted so the next run retries instead of leaving poison
behind. Files with no pin are accepted but warned about. `verify_sources.py` is
the standalone, stdlib-only gate over the same pins.

Exit codes: 0 = every album fetched and every pinned file hash-exact. 1 = an
album failed, a track failed, or a hash mismatched — a bootstrap script must
treat non-zero as "do not ship". `--status` exits 1 when a pinned source is
missing or mismatched.

The shipped pool is built by build_map.py + build.py, which pin each .dsp to its
source FLAC byte-exactly. See soundtracks/README.md before reaching for
--convert.

Re-runnable: existing audio files are skipped, so an interrupted run resumes —
"skipped" now means "hash matches the pin", so a truncated file is repaired
rather than kept forever. Audio is gitignored (see the `soundtracks/` block in
the root .gitignore); only the scripts and the generated `dsp/albums.json`
mapping are versioned.

Deps: pip install curl_cffi beautifulsoup4  (+ ffmpeg, librosa for conversion).
"""
import argparse
import os
import random
import subprocess
import sys
import time
from urllib.parse import unquote, urljoin

try:
    from bs4 import BeautifulSoup
except ImportError:
    sys.exit("need beautifulsoup4: pip install beautifulsoup4 curl_cffi")

try:
    from curl_cffi import requests
except ImportError:
    sys.exit("need curl_cffi: pip install beautifulsoup4 curl_cffi")

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = "https://downloads.khinsider.com"

# Same directory, stdlib only — one implementation of the pins for both scripts.
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import verify_sources  # noqa: E402  (local sibling module, must follow HERE)

# folder -> (albums.json display name, KHInsider album slug)
ALBUM_SOURCES = [
    ("tempest2000_soundtrack", "Tempest 2000", "tempest-2000-the-soundtrack-1995"),
    ("tempest3000_soundtrack", "Tempest 3000", "tempest-3000-nuon-gamerip-2000"),
    ("tempest4000_soundtrack", "Tempest 4000", "tempest-4000-gamerip"),
    ("TxK", "TxK", "txk-ps-vita-gamerip-2014"),
    ("Space_Giraffe", "Space Giraffe", "space-giraffe-windows-gamerip-2009"),
]

AUDIO_EXTS = (".flac", ".mp3", ".ogg", ".wav")


IMPERSONATE = "chrome120"
RETRIES = 10

_PINS = None


def source_pins():
    """relpath -> pinned sha256 from track_map.json ({} if the map is unusable).

    A missing map must not stop a fetch: it degrades to the old size-based skip
    plus a warning, and verify_sources.py is the script that fails loudly.
    """
    global _PINS
    if _PINS is None:
        try:
            _PINS = verify_sources.pin_map(HERE)
        except Exception as e:  # noqa: BLE001 - no map must not break a fetch
            print(f"[warn] no usable track_map.json ({e}) — sources cannot be "
                  "hash-verified; run verify_sources.py to gate on hashes")
            _PINS = {}
    return _PINS


def pin_for(path, pins):
    """The sha256 pinned for a path under HERE, or None when it is unpinned."""
    return pins.get(os.path.relpath(path, HERE))


def cached_is_good(path, expected):
    """True when a cached file is byte-exact (pinned) or merely non-empty (not).

    The old test was `getsize() > 0`, which kept a truncated file forever: the
    next run saw a non-empty file, called it cached, and never repaired it.
    """
    if not os.path.exists(path):
        return False
    if not expected:
        return os.path.getsize(path) > 0
    got = verify_sources.sha256_file(path)
    if got == expected:
        return True
    print(f"    [stale] {os.path.basename(path)} does not match its pin — "
          f"{os.path.getsize(path)} bytes, {got[:12]}… != {expected[:12]}…; re-fetching")
    return False


class Client:
    """KHInsider sits behind Cloudflare, which answers `cf-mitigated: challenge`
    on a random fraction of requests (~42% pass measured 2026-09-27) — the same
    URL passes and fails at different times, so it is a per-request lottery, not
    a per-URL or permanent block.

    A fresh TLS + cookie context per attempt is what demonstrably clears it:
    reusing one session across retries keeps getting the same 403 even when a
    brand-new session sails through on the next try. So every retry rebuilds the
    session and warms it on the homepage before the real request.

    When Cloudflare throttles hard for a long stretch, pass a `cf_clearance`
    cookie lifted from a browser that already solved the challenge (--cookie or
    $KHINSIDER_COOKIE). The User-Agent MUST be the one that solved it, since
    cf_clearance is bound to it (--user-agent / $KHINSIDER_UA).
    """

    def __init__(self, cookie=None, user_agent=None):
        self.cookie = cookie
        self.user_agent = user_agent
        self.session = self._build()

    def _build(self):
        session = requests.Session(impersonate=IMPERSONATE)
        if self.cookie:
            session.headers["Cookie"] = self.cookie
        if self.user_agent:
            session.headers["User-Agent"] = self.user_agent
        try:
            session.get(BASE + "/", timeout=30)
        except Exception as e:  # noqa: BLE001 - warm-up is best effort
            print(f"  [warn] homepage warm-up failed: {e}")
        return session

    def get(self, url, timeout=60, stream=False):
        last = None
        for attempt in range(1, RETRIES + 1):
            try:
                resp = self.session.get(url, timeout=timeout, stream=stream)
            except Exception as e:  # noqa: BLE001 - network blips are retryable
                last = e
            else:
                if resp.status_code != 403:
                    resp.raise_for_status()
                    return resp
                last = RuntimeError(f"HTTP 403 (Cloudflare) at {url}")
            time.sleep(min(1.5 * attempt, 8.0) + random.random())
            self.session = self._build()
        raise last


def new_session(cookie=None, user_agent=None):
    return Client(cookie, user_agent)


def album_tracks(session, slug):
    """Return the intermediate per-track page URLs for an album."""
    url = slug if slug.startswith("http") else f"{BASE}/game-soundtracks/album/{slug}"
    resp = session.get(url)
    table = BeautifulSoup(resp.text, "html.parser").find("table", id="songlist")
    if not table:
        raise RuntimeError(f"no #songlist table at {url}")

    links = []
    for row in table.find_all("tr"):
        if row.get("id") in ("songlist_header", "songlist_footer"):
            continue
        cell = row.find("td", class_="fullname") or row.find("td", class_="clickable-row")
        anchor = cell.find("a") if cell else None
        if anchor and anchor.get("href"):
            full = urljoin(BASE, anchor["href"])
            if full not in links:
                links.append(full)
    return links


def harvest(session, page_url, out_dir, audio_format, pins=None):
    """Download the audio file behind one track page. Returns (filename, status).

    status is "exists" (cached and hash-exact), "downloaded", "fallback", or
    "hash-mismatch" (fetched bytes rejected by the pin and deleted so a re-run
    retries). None filename means the page had no audio link.
    """
    page = BeautifulSoup(session.get(page_url).text, "html.parser")
    target_ext = f".{audio_format.lower()}"

    direct = fallback = None
    for a in page.find_all("a", href=True):
        href = a["href"]
        if href.lower().endswith(target_ext):
            direct = href
            break
        if href.lower().endswith((".flac", ".mp3")):
            fallback = href

    download_url = direct or fallback
    if not download_url:
        return None, "no-audio-link"

    filename = unquote(download_url.split("/")[-1])
    path = os.path.join(out_dir, filename)
    pins = source_pins() if pins is None else pins
    expected = pin_for(path, pins)
    if cached_is_good(path, expected):
        return filename, "exists"

    tmp = path + ".part"
    resp = session.get(download_url, timeout=120, stream=True)
    with open(tmp, "wb") as f:
        for chunk in resp.iter_content(chunk_size=16384):
            if chunk:
                f.write(chunk)
    os.replace(tmp, path)

    # Verify what we just wrote. A truncated transfer or a Cloudflare HTML error
    # page saved as `.flac` is not audio, and leaving it on disk means the next
    # run calls it cached and build.py encodes a wrong .dsp from it.
    size = os.path.getsize(path)
    if expected:
        got = verify_sources.sha256_file(path)
        if got != expected:
            os.remove(path)
            print(f"    [hash mismatch] {filename}: expected {expected[:12]}… "
                  f"got {got[:12]}… ({size} bytes) — deleted, re-run to retry")
            return filename, "hash-mismatch"
    else:
        print(f"    [warn] {filename}: no sha256 pin in track_map.json — "
              "accepted unverified")
    return filename, "fallback" if not direct else "downloaded"


def fetch_album(session, folder, slug, audio_format, pins=None):
    out_dir = os.path.join(HERE, folder)
    os.makedirs(out_dir, exist_ok=True)
    try:
        links = album_tracks(session, slug)
    except Exception as e:  # noqa: BLE001 - one bad album must not kill the run
        print(f"  [error] {folder}: {e}")
        if "403" in str(e):
            print("          Cloudflare is throttling this IP. Solve the challenge in a "
                  "browser, then re-run with --cookie/--user-agent (see --help).")
        return None

    print(f"  {folder}  <-  {slug}  ({len(links)} tracks)")
    got = skipped = failed = 0
    for i, page_url in enumerate(links, 1):
        try:
            name, status = harvest(session, page_url, out_dir, audio_format, pins)
        except Exception as e:  # noqa: BLE001
            print(f"    [{i}/{len(links)}] ERROR {e}")
            failed += 1
            continue
        if status == "exists":
            skipped += 1
        elif name is None:
            print(f"    [{i}/{len(links)}] no audio link")
            failed += 1
        elif status == "hash-mismatch":
            failed += 1
        else:
            note = " (fallback format)" if status == "fallback" else ""
            print(f"    [{i}/{len(links)}] {name}{note}")
            got += 1
    print(f"  {folder}: +{got} new, {skipped} cached, {failed} failed")
    return got, skipped, failed


def audio_count(folder):
    d = os.path.join(HERE, folder)
    if not os.path.isdir(d):
        return 0
    return sum(1 for f in os.listdir(d) if f.lower().endswith(AUDIO_EXTS))


def resolve(keys):
    """Map user-supplied album keys (folder / display name / slug) to sources."""
    chosen = []
    for key in keys:
        k = key.strip().lower()
        match = next((s for s in ALBUM_SOURCES
                     if k in (s[0].lower(), s[1].lower(), s[2].lower())), None)
        if match is None:
            known = ", ".join(s[1] for s in ALBUM_SOURCES)
            sys.exit(f"unknown album '{key}'. known: {known}")
        if match not in chosen:
            chosen.append(match)
    return chosen


def run_stage(script):
    print(f"\n=== {script} ===")
    proc = subprocess.run([sys.executable, os.path.join(HERE, script)], cwd=HERE)
    if proc.returncode != 0:
        sys.exit(f"{script} failed ({proc.returncode}) — pipeline stopped")


def status():
    """Report presence AND hash state. Returns True when every pin verifies.

    Counting files by extension is not a check: a truncated FLAC and a complete
    one both count as one file, which is how a corrupt source reached a CIA.
    """
    print("album sources (KHInsider) -> local folders")
    for folder, display, slug in ALBUM_SOURCES:
        n = audio_count(folder)
        flag = "present" if n else "EMPTY  "
        print(f"  {flag}  {display:14s} {folder}/  ({n} audio)  <- {slug}")
    mod_dir = os.path.join(HERE, "mod")
    mods = len(os.listdir(mod_dir)) if os.path.isdir(mod_dir) else 0
    print(f"  {'present' if mods else 'EMPTY  '}  Tempest 2000 MOD  mod/  ({mods})"
          "  (not on KHInsider — keep local)")
    for name, rel in (("catalog.json", "catalog.json"),
                      ("unique.json", "unique.json"),
                      ("dsp/albums.json", "dsp/albums.json")):
        p = os.path.join(HERE, rel)
        print(f"  {'present' if os.path.exists(p) else 'missing'}  {name}")
    dsp = os.path.join(HERE, "dsp")
    n_dsp = len([f for f in os.listdir(dsp) if f.endswith(".dsp")]) \
        if os.path.isdir(dsp) else 0
    print(f"  {n_dsp} .dsp in dsp/")

    print("\npinned sources (sha256 from track_map.json)")
    try:
        summary, results = verify_sources.verify(HERE)
    except Exception as e:  # noqa: BLE001 - report, do not crash a status call
        print(f"  [error] {e}")
        return False
    for name, key in (("sources", "sources"), ("mods", "mods"), ("total", "total")):
        print("  " + verify_sources.counts_line(name, summary[key]))
    for r in results:
        if r["status"] == "match":
            continue
        print(f"    {r['status']:9s} {r['path']}  ({r['label']})  "
              f"expected {(r['expected'] or 'none')[:12]}…  "
              f"actual {(r['actual'] or 'absent')[:12]}…  size {r['size']}")
    return all(r["status"] == "match" for r in results)


def main():
    parser = argparse.ArgumentParser(
        description="Fetch every production soundtrack, organize it, and build the DSP pool.")
    parser.add_argument("albums", nargs="*",
                      help="subset by folder / display name / slug (default: all)")
    parser.add_argument("-f", "--format", default="flac", choices=["flac", "mp3"],
                      help="preferred source format (default: flac)")
    parser.add_argument("--no-convert", action="store_true",
                      help="accepted for compatibility; conversion is now opt-in "
                           "via --convert")
    parser.add_argument("--convert", action="store_true",
                      help="opt in to the SUPERSEDED catalog/dedupe/convert chain "
                           "(the shipped pool is built by build_map.py + build.py)")
    parser.add_argument("--status", action="store_true",
                      help="report what is present/missing, hash-verify every pinned "
                           "source, and exit (1 if any pin fails)")
    parser.add_argument("--url", action="append", default=[],
                      help="extra ad-hoc album URL (repeatable)")
    parser.add_argument("--into", action="append", default=[],
                      help="target folder for each --url (repeatable, same order)")
    parser.add_argument("--cookie", default=None,
                      help="cookie header from a browser that solved the Cloudflare "
                           "challenge (or $KHINSIDER_COOKIE)")
    parser.add_argument("--user-agent", default=None,
                      help="User-Agent that solved the challenge; cf_clearance is "
                           "bound to it (or $KHINSIDER_UA)")
    args = parser.parse_args()

    # line-buffer stdout: this is a long, piped run and progress is the only signal
    sys.stdout.reconfigure(line_buffering=True)

    if args.status:
        sys.exit(0 if status() else 1)

    sources = resolve(args.albums) if args.albums else list(ALBUM_SOURCES)
    if args.url:
        if len(args.into) != len(args.url):
            sys.exit("--into must be given once per --url")
        for url, folder in zip(args.url, args.into):
            sources.append((folder, folder, url))

    pins = source_pins()
    pinned_bytes = sum(os.path.getsize(os.path.join(HERE, rel))
                       for rel in pins if os.path.isfile(os.path.join(HERE, rel)))
    print(f"fetching {len(sources)} album(s) into {HERE}")
    print(f"cached files are verified by SHA-256 against track_map.json "
          f"({len(pins)} pinned sources, {pinned_bytes:,} bytes on disk) — "
          f"hashing takes seconds, not minutes\n")

    session = new_session(args.cookie or os.environ.get("KHINSIDER_COOKIE"),
                          args.user_agent or os.environ.get("KHINSIDER_UA"))
    results = {}
    album_errors = []
    for folder, display, slug in sources:
        r = fetch_album(session, folder, slug, args.format, pins)
        if r is None:
            album_errors.append(folder)
        else:
            results[folder] = r

    total_new = sum(r[0] for r in results.values())
    total_fail = sum(r[2] for r in results.values())
    print(f"\ndownloads: {total_new} new across {len(results)} album(s), {total_fail} failed")
    if album_errors:
        print(f"album errors: {len(album_errors)} — {', '.join(album_errors)}")

    # An incomplete or hash-bad source set must not look like success: build.py
    # treats a hash mismatch as non-fatal (exit 2) and build.sh turns that into
    # a warning, so a silent exit 0 here is what let corrupt audio ship.
    if album_errors or total_fail:
        print(f"\nFAILED: {len(album_errors)} album error(s), {total_fail} track "
              "failure(s) — sources are incomplete or not hash-exact. Fix and "
              "re-run (it resumes); verify with verify_sources.py.")
        sys.exit(1)

    if not args.convert:
        # Default changed: the catalog/dedupe/convert chain is SUPERSEDED for the
        # shipped pool. build_map.py + build.py pin every .dsp to its source FLAC
        # byte-exactly and own the album map; the mel matcher here cannot see the
        # re-rip duplicates the manifest already aliases (a same-track lossy
        # re-rip scores 0.43 under its own 0.90 threshold, below a different
        # track at 0.53), so running it by default kept re-emitting retired audio.
        print("fetched and organized. Skipping the superseded "
              "catalog/dedupe/convert chain -- pass --convert to run it, or use "
              "build_map.py + build.py for the shipped pool.")
        return

    run_stage("catalog_songs.py")
    run_stage("dedupe.py")
    run_stage("convert_dsp.py")
    print("\nDone. Deploy dsp/*.dsp + dsp/albums.json (+ staged .mod) to "
          "sdmc:/3ds/t2k/music/")


if __name__ == "__main__":
    main()
