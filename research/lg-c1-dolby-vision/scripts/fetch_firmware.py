#!/usr/bin/env python3
"""Fetch an official, hash-pinned firmware archive for offline analysis."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import urllib.request
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", help="Entry name in firmware-manifest.json, e.g. c1")
    parser.add_argument("--work", type=Path, default=Path("work"))
    parser.add_argument("--verify-only", action="store_true", help="Validate the archive without extracting another EPK copy")
    args = parser.parse_args()
    manifest_path = Path(__file__).resolve().parents[1] / "firmware-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    item = manifest["firmware"][args.model]
    args.work.mkdir(parents=True, exist_ok=True)
    archive = args.work / item["archive"]
    candidate = archive
    if not archive.exists():
        partial = archive.with_suffix(archive.suffix + ".part")
        with urllib.request.urlopen(item["download_url"], timeout=60) as source, partial.open("wb") as dest:
            shutil.copyfileobj(source, dest, length=1024 * 1024)
        candidate = partial
    with candidate.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    if digest != item["sha256"] or candidate.stat().st_size != item["archive_bytes"]:
        raise SystemExit(f"Archive validation failed: {archive.name}; SHA-256={digest}")
    if candidate != archive:
        candidate.replace(archive)
    if args.verify_only:
        print(f"Verified archive size and SHA-256: {archive}")
        return
    output = args.work / args.model
    output.mkdir(exist_ok=True)
    name = item["epk_member"]
    if Path(name).name != name:
        raise SystemExit("Manifest contains an unsafe EPK member path")
    target = output / name
    with zipfile.ZipFile(archive) as package, package.open(name) as source, target.open("wb") as dest:
        shutil.copyfileobj(source, dest, length=1024 * 1024)
    print(f"Verified archive and ZIP member CRC; EPK: {target}")


if __name__ == "__main__":
    main()
