# CVTE AIC8800D Driver Drop

This directory contains the VLA11 SDIO integration of the CVTE/AIC vendor SDK:

- Package: `iot_aic8800d_linux_sdk_V5.0_2026_0509_2195ae36`
- Release: V5.0, 2026-05-09, revision `2195ae36`
- Source archive SHA-256: `dd277dbf48d608f0bb8903c88069b627c9f4c09c7428434ae41dfb1f71143a36`
- Driver source: `SDIO/driver_fw/driver/aic8800`
- Firmware source: `SDIO/driver_fw/fw/aic8800D80`

The VLA11 board uses the AIC8800D40 SDIO module. The vendor package retains
the `aic8800D80` firmware directory name, while this repository maps the
content to the board-specific `AIC8800D40_VLA11` variant selected by the BSP.

Local integration retained on top of the vendor drop:

1. Default runtime logging is limited to `LOGERROR` to avoid high-rate vendor
   debug/trace output in production and factory test images.
2. Rockchip Linux 6.1 cfg80211 channel-switch API compatibility is retained
   for the VLA11 kernel.

The original vendor archive and extracted reference tree are intentionally
not tracked. Only the driver, matching firmware, and this provenance record
belong in Git.
