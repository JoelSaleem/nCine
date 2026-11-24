#!/usr/bin/env bash
# GL vs Vulkan frame times on a host with a GPU, in one command. Builds both backends (Release), measures every
# apptest on both with uncapped frames and no validation layer (verify.sh capture-perf), and prints them side by side.
#   tools/gpu_perf.sh [path to nCine-data]
# Optional: BUILD_GL / BUILD_VK reuse existing build directories, ONLY=a,b,c limits the apptests.
# Needs a desktop session (the apptests open a window) and the Vulkan loader, glslang and GLFW development packages.
set -euo pipefail
cd "$(dirname "$0")/.."

DATA="${1:-${NCINE_DATA_DIR:-$PWD/../nCine-data}}"
[ -d "$DATA/textures" ] || { echo "nCine-data not found at $DATA: clone https://github.com/nCine/nCine-data next to this checkout or pass its path"; exit 2; }
OUT=gpu-perf
mkdir -p "$OUT/gl" "$OUT/vk"

build() { # <dir> <NCINE_WITH_VULKAN>
	cmake -S . -B "$1" -DCMAKE_BUILD_TYPE=Release -DNCINE_DATA_DIR="$DATA" -DNCINE_WITH_VULKAN="$2" >"$OUT/configure-$(basename "$1").log"
	cmake --build "$1" -j"$(nproc)" >"$OUT/build-$(basename "$1").log"
}
BUILD_GL="${BUILD_GL:-build-gpu-gl}"
BUILD_VK="${BUILD_VK:-build-gpu-vk}"
[ -x "$BUILD_GL/tests/apptest_clear" ] || build "$BUILD_GL" OFF
[ -x "$BUILD_VK/tests/apptest_clear" ] || build "$BUILD_VK" ON

for dir in "$OUT/gl" "$OUT/vk"; do
	cp goldens/apptest_list.txt goldens/nondeterministic.txt "$dir/"
done
[ -e /dev/dri ] && echo "GPU device found: measuring on it" || echo "No /dev/dri: verify.sh falls back to software rendering, numbers are advisory"
only=(); [ -n "${ONLY:-}" ] && only=(--only "$ONLY")
tools/verify.sh capture-perf --build "$BUILD_GL" --goldens "$OUT/gl" "${only[@]}"
tools/verify.sh capture-perf --backend vulkan --build "$BUILD_VK" --goldens "$OUT/vk" "${only[@]}"

python3 - "$OUT/gl/perf_baseline.json" "$OUT/vk/perf_baseline_vulkan.json" <<'EOF' | tee "$OUT/summary.txt"
import json, math, sys
gl, vk = (json.load(open(path)) for path in sys.argv[1:3])
ratios = []
print(f"{'apptest':24} {'GL ms':>8} {'Vulkan ms':>10} {'VK/GL':>7}")
for name in sorted(set(gl) & set(vk)):
    g, v = gl[name]['frame_ms_avg'], vk[name]['frame_ms_avg']
    if g > 0 and v > 0:
        ratios.append(v / g)
    print(f"{name:24} {g:8.3f} {v:10.3f} {v / g if g > 0 else float('nan'):6.2f}x")
if ratios:
    print(f"geomean Vulkan/GL over {len(ratios)} apptests: {math.exp(sum(map(math.log, ratios)) / len(ratios)):.3f}")
EOF
echo "Raw numbers in $OUT/, send $OUT/summary.txt back"
