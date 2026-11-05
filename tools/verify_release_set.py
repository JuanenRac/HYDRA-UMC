#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/verify_release_set.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Verifies a folder of signed firmware files before the release workflow publishes them.

    python tools/verify_release_set.py DIR --tag v0.2.8 [--list-file FILE]

The files are signed on the maintainer's machine (docs/RELEASE_SIGNING.md); the workflow holds no signing key. This tool needs no secret: it uses
only public keys (the key of the releases pinned in tools/release_sign.py and the release table of docs/image_trust.json), and checks, for DIR:

  * the tag is v plus the version of hydra-umc.project.json;
  * the set is complete and consistent (the checks of tools/publish_release_assets.py: the digests of the build against SHA256SUMS and the build
    manifest, the version, the release trust table in every bootloader, the image signature of every application image as a bootloader decides it,
    and the build made for an official release, never with the development table);
  * every file has its .sha256 and .sig and each one verifies against the pinned public key of the releases;
  * no file is in DIR that the set does not account for.

Prints VERIFY_RELEASE_SET=PASS and exits 0 only when all of that holds; otherwise nothing may be published. --list-file writes the names of
the files to publish, one per line.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import publish_release_assets  # noqa: E402


def verify_set(folder: Path, tag: str, root: Path = ROOT, doc: dict | None = None) -> tuple[list[str], list[str]]:
    """Returns (problems, names of the files to publish)."""
    problems: list[str] = []
    if not folder.is_dir():
        return [f"{folder} is not a folder"], []
    _, version = publish_release_assets.project_info(root)
    if tag != f"v{version}":
        problems.append(f"the tag {tag} is not v{version}, the version of hydra-umc.project.json")
    plan = publish_release_assets.build_plan(root, folder, None, allow_dev_key=False, doc=doc)
    problems.extend(plan["problems"])
    names = sorted(Path(item).name for item in plan["files"])
    present = sorted(path.name for path in folder.iterdir() if path.is_file())
    for extra in sorted(set(present) - set(names)):
        problems.append(f"{extra} is in the folder but belongs to no file of the set")
    for directory in sorted(path.name for path in folder.iterdir() if not path.is_file()):
        problems.append(f"{directory} is in the folder but is not a file")
    return problems, names


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--list-file", default=None)
    args = parser.parse_args(argv)
    problems, names = verify_set(Path(args.folder), args.tag)
    for name in names:
        print(f"  {name}")
    for problem in problems:
        print(f"  PROBLEM {problem}")
    if not problems and args.list_file:
        Path(args.list_file).write_text("".join(name + "\n" for name in names), encoding="utf-8")
    print("VERIFY_RELEASE_SET=PASS" if not problems else "VERIFY_RELEASE_SET=FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
