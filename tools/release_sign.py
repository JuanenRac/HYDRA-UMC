#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/release_sign.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Checks the release signature of an asset: the one the maintainer's machine puts on every file of an official release.

    python tools/release_sign.py public                    the public key pinned below (64 hex digits)
    python tools/release_sign.py verify FILE...            checks FILE.sha256 and FILE.sig against the pinned key (no private key, standard library only)

The format is the one of `hydra-sign` (HYDRA-UMC-TOOL-CLI), the tool that signs every release of the ecosystem: for a file F there are
F.sha256, one line in the `sha256sum` format (`<digest>  <file name>`), and F.sig, the Ed25519 signature of the CONTENTS of F in base64
(RFC 8032, 64 bytes). The private key is never read here: it lives outside every repository, in a PEM on the signing machine, and signing is
done by `hydra-sign` on the signing machine. `sign_file` below takes a raw seed and exists for the tests, which use a throw-away key; the key
of the releases is never given to it.

Do not mix this up with the image signature of the bootloaders (docs/SIGNATURE_SEPARATION.md): `<image>.hdr2` and `<image>.sig2` are made by
the image key and checked by the bootloader; `<asset>.sig` here is made by the key of the releases and checked by people and tools before
anything is flashed. A signature made for the format 1 of the headers is named `<image>.sig` (without `.bin`), the release signature of an
image is `<image>.bin.sig`.
"""
from __future__ import annotations

import base64
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ed25519_ref as ed25519  # noqa: E402

#: The public key of the releases of HYDRA-UMC (the same one the updater and the other programs of the ecosystem pin).
PUBLIC_KEY_HEX = "1228393cae6cb9fba1d98e300446956de2bc1e9c5b59cc58035a65dbcf4f2331"


def digest_of(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def problem_with(path: Path, public_hex: str | None = None) -> str | None:
    """None when FILE.sha256 and FILE.sig both belong to FILE and verify under the key; otherwise one sentence."""
    public_hex = (public_hex or PUBLIC_KEY_HEX).lower()
    try:
        public = bytes.fromhex(public_hex)
    except ValueError:
        return "the public key is not hexadecimal"
    if len(public) != 32:
        return "the public key is not 32 bytes"
    sidecar = Path(str(path) + ".sha256")
    signature_file = Path(str(path) + ".sig")
    if not path.is_file():
        return f"{path.name} is missing"
    if not sidecar.is_file():
        return f"{path.name}.sha256 is missing"
    if not signature_file.is_file():
        return f"{path.name}.sig is missing"
    fields = sidecar.read_bytes().decode("ascii", errors="replace").split()
    digest = digest_of(path)
    if len(fields) != 2 or fields[0] != digest or fields[1] != path.name:
        return f"{path.name}.sha256 does not describe {path.name}"
    try:
        signature = base64.b64decode(signature_file.read_bytes().decode("ascii").strip(), validate=True)
    except ValueError:
        return f"{path.name}.sig is not a base64 Ed25519 signature"
    if len(signature) != ed25519.SIGNATURE_SIZE:
        return f"{path.name}.sig is not a base64 Ed25519 signature"
    if not ed25519.verify(public, path.read_bytes(), signature):
        return f"the signature of {path.name} does not verify against the pinned public key of the releases"
    return None


def verify_file(path: Path, public_hex: str | None = None) -> bool:
    return problem_with(path, public_hex) is None


def sign_file(path: Path, seed: bytes) -> None:
    """Writes FILE.sha256 and FILE.sig exactly as `hydra-sign sign` does. For tests with a throw-away seed; the real key is only ever read by hydra-sign."""
    contents = path.read_bytes()
    Path(str(path) + ".sha256").write_bytes(f"{hashlib.sha256(contents).hexdigest()}  {path.name}\n".encode("ascii"))
    Path(str(path) + ".sig").write_bytes(base64.b64encode(ed25519.sign(seed, contents)) + b"\n")


def main(argv: list[str]) -> int:
    if argv[:1] == ["public"]:
        print(PUBLIC_KEY_HEX)
        return 0
    if argv[:1] != ["verify"] or len(argv) < 2:
        print("usage: release_sign.py public | verify FILE...", file=sys.stderr)
        return 2
    failed = False
    for name in argv[1:]:
        problem = problem_with(Path(name))
        print(f"{'OK  ' if problem is None else 'FAIL'} {name}" + ("" if problem is None else f": {problem}"))
        failed = failed or problem is not None
    print("RELEASE_SIGN=" + ("FAIL" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
