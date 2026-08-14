# first_boot

**Status:** placeholder — no provisioning script exists yet.

Once `../README.md`'s base-OS decision is made, this is where a first-boot
provisioning script belongs: hostname assignment, network config, root
filesystem resize (if imaging via a fixed-size `.img`), installing/enabling
the `../systemd/` unit files, and whatever else a freshly flashed
SD-card/eMMC needs done exactly once before it's a working HYDRA-UMC unit.

Not written yet because it depends on the base-OS choice — a Raspberry Pi
OS first-boot script (typically a `systemd` oneshot service reading
`/boot/firstrun.sh`-style, matching Raspberry Pi Imager's own convention)
looks very different from a Yocto image's own first-boot init.
