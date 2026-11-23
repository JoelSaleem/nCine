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
| GOLDEN_MAX_FRAC | 0.005 |
| PERF_THRESHOLD_PCT | 10, advisory (software rendering timings are noise) |

## Plan corrections found in Task 0.2

- Apptests live in `tests/`, binaries in `build/tests/` (plan assumed `src/tests/`). There is no `apptest_sprites`.
- Tests need the sibling data repo: `git clone https://github.com/nCine/nCine-data ../nCine-data` (or `-DNCINE_DATA_DIR=`).
- CMake option names `NCINE_PREFERRED_BACKEND`, `NCINE_BUILD_TESTS`, `NCINE_BUILD_UNIT_TESTS` are correct.
- CMake's `file(DOWNLOAD)` of googletest/ImGui archives is blocked here; `git clone --depth 1 --branch <tag>` them into `build/googletest-1.18.0` and `build/imgui-1.93.0` instead.

## Configure

```bash
CC=/opt/rh/gcc-toolset-15/root/usr/bin/gcc CXX=/opt/rh/gcc-toolset-15/root/usr/bin/g++ \
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNCINE_PREFERRED_BACKEND=GLFW \
  -DNCINE_BUILD_TESTS=ON -DNCINE_BUILD_UNIT_TESTS=ON -DNCINE_DATA_DIR=$HOME/nCine-data
```

Baseline: upstream `a1b5f40a`, unit tests 117/117 pass.
