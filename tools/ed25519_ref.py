#!/usr/bin/env python3
# =============================================================================
# HYDRA-UMC - tools/ed25519_ref.py
# Copyright (C) 2026 JuanenRac (Electro Hobby 3D) <electrohobby3d@gmail.com>
# GPL-3.0-or-later - see LICENSE
# =============================================================================
"""Ed25519 (RFC 8032) in pure Python, with no dependency: the host side of the signature the bootloaders verify.

It follows the reference code of RFC 8032 section 6 and is checked against the vectors of section 7.1 (tests/test_ed25519_ref.py) and,
where the `cryptography` package is installed, against OpenSSL. It is not constant time: sign on the machine of the operator who holds the
private key, never on a shared or remote one. The board only ever verifies.
"""
from __future__ import annotations

import hashlib

_P = 2**255 - 19
_Q = 2**252 + 27742317777372353535851937790883648493
_D = -121665 * pow(121666, _P - 2, _P) % _P
_SQRT_M1 = pow(2, (_P - 1) // 4, _P)

SEED_SIZE = 32
PUBLIC_KEY_SIZE = 32
SIGNATURE_SIZE = 64


def _inv(x: int) -> int:
    return pow(x, _P - 2, _P)


def _add(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    aa = (a[1] - a[0]) * (b[1] - b[0]) % _P
    bb = (a[1] + a[0]) * (b[1] + b[0]) % _P
    cc = 2 * a[3] * b[3] * _D % _P
    dd = 2 * a[2] * b[2] % _P
    e, f, g, h = bb - aa, dd - cc, dd + cc, bb + aa
    return (e * f % _P, g * h % _P, f * g % _P, e * h % _P)


def _mul(scalar: int, point: tuple[int, int, int, int]) -> tuple[int, int, int, int]:
    result = (0, 1, 1, 0)
    while scalar > 0:
        if scalar & 1:
            result = _add(result, point)
        point = _add(point, point)
        scalar >>= 1
    return result


def _equal(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> bool:
    return (a[0] * b[2] - b[0] * a[2]) % _P == 0 and (a[1] * b[2] - b[1] * a[2]) % _P == 0


def _recover_x(y: int, sign: int) -> int | None:
    if y >= _P:
        return None
    x2 = (y * y - 1) * _inv(_D * y * y + 1) % _P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (_P + 3) // 8, _P)
    if (x * x - x2) % _P != 0:
        x = x * _SQRT_M1 % _P
    if (x * x - x2) % _P != 0:
        return None
    if (x & 1) != sign:
        x = _P - x
    return x


_GY = 4 * _inv(5) % _P
_GX = _recover_x(_GY, 0)
assert _GX is not None
_BASE = (_GX, _GY, 1, _GX * _GY % _P)


def _compress(point: tuple[int, int, int, int]) -> bytes:
    zinv = _inv(point[2])
    x = point[0] * zinv % _P
    y = point[1] * zinv % _P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _decompress(data: bytes) -> tuple[int, int, int, int] | None:
    if len(data) != 32:
        return None
    y = int.from_bytes(data, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % _P)


def _expand(seed: bytes) -> tuple[int, bytes]:
    if len(seed) != SEED_SIZE:
        raise ValueError(f"an Ed25519 seed is {SEED_SIZE} bytes, got {len(seed)}")
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return scalar, digest[32:]


def _hash_modq(data: bytes) -> int:
    return int.from_bytes(hashlib.sha512(data).digest(), "little") % _Q


def public_key(seed: bytes) -> bytes:
    """The 32-byte public key of a 32-byte seed."""
    scalar, _ = _expand(seed)
    return _compress(_mul(scalar, _BASE))


def sign(seed: bytes, message: bytes) -> bytes:
    """The 64-byte signature of `message` under the key made from `seed`."""
    scalar, prefix = _expand(seed)
    a_bytes = _compress(_mul(scalar, _BASE))
    r = _hash_modq(prefix + message)
    r_bytes = _compress(_mul(r, _BASE))
    h = _hash_modq(r_bytes + a_bytes + message)
    s = (r + h * scalar) % _Q
    return r_bytes + int.to_bytes(s, 32, "little")


def verify(public: bytes, message: bytes, signature: bytes) -> bool:
    """True when `signature` is a valid signature of `message` under `public` (a canonical S is required, like the board does)."""
    if len(public) != PUBLIC_KEY_SIZE or len(signature) != SIGNATURE_SIZE:
        return False
    a_point = _decompress(public)
    if a_point is None:
        return False
    r_point = _decompress(signature[:32])
    if r_point is None:
        return False
    s = int.from_bytes(signature[32:], "little")
    if s >= _Q:
        return False
    h = _hash_modq(signature[:32] + public + message)
    return _equal(_mul(s, _BASE), _add(r_point, _mul(h, a_point)))
