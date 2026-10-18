# Security Policy 🔒 (HYDRA-UMC)

## Supported Versions

| Version | Supported          |
| ------- | ------------------ |
| 0.1.x   | ✅ Yes             |
| 0.0.x   | ❌ No              |

## Reporting a Vulnerability

**CRITICAL: Do not report safety-critical vulnerabilities through public GitHub issues.**

In a motion control system, a security flaw can lead to physical damage. If you discover a vulnerability affecting the **bootloaders** (CRC32 + HMAC-SHA256 image verification, anti-rollback), the **CAN/SPI protocols**, or the **planned motion engine and safety interlocks** (neither exists in the firmware yet, so reports about the design are welcome too):

1. **Email**: Send a detailed report to `electrohobby3d@gmail.com`.
2. **Impact**: Describe if the bug allows bypassing software limits, disabling watchdogs, or hijacking motor control.
3. **Response**: Initial acknowledgment within 48 hours.

We follow a coordinated disclosure policy to ensure hardware safety before public release.
