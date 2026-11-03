# Separating the signature from the image

**Status: design. Nothing described here is built for a board and no bootloader is changed by this page.** It sets out how the official firmware of the three controllers (STM32G474 robot controller, STM32H745 CM7 and CM4) can be **compiled by a public build service with no private key leaving the maintainer's computer**.

## 1. The model today

* The bootloaders already authenticate an update with **Ed25519**: the board holds only the 32-byte public key (`UPDATE_PUBLIC_KEY`), the signature covers a signed 64-byte header and the image (`tools/image_header.py`), and the verification is `tweetnacl.c` with `update_signature.c`. That key is a public value: a build service may be given it.
* What is secret and compiled into the bootloaders is the key `HYDRA_FW_HMAC_KEY_HEX`. It is the key of the **local integrity check** of the installed application (`ApplicationIsValid`, `hmac_sha256_flash_region`), not of the authentication of an update. Because it sits in the image, a release build needs it, and a public build service cannot have it: the service builds with the development keys (`signing_key_kind: dev`), which is a bench build, and the official release is compiled on the maintainer's machine with `HYDRA_UMC_RELEASE=1`.
* The bootloaders are flashed through the debug port; only application images are updated through the bootloader and signed.
* Missing from the update path: a table of keys with rotation, a monotonic rollback counter (the version is compared as three numbers), and metadata that survives a power cut at any point.

## 2. Target architecture

1. **No secret in any image.** The local-integrity HMAC is replaced by a SHA-256 of the installed application stored in the metadata (no key). The signature already checked at install time is what authenticates; the boot check only has to catch corruption of the flash.
2. **The update key comes from a public file** in the repository (the key identifiers accepted, the lowest counter, the default policy), not from a secret variable. The build service compiles the release flavour with the production public keys and a dev flavour with the development key whose seed is public. Only the table differs.
3. **The signature stays a separate file** made on the maintainer's computer after the build: `<image>.sig` over the signed header and the image. The same bytes the service built are signed; nothing is rebuilt.
4. **The local release step pins the trust table**: before signing it reads the table of each bootloader at its fixed offset and compares the key identifiers with the ones it has pinned. A build made with another table is refused before any key is read.

## 3. Formats (schema 2)

### 3.1 Header, format 2

The 64-byte little-endian header keeps its first 40 bytes (magic `HYDR`, format, flags, hardware id, major, minor, patch, image size, link address, the two vector words, image CRC-32). The format field becomes 2 and the reserved bytes gain:

| Offset | Size | Field |
|---|---|---|
| 40 | 4 | `rollback_counter` |
| 44 | 8 | `key_id`: first 8 bytes of the SHA-512 of the public key that signs |
| 52 | 12 | reserved, zero |

The signature is Ed25519 over a domain string that differs from the format 1 one, followed by the SHA-256 of header and image (the exact domain bytes are fixed with the vectors). The authorised downgrade, which signs a session nonce, the two versions and the digest, is kept. A bootloader that does not know a format refuses it and keeps the installed application.

### 3.2 Trust table

A linker section at a fixed offset of each bootloader, little-endian: magic `TKEY` (4), schema 1 (1), number of keys 1 or 2 (1), flags (2), then per key the key id (8), the public key (32), flags (1: current, 2: next, 4: revoked) and 3 zero bytes, and a CRC-32 of all of it (4). With two keys it is 100 bytes. A test reads it back from the compiled binary and compares it with the public key file.

### 3.3 Rotation and revocation

The table lists `[current, next]`. Releases are signed with `current` until the fleet has the bootloader that knows `next`; the following bootloader lists `[next, next2]`. The overlap window is a configuration parameter. Revocation is: the next bootloader leaves the key out; a release raises the counter so nothing signed earlier returns; a *revoked* flag retires a key that is still in the table. There is no revocation command on the bus: a stolen key could use it to lock out the good one.

### 3.4 Anti-rollback and metadata

* The floor is the larger of the one kept in the metadata and the minimum compiled into the bootloader. An image with a counter below it is refused; after a good install the floor becomes the image's counter. Nothing overrides it.
* The metadata is an **append-only log with a generation number** (record: magic, schema, type, length, generation, payload, CRC-32, padded to the programming unit: 8 bytes on the G474, 32 on the H745). The valid record with the highest generation wins; a record cut short fails its CRC and the previous one stays. On the G474 the gap before the application is 4 KB, i.e. two 2 KB pages, so two alternating units are possible there if the second is free (to be checked); on the H745 the metadata sector is a single 128 KB sector, so the log is used.
* This is a staged slot with an atomic commit, not an executable A/B: the application is linked for one address. There is no flash for a third slot; a known-good image and signed deltas live on the host side and the board always receives a complete image with its signature.

## 4. Cost

Nothing has been measured for this design, so there are no new figures.

* **Already in the repository:** the published bootloader binaries are 14340 bytes (RCB, region 32 KB), 11032 bytes (CM7, region 128 KB) and 16924 bytes (CM4, region 128 KB). They are from earlier firmware versions and may predate the signature code, so they do not measure the cost of Ed25519.
* **Library:** the bootloaders already use TweetNaCl (public domain), with a hook that refreshes the watchdog during the long scalar multiplications. Monocypher (BSD-2-Clause or CC0) and ed25519-donna (public domain or MIT) are faster and larger and are compatible with GPL-3.0-or-later. Keep TweetNaCl unless a measurement says otherwise.
* **How to measure without compiling on a workstation:** a read-only job of the existing firmware build compiles with the fixed toolchain, prints `arm-none-eabi-size -A` of every bootloader and the stack-usage files (`-fstack-usage`), and the headroom against each linker region, failing under a configured threshold. The time of one verification is measured on a board with the cycle counter; an emulator's instruction count gives only an order of magnitude.

## 5. Tests

* **On the host, in CI:** the existing bootloader host tests and the image-header vectors extended to format 2, the table, the counter and the log; RFC 8032 vectors; a flipped bit in each field of the header, in the image, in the signature and in the key id; a counter below the floor; an older version; a corrupt table; rotation with one and two keys; and a power cut at every write of the metadata on a simulated flash.
* **On the bench:** verification time and stack for each controller, the watchdog during verification, real power cuts during the copy and during a metadata write, the first flashing of the new bootloader.

## 6. Release flow

1. The build service compiles the release and dev flavours with no secret, produces checksums, a build manifest and an attestation, and declares which table each binary carries.
2. The local release tool verifies the attestation and the commit, checks the table against the pinned key identifiers, signs each application image into a temporary folder and publishes the set only when every signature verified. The seed of the image-signing key is a file outside the repository; the key that signs release assets is a different one.
3. The release is verified without any secret: every signature against the public key of the table.
4. The updater, studio and suite verify the signature before forwarding an image to the board and show the key identifiers.

## 7. Boards already deployed

A bootloader of the previous scheme keeps checking format 1 headers and is changed only through the debug port. A bootloader of the new scheme refuses format 1 by default. A bridge policy that accepts both is possible only for bootloaders built for it, is never the default, and has a counter above which format 1 is refused.

## 8. Plan

1. The public key file and the generator of the table; the test that reads it back from a binary.
2. Header format 2 with its vectors (Python and C) and the signing tool.
3. The metadata log in portable C with a power-cut test.
4. The bootloaders: table, format 2, counter, SHA-256 instead of the local HMAC.
5. The build script and the workflow: no secret variable, release flavour from the public file, size report.
6. The release tool: pin of the table, signing, verification of the set.
7. The updater, studio, suite and image-builder: verify and show the identifiers, in eight languages.
8. Bench: time, stack, power cuts, first flashing.
9. First official release built by the service and signed locally.

Steps 1 to 3, 5 to 7 need no board; 4 is tested on the host and 8 and 9 need one.
