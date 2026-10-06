# Sums up the emulator's per-picture headset log, xr_frames.csv (SHADPS4_XR_FRAME_LOG=1; written
# to the log folder, e.g. build/pc-bench/bin/user/log/ after a tools/pc-bench.sh run). Each row
# is one picture the headset took: when (ms since the XR session started), which of the game's
# frames it showed, whether that frame was new (1) or a repeat of the one before (0), and how
# long the frame had waited between being handed over and being taken.
#
# Prints, for the pictures from [from_ms] on (default 85000: inside level 1 of the scripted
# walk) up to the last new frame: how many pictures repeated the one before (and how many a
# second), how many of the game's frames were never shown, the first few repeats with the
# pictures around them ("<frame id>[R] w<waited ms>"), and percentiles of the waits.
#
# usage: python tools/bench/xrframes.py <xr_frames.csv> [from_ms]
import csv, sys
rows=[r for r in csv.DictReader(open(sys.argv[1])) if r.get('ms') and r.get('waited_ms')]
start=float(sys.argv[2]) if len(sys.argv)>2 else 85000
last_new=max(i for i,r in enumerate(rows) if r['new']=='1')
lv=[r for r in rows[:last_new+1] if float(r['ms'])>start]
reps=[i for i,r in enumerate(lv) if r['new']=='0']
secs=(float(lv[-1]['ms'])-float(lv[0]['ms']))/1000
ids=[int(r['frame']) for r in lv if r['new']=='1']
skips=sum(b-a-1 for a,b in zip(ids,ids[1:]) if b>a)
print(f"{len(lv)} pictures in {secs:.1f} s from {start/1000:.0f} s on: {len(reps)} repeated the one before ({len(reps)/secs:.2f} a second), {skips} of the title's frames never shown")
for i in reps[:8]:
    ctx=lv[max(0,i-3):i+3]
    print('   ', ' | '.join(f"{r['frame']}{'' if r['new']=='1' else 'R'} w{float(r['waited_ms']):.1f}" for r in ctx))
w=sorted(float(r['waited_ms']) for r in lv if r['new']=='1')
print('waited ms percentiles 0.1/1/5/25/50/75/95/99/99.9:', [round(w[min(len(w)-1,int(len(w)*q))],2) for q in (0.001,0.01,0.05,0.25,0.5,0.75,0.95,0.99,0.999)])
