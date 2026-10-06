#!/usr/bin/env bash
# Measures the PC build in the same scene every time, in VR: the first level of World 1, walked
# into by a script (tools/bench/level1-walk.txt), a few runs in a row, and what the emulator
# logs of each (SHADPS4_BENCH) summed up. The game is uncapped: the emulated headset refreshes
# by its own clock (SHADPS4_VR_UNCAPPED), so the frame rate says what the PC can do, not what the
# headset's display allows. A headset runtime still shows the frames: the newest one at each of
# its refreshes (its own frame rate counter counts those, repeats included: it is not the
# game's). The first time, the opening is played to make a save in World 1 (about 4 minutes);
# the runs start from that save.
#
#   tools/pc-bench.sh <name> [runs] [NAME=value ...]
#
# The NAME=value settings go to the emulator, e.g. SHADPS4_PERF_ALL=0 to compare with the
# optimizations off. Settings of this script (environment):
#   EXE=<shadps4.exe>      the build to measure (build/win-x64/shadps4.exe); its .pdb comes along
#   XR=sim|none|<file>     the headset: the OpenXR Simulator (default when found: OPENXR_SIM=
#                          <its openxr_simulator.json>, else E:\Github\OpenXR-Simulator), none
#                          (the monitor only), or any OpenXR runtime's .json
#   UNCAPPED=<Hz>          how often the emulated headset refreshes, the most frames a second that
#                          can be measured (250). 0: paced by the headset's display, as when playing
#   FPS_CAP=<n>            with UNCAPPED=0, the most frames a second (90)
#   RUN_SECONDS=<s>        length of a run (170: the level is reached at about 75 s)
#   FROM_WINDOW=<n>        the first 10-second window counted (8: inside the level)
#   SCRIPT=<file>          the input script of the runs (tools/bench/level1-walk.txt)
#   GAME_ARGS="<args>"     arguments for the game itself (after --)
#   SAVE=<dir>             the save the runs start from (build/pc-bench/save: World 1 reached)
# Results in build/pc-bench/<name>/: run-<n>/ (log, Bench lines, pictures) and summary.txt.
# Nothing else may be running the emulator meanwhile: each one asks Windows for about 14 GB.
set -u
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
name=${1:?usage: tools/pc-bench.sh <name> [runs] [NAME=value ...]}
runs=${2:-3}
shift; [ $# -gt 0 ] && shift
exe=${EXE:-$root/build/win-x64/shadps4.exe}
uncapped=${UNCAPPED:-250}
run_seconds=${RUN_SECONDS:-170}
from_window=${FROM_WINDOW:-8}
base=$root/build/pc-bench
bin=$base/bin
out=$base/$name
eboot=$(cygpath -w "$root/games/CUSA12392/eboot.bin")

running() { tasklist //FI "IMAGENAME eq shadps4.exe" //FO CSV //NH | grep -qi shadps4; }

[ -f "$exe" ] || { echo "no emulator at $exe" >&2; exit 1; }
[ -f "$root/games/CUSA12392/eboot.bin" ] || { echo "the game is not in games/CUSA12392" >&2; exit 1; }
if running; then
    echo "the emulator is running already: close it first" >&2
    exit 1
fi

# The headset.
xr_env=()
xr=${XR:-sim}
if [ "$xr" = "sim" ]; then
    sim=${OPENXR_SIM:-E:\\Github\\OpenXR-Simulator\\bin\\openxr_simulator.json}
    if [ -f "$(cygpath -u "$sim")" ]; then
        xr_env=(XR_RUNTIME_JSON="$sim")
    else
        echo "OpenXR Simulator not found ($sim): measuring on the monitor"
        xr=none
    fi
elif [ "$xr" != "none" ]; then
    xr_env=(XR_RUNTIME_JSON="$xr")
fi
[ "$xr" = "none" ] && xr_env=(SHADPS4_OPENXR=0)

if [ "$uncapped" != "0" ]; then
    pace_env=(SHADPS4_VR_UNCAPPED="$uncapped")
else
    pace_env=(SHADPS4_VR_FPS_CAP="${FPS_CAP:-90}" SHADPS4_VR_PACE=1)
fi

# The build, with a user folder of its own (the play folder's settings, the frame rate shown).
mkdir -p "$bin/user" "$out"
cp "$exe" "$bin/shadps4.exe"
[ -f "${exe%.exe}.pdb" ] && cp "${exe%.exe}.pdb" "$bin/shadps4.pdb"
cp -r "$root/pc-vr/user/input_config" "$bin/user/" 2>/dev/null
python - "$(cygpath -w "$root/pc-vr/user/config.json")" "$(cygpath -w "$bin/user/config.json")" <<'EOF'
import json, sys
c = json.load(open(sys.argv[1]))
c.setdefault('General', {})['show_fps_counter'] = True
json.dump(c, open(sys.argv[2], 'w'), indent=2)
EOF

# One run: <dir> <seconds> <input script> [NAME=value ...]
play() {
    local dir=$1 seconds=$2 script=$3
    shift 3
    local user=$bin/user
    rm -rf "$dir" "$user/screenshots" "$user/log"
    mkdir -p "$dir"
    echo 1000000 > "$user/auto_shot_every"
    (cd "$bin" && env SHADPS4_BENCH=10 SHADPS4_VR_DEMO=0 SHADPS4_TITLE_EYE_WIDTH=2880 \
        SHADPS4_TITLE_RESOLUTION=6 SHADPS4_VR_SHARPEN=0.3 SHADPS4_VR_FOV_OF=headset \
        SHADPS4_XR_HEAD=0 SHADPS4_XR_PAUSE=0 SHADPS4_XR_WAIT=20 SHADPS4_SHOT_SECONDS=15 \
        SHADPS4_INPUT_SCRIPT="$(cygpath -w "$script")" "${xr_env[@]}" "${pace_env[@]}" "$@" \
        ./shadps4.exe -g "$eboot" ${GAME_ARGS:+-- $GAME_ARGS} > "$dir/stdout.txt" 2>&1) &
    local pid=$!
    sleep "$seconds"
    local wpid
    wpid=$(tasklist //FI "IMAGENAME eq shadps4.exe" //FO CSV //NH | grep -i shadps4 | head -1 | cut -d, -f2 | tr -d '"')
    [ -n "$wpid" ] && taskkill //PID "$wpid" > /dev/null 2>&1
    for _ in $(seq 1 15); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    [ -n "$wpid" ] && taskkill //F //PID "$wpid" > /dev/null 2>&1
    wait $pid 2>/dev/null
    for _ in $(seq 1 60); do running || break; sleep 1; done
    cp "$user/log/shad_log.txt" "$dir/log.txt" 2>/dev/null
    cp -r "$user/screenshots" "$dir/shots" 2>/dev/null
    grep -a "Bench:" "$dir/log.txt" 2>/dev/null | sed 's/^.*Bench: //' > "$dir/bench.txt"
    grep -a "Headset: the title delivered" "$dir/log.txt" 2>/dev/null |
        sed 's/^.*Headset: //' > "$dir/headset.txt"
}

savedata=$bin/user/home/1000/savedata
if [ ! -d "$base/save/CUSA12392" ]; then
    echo "No save yet: playing the opening into World 1 to make one (4 minutes)..."
    rm -rf "$savedata/CUSA12392"
    mkdir -p "$savedata"
    play "$base/making-save" 240 "$root/tools/bench/opening-to-level1.txt"
    if [ ! -d "$savedata/CUSA12392" ]; then
        echo "the game made no save: see $base/making-save/log.txt" >&2
        exit 1
    fi
    mkdir -p "$base/save"
    cp -r "$savedata/CUSA12392" "$base/save/"
fi

save=${SAVE:-$base/save}
for r in $(seq 1 "$runs"); do
    rm -rf "$savedata/CUSA12392"
    mkdir -p "$savedata"
    cp -r "$save/CUSA12392" "$savedata/"
    echo "run $r of $runs..."
    play "$out/run-$r" "$run_seconds" "${SCRIPT:-$root/tools/bench/level1-walk.txt}" "$@"
done

python - "$out" "$from_window" "$runs" "$name" "$uncapped" "$xr" "$*" <<'EOF' | tee "$out/summary.txt"
import re, statistics, sys
out, first, runs, name, uncapped, xr, extra = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), *sys.argv[4:]
bench = re.compile(r"(\d+) frames in ([\d.]+) s .*?avg ([\d.]+) p99 ([\d.]+) max ([\d.]+); over 25 ms "
                   r"(\d+), over 50 ms (\d+), over 100 ms (\d+); CPU ms a frame: GPU thread ([\d.]+), "
                   r"process ([\d.]+)")
head = re.compile(r"delivered ([\d.]+) frames a second .*?, ([\d.]+) were shown")
print(f"{name}: {runs} run(s), {'uncapped at ' + uncapped + ' Hz' if uncapped != '0' else 'paced by the headset'}, "
      f"headset: {xr}{', ' + extra if extra else ''}; from 10 s window {first} on")
print(f"{'run':>4} {'fps':>6} {'p99 ms':>7} {'1% low':>7} {'max ms':>7} {'>25ms':>6} {'GPU thr':>8} {'process':>8} {'shown':>6}")
rows = []
for r in range(1, runs + 1):
    try:
        lines = open(f"{out}/run-{r}/bench.txt", encoding="utf-8", errors="replace").read().splitlines()
    except OSError:
        lines = []
    found = [m for m in (bench.search(l) for l in lines) if m][first:]
    if not found:
        print(f"{r:>4}  no data (the run ended early? see run-{r}/log.txt)")
        continue
    frames = sum(int(m[1]) for m in found); secs = sum(float(m[2]) for m in found)
    w = lambda i: sum(float(m[i]) * int(m[1]) for m in found) / frames
    p99 = max(float(m[4]) for m in found)
    row = dict(fps=frames / secs, p99=p99, low=1000.0 / p99, mx=max(float(m[5]) for m in found),
               over25=sum(int(m[6]) for m in found), gpu=w(9), proc=w(10))
    try:
        hl = [m for m in (head.search(l) for l in open(f"{out}/run-{r}/headset.txt", encoding="utf-8",
                                                       errors="replace")) if m][first:]
    except OSError:
        hl = []
    row['shown'] = statistics.mean(float(m[2]) for m in hl) if hl else float('nan')
    rows.append(row)
    print(f"{r:>4} {row['fps']:6.1f} {row['p99']:7.2f} {row['low']:7.1f} {row['mx']:7.1f} {row['over25']:>6} "
          f"{row['gpu']:8.3f} {row['proc']:8.3f} {row['shown']:6.1f}")
if len(rows) > 1:
    med = lambda k: statistics.median(x[k] for x in rows)
    print(f"{'med':>4} {med('fps'):6.1f} {med('p99'):7.2f} {med('low'):7.1f} {med('mx'):7.1f} {med('over25'):>6.0f} "
          f"{med('gpu'):8.3f} {med('proc'):8.3f} {med('shown'):6.1f}")
print("fps: the game's own frames a second (no repeats). p99 / 1% low: the slowest 10-second window's "
      "99th-percentile frame. GPU thr / process: CPU ms a frame of the emulator's GPU command "
      "thread / of the whole emulator. shown: new frames the headset showed a second.")
EOF
