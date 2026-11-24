# Verify environment

Host: devvm (CentOS Stream 9), 192 cores, no GPU (`lspci` shows none, no `/dev/dri`).

**HEADLESS: using lavapipe/llvmpipe (plan Appendix D), perf=ADVISORY.**

| Setting | Value |
|---|---|
| GL | Mesa llvmpipe under `xvfb-run -a -s "-screen 0 1920x1080x24"`, `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe` |
| Vulkan | lavapipe, `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` (Vulkan 1.4, llvmpipe LLVM 22) |
| Audio | `ALSOFT_DRIVERS=null` (no sound card; opening ALSA is fatal in `ALAudioDevice`) |
| Compiler | gcc-toolset-15 (GCC 15.2.1); system GCC 11 lacks `__is_convertible` used by `nctl/type_traits.h` |
| GOLDEN_TOLERANCE_VK | 2 (per channel, /255) |
| GOLDEN_MAX_FRAC | 0.0001 (0.005 let a missing sprite pass, caught by mutation test) |
| PERF_THRESHOLD_PCT | 10, advisory (software rendering timings are noise) |

## Plan corrections found in Task 0.2

- Apptests live in `tests/`, binaries in `build/tests/` (plan assumed `src/tests/`). There is no `apptest_sprites`.
- Tests need the sibling data repo: `git clone https://github.com/nCine/nCine-data ../nCine-data` (or `-DNCINE_DATA_DIR=`).
- CMake option names `NCINE_PREFERRED_BACKEND`, `NCINE_BUILD_TESTS`, `NCINE_BUILD_UNIT_TESTS` are correct.
- CMake's `file(DOWNLOAD)` of googletest/ImGui archives is blocked here; `git clone --depth 1 --branch <tag>` them into `build/googletest-1.18.0` and `build/imgui-1.93.0` instead.
- Lua apptests need `scripts/` inside the data dir (upstream only installs it): `ln -s $PWD/scripts ../nCine-data/scripts`.
- `python3` on PATH (`/usr/local/bin`) lacks Pillow/numpy; `verify.sh` falls back to `/usr/bin/python3`.

## Determinism (fixes review gap: wall-clock animation)

- `NCINE_CAPTURE` switches the engine `Clock` to virtual time (+1/60 s per frame) and fixes the RNG seeds.
- Goldens are frame 30 (0.5 s virtual). `capture-goldens` captures twice; mismatches go to `goldens/nondeterministic.txt` and `check` reports them as SKIP.
- Goldens are per windowing backend: `goldens/` = GLFW, `goldens/sdl2/` = SDL2 (`--goldens goldens/sdl2`). They differ by ±1 in ImGui apps (different platform timers) and in backend-specific text (multitouch, scaling).
- Currently nondeterministic: GLFW `apptest_filebrowser` (lists the live filesystem); SDL2 `apptest_filebrowser`, `apptest_audio`.
- SDL2 on Xvfb picks EGL unless `SDL_VIDEO_X11_FORCE_EGL=0`, and GLX-built GLEW then fails (`GLEW error: Unknown error`); `verify.sh` sets it.
- ImGui under GLFW still reads `glfwGetTime()`; stable at frame 30 so far, revisit if a GUI apptest flakes.

## RHI and Vulkan builds

- GL always renders through igfx (the legacy path is gone, `NCINE_RHI` no longer exists); Vulkan needs `-DNCINE_WITH_VULKAN=ON`.
- Vulkan: plus `-DNCINE_WITH_VULKAN=ON`: `build-vk` (Debug, asserts + validation) and `build-vk-rel` (Release, for golden checks: `./tools/verify.sh all --backend vulkan --build build-vk-rel`). Goldens are Release captures: Debug builds differ in `apptest_jobsystem` (extra debug-statistics window), so golden-check Release builds only. Smoke test: `./tools/verify.sh vk-smoke --build build-vk` (clear + readback on lavapipe, exits non-zero on any validation message). Needs `vulkan-validation-layers`; VMA 3.5.0 is vendored in `src/graphics/vulkan/vk_mem_alloc.h`.

## Vulkan comparison

`check --backend vulkan` passes when at most `GOLDEN_MAX_FRAC` (0.01%) of pixels differ by more than 2/255, or when the
largest 8-connected region of differing pixels is at most `GOLDEN_MAX_BLOB` (32) and at most `GOLDEN_BLOB_MAX_FRAC` (0.1%)
differ. The second rule admits fill-rule ownership flips: the flipped viewport hands edges lying exactly on pixel centers
to the other side, giving scattered edge pixels (apptest_joystick: 25 regions, the largest 13 px). Mutations that drop a
draw (one 578 px region) or shift everything by one pixel (229 px) still fail.

## Tie order (2026-11-24)

Sprites without visit order tie on their layer and were ordered by a hash of GL object names, which depends on the driver
and cannot be reproduced on Vulkan. Material sort keys now use creation-order texture ids and fixed engine program ids
(user programs from 1000) in every build. Recaptured for it: `apptest_camera`, `apptest_meshsprites` (GLFW and SDL2).

## Perf method

`frame_ms_avg = (wall time to frame 330 - wall time to frame 30) / 300`, so startup cancels out. Advisory on this host.

## Configure

```bash
CC=/opt/rh/gcc-toolset-15/root/usr/bin/gcc CXX=/opt/rh/gcc-toolset-15/root/usr/bin/g++ \
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNCINE_PREFERRED_BACKEND=GLFW \
  -DNCINE_BUILD_TESTS=ON -DNCINE_BUILD_UNIT_TESTS=ON -DNCINE_DATA_DIR=$HOME/nCine-data
```

Baseline: upstream `a1b5f40a`, unit tests 117/117 pass.
