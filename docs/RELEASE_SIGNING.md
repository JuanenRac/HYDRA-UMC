# Release signing

An official firmware release of HYDRA-UMC carries two independent signatures, made by two different keys, both of which stay on the maintainer's
machine. GitHub holds neither: the workflow that publishes a release only **verifies** with public keys.

| Signature | Files | Made by | Checked by |
| --- | --- | --- | --- |
| Image signature | `<image>.hdr2` (64-byte header of format 2) and `<image>.sig2` (64-byte Ed25519 signature) of every application image | the **image key** (`docs/SIGNATURE_SEPARATION.md`) | the bootloader, against the table of public keys it carries (`docs/image_trust.json`) |
| Release signature | `<file>.sha256` and `<file>.sig` of **every** file of the release | the **key of the releases** | people, tools and the release workflow, before anything is flashed |

The release signature is a layer in front of the image signature: it lets a person or a program know that a file really comes from the maintainer
before it is sent to a board.

## The public key of the releases

```
ed25519 1228393cae6cb9fba1d98e300446956de2bc1e9c5b59cc58035a65dbcf4f2331
```

It is pinned in `tools/release_sign.py` (a test compares it with this page) and is the key every program of the ecosystem pins for its releases. The
private half lives outside every repository, with an offline copy, in a PEM file on the signing machine.

## What is signed

For a file `<asset>` there are `<asset>.sha256`, one line in the `sha256sum` format (`<digest>  <file name>`), and `<asset>.sig`, the Ed25519
signature (RFC 8032) of the **contents** of the file, in base64. They are made by `hydra-sign` (HYDRA-UMC-TOOL-CLI), the tool that signs every
release of the ecosystem, and checked without any private key by:

```
python tools/release_sign.py verify FILE...      # standard library only
```

Because the signature covers the contents and not the name, the name is bound by the lists that are signed too: `SHA256SUMS` and
`build-manifest.json` list every image by name and digest, and the header of an application image names its board and version.

`<image>.sig` (without `.bin`) is the older signature of header format 1. The bootloaders no longer read it; it is accepted in a release when it is
there and never required. The release signature of an image is `<image>.bin.sig`, so the two never share a name.

## The set of a release

For the version V of `hydra-umc.project.json`, the tag is `vV` and the set is:

* the six components of the build, `HYDRA_<RCB|KB_CM7|KB_CM4>_<APP|BOOTLOADER>_v<A.B.C>.bin` and `.hex` (the controller application is version V),
  with `SHA256SUMS` and `build-manifest.json` exactly as the build service wrote them (`signed` is false there: nothing is signed in the build);
* `<image>.hdr2` and `<image>.sig2` of each of the three application images;
* `<file>.sha256` and `<file>.sig` of each of those files.

`tools/publish_release_assets.py` and `tools/verify_release_set.py` refuse a set when:

* the build was not made with the release table (`signing_key_kind` is not `release`) or a bootloader does not hold exactly the release table of
  `docs/image_trust.json` at its fixed offset: **a set built with the public development key is never official**;
* a file, a digest, a size or a version differs between the files and the two lists, or a component is missing;
* an application image has no `.hdr2` or `.sig2`, or the pair does not verify the way a bootloader with the release table decides it (known key,
  Ed25519 signature, CRC, board, link address, counter, version);
* a file lacks its `.sha256` or `.sig`, or a signature does not verify against the pinned key of the releases;
* (the verifier) any file is in the folder that the set does not account for.

## Publishing an official release

The build is public and keyless; the signing is local; the publishing is a workflow that verifies.

**1. Raise the version, then build in GitHub.** Raise the version of the firmware before the build (compiling never does), write the CHANGELOG
section of the new version, run `HYDRA_UMC_CI=1 py -3 tools/ci_validate.py` and push to `main`. Run the workflow *Firmware build (manual)* on `main`
with `flavour` = `release` and `dry_run` = true: it compiles the six components with the release table, checks the size of the bootloaders, and uploads
them as an artifact with `SHA256SUMS`, `build-manifest.json` and a build-provenance attestation. It signs nothing and holds no key. A build whose
bootloaders do not hold exactly the release table fails.

**2. On the maintainer's machine (nothing leaves it yet).** Download the artifact of that run, verify its attestation and its digests, check that
the bootloaders carry exactly the release table (the signing tool refuses to read a key otherwise), and then:

1. make the image signature of each application image with the image key and the rollback counter of the release (`<image>.hdr2`, `<image>.sig2`);
   the tool checks every result the way a bootloader with the release table would decide it before it writes anything;
2. sign every file of the set with the key of the releases (`hydra-sign sign --key <releases PEM> FILE...`), which writes `FILE.sha256` and
   `FILE.sig`;
3. collect the whole set in one folder, and check it:

```
python tools/verify_release_set.py <folder> --tag v<version>        # the very check the workflow will repeat
python tools/publish_release_assets.py --firmware-dir <folder> --draft
```

The second command prints a `gh release create ... --draft` command; run it. It uploads the signed files as an **unpublished draft** of the tag,
which nobody else can see. Only one draft per tag.

**3. On GitHub.** Actions, *HYDRA-UMC firmware release*, Run workflow, branch `main`, the tag (`v` plus the version). The job waits for the approval
that the `release` environment requires (required reviewer, deployment branch `main`; set up once under Settings, Environments). It needs no secret.
Then it:

1. checks that the tag is the version of the commit and that no **published** release of it exists (the draft is expected; it is counted, and there
   must be exactly one);
2. downloads the files of the draft and validates `docs/image_trust.json`;
3. runs `tools/verify_release_set.py`: every check listed above, with public keys only;
4. only if all of it passes, creates the release from the files it just verified (not from the draft, which could change in between), with the
   section of the CHANGELOG as its notes, downloads the published files and verifies them again, and removes the draft.

If any check fails nothing is published; the draft stays, so you can fix it (delete the draft, sign again, upload again) and run the workflow once
more. A version that already has a published release is never replaced. The job has `contents: write` because GitHub shows drafts only to a token that
can write; no other permission is granted and every action it uses is pinned by commit.

## Bench builds

A build with `flavour` = `dev` carries the public development table, whose seed is public: it is a bench build and is never an official release.
`tools/publish_release_assets.py --allow-dev-key` accepts such a set as a pre-release only (the printed command carries `--prerelease`); the release
workflow refuses it.

## Commands for the maintainer (PowerShell)

Everything below runs on the signing machine, from the root of the HYDRA-UMC checkout, and prints no key. `$keys` is the folder that holds the two
private files (the image seed `hydra_image_ed25519.seed` and the release key `hydra_release_ed25519.pem`); neither file is ever opened by hand, copied,
committed or sent to GitHub.

**1. What the build holds, before any key is read.** Download the artifact of the build run and verify it:

```powershell
$run = gh run list --workflow "Firmware build (manual)" --branch main --status success --limit 1 --json databaseId --jq ".[0].databaseId"
$work = Join-Path $env:TEMP "hydra-release"
New-Item -ItemType Directory -Force $work | Out-Null
gh run download $run --dir $work
Get-ChildItem -Recurse $work -File | ForEach-Object { gh attestation verify $_.FullName --repo JuanenRac/HYDRA-UMC }
```

**2. The image signature (STM32 firmware, header format 2).** One header and one signature per application image, made with the image key and the
rollback counter of the release. The signing tool pins the trust table that the bootloaders of the same build carry before it reads the seed, signs
every image, decides each result the way the bootloader decides it, and writes `<image>.hdr2` and `<image>.sig2` only if all of them pass. What it
writes is checked by `tests/test_image_format2.py` and by the host test of the bootloader against fixed bytes (`tests/image_format2_vectors.json`):
the board receives `header || image || signature`.

**3. The release signature (every file, including the two lists).** `SHA256SUMS` and `build-manifest.json` are the manifest of the set: signing them
signs what each image is called and what its digest is. Build `hydra-sign` from HYDRA-UMC-TOOL-CLI and sign the whole folder:

```powershell
$cli = "<path to>\HYDRA-UMC-TOOL-CLI\src"
Push-Location $cli
go build -o "$work\hydra-sign.exe" ./cmd/hydra-sign
Pop-Location
$files = Get-ChildItem $set -File | Where-Object { $_.Extension -notin ".sha256", ".sig" } | ForEach-Object FullName
& "$work\hydra-sign.exe" sign --key "$keys\hydra_release_ed25519.pem" @files
```

`$set` is the folder with the six components, `SHA256SUMS`, `build-manifest.json` and the `.hdr2` / `.sig2` files. Each file gets `<file>.sha256` and
`<file>.sig`.

**4. Check the set, then the draft** (public keys only, the very check the workflow repeats):

```powershell
python tools/release_sign.py verify (Get-ChildItem $set -File | Where-Object { $_.Extension -notin ".sha256", ".sig" }).FullName
python tools/verify_release_set.py $set --tag v<version>
python tools/publish_release_assets.py --firmware-dir $set --draft
```

The last command prints the `gh release create ... --draft` command; run it as printed, then start *HYDRA-UMC firmware release* on `main` and approve
the `release` environment.

**5. A single file signed by hand** (an APK, an archive, a manifest of another program of the ecosystem):

```powershell
& "$work\hydra-sign.exe" sign --key "$keys\hydra_release_ed25519.pem" .ile.bin
python tools/release_sign.py verify .ile.bin
```

## How the programs check a release

The Android phone app checks the digest of what it downloads (rule 1). The watch, iOS and DSI apps apply both rules to a release asset, in this order:

1. **Digest.** The release feed publishes `sha256:<hex>` for the asset; the bytes must match it. A feed without a digest cannot be compared.
2. **Release signature** (off by default, switched on in the settings of the watch, iOS and DSI apps). `<asset>.sig` is the base64 Ed25519 signature
   of the exact bytes of the asset; it must verify against the pinned key of the releases above. With the switch on, a release without a signature, with
   a malformed one or with one that does not verify is refused. The key is compiled into the application: replacing it is a new version of the app.

The checks use no network and no private key; their tests make throwaway keys at run time.

## The update manifest

The `UpdateManifest` that names an artifact (`artifact_url`, `sha256`, and optionally `target` and `min_bootloader`) is signed apart from the firmware image,
also with Ed25519 and also with a key of the releases, never with a shared secret. The signed bytes, the list of trusted public keys (each `current`, `next`,
`retired` or `revoked`, with a validity window, which is how a key is rotated) and the rule are in the SDK page `docs/MANIFEST_SIGNING.md`; the programs that
read a manifest are the updater (`manifest verify`) and the server. The switch `require_asymmetric_signature` is off by default while the ecosystem moves over: a
manifest without a signature passes as a legacy manifest, and with the switch on it is refused as a downgrade, whereas a signature that is present is always
checked. The firmware image itself is checked by the bootloader against its trust table (`docs/SIGNATURE_SEPARATION.md`); the SPI bridge never holds a signing
key and a controller board of HYDRA-UMC never takes the old shared-key (HMAC) check, which only the URTC bootloaders behind the relay still use. Both are
covered on the host; the check on a real board is pending (see the hardware list).

## Losing or replacing the key

Publish a new public key in a release signed with the old one and on this page, and ship it with the next version of every program that pins it. The
image key is replaced through the `next` slot of the trust table (`docs/SIGNATURE_SEPARATION.md`). If a key is lost, the programs that pin it have to be
updated by hand once.
