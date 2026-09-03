#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
PROTOCOL = 6
SECURE_VERSION = 0
BOARDS = ["waveshare-rp2040-zero"]
APP_MAX_BYTES = 992 * 1024
SIGNATURE_BYTES = 384


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def copy_asset(source: Path, destination: Path) -> dict:
    if not source.is_file():
        raise SystemExit(f"missing build artifact: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return {
        "file": destination.name,
        "size": destination.stat().st_size,
        "sha256": digest(destination),
    }


def require_consistent_asset_names(value: object, seen: dict[str, str] | None = None) -> None:
    """Reject manifests where one public filename refers to different bytes."""
    if seen is None:
        seen = {}
    if isinstance(value, dict):
        if isinstance(value.get("file"), str) and isinstance(value.get("sha256"), str):
            filename = value["file"]
            checksum = value["sha256"]
            if filename in seen and seen[filename] != checksum:
                raise SystemExit(f"public asset name has conflicting contents: {filename}")
            seen[filename] = checksum
        for child in value.values():
            require_consistent_asset_names(child, seen)
    elif isinstance(value, list):
        for child in value:
            require_consistent_asset_names(child, seen)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--firmware-build", type=Path, required=True)
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "dist" / "release")
    parser.add_argument("--build-id")
    args = parser.parse_args()

    output = args.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    ota_source = args.firmware_build / "tiny_touch_unified.signed.bin"
    if not ota_source.is_file():
        raise SystemExit(f"missing build artifact: {ota_source}")
    app_size = ota_source.stat().st_size - SIGNATURE_BYTES
    if app_size <= 0 or app_size > APP_MAX_BYTES:
        raise SystemExit(f"application does not fit the {APP_MAX_BYTES} byte flash region")

    factory = output / "factory"
    image = copy_asset(args.firmware_build / "tiny_touch_unified.uf2",
                       factory / "tiny_touch_unified.uf2")
    layout = {
        "version": VERSION,
        "protocol": PROTOCOL,
        "secureVersion": SECURE_VERSION,
        "board": BOARDS[0],
        "images": [{"name": "Factory image", "address": 0x0, **image}],
    }
    (factory / "manifest.json").write_text(json.dumps(layout, indent=2) + "\n", encoding="utf-8")

    ota = copy_asset(ota_source, output / "tiny_touch_unified.bin")
    build_id = args.build_id or os.environ.get("GITHUB_SHA", "")[:12]
    if not build_id:
        build_id = subprocess.run(
            ["git", "rev-parse", "--short=12", "HEAD"], cwd=ROOT,
            check=True, capture_output=True, text=True,
        ).stdout.strip()
    if len(build_id) != 12 or any(character not in "0123456789abcdef" for character in build_id):
        raise SystemExit("build ID must be the first 12 lowercase hex characters of the commit SHA")
    release = {
        "version": VERSION,
        "build": build_id,
        "protocol": PROTOCOL,
        "secureVersion": SECURE_VERSION,
        "boards": BOARDS,
        "firmware": {"factory": layout},
        "ota": ota,
    }
    if args.cli:
        cli_target = output / "tinytouch-macos-arm64.tar.gz"
        shutil.copy2(args.cli, cli_target)
        release["cli"] = {
            "macos-arm64": {
                "file": cli_target.name,
                "size": cli_target.stat().st_size,
                "sha256": digest(cli_target),
                "format": "tar.gz",
            }
        }
    require_consistent_asset_names(release)
    (output / "release-manifest.json").write_text(
        json.dumps(release, indent=2) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
