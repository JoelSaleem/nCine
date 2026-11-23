#!/usr/bin/env bash
# Golden-image + perf verification harness. See VERIFY_ENV.md for the host setup.
set -euo pipefail
cd "$(dirname "$0")/.."

GOLDENS=goldens
FRAME="${NCINE_CAPTURE_FRAME:-30}" # 0.5 s of virtual time: animations and emitters have progressed
TOL_VK="${GOLDEN_TOLERANCE_VK:-2}"
MAXFRAC="${GOLDEN_MAX_FRAC:-0.005}"
PCT="${PERF_THRESHOLD_PCT:-10}"
RUN_TIMEOUT="${RUN_TIMEOUT:-60}"
ADVISORY="${PERF_ADVISORY:-}"
# Perf frame_ms_avg = (time to frame PERF_HI - time to frame PERF_LO) / (PERF_HI - PERF_LO), so startup cancels out
PERF_LO=30
PERF_HI=330

PY=""
for p in python3 /usr/bin/python3; do
	if "$p" -c 'import PIL, numpy' 2>/dev/null; then PY="$p"; break; fi
done
[ -n "$PY" ] || { echo "need a python3 with Pillow and numpy"; exit 2; }

# No DRI device = headless: software GL/Vulkan under a virtual X server, perf numbers are only advisory
RUNNER=()
if [ ! -e /dev/dri ] && [ -z "${VERIFY_FORCE_GPU:-}" ]; then
	export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
	export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
	export SDL_VIDEO_X11_FORCE_EGL=0 # SDL2 otherwise picks EGL on Xvfb and GLX-built GLEW fails to init
	RUNNER=(xvfb-run -a -s "-screen 0 1920x1080x24")
	ADVISORY=--advisory
fi
export ALSOFT_DRIVERS="${ALSOFT_DRIVERS:-null}" # audio never affects pixels; don't depend on a sound card

TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT

cmd="${1:-}"
shift || true
BACKEND="gl"; BUILD="build"; ONLY=""
while [ $# -gt 0 ]; do
	case "$1" in
		--backend) BACKEND="$2"; shift 2;;
		--build) BUILD="$2"; shift 2;;
		--only) ONLY="$2"; shift 2;;
		--goldens) GOLDENS="$2"; shift 2;; # one set per windowing backend, e.g. goldens/sdl2
		*) echo "unknown argument: $1"; exit 2;;
	esac
done

if [ "$BACKEND" = vulkan ]; then
	export NCINE_GFX_BACKEND=vulkan NCINE_VK_VALIDATION=1
fi

apptests() {
	if [ -n "$ONLY" ]; then tr ',' '\n' <<<"$ONLY"; else cat "$GOLDENS/apptest_list.txt"; fi
}
nondeterministic() { grep -qx "$1" "$GOLDENS/nondeterministic.txt" 2>/dev/null; }

# capture <apptest> <frame> <out.png>: succeeds only if a non-empty PNG was written
capture() {
	rm -f "$3"
	NCINE_CAPTURE="$3" NCINE_CAPTURE_FRAME="$2" timeout "$RUN_TIMEOUT" "${RUNNER[@]}" "$BUILD/tests/$1" >"$TMPD/$1.log" 2>&1 || true
	if [ -s "$3" ]; then return 0; fi
	echo "  capture failed for $1, last log lines:"; tail -3 "$TMPD/$1.log" | sed 's/^/    /'
	return 1
}

seconds_to_frame() { # <apptest> <frame>
	local start end
	start=$(date +%s.%N)
	capture "$1" "$2" "$TMPD/perf.png" >/dev/null || return 1
	end=$(date +%s.%N)
	awk -v s="$start" -v e="$end" 'BEGIN { print e - s }'
}

perf_json() { # <out.json>
	: >"$TMPD/perf.txt"
	for a in $(apptests); do
		lo=$(seconds_to_frame "$a" "$PERF_LO") && hi=$(seconds_to_frame "$a" "$PERF_HI") || { echo "perf skip $a (capture failed)"; continue; }
		ms=$(awk -v l="$lo" -v h="$hi" -v n=$((PERF_HI - PERF_LO)) 'BEGIN { printf "%.4f", (h - l) / n * 1000 }')
		echo "$a $ms" >>"$TMPD/perf.txt"; echo "perf $a ${ms}ms"
	done
	"$PY" -c 'import json,sys; print(json.dumps({n: {"frame_ms_avg": float(m)} for n, m in (l.split() for l in open(sys.argv[1]))}, indent=1, sort_keys=True))' "$TMPD/perf.txt" >"$1"
}

case "$cmd" in
	list)
		mkdir -p "$GOLDENS"
		find "$BUILD/tests" -maxdepth 1 -type f -executable -name 'apptest_*' -printf '%f\n' | sort >"$GOLDENS/apptest_list.txt"
		wc -l <"$GOLDENS/apptest_list.txt" | xargs echo "apptests listed:";;
	capture-goldens)
		# Captures every apptest twice; any that differ go to nondeterministic.txt and are skipped by `check`
		rc=0
		[ -n "$ONLY" ] || : >"$GOLDENS/nondeterministic.txt"
		for a in $(apptests); do
			if ! capture "$a" "$FRAME" "$GOLDENS/$a.png"; then echo "FAIL $a (no golden)"; rc=1; continue; fi
			if capture "$a" "$FRAME" "$TMPD/again.png" && "$PY" tools/img_diff.py "$GOLDENS/$a.png" "$TMPD/again.png" --tol 0 --max-frac 0 >/dev/null; then
				echo "golden $a"
			else
				grep -qx "$a" "$GOLDENS/nondeterministic.txt" 2>/dev/null || echo "$a" >>"$GOLDENS/nondeterministic.txt"
				echo "golden $a (NONDETERMINISTIC: two captures differ)"
			fi
		done
		exit $rc;;
	check|all)
		[ "$cmd" = all ] && ONLY=""
		rc=0; pass=0; fail=0; skip=0
		tol=0; [ "$BACKEND" = vulkan ] && tol="$TOL_VK"
		maxfrac="$MAXFRAC"; [ "$tol" = 0 ] && maxfrac=0 # exact means every pixel
		for a in $(apptests); do
			result=""
			if [ ! -s "$GOLDENS/$a.png" ]; then echo "FAIL $a (no golden)"; rc=1; fail=$((fail + 1)); continue; fi
			if nondeterministic "$a"; then echo "SKIP $a (nondeterministic)"; skip=$((skip + 1)); continue; fi
			if [ "$BACKEND" = vulkan ] && capture "$a" "$FRAME" "$TMPD/$a.png" && grep -q "Vulkan validation" "$TMPD/$a.log"; then
				echo "FAIL $a (Vulkan validation messages, first:)"; grep -m1 "Vulkan validation" "$TMPD/$a.log" | cut -c1-300; rc=1; fail=$((fail + 1)); continue
			fi
			if capture "$a" "$FRAME" "$TMPD/$a.png" &&
				result=$("$PY" tools/img_diff.py "$GOLDENS/$a.png" "$TMPD/$a.png" --tol "$tol" --max-frac "$maxfrac" --diff-out "$TMPD/$a.diff.png"); then
				echo "PASS $a"; pass=$((pass + 1))
			else
				mkdir -p failures; cp -f "$TMPD/$a.png" "failures/$a.png" 2>/dev/null || true; cp -f "$TMPD/$a.diff.png" "failures/$a.diff.png" 2>/dev/null || true
				echo "FAIL $a ${result:-} (actual + diff in failures/)"; rc=1; fail=$((fail + 1))
			fi
		done
		echo "backend=$BACKEND pass=$pass fail=$fail skip=$skip"
		exit $rc;;
	capture-perf)
		perf_json "$GOLDENS/perf_baseline.json";;
	check-perf|all-perf)
		[ "$cmd" = all-perf ] && ONLY=""
		perf_json "$TMPD/perf_sample.json"
		"$PY" tools/perf_check.py "$GOLDENS/perf_baseline.json" "$TMPD/perf_sample.json" --pct "$PCT" ${ADVISORY};;
	vk-smoke)
		# Vulkan backend smoke test: swapchain clear + readback, fails on any validation message
		"${RUNNER[@]}" "$BUILD/tests/vktest_clear" ${ONLY:+"$ONLY"};;
	*)
		echo "usage: verify.sh {list|capture-goldens|capture-perf|check|all|check-perf|all-perf|vk-smoke} [--backend gl|gl-rhi|vulkan] [--build DIR] [--only a,b,c] [--goldens DIR]"
		exit 2;;
esac
