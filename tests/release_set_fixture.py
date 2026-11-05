# =============================================================================
# HYDRA-UMC - tests/release_set_fixture.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""A signed firmware set for the release tests, made with throw-away keys (the real keys are never read by a test).

`make_set(folder, flavour)` writes what the build service and the maintainer's machine leave in the folder of a release: the six components with
their lists, the image signature of each application and the release signature of every file. `Keys` holds the throw-away image key and release
key and the trust document built from them.
"""
import copy
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import image_header as ih  # noqa: E402
import image_header2 as h2  # noqa: E402
import image_trust  # noqa: E402
import publish_release_assets as pra  # noqa: E402
import release_sign  # noqa: E402
import ed25519_ref as ed25519  # noqa: E402

VERSION = "1.2.3"
COUNTER = 5
IMAGE_SEED = hashlib.sha256(b"test image key for the release tests").digest()
RELEASE_SEED = hashlib.sha256(b"test release key for the release tests").digest()
RELEASE_PUBLIC = ed25519.public_key(RELEASE_SEED).hex()
VERSIONS = {"RCB_APP": VERSION, "RCB_BOOTLOADER": "0.1.8", "KB_CM7_APP": "0.1.4", "KB_CM7_BOOTLOADER": "0.1.2", "KB_CM4_APP": "0.1.6", "KB_CM4_BOOTLOADER": "0.1.1"}


def trust_doc() -> dict:
    """The real layout of docs/image_trust.json with the release flavour holding the throw-away image key."""
    doc = copy.deepcopy(image_trust.load())
    public = ed25519.public_key(IMAGE_SEED)
    doc["flavours"]["release"]["keys"] = [{"state": "current", "public": public.hex(), "key_id": image_trust.key_id(public).hex()}]
    image_trust.validate(doc)
    return doc


def application(board: str, tag: int) -> bytes:
    return bytes((0x00, 0x00, 0x01, 0x20, 0x41, 0x90, 0x00, 0x08)) + bytes((i * tag) & 0xFF for i in range(120))


def bootloader(doc: dict, board: str, flavour: str) -> bytes:
    target = pra.BOARDS[board][0]
    offset = int(doc["targets"][target]["table_offset"], 16)
    blob = bytearray(offset + 160)
    table = image_trust.bootloader_table(doc, flavour)
    blob[offset:offset + len(table)] = table
    return bytes(blob)


def make_set(folder: Path, flavour: str = "release", doc: dict | None = None, release_signed: bool = True, records: bool = True, legacy: bool = False) -> None:
    doc = doc or trust_doc()
    folder.mkdir(parents=True, exist_ok=True)
    listed = []
    for component, version in VERSIONS.items():
        board, role = component.rsplit("_", 1)
        data = application(board, 3 + len(listed)) if role == "APP" else bootloader(doc, board, flavour)
        for extension, payload in (("bin", data), ("hex", b":00000001FF\n" + data.hex().encode("ascii"))):
            name = f"HYDRA_{component}_v{version}.{extension}"
            (folder / name).write_bytes(payload)
            listed.append((name, payload))
    sums = "".join(f"{hashlib.sha256(payload).hexdigest()}  {name}\n" for name, payload in listed)
    (folder / "SHA256SUMS").write_text(sums, encoding="ascii")
    manifest = {
        "schema": pra.SCHEMA, "repository": "o/r", "project": "hydra-umc", "version": VERSION, "commit": "0" * 40, "built_on": "github-actions",
        "toolchain": "test", "signing_key_kind": flavour, "signed": False,
        "files": [{"name": name, "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)} for name, payload in listed],
    }
    (folder / "build-manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    if records:
        for component, version in VERSIONS.items():
            board, role = component.rsplit("_", 1)
            if role != "APP":
                continue
            image = (folder / f"HYDRA_{component}_v{version}.bin").read_bytes()
            hardware_id = pra.BOARDS[board][1]
            parts = tuple(int(p) for p in version.split("."))
            header = h2.make_header2(IMAGE_SEED, image, hardware_id, parts, ih.LINK_ADDRESS_BY_HARDWARE_ID[hardware_id], COUNTER)
            stem = f"HYDRA_{component}_v{version}"
            (folder / f"{stem}.hdr2").write_bytes(header.pack())
            (folder / f"{stem}.sig2").write_bytes(h2.sign_image2(IMAGE_SEED, header, image))
            if legacy:
                (folder / f"{stem}.sig").write_bytes(bytes(64))
    if release_signed:
        sign_everything(folder)


def sign_everything(folder: Path) -> None:
    """What the maintainer's machine does last: the release signature (.sha256 and .sig) of every file that is there."""
    for path in [p for p in sorted(folder.iterdir()) if p.is_file()]:
        release_sign.sign_file(path, RELEASE_SEED)
