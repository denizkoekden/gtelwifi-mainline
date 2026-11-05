# Kernel-Config für LineageOS (Entwurf, nie gebaut, nie gebootet)

Ziel: derselbe Kernel wie für Nura (Linux 7.3.1 + `patches/0001–0035` + `0100`, clang + kCFI), nur mit den
Optionen, die Android-Userspace verlangt. Zusammengesetzt nach LineageOS-Konvention
(`device/mainline/generic`, `Generic_arm64/BoardConfig.mk`) aus AOSP- und LineageOS-Fragmenten.

## Dateien

| Datei | Inhalt |
|---|---|
| `aosp/android17-release_b-6.12_android-base.config` | AOSP-Pflichtoptionen, wörtlich aus `kernel/configs`, Branch `android17-release` (Commit 553fd8d, 20.03.2026), Ordner `b/android-6.12` = letzter befüllter Satz |
| `aosp/android17-release_b-6.12_android-base-conditional.xml` | bedingte Anforderungen, für `CONFIG_ARM`: `AEABI=y`, `OABI_COMPAT=n`, `DEVKMEM=n` |
| `aosp/android17-release_c-6.18_android-base-conditional.xml` | Android 17 selbst: nur `<kernel minlts="6.18.0" />`, die `android-base.config` daneben ist **leer** |
| `aosp/android17-release_kernel-lifetimes.xml` | Laufzeiten, u. a. `android17-6.18` bis 2030-07-01 |
| `gtelwifi-android.config` | unsere Overrides (Cmdline, Module, cgroup v1 cpuset, Adiantum, Abhängigkeiten), wird zuletzt gemergt |
| `mergecheck.sh` | mergt alles gegen den 7.3.1-Baum im Volume `gtelwifi-linux` (nur lesend, Ausgabe im Container), listet, was nicht durchkommt, legt `out/merged.config` ab und schreibt per `savedefconfig` `device/samsung/gtelwifi/configs/kernel/gtelwifi_android_defconfig` (Round-Trip geprüft) |

LineageOS-Fragmente (lesend aus `../ref/LineageOS_android_kernel_mainline_configs/`):
`fragments/android-base-pre/common.config`, `fragments/android-base-conditional/CONFIG_ARM-y.config`,
`fragments/common.config`.

Achtung bei Google-Quellen: `main` ist seit 03/2025 eingefroren, gültig ist `android17-release`. Der Inhalt von
`b/android-6.12` ist in beiden gleich (geprüft), die Ordnerstruktur nicht.

## Ergebnis (02.11.2026)

Reihenfolge: `allnoconfig` → `buildkit/gtelwifi.config` → `buildkit/cfi.config` → LOS `android-base-pre` →
AOSP base → LOS `CONFIG_ARM-y` → LOS `common` → `gtelwifi-android.config`. **1132 Optionen** (Stand Patch 0056: lima, DISPC-DRM mit
Hardware-VBlank, CMA, PMIC-IRQs, DMA, ASoC mit VBC + SC2723-Codec), nicht durchgekommen:

| Gruppe | Optionen | Umgang |
|---|---|---|
| nur im ACK | `ASHMEM`, `ASHMEM_RUST`, `MEMFD_ASHMEM_SHIM`, `DM_DEFAULT_KEY`, `NETFILTER_XT_MATCH_QUOTA2(_LOG)`, `UID_SYS_STATS`, `CPU_FREQ_TIMES`, `ANDROID_USB_CONFIGFS_UEVENT` | ashmem → `sys.use_memfd=true` (LOS `use_memfd.rc`); kein `metadata_encryption`; quota2 erst nach dem ersten Boot bewerten (`../notes/kernel-userspace.md`) |
| in Mainline entfernt | `SCHED_DEBUG`, `NF_CT_PROTO_DCCP`, `NF_CT_PROTO_UDPLITE` | folgenlos |
| irrelevant | `HID_PLAYSTATION`/`HID_SONY` (+FF), `USB_CONFIGFS_F_UVC` (braucht V4L2), `VHOST_VSOCK` (VMs) | – |
| Toolchain | `DEBUG_INFO_BTF` | braucht `DEBUG_INFO` + pahole im Build, für Android nicht nötig |

Gefunden und behoben: Unserer Nura-Config fehlten **SELinux**, `AUDIT`, `ANDROID_BINDERFS`, `FS_ENCRYPTION`;
iptables-legacy hängt in 7.3 an `NETFILTER_XTABLES_LEGACY` (deckt LOS `android-base-pre` ab); libprocessgroup
braucht `CPUSETS_V1=y`; `CONFIG_LOCALVERSION="-mainline-gtelwifi"` (Release `7.3.1-mainline-gtelwifi+`) lässt libvintf die Kernelmatrix überspringen.

## Muss es 7.3.1 sein?

Nein, aber es ist die beste Wahl. `device/mainline/common` unterstützt laut README ACK **und** reines Mainline.
Der LineageOS-Standardkernel für Mainline-Geräte ist ACK `android-mainline`, und der steht gerade auf **7.3.0**
(`Makefile`, 02.11. gelesen) – praktisch unsere Basis.

| | Mainline 7.3.1 (wie Nura) | ACK `android-mainline` (7.3) | ACK `android16-6.12` |
|---|---|---|---|
| auf dem Tab getestet | ja (SMP, eMMC DDR52, Lader, WLAN, BT …) | nein, aber gleicher Stand | nein |
| ACK-Teile | fehlen | da (ashmem in **Rust**) | da (ashmem in Rust) |
| arm32 bei Google | – | nicht gebaut (`BUILD.bazel`: nur aarch64/x86_64) | nicht gebaut |
| Aufwand | keiner | Rebase der Serie, kaum Drift | Backport 7.3 → 6.12, 5 Upstream-Treiber + API-Drift |
| Binder | C-Binder (fällt in Mainline mit 7.4) | C-Binder | C-Binder |
| Pflege | ein Kernel für Nura + LineageOS | zweiter Baum, wandert mit Mainline | zweiter Baum |

**Empfehlung:** Mainline 7.3.1. Auf ACK `android-mainline` (auf einem 7.3-Commit festgenagelt) erst wechseln,
wenn netd/Userspace hart an ACK-Teilen hängt. Bei 7.3.x bleiben, solange Rust-Binder auf arm32 ungeprüft ist.

## Cmdline

sboot verwirft die Header-Cmdline. In `CONFIG_CMDLINE` (`CMDLINE_EXTEND`) stehen deshalb die Werte von
`MAINLINE_COMMON_KERNEL_PARAMS`/`_ANDROIDBOOT_PARAMS` (ohne `binder.impl=rust`) plus `androidboot.hardware`,
`androidboot.boot_devices=20600000.mmc`, `androidboot.super_partition=SYSTEM`. Zusammen mit sboots Teil 965 von
1024 Zeichen – weitere `androidboot.*` in den DT-Knoten `/firmware/android`.

## Module

brcmfmac und hci_uart/btbcm laden Firmware aus `/vendor/firmware`. Fest eingebaut proben sie, bevor `/vendor`
gemountet ist (dasselbe Problem wie unter Alpine, dort per SDIO-Rebind umgangen). Deshalb `MODULES=y` und diese
beiden als Module in `vendor_dlkm` (lädt `dlkm_loader_mainline`), alles andere bleibt `=y` wie getestet.
`FW_LOADER_USER_HELPER=y` aus LOS `common.config` würde notfalls auch eingebaute Treiber bedienen.
