# Handoff: Switchfin PS Vita playback-performance testing

You are picking up work on a fork of Switchfin (a Jellyfin client built on borealis + libmpv).
Goal: build the Vita `.vpk` from the branch below, then help the user test six commits on a
**PS Vita 1000 (original model)**. Each commit tries to reduce **output (VO) frame drops** during video
playback. The cloud session that wrote these changes could not build with VitaSDK or run anything on
hardware: the app code was compile-checked with stand-in Vita headers, and the mpv patch was checked by
applying it to mpv 0.36.0. **Nothing has been built for real or tested on a Vita yet.**

## Repo and rules

- Repo: `https://github.com/NotTimm/switchfin`, branch `claude/nifty-dijkstra-hefsss`, based on upstream
  `dev` at `0c84c67`. Upstream is `dragonflylee/switchfin`.
- **Commit identity:** the user wants every commit to look like theirs. Use
  `git config user.name "NotTimm"` and
  `git config user.email "91497673+NotTimm@users.noreply.github.com"` (a GitHub noreply address, which keeps
  their real email private). **No `Co-Authored-By`, no Claude/Anthropic mentions, and no AI attribution of
  any kind** in commits, PR text or code comments. Upstream might want some of these changes.
- The user prefers **neovim** over nano in any terminal instructions.
- The user's machines are Fedora (COSMIC spin) at home and Windows at work.

## The symptom being fixed

On the Vita, the in-player video info overlay shows `0 (decoder)` drops but many `(output)` drops, so
playback looks jittery even on easy content (4:3 Dragon Ball, roughly 30 fps h264). The analysis behind
the fixes:

1. **Decoding is fine.** The hardware decoder (`h264_vita`) decodes straight into GPU memory, and GXM
   samples it with no copy.
2. **The handoff to the screen is the bottleneck.** mpv rendered each frame from a `brls::sync` task. Those
   tasks run *after* `Application::frame()`, which also waits for vsync. So mpv's video thread blocked in
   `flip_page()` until the UI loop got around to it, and every frame reached the screen one UI frame late.
3. **mpv had no slack.** `video-timing-offset=0` handed each frame over exactly when it was due, so any slow
   UI frame made it late, and the VO dropped it.
4. **`video-latency-hacks=yes` made it worse.** That option disables frame lookahead, so frame durations come
   from the container fps. Interlaced or telecined sources often report 59.94 for 29.97 content, which makes
   frames look late when they aren't.
5. **The app never raised the Vita's clocks.** It ran at the homebrew defaults (ARM 333 MHz, GPU 111 MHz).
6. **The app asked for up to 720p video** even though the screen is 960x544.

## The commits (oldest first)

| Commit | What it does | Default | Key files |
|---|---|---|---|
| `b98b70a` feat(psv): boost CPU/GPU clocks during video playback | Raises clocks to 444/222/222/166 MHz (ARM/bus/GPU/GPU crossbar) while a `VideoView` is open, then restores the previous values. Setting: **Others → "Boost CPU/GPU clocks during playback"** (reuses the `overclock` config key). Logs `VitaSys: clocks a/b/c/d -> ...` | ON | `app/src/utils/overclock.cpp`, `app/include/utils/overclock.hpp`, `app/src/view/video_view.cpp` |
| `63817d9` feat(psv): make mpv video-latency-hacks configurable | Setting: **Playback → "Low latency video timing (may cause dropped frames)"**. Toggling it restarts mpv. Config key `player_latency_hacks` | ON (same as before) | `app/src/view/mpv_core.cpp`, `app/src/tab/setting_tab.cpp` |
| `c8bebe2` feat(psv): add max resolution setting (544p/720p) | Setting: **Transcode → "Max resolution"**. 544p sends Jellyfin `Height<=544` and `Width<=960`. Config key `player_max_height` | 720p (same as before) | `app/src/activity/player_view.cpp` |
| `30c230b` perf(psv): pool CDRAM blocks for hardware decoder output | **Changes the mpv patch, not the app.** Reuses up to 8 CDRAM memblocks for decoder output instead of one alloc+free per frame. Pooled blocks are freed when the frame size changes and in `uninit_avctx`. Bumps the mpv package to `pkgrel=4` | n/a | `scripts/vita/mpv/gxm.patch` (`vd_lavc.c` section), `scripts/vita/mpv/VITABUILD` |
| `83b5400` perf(psv): render mpv frames before the UI frame on GXM | `on_update` now only sets an atomic flag. A `brls::Ticking` (`MPVRenderTicking`) renders the frame. Tickings run *before* `frame()`, so the new frame appears in the same UI frame. Also passes `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME=0` and sets `video-timing-offset=0.033` (GXM only). **The riskiest change.** | always on | `app/src/view/mpv_core.cpp`, `app/include/view/mpv_core.hpp` |
| `c7a77f4` perf(psv): use 3 decoder threads | `vd-lavc-threads` 4 → 3 (apps get 3 of the 4 cores). Only matters with software decoding | always on | `app/src/view/mpv_core.cpp` |

Every commit only affects the PSV build. Any of them can be undone on its own with `git revert <hash>`.

## Step 0: one edit that couldn't be pushed (needed for `30c230b`)

The Vita build does **not** compile mpv. It downloads a prebuilt package from the repo's own
`vita-packages` GitHub release. GitHub refused the cloud session's push because its token can't change
workflow files, so this edit still has to be made:

- In `.github/workflows/build.yaml`, job `build-vita` (the `for pkg in mbedtls-3.6.8-1 ...` line, around
  line 200): change `mpv-0.36.0-3` to `mpv-0.36.0-4`.
- **Do not** touch the PS4 line, which has `libmpv-0.36.0-3`. That's a different package.
- Commit it with the identity above, for example `ci(psv): use mpv 0.36.0-4 package`.

## Step 1: build

### Option A: GitHub Actions on the fork

1. Enable Actions on the fork if they're off. Forks often have them off by default.
2. Push the Step 0 commit.
3. Run the **vita-packages** workflow by hand. It builds mbedtls, ffmpeg, mpv, mupdf and curl and publishes
   them to a `vita-packages` prerelease on the fork. The fork probably has no such release yet, so this
   step is needed whatever else happens.
4. Check that the package versions in that release match the `build-vita` list (mbedtls 3.6.8-1,
   curl 8.16.0-3, mupdf 1.28.6-1, ffmpeg n6.0-3, mpv 0.36.0-4). If any differ, fix the list in
   `build.yaml`.
5. Run the **build** workflow by hand with platform `psvita` on this branch. The artifact is
   `Switchfin-Vita-<run>/Switchfin.vpk`.

### Option B: local build in the VitaSDK container (Fedora: podman works)

Mirror the CI steps in the `vitasdk/vitasdk:2026.08` image. Run every package build as the `vitasdk`
user, the same way the workflow does:

1. Build the dependency packages: for each of `mbedtls ffmpeg mpv mupdf curl`, go into
   `scripts/vita/<pkg>` and run `vita-makepkg -siCf --noconfirm`. This needs `meson` installed. Building
   them locally means you don't need the release download.
2. Build the app:
   ```bash
   git submodule update --init --recursive
   ```
   ```bash
   cmake -B build -G Ninja -DPLATFORM_PSV=ON -DUSE_GXM=ON -DUSE_MUPDF=ON -DUSE_VITA_SHARK=OFF
   ```
   ```bash
   cmake --build build
   ```
   The output is `build/Switchfin.vpk`.

**If mpv fails to build,** the likely cause is the `vd_lavc.c` hunk of `gxm.patch` (the memory pool). As a
quick unblock, revert `30c230b` and use the released `mpv-0.36.0-3` package. Then report the error so it
can be fixed.

## Step 2: test plan on the Vita

**Setup:**
- Install the `.vpk` with VitaShell.
- In settings, turn on **Show FPS**. Make sure **VSync = On**.
- Leave hardware decode on.

**Test content:**
- the same 4:3 Dragon Ball episode;
- a 720p h264 file that direct plays;
- one transcoded stream (pick a lower bitrate in the player's quality menu).

**Record these for every run:**
- **output and decoder drops** after a fixed 2 minutes, from the player overlay (the info/profile panel);
- **the UI FPS** shown by Show FPS;
- **the `@fps` value** in the overlay's resolution line. This is the container fps. A value around 59–60
  for content that is really 24 or 30 fps confirms the latency-hacks theory;
- **the hardware decode field.** It should say `vita`. `vita-copy` or blank is a problem;
- **the A/V sync value**, and whether it stays near 0.

**Order of runs** (change one thing at a time):

1. **Baseline:** the current release `.vpk`, if the user still has it.
2. **New build with defaults.** Boost is ON, latency hacks ON, 720p, plus the new render path.
3. **Boost OFF**, to measure what the clocks contribute. Then turn it back ON.
4. **Latency hacks OFF.** Watch for a hang or black screen at start or after a seek. Avoiding that hang is
   why upstream turned the option on in the first place.
5. **Max resolution 544p** on the 720p file. The overlay should show a width of 960 or less, and Jellyfin
   should report a transcode.
6. **Long playback** (a full episode or more) and several seeks. This checks the memory pool for
   `vram alloc failed` in the log, and the render path for freezes.

**Logs:** Switchfin's debug view and the mpv log are enabled by the **Debug View** setting. mpv writes
`mpv_<time>.log` when it's on.

## Tuning without rebuilding: `ux0:data/Switchfin/mpv.conf`

mpv loads this file during `mpv_initialize`, after the app has set its options, so its values should win.
This hasn't been confirmed on a device. Copy it over with VitaShell's FTP. Useful lines, one experiment at
a time:

```
# render path timing (commit 83b5400 uses 0.033)
video-timing-offset=0.016
# show late frames instead of dropping them (diagnostic)
framedrop=no
# force zero-copy hardware decode
hwdec=vita
```

**If commit `83b5400` puts video ahead of audio** (A/V sync consistently negative), first try
`video-timing-offset=0.016` or `0`. Revert the commit only if that doesn't help.

## Known risks and things to fix if they show up

- **`83b5400`:**
  - The single mpv FBO is now written earlier in the frame, so watch for tearing or flicker.
  - `setFrameSize()` still forces a redraw through `brls::sync`. That's intentional; it only happens on
    resize.
- **`30c230b`:**
  - The pool is a global that holds up to 8 blocks. At 720p NV12 that's about 1.5 MB each, roughly 12 MB
    of CDRAM.
  - If GPU memory gets tight (nanovg allocation failures), lower `VRAM_POOL_MAX`.
- **`b98b70a`:** uses more battery while a video is open. The clocks are restored when the player closes,
  which the log should confirm.
- **`63817d9` and `c8bebe2`** keep the old behaviour by default, so they're safe to ship. Whether to change
  those defaults should depend on the test results.

## Not done yet (ideas if testing points that way)

- Jellyfin may send h264 above the `VideoLevel<=40` limit for some files. Check what transcodes Jellyfin
  chooses.
- `mapper_map` in the GXM hwdec still calls `sceGxmMapMemory` for every frame. With the memory pool, the
  blocks could be mapped once when they're allocated instead.

## When testing is done

1. Summarize the per-commit results in a table: drops, UI FPS and A/V sync for each run.
2. Revert anything that hurt.
3. If the user wants to offer the changes upstream, push to a branch without `claude` in its name, for
   example `git push origin claude/nifty-dijkstra-hefsss:vita-playback-perf`, and open the PR from there.
   Follow the attribution rules above.
