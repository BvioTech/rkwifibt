# VLA11 RX ownership fix and optional observation

## Scope

This change targets `AIC8800D40_VLA11`, based on VLA11 commit
`aab3ec6245e4e6df23b7aa68ed573eac86f669f5`. It does not replace the VLA10
AIC8800D80 driver, change firmware, or modify the kernel or main application.

The receive-side ownership/lifetime fix is `20261019-r2`. It initializes pooled
descriptors, separates STA and AP skb ownership, validates TIDs, retains reorder
owners while readers are active, and serializes teardown with timer/work/thread
shutdown. These are the source changes used on devices 3 and 4.

The optional observer is `20261021-d2`, also used on both devices. Its recorder
is **disabled by default**. No collector, packet capture service, NetworkManager
logging override, automatic recovery, or background retry service is installed
by this source change. The observer allocates a bounded ring at driver setup;
disabled hooks return without recording packets. It is not zero-overhead and
is not itself a fix for a missing EAPOL handshake packet.

The only additional runtime-source difference from the tested D2 driver is
suppression of the two unconditional periodic `get_txpwr_min/max` console
messages. Error and rate-limited ownership warnings remain. The BSP companion
source is kept at its VLA11 baseline; neither BSP nor Bluetooth modules are
replaced in the combined test image.

## Provenance and limits

- Device 3/4 diagnostic module SHA256:
  `06efca690c6dac60a96f9c23866a626b0b42c4d137d1d4259f189d44901756d1`.
- Module source version: `B117CAE55AEAE3DB5492FAF`.
- Kernel used in that field test: 6.1.99, 2026-09-15 build.
- No new upper-layer NO_SECRETS reconnection package was installed on either
  device. Such a package is deliberately excluded here.
- Successful authentication and pressure-test observations do not prove the
  root cause of the original intermittent handshake timeout is fixed.
- A module rebuilt for an ESD kernel is a new artifact, even when its receive
  logic matches the tested source. Verify its kernel/configuration identity and
  repeat boot, authentication, traffic, disconnect and reboot acceptance tests.

## Build and verification

Build external modules against the **exact prepared target kernel**, using its
`.config`, generated headers and `Module.symvers`. Matching `vermagic` alone is
not a sufficient ABI check. The BSP module pipeline already builds this driver
through kernel kbuild. The rkwifibt Debian package workflow alone does not build
the kernel module.

For optional manual diagnostics, find this module's debugfs `wifi_diag`
directory. Read `status` first; recording must initially be disabled. To record
a selected interface, write `start <ifindex>` to `control`; write `freeze` to
stop, then read `status` and `events`. This does not reconnect or recover Wi-Fi.
Do not enable continuous recording in ordinary production operation.
