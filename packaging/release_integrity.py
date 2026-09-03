#!/usr/bin/env python3
"""Validate and safely unpack immutable tinyTouch release candidates."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import tarfile
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parent.parent
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
PROTOCOL = 6
SECURE_VERSION = 0
BOARDS = ["waveshare-rp2040-zero"]
APP_MAX_BYTES = 992 * 1024
SIGNATURE_BYTES = 384
DESCRIPTOR_MAGIC = b"\x7fTTFW\x01"
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLASH_BASE = 0x10000000
UF2_STATE_OFFSET = 0x1F0000
UF2_FLASH_SIZE = 0x200000
SHA256_PATTERN = re.compile(r"[0-9a-f]{64}")
BUILD_PATTERN = re.compile(r"[0-9a-f]{12}")
NAME_PATTERN = re.compile(r"[A-Za-z0-9._-]+")
EXPECTED_IMAGES = {
    "factory": {
        0x0: "tiny_touch_unified.uf2",
    },
}


class IntegrityError(RuntimeError):
    """Release data is incomplete, inconsistent, or unsafe."""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise IntegrityError(message)


def load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise IntegrityError(f"invalid JSON file: {path}") from exc
    require(isinstance(value, dict), f"JSON root must be an object: {path}")
    return value


def checked_name(value: object, field: str) -> str:
    require(isinstance(value, str) and NAME_PATTERN.fullmatch(value) is not None,
            f"invalid {field}")
    return value


def checked_asset(metadata: object, path: Path, label: str) -> dict:
    require(isinstance(metadata, dict), f"invalid {label} metadata")
    name = checked_name(metadata.get("file"), f"{label} filename")
    size = metadata.get("size")
    checksum = metadata.get("sha256")
    require(isinstance(size, int) and not isinstance(size, bool) and 0 < size <= 256 * 1024 * 1024,
            f"invalid {label} size")
    require(isinstance(checksum, str) and SHA256_PATTERN.fullmatch(checksum) is not None,
            f"invalid {label} SHA-256")
    require(path.name == name and path.is_file(), f"missing {label}: {name}")
    require(path.stat().st_size == size, f"wrong size for {label}: {name}")
    require(digest(path) == checksum, f"wrong SHA-256 for {label}: {name}")
    return metadata


def app_description(data: bytes, name: str) -> dict[str, str]:
    """Parse the descriptor the firmware embeds (see main/main.c)."""
    offset = data.find(DESCRIPTOR_MAGIC)
    end = data.find(b"\0", offset) if offset >= 0 else -1
    require(offset >= 0 and end > offset, f"missing firmware descriptor: {name}")
    try:
        text = data[offset + len(DESCRIPTOR_MAGIC):end].decode("ascii")
    except UnicodeDecodeError as exc:
        raise IntegrityError(f"invalid firmware descriptor text: {name}") from exc
    fields = {}
    for item in text.split(";"):
        if item:
            key, _, value = item.partition("=")
            fields[key] = value
    return fields


def validate_app(path: Path, version: str, build: str) -> bytes:
    """Check the signed OTA image and return its application bytes."""
    data = path.read_bytes()
    require(len(data) > SIGNATURE_BYTES, f"OTA image has no signature trailer: {path.name}")
    application = data[:-SIGNATURE_BYTES]
    require(len(application) <= APP_MAX_BYTES, f"application exceeds the flash region: {path.name}")
    description = app_description(application, path.name)
    require(description.get("version") == version,
            f"embedded version mismatch in {path.name}: {description.get('version')}")
    require(description.get("project") == "tiny_touch_unified",
            f"embedded project mismatch in {path.name}: {description.get('project')}")
    require(description.get("board") == "rp2040-zero",
            f"embedded board mismatch in {path.name}: {description.get('board')}")
    require(description.get("protocol") == str(PROTOCOL),
            f"embedded protocol mismatch in {path.name}: {description.get('protocol')}")
    require(description.get("build") == build,
            f"build ID {build} is not embedded in {path.name}")
    return application


def uf2_payload(path: Path) -> bytes:
    """Reassemble the flash contents of a UF2 written from the start of flash."""
    data = path.read_bytes()
    require(len(data) > 0 and len(data) % 512 == 0, f"invalid UF2 size: {path.name}")
    blocks: dict[int, bytes] = {}
    for offset in range(0, len(data), 512):
        start0, start1, _flags, address, size, _number, _count, _family = struct.unpack_from(
            "<8I", data, offset)
        end = struct.unpack_from("<I", data, offset + 508)[0]
        require(start0 == UF2_MAGIC_START0 and start1 == UF2_MAGIC_START1 and end == UF2_MAGIC_END,
                f"invalid UF2 block: {path.name}")
        require(0 < size <= 476 and address >= UF2_FLASH_BASE and address not in blocks,
                f"invalid UF2 block layout: {path.name}")
        blocks[address - UF2_FLASH_BASE] = data[offset + 32:offset + 32 + size]
    payload = b""
    state = b""
    for address in sorted(blocks):
        if address < UF2_STATE_OFFSET:
            require(address == len(payload), f"UF2 is not contiguous from the start of flash: {path.name}")
            payload += blocks[address]
        else:
            require(address == UF2_STATE_OFFSET + len(state),
                    f"UF2 state-erase blocks are not contiguous: {path.name}")
            state += blocks[address]
    # The factory image must clear every device-state sector so a reflash
    # always returns the board to first setup.
    require(len(state) == UF2_FLASH_SIZE - UF2_STATE_OFFSET and state == b"\xff" * len(state),
            f"UF2 does not erase the device-state sectors: {path.name}")
    return payload


def asset_path(root: Path, kind: str, name: str, flat: bool) -> Path:
    return root / name if flat else root / kind / name


def validate_layout(root: Path, kind: str, layout: object, version: str,
                    protocol: int, build: str, flat: bool) -> dict[str, str]:
    require(isinstance(layout, dict), f"missing {kind} layout")
    require(layout.get("version") == version, f"{kind} version mismatch")
    require(layout.get("protocol") == protocol, f"{kind} protocol mismatch")
    require(layout.get("secureVersion") == SECURE_VERSION,
            f"{kind} secure version mismatch")
    require(layout.get("board") == BOARDS[0], f"{kind} board mismatch")
    images = layout.get("images")
    require(isinstance(images, list) and len(images) == len(EXPECTED_IMAGES[kind]),
            f"{kind} must contain exactly one flash image")
    seen: dict[int, tuple[int, str]] = {}
    public: dict[str, str] = {}
    for index, image in enumerate(images):
        require(isinstance(image, dict), f"invalid {kind} image {index}")
        address = image.get("address")
        require(isinstance(address, int) and not isinstance(address, bool),
                f"invalid {kind} image address")
        name = checked_name(image.get("file"), f"{kind} image filename")
        require(EXPECTED_IMAGES[kind].get(address) == name,
                f"unexpected {kind} image {name} at {address!r}")
        path = asset_path(root, kind, name, flat)
        checked_asset(image, path, f"{kind} image")
        require(address not in seen, f"duplicate {kind} image address: {name}")
        seen[address] = (address + image["size"], name)
        checksum = image["sha256"]
        if name in public:
            require(public[name] == checksum, f"conflicting public asset: {name}")
        public[name] = checksum
    return public


def validate_cli_archive(path: Path) -> None:
    try:
        with tarfile.open(path, "r:gz") as archive:
            members = archive.getmembers()
    except (OSError, tarfile.TarError) as exc:
        raise IntegrityError(f"invalid CLI archive: {path.name}") from exc
    require(members, f"empty CLI archive: {path.name}")
    names = set()
    for member in members:
        pure = PurePosixPath(member.name)
        require(not pure.is_absolute() and ".." not in pure.parts,
                f"unsafe path in {path.name}: {member.name}")
        names.add(pure.as_posix().rstrip("/"))
    require("tinytouch/tinytouch" in names, f"CLI executable missing from {path.name}")
    require(any(name == "tinytouch/_internal" or name.startswith("tinytouch/_internal/") for name in names),
            f"CLI runtime missing from {path.name}")


def validate_release(root: Path, commit: str, *, flat: bool = False,
                     require_cli: bool = True) -> dict:
    require(re.fullmatch(r"[0-9a-f]{40}", commit) is not None, "commit must be a full SHA")
    manifest = load_json(root / "release-manifest.json")
    version = manifest.get("version")
    protocol = manifest.get("protocol")
    build = manifest.get("build")
    require(version == VERSION, f"release version {version!r} does not match VERSION {VERSION!r}")
    require(protocol == PROTOCOL, f"release protocol must be {PROTOCOL}")
    require(manifest.get("secureVersion") == SECURE_VERSION,
            f"release secure version must be {SECURE_VERSION}")
    require(isinstance(build, str) and BUILD_PATTERN.fullmatch(build) is not None,
            "invalid release build ID")
    require(build == commit[:12], "release build ID does not match candidate commit")
    require(manifest.get("boards") == BOARDS, "unexpected board compatibility list")
    firmware = manifest.get("firmware")
    require(isinstance(firmware, dict) and set(firmware) == {"factory"},
            "release must contain one factory layout")
    public: dict[str, str] = {}
    for kind in ("factory",):
        for name, checksum in validate_layout(
            root, kind, firmware[kind], version, protocol, build, flat
        ).items():
            if name in public:
                require(public[name] == checksum, f"conflicting public asset: {name}")
            public[name] = checksum
    ota = checked_asset(manifest.get("ota"), root / "tiny_touch_unified.bin", "OTA image")
    public[ota["file"]] = ota["sha256"]
    application = validate_app(root / "tiny_touch_unified.bin", version, build)
    factory = uf2_payload(asset_path(root, "factory", "tiny_touch_unified.uf2", flat))
    require(factory.startswith(application) and len(factory) - len(application) < 512,
            "factory UF2 differs from the OTA application")
    cli = manifest.get("cli")
    if not require_cli:
        require(cli is None, "firmware-only release unexpectedly contains CLI metadata")
        return manifest
    require(isinstance(cli, dict) and set(cli) == {"macos-arm64"},
            "release must contain the Apple silicon CLI")
    for key, name in (
        ("macos-arm64", "tinytouch-macos-arm64.tar.gz"),
    ):
        metadata = cli[key]
        require(isinstance(metadata, dict) and metadata.get("file") == name and
                metadata.get("format") == "tar.gz", f"invalid {key} CLI metadata")
        path = root / name
        checked_asset(metadata, path, f"{key} CLI")
        validate_cli_archive(path)
        public[name] = metadata["sha256"]
    if flat:
        installer = root / "install.sh"
        require(installer.is_file() and installer.stat().st_size < 64 * 1024,
                "missing or invalid installer")
        require(installer.read_bytes().startswith(b"#!/bin/sh\n"),
                "installer must be a POSIX shell script")
        expected = set(public) | {
            "release-manifest.json",
            "tinytouch-firmware.tar.gz",
            "checksums.txt",
            "install.sh",
        }
        actual = {path.name for path in root.iterdir() if path.is_file()}
        require(actual == expected,
                f"published asset set mismatch; missing={sorted(expected - actual)}, "
                f"extra={sorted(actual - expected)}")
    return manifest


def validate_checksums(root: Path) -> None:
    checksum_path = root / "checksums.txt"
    try:
        lines = checksum_path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeDecodeError) as exc:
        raise IntegrityError("missing or invalid checksums.txt") from exc
    expected = {path.name for path in root.iterdir() if path.is_file() and path.name != "checksums.txt"}
    actual: set[str] = set()
    for line in lines:
        parts = line.split("  ", 1)
        require(len(parts) == 2 and SHA256_PATTERN.fullmatch(parts[0]) is not None,
                "invalid checksums.txt line")
        name = checked_name(parts[1], "checksum filename")
        require(name not in actual, f"duplicate checksum entry: {name}")
        path = root / name
        require(path.is_file() and digest(path) == parts[0], f"checksum mismatch: {name}")
        actual.add(name)
    require(actual == expected,
            f"checksum closure mismatch; missing={sorted(expected - actual)}, extra={sorted(actual - expected)}")


def safe_extract(candidate: Path, destination: Path) -> None:
    require(not destination.exists(), f"extraction destination already exists: {destination}")
    try:
        with tarfile.open(candidate, "r:gz") as archive:
            members = archive.getmembers()
            for member in members:
                pure = PurePosixPath(member.name)
                require(not pure.is_absolute() and ".." not in pure.parts,
                        f"unsafe candidate path: {member.name}")
                require(not member.issym() and not member.islnk(),
                        f"candidate archive links are not allowed: {member.name}")
            archive.extractall(destination, filter="data")
    except (OSError, tarfile.TarError) as exc:
        raise IntegrityError(f"invalid candidate archive: {candidate}") from exc


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name in ("firmware", "assembled", "publish"):
        command = subparsers.add_parser(name)
        command.add_argument("directory", type=Path)
        command.add_argument("--commit", required=True)
    extract = subparsers.add_parser("extract")
    extract.add_argument("archive", type=Path)
    extract.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        if args.command == "extract":
            safe_extract(args.archive.resolve(), args.destination.resolve())
        else:
            directory = args.directory.resolve()
            validate_release(
                directory,
                args.commit,
                flat=args.command == "publish",
                require_cli=args.command != "firmware",
            )
            if args.command == "publish":
                validate_checksums(directory)
    except IntegrityError as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
