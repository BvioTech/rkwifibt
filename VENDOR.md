# Vendored source provenance — rkwifibt (AIC8800D80 wifi/bt)

No official public upstream (`rockchip-linux/rkwifibt` and `airockchip/rkwifibt`
both 404). Source vendored from the Rockchip internal gerrit snapshot used by
dev11. This **one repo feeds three layers** (same snapshot → versions never
mismatch):

- **BSP / kernel modules** — `drivers/aic/AIC8800D80/` built against *our* kernel
  → `aic8800_{bsp,fdrv,btlpm}.ko` in `modules.tar.zst` (see bsp `bsp-build-plan.md §12`).
- **BSP / firmware** — `firmware/aic/AIC8800D80/*` → `firmware.tar.zst` (`/lib/firmware/`).
- **packaging** — `debian/` + `tools/` + `scripts/` → `rkwifibt-dev-tools` deb
  (BT attach tool + `wifibt-init.service`); see `userspace-deb-packaging.md §4.6`.

| | |
|---|---|
| Upstream (gerrit) | `https://gerrit.rock-chips.com:8443/linux/linux/external/rkwifibt` |
| Commit | `c0260f5905883ecec4b571eeef3f949c2004d15c` |
| Branch | `linux-6.1-stan-rkr5` |
| SDK manifest | `rk3576_linux6.1_release_v1.1.0_20241220` (same as dev11) |
| Local snapshot | `/home/alex/project/rk3576-src/evb-rk3576/external/rkwifibt` |
| NIC / BT chip | **AIC8800D80** (RK3576 EVB1) |

## ⚠️ This is a WORKING-TREE snapshot, not `git archive HEAD`

In the gerrit checkout, the AIC8800 bits are **not git-tracked**:
- `drivers/aic/` and `firmware/aic/` are untracked (the AIC vendor drop is laid
  in by an SDK step, not committed).
- `scripts/wifibt-init.sh` and `scripts/wifibt-util.sh` are **modified** vs HEAD
  (dev11 customizations).

So this repo vendors the **working tree** = tracked files (in their working-tree
state) **+** untracked-non-ignored source/firmware, i.e.:
`git ls-files` ∪ `git ls-files --others --exclude-standard`.
Build artifacts (`*.ko *.o *.cmd Module.symvers modules.order`, all `.gitignore`d)
are **excluded** — the BSP layer rebuilds the `.ko` against our kernel.

## ⚠️ AIC module Makefile hardcodes an Android KDIR

`drivers/aic/AIC8800D80/*/Makefile` hardcode the original author's
`KDIR=/home/yaya/...`. The BSP build MUST override with `make -C $KERNEL_DIR M=$AIC`
— do not rely on the in-file KDIR.

## Re-vendoring / updating
1. Check out the desired gerrit snapshot; ensure the AIC drop is laid in (the
   working tree must contain `drivers/aic/` + `firmware/aic/`).
2. Re-copy the working-tree source set (the ls-files union above) over this tree.
3. Bump `debian/changelog` if the version changed; set `version`/tag.
4. Keep the BSP `rkwifibt` ref (bsp `boards/*.env` RKWIFIBT_REF) pinned to the
   SAME commit as the packaging release — three layers must stay in lockstep.
