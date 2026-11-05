#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/publish_release_assets.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Checks the signed firmware set of this version and prints the command that uploads it as a draft release.

    python tools/publish_release_assets.py --firmware-dir DIR [--repo OWNER/NAME] [--allow-dev-key] [--draft] [--json]
                                           [--list-file FILE] [--tag-file FILE]

Firmware images are not kept in git. A build in GitHub Actions makes them without any key; the maintainer's machine adds the signatures; the set is
published as the release of its version. This script reads a folder that holds the signed set, checks that it is complete and consistent with
hydra-umc.project.json, and prints the exact `gh release create` command. It never runs gh, signs nothing and never changes a file.
With --draft the printed command uploads the set as an unpublished draft, which .github/workflows/release.yml (tools/verify_release_set.py)
verifies and publishes (docs/RELEASE_SIGNING.md).

The set, for the version V of the project:
  * the six components of the build, HYDRA_<RCB|KB_CM7|KB_CM4>_<APP|BOOTLOADER>_v<A.B.C>.bin and .hex, with SHA256SUMS and build-manifest.json (the files of
    the build in GitHub Actions: their digests are checked against both lists, the controller application carries the version V);
  * for every application image its image signature: <image>.hdr2 (the 64-byte header of format 2) and <image>.sig2 (the 64-byte Ed25519 signature),
    made on the maintainer's machine with the image key. The set is checked the way a bootloader with the release table decides it (tools/image_header2.py,
    docs/image_trust.json); <image>.sig of the older header format is accepted when it is there and never required (the bootloaders no longer read it);
  * for every one of those files, its release signature: <file>.sha256 and <file>.sig made with the key of the releases (tools/release_sign.py).
An official release is refused unless the build was made with the release table (signing_key_kind "release") and every bootloader holds exactly that
table, so a set built with the public development key cannot be published as official. --allow-dev-key accepts such a set as a bench pre-release
only (the printed command then carries --prerelease). Exit status 1 when anything is missing or inconsistent.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import image_header as ih  # noqa: E402
import image_header2 as h2  # noqa: E402
import image_trust  # noqa: E402
import release_sign  # noqa: E402

DEFAULT_OWNER = "JuanenRac"
SCHEMA = "hydra-build/1"
BOARDS = {"RCB": ("rcb", 0x47343031), "KB_CM7": ("kb_cm7", 0x48374337), "KB_CM4": ("kb_cm4", 0x48374334)}
COMPONENTS = ("KB_CM4_APP", "KB_CM4_BOOTLOADER", "KB_CM7_APP", "KB_CM7_BOOTLOADER", "RCB_APP", "RCB_BOOTLOADER")
IMAGE_NAME = re.compile(r"^HYDRA_(?P<board>RCB|KB_CM7|KB_CM4)_(?P<role>APP|BOOTLOADER)_v(?P<version>\d+\.\d+\.\d+)\.(?P<ext>bin|hex)$")
RECORD_SIZE = 64  # the header of format 2 and the Ed25519 signature are 64 bytes each; so is a signature of the older format


def project_info(root: Path) -> tuple[str, str]:
    manifest = json.loads((root / "hydra-umc.project.json").read_text(encoding="utf-8"))
    return str(manifest["name"]), str(manifest["version"])


def repo_slug(root: Path, name: str) -> str:
    try:
        url = subprocess.run(["git", "config", "--get", "remote.origin.url"], cwd=root, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        url = ""
    match = re.search(r"github\.com[:/]+([^/]+)/([^/]+?)(?:\.git)?$", url)
    return f"{match.group(1)}/{match.group(2)}" if match else f"{DEFAULT_OWNER}/{name}"


def parse_sums(text: str, problems: list[str]) -> dict[str, str]:
    sums: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (\S+)", line)
        if not match:
            problems.append("SHA256SUMS has a line that is not in the sha256sum format")
            continue
        sums[match.group(2)] = match.group(1)
    return sums


def collect_build(fw_dir: Path, version: str, problems: list[str]) -> tuple[list[Path], str | None, list[tuple[Path, str, tuple[int, int, int]]]]:
    """The files of the build (checked against both digest lists) and its application images: (path, board, version). Also the signing key kind of the build."""
    manifest_file = fw_dir / "build-manifest.json"
    sums_file = fw_dir / "SHA256SUMS"
    for needed in (manifest_file, sums_file):
        if not needed.is_file():
            problems.append(f"{needed.name} is missing")
    if not (manifest_file.is_file() and sums_file.is_file()):
        return [], None, []
    try:
        manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
        entries = {item["name"]: item for item in manifest["files"]}
    except (ValueError, KeyError, TypeError):
        problems.append("build-manifest.json is not a build manifest")
        return [], None, []
    if manifest.get("schema") != SCHEMA or manifest.get("project") != "hydra-umc":
        problems.append("build-manifest.json is not the manifest of a HYDRA-UMC firmware build")
    if manifest.get("version") != version:
        problems.append(f"build-manifest.json is the build of version {manifest.get('version')}, hydra-umc.project.json says {version}")
    if manifest.get("signed") is not False:
        problems.append("build-manifest.json says the build was signed: nothing is signed in the build service")
    kind = manifest.get("signing_key_kind")
    if kind not in ("dev", "release"):
        problems.append(f"build-manifest.json: unknown signing_key_kind {kind!r}")
    sums = parse_sums(sums_file.read_text(encoding="utf-8", errors="replace"), problems)
    if sums != {name: item.get("sha256") for name, item in entries.items()}:
        problems.append("SHA256SUMS and build-manifest.json do not list the same files and digests")
    files: list[Path] = [manifest_file, sums_file]
    seen: dict[str, set[str]] = {}
    apps: list[tuple[Path, str, tuple[int, int, int]]] = []
    for name in sorted(entries):
        match = IMAGE_NAME.match(name)
        if not match:
            problems.append(f"{name} is not a firmware image of the build")
            continue
        path = fw_dir / name
        if not path.is_file():
            problems.append(f"{name} is missing")
            continue
        files.append(path)
        if release_sign.digest_of(path) != entries[name].get("sha256"):
            problems.append(f"{name} does not match the digest of the build manifest")
        if path.stat().st_size != entries[name].get("bytes"):
            problems.append(f"{name} does not have the size of the build manifest")
        component = f"{match['board']}_{match['role']}"
        seen.setdefault(component, set()).add(match["ext"])
        image_version = tuple(int(part) for part in match["version"].split("."))
        if component == "RCB_APP" and match["version"] != version:
            problems.append(f"{name}: the controller application is not version {version}")
        if match["ext"] == "bin" and match["role"] == "APP":
            apps.append((path, match["board"], image_version))
    if sorted(seen) != sorted(COMPONENTS):
        problems.append("the build does not hold exactly the six components: " + ", ".join(COMPONENTS))
    for component, extensions in seen.items():
        if extensions != {"bin", "hex"}:
            problems.append(f"{component} needs both its .bin and its .hex")
    return files, kind, apps


def collect_records(apps, official: bool, doc: dict | None, problems: list[str], missing: list[str]) -> list[Path]:
    """<image>.hdr2 and <image>.sig2 of every application image (and the older <image>.sig when it is there). Official: they must verify as the release table decides."""
    files: list[Path] = []
    trusted: list[bytes] | None = None
    if official:
        try:
            trusted = image_trust.trusted_keys(image_trust.parse_table(image_trust.table_bytes(doc, "release")))
        except (image_trust.TrustError, KeyError, TypeError, ValueError) as error:
            problems.append(f"the release trust table cannot be used: {error}")
    for image, board, version in apps:
        stem = str(image)[:-len(".bin")]
        header_file, signature_file, legacy = Path(stem + ".hdr2"), Path(stem + ".sig2"), Path(stem + ".sig")
        if legacy.is_file():
            if legacy.stat().st_size != RECORD_SIZE:
                problems.append(f"{legacy.name} is not a 64-byte signature")
            else:
                files.append(legacy)
        have = [p for p in (header_file, signature_file) if p.is_file()]
        for absent in (p for p in (header_file, signature_file) if not p.is_file()):
            missing.append(f"{absent.name} is missing")
        files.extend(have)
        for path in have:
            if path.stat().st_size != RECORD_SIZE:
                problems.append(f"{path.name} is not 64 bytes")
        if official and trusted is not None and len(have) == 2 and all(p.stat().st_size == RECORD_SIZE for p in have):
            blob = header_file.read_bytes() + image.read_bytes() + signature_file.read_bytes()
            hardware_id = BOARDS[board][1]
            try:
                header = h2.parse_header2(blob)
            except ih.ImageError as error:
                problems.append(f"{header_file.name}: {error}")
                continue
            if header.version != version:
                problems.append(f"{header_file.name}: the header says version {'.'.join(map(str, header.version))}, the file name says {'.'.join(map(str, version))}")
            verdict = h2.decide2(blob, trusted, hw_id=hardware_id, link_address=ih.LINK_ADDRESS_BY_HARDWARE_ID[hardware_id],
                                 floor=int(doc.get("min_counter", 0)), installed_version=(0, 0, 0))
            if verdict != "ok":
                problems.append(f"{image.name}: its .hdr2 and .sig2 do not verify as a bootloader with the release table would decide them ({verdict})")
    return files


def check_bootloaders(fw_dir: Path, flavour: str, doc: dict, problems: list[str]) -> None:
    """Every bootloader of the set holds, at its fixed offset, exactly the trust table of the flavour."""
    for path in sorted(fw_dir.glob("HYDRA_*_BOOTLOADER_v*.bin")):
        match = IMAGE_NAME.match(path.name)
        if not match:
            continue
        try:
            problem = image_trust.check_binary(doc, path.read_bytes(), BOARDS[match["board"]][0], flavour)
        except (image_trust.TrustError, KeyError, ValueError) as error:
            problem = str(error)
        if problem:
            problems.append(f"{path.name}: {problem} (it must hold the {flavour} table)")


def attach_signatures(files: list[Path], official: bool, problems: list[str]) -> list[Path]:
    """Adds <file>.sha256 and <file>.sig of every file, checked against the pinned key of the releases. An official release needs all of them."""
    result: list[Path] = []
    for path in files:
        result.append(path)
        present = [Path(str(path) + ".sha256"), Path(str(path) + ".sig")]
        if not any(p.is_file() for p in present) and not official:
            continue
        problem = release_sign.problem_with(path)
        if problem:
            problems.append(problem)
        result.extend(p for p in present if p.is_file())
    return result


def build_plan(root: Path, fw_dir: Path, repo: str | None = None, allow_dev_key: bool = False, doc: dict | None = None) -> dict:
    name, version = project_info(root)
    problems: list[str] = []
    missing: list[str] = []
    files: list[Path] = []
    prerelease = False
    if not fw_dir.is_dir():
        problems.append(f"{fw_dir} does not exist")
    else:
        build_files, kind, apps = collect_build(fw_dir, version, problems)
        prerelease = kind != "release"
        if prerelease and not allow_dev_key:
            problems.append(f"the build holds the {kind!r} trust table, not the release one; an official release needs signing_key_kind release "
                            "(use --allow-dev-key to publish a bench pre-release)")
        try:
            doc = doc if doc is not None else image_trust.load()
        except (OSError, ValueError) as error:
            problems.append(f"docs/image_trust.json cannot be read: {error}")
            doc = {}
        if doc:
            check_bootloaders(fw_dir, "dev" if prerelease else "release", doc, problems)
        records = collect_records(apps, not prerelease, doc or None, problems, missing)
        if not prerelease:
            problems.extend(f"{item}: an official release carries the image signature of every application image (docs/RELEASE_SIGNING.md)" for item in missing)
        files = attach_signatures(build_files + records, not prerelease, problems)
    slug = repo or repo_slug(root, name)
    tag = f"v{version}"
    return {
        "repository": slug,
        "tag": tag,
        "title": f"{name} firmware {version}",
        "prerelease": prerelease,
        "files": [Path(os.path.abspath(path)).as_posix() for path in files],
        "problems": problems,
    }


def gh_command(plan: dict, draft: bool = False) -> str:
    notes = "Signed firmware set for the release workflow; it is not public until that workflow has verified and published it."
    head = f'gh release create {plan["tag"]} --repo {plan["repository"]} --title "{plan["title"]}" --notes "{notes}"'
    if plan.get("prerelease"):
        head += " --prerelease"
    if draft:
        head += " --draft"
    continuation = " " + chr(92) + chr(10)
    return continuation.join([head] + [f'  "{path}"' for path in plan["files"]])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware-dir", default=None, help="the folder with the signed set (default: firmware/)")
    parser.add_argument("--repo", default=None, help="OWNER/NAME of the GitHub repository (default: from the origin remote)")
    parser.add_argument("--allow-dev-key", action="store_true", help="accept a set built with the development trust table, as a pre-release only")
    parser.add_argument("--draft", action="store_true", help="print the gh command with --draft: the signed set is uploaded unpublished and the release workflow verifies and publishes it")
    parser.add_argument("--json", action="store_true", help="print the plan as JSON instead of text")
    parser.add_argument("--list-file", default=None, metavar="FILE", help="when nothing is wrong, write the assets to FILE, one path per line")
    parser.add_argument("--tag-file", default=None, metavar="FILE", help="when nothing is wrong, write the tag and, on a second line, 'prerelease' or 'official'")
    args = parser.parse_args(argv)
    fw_dir = Path(args.firmware_dir) if args.firmware_dir else ROOT / "firmware"
    plan = build_plan(ROOT, fw_dir, args.repo, args.allow_dev_key)
    if args.json:
        print(json.dumps(plan, indent=2))
    else:
        print(f'Release {plan["tag"]} of {plan["repository"]}: {len(plan["files"])} asset(s)')
        for path in plan["files"]:
            print(f"  {Path(path).name}")
        for problem in plan["problems"]:
            print(f"  PROBLEM {problem}")
        if not plan["problems"]:
            print()
            print(gh_command(plan, args.draft))
    if not plan["problems"]:
        if args.list_file:
            Path(args.list_file).write_text("".join(path + "\n" for path in plan["files"]), encoding="utf-8")
        if args.tag_file:
            Path(args.tag_file).write_text(plan["tag"] + "\n" + ("prerelease" if plan["prerelease"] else "official") + "\n", encoding="utf-8")
    print("RELEASE_ASSETS=PASS" if not plan["problems"] else "RELEASE_ASSETS=FAIL")
    return 0 if not plan["problems"] else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
