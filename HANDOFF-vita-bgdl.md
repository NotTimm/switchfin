# Handoff: Vita background downloads + TLS (local testing)

Written 2026-11-23. Not committed: it's a local note.

## 1. What's where

| Thing | Location | State |
| --- | --- | --- |
| Background downloads (system downloader) | `NotTimm/switchfin`, branch `claude/wizardly-keller-i8f3r8`, commit `0bcda7c` | Pushed. **Not built with the real VitaSDK, not run on hardware.** |
| printf `%zd` crash fix (wrapper) | branch `fix-psv-printf`, commit `e84edf0` | Upstream PR dragonflylee/switchfin#291 open |
| Linker headroom fix ("segment 1 overlaps") | branch `vita-playback-perf`, commit `56d3db7` | Part of dragonflylee/switchfin#288 |
| Vita playback perf | branch `vita-playback-perf` | Upstream PR dragonflylee/switchfin#288 open |
| Root-cause printf fix | vitasdk/buildscripts#196 (`NotTimm/buildscripts`, branch `newlib-io-c99-formats`) | Open. Will reach `nightly` soon after merge; the `2026.08` series never gets it (newlib is never changed in patch releases) |

The background-download branch is based on your fork's `dev` (`0c84c67`). It
has **none** of the other fixes above.

## 2. Build it (Fedora, podman)

Switchfin's CI builds the Vita target in `vitasdk/vitasdk:2026.08`. Your fork
has no `vita-packages` release, so take the prebuilt Vita libraries from
upstream's release.

```bash
git clone --recurse-submodules -b claude/wizardly-keller-i8f3r8 https://github.com/NotTimm/switchfin
```

Optional but recommended for testing (see section 5):

```bash
cd switchfin && git cherry-pick 56d3db7 e84edf0
```

```bash
podman run --rm -it -v "$PWD:/src:Z" -w /src docker.io/vitasdk/vitasdk:2026.08 bash
```

Inside the container:

```bash
for pkg in mbedtls-3.6.8-1 curl-8.16.0-3 mupdf-1.28.6-1 ffmpeg-n6.0-3 mpv-0.36.0-3; do vdpm pacman --noconfirm -U https://github.com/dragonflylee/switchfin/releases/download/vita-packages/${pkg}-vita.pkg.tar.xz; done
```

```bash
git config --global --add safe.directory /src && cmake -B build -G Ninja -DPLATFORM_PSV=ON -DUSE_GXM=ON -DUSE_MUPDF=ON -DUSE_VITA_SHARK=OFF && cmake --build build
```

Output: `build/Switchfin.vpk`. It should contain `module/switchfin_bgdl.suprx`.
Check before installing:

```bash
unzip -l build/Switchfin.vpk | grep -i suprx
```

If the plugin target fails to build, the likely suspects (none of these were
checked against the real SDK) are:

- `app/platform/vita/bgdl_plugin/CMakeLists.txt`: the stub library names
  (`taihen_stub`, `SceLibKernel_stub`, `SceIofilemgr_stub`,
  `SceKernelModulemgr_stub`, `SceKernelThreadMgr_stub`) and `-nostdlib` + `gcc`.
- Top-level `CMakeLists.txt`: `add_dependencies(Switchfin.vpk-vpk switchfin_bgdl.suprx-self)`.
  Those target names come from vita.cmake for CMake 3.20+.
- `library/CMakeLists.txt`: `taihen_stub_weak` added to the app's libraries.

## 3. TLS on the Vita

Only matters if your Jellyfin server is reached over **HTTPS**. Over plain
HTTP on your LAN, skip this. Note: the in-app player and curl downloads use
the app's own mbedTLS, so this mainly affects the **system downloader** (the
background downloads).

The options I know of (pick one; they don't combine):

1. **iTLS-Enso** (SKGleba). Adds TLS 1.2 and newer root certificates. Needs
   Enso. Last updated 2023 (v3.2.1). The older, widely used option.
2. **Vitals**. Replaces only the TLS layer with mbedTLS and installs an
   up-to-date certificate store. Firmware **3.60 and 3.65 only**. Listed as
   **not compatible with iTLS-Enso, ReAuth or tls_fixer**.
3. **tls_fixer** (devnoname120). TLS 1.2 and 1.3, tested on 3.60, with a tester
   app for checking connections.

If you'd rather not touch system TLS: keep Jellyfin reachable over plain HTTP
on the LAN, or put a reverse proxy in front of it that still offers TLS 1.2
with older cipher suites.

Read each project's README for install steps and firmware support before
installing. A wrong kernel plugin can stop the Vita from booting properly.

## 4. Testing the background downloads

Needs taiHEN and firmware 3.60–3.74. The setting is
**Settings → Downloads → Background Downloads**, on by default.

1. Install the vpk and open Switchfin once, so it loads the plugin into the
   home screen (SceShell).
2. Check that the plugin is active: `ux0:data/switchfin_bgdl.pid` should exist
   (VitaShell). If it doesn't, the plugin didn't load or hook. See section 6.
3. Start a download at **Original** quality. Press PS. It should appear in the
   notification tray with the episode name and Switchfin's icon.
4. While it runs, `ux0:bgdl/t/<8 hex digits>/` should contain the downloader's
   `.pdb` files, the growing video file, and `switchfin.dst` (the destination
   path).
5. Switch to another app, come back later, then reopen Switchfin. The item
   should move to Downloaded within about 2 s and play offline from
   `ux0:data/<package>/downloads/<itemId>/video.<ext>`.
6. Repeat at **720p** (a transcode). Jellyfin sends no file size for
   transcodes, so the system downloader may struggle. If 720p fails but
   Original works, that's the reason.
7. Cancel test: cancel a background item in Switchfin. It should say to remove
   it from the notification tray too. Removing it there should leave nothing in
   Switchfin's folder.
8. Turn the setting off and download again. It should use the old in-app
   download.

## 5. Things that can get in the way of the test

- **"segment 1 overlaps"** at build time: cherry-pick `56d3db7`.
- **Crash after seeking** during playback: the `%zd` printf crash
  (dragonflylee/switchfin#291). Cherry-pick `e84edf0`, or don't seek.
- The mpv open-thread crash (#292) is fixed upstream in `gxm.patch`
  (`272d9b1`), but the `mpv-0.36.0-3` package was not rebuilt, so your build
  won't have it.

## 6. If it doesn't work

| Symptom | Likely cause | Where to look |
| --- | --- | --- |
| No `switchfin_bgdl.pid` | Plugin not loaded (no taiHEN, missing from vpk, wrong path) or firmware's SceShell not in the offset table | `ux0:app/SWITCHFIN/module/switchfin_bgdl.suprx` exists? Firmware NID in `get_shell_offsets()` in `app/platform/vita/bgdl_plugin/main.c` |
| pid file exists but downloads use the in-app path | Connecting to the SceDownload service failed | App log lines starting with `bgdl:` |
| Tray shows the download failing right away | HEAD→GET patch not applied (the bytes didn't say "HEAD" on your firmware), or TLS | Try an HTTP server URL. On 3.74 the HEAD offset was assumed to match 3.73 |
| Download finishes but Switchfin says Failed | Plugin didn't move the file (marker missing, or rename failed) | File left in `ux0:bgdl/t/<id>/`? Is `switchfin.dst` there? |
| Stuck at Downloading forever | Task failed but is still listed in the tray | Remove it from the tray; Switchfin then marks it Failed |

The app logs through borealis' logger, which prints to stdout on the Vita, so
you need a log-capturing plugin or debugger to see it. The file checks above
work without logs.

### The parts most likely to be wrong

1. The IPMI data layouts and command numbers in `app/src/utils/psv_bgdl.cpp`
   (from bgvpk, which used them on 3.65). Type `1` = generic file download.
2. The 3.74 entry in the plugin's offset table: assumed identical to 3.73
   (NetStream's plugin uses the same export/MIME offsets for 3.74; the HEAD
   offset is only patched after checking the bytes).
3. The `.vpk` packaging of the plugin (section 2).

## 7. Files changed on the branch

- `app/platform/vita/bgdl_plugin/` (new): `main.c`, `exports.yml`, `CMakeLists.txt`
- `app/src/utils/psv_bgdl.cpp`, `app/include/utils/psv_bgdl.hpp`, `app/include/utils/psv_bgdl_common.h` (new)
- `app/src/utils/download.cpp`, `app/include/utils/download.hpp`: handoff, polling, cancel
- Setting: `config.hpp/.cpp`, `setting_tab.hpp/.cpp`, `resources/xml/tabs/settings.xml`, `resources/i18n/en-US/main.json`
- Build: `CMakeLists.txt`, `library/CMakeLists.txt`

Credits: SceShell offsets and IPMI layouts from bgvpk by SKGleba (MIT).
