#!/usr/bin/env python3
"""MechCommander Gold conformance harness (REPO_RULES section 9).

Three fixed corpora, each a pass/fail count, compared against the committed
baseline in conformance.json; a regression in any of them fails the run:

* **Milestones**: one headless run of the host with the scripted clicks that
  start the first campaign mission (docs/bringup.md, "Scripted input"), scored
  by the lines the host prints at each stage, in boot order.
* **Self-test**: `--selftest`, the lifted CRT string routines, the engine's own
  number parser, the printf float path and the VFX blitters, each against the
  host's answer for the same input.
* **Lift health**: from the generated tree: lift errors and UNIMPLEMENTED
  instructions.

The game is not in the repo. Without game/MCX.EXE and the built host this
prints "conformance: skipped" and exits 0, so it can sit in CI without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
    py -3 tools/conformance.py --exe build-trace/mcg_recomp.exe
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen')
BASELINE = os.path.join(ROOT, 'conformance.json')

# New Campaign (three clicks: the menu, then two confirmations), three drags
# from the Ready Area into the force groups, then launch. Then, in the mission
# (1280x1024), select the first 'Mech on the mech bar and send it down to the
# road south-east of the drop zone, where an armored car is waiting; the 'Mech
# engages it on its own. Seconds from the logistics shell coming up,
# which is when the host starts the script.
CLICKS = ('477,165@8;477,165@18;477,165@28;37,376>248,61@45;'
          '37,376>304,61@55;37,376>246,172@65;570,339@78;570,339@90;'
          '15,1005@115;764,891@120')
SECONDS = 230

# (name, what the host prints when it is reached). Order is boot order.
MILESTONES = [
    ('image mapped at 0x00600000', r'\[runtime\] mapped MCX\.EXE at 0x00600000'),
    ('every import bridged', r'\[runtime\] \d+ lifted functions, 204 import bridges'),
    ('static constructors run, balanced', r'\[initterm\] 26 constructors, 0 unbalanced'),
    ('window created', r'\[win\] CreateWindowExA\('),
    ('display mode set', r'\[ddraw\] SetDisplayMode2 1280x1024 8bpp'),
    ('first frame presented', r'\[capture\] frame 1 presented'),
    ('intro sequence done: the logistics shell', r'\[mission\] state \d+ -> 3'),
    ('a mission launches', r'\[mission\] state 3 -> 7'),
    ('scripted clicks all delivered', r'\[input\] firing 9 '),
    ('a click on open ground is a player order',
     r'(?s)\[input\] firing 9 .*\[engine\] Parser::SendTacOrder'),
    ("a 'Mech fires", r'\[engine\] BattleMech::fireWeapon'),
    ('an enemy vehicle is destroyed', r'\[engine\] GroundVehicleType::handleDestruction'),
    ('1000 frames', r'\[capture\] frame 1000 presented'),
    ('still in the mission at the time limit',
     r'(?s)\[mission\] state 3 -> 7(?:(?!\[mission\] state 7 -> ).)*\[watchdog\] \d+ s: stopping'),
    ('no fault', None),     # scored by absence, below
]
# --playthrough: campaign mission 1 ("In The Beginning") from the launch to the
# debriefing, about 17 minutes. The same start, then three cheat codes (reveal
# the map, invulnerable, every shot hits; enabled by the developers' own switch
# file, created for the run). Each objective is reached by clicking the
# minimap to put the camera on it and then clicking the target, which with the
# camera parked by the minimap is always at the same screen point. Targets are
# the mission script's own (docs/bringup.md, "Playing mission 1 through").
PLAY_CLICKS = ('477,165@8;477,165@18;477,165@28;37,376>248,61@45;'
               '37,376>304,61@55;37,376>246,172@65;570,339@78;570,339@90;'
               ':mineeyeshaveseentheglory@105;:osmiu@106;:deadeye@107;'
               '560,380>720,560@112;'                       # select the lance
               '86,89@116;625,507@119;648,557@200;'         # Camp Alpha: HQ, gas tower
               '88,125@262;733,642@265;668,544@470;550,477@590;'   # the Garrison
               '117,100@700;720,480@703;'                   # Camp Beta's guard
               '115,91@880;652,544@883;'                    # Camp Beta HQ
               '537,30@1000')                               # debrief: NEXT MISSION
PLAY_SECONDS = 1060
CHEAT_SWITCH = 'ixtlriimceourl'
PLAY_MILESTONES = [
    ('a mission launches', r'\[mission\] state 3 -> 7'),
    ('objective 0: Camp Alpha HQ and gas tower', r'objective 0 -> 1 '),
    ('objective 1: the three Garrison buildings', r'objective 1 -> 1 '),
    ('objective 2: Camp Beta HQ and its guard', r'objective 2 -> 1 '),
    ('the mission ends: the debriefing', r'\[mission\] state 7 -> 6'),
    ('NEXT MISSION: back to the logistics shell', r'\[mission\] state 6 -> 3'),
    ('no fault', None),
]
FAULT = re.compile(r'access violation|stack overflow|illegal instruction|'
                   r'integer divide by zero|in-page error')


def run(cmd, timeout):
    p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors='replace', timeout=timeout)
    return p.returncode, p.stdout


def milestones(exe, clicks=CLICKS, seconds=SECONDS, table=MILESTONES, logname='conformance.log'):
    _, log = run([exe, '--run', '--headless', '--watchdog', str(seconds), '--click', clicks],
                 seconds + 60)
    with open(os.path.join(ROOT, logname), 'w', encoding='utf-8') as fh:
        fh.write(log)
    got = []
    for name, rx in table:
        ok = not FAULT.search(log) if rx is None else bool(re.search(rx, log))
        got.append((name, ok))
    return got


def selftest(exe):
    _, log = run([exe, '--selftest'], 120)
    ok = len(re.findall(r'\[ ok \]', log))
    bad = len(re.findall(r'\[FAIL\]', log))
    return ok, ok + bad


def lift_health():
    stats = os.path.join(ROOT, 'work', 'phase1_codegen.json')
    with open(stats, encoding='utf-8') as fh:
        s = json.load(fh)
    unimpl = 0
    for f in glob.glob(os.path.join(GEN, 'recomp_*.c')):
        with open(f, encoding='utf-8', errors='replace') as fh:
            unimpl += fh.read().count('UNIMPLEMENTED:')
    return {'lifted': s['functions'], 'errors': s['errors'], 'unimplemented': unimpl}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join('build', 'mcg_recomp.exe'))
    ap.add_argument('--update', action='store_true')
    ap.add_argument('--playthrough', action='store_true',
                    help='also play mission 1 through to the debriefing (~18 min)')
    a = ap.parse_args()
    exe = os.path.join(ROOT, a.exe)
    if not os.path.exists(os.path.join(ROOT, 'game', 'MCX.EXE')) or not os.path.exists(exe):
        print('conformance: skipped (needs game/MCX.EXE and a built host '
              f'{a.exe}; run Setup.cmd first)')
        return 0

    ms = milestones(exe)
    st_ok, st_n = selftest(exe)
    lh = lift_health()
    passed = sum(ok for _, ok in ms)
    result = {'milestones': passed, 'of': len(ms), 'selftest': st_ok,
              'selftest_of': st_n, **lh}
    pt = []
    if a.playthrough:
        switch = os.path.join(ROOT, 'game', CHEAT_SWITCH)
        made = not os.path.exists(switch)
        if made:
            open(switch, 'w').close()
        try:
            pt = milestones(exe, PLAY_CLICKS, PLAY_SECONDS, PLAY_MILESTONES, 'playthrough.log')
        finally:
            if made:
                os.remove(switch)
        result['playthrough'] = sum(ok for _, ok in pt)
        result['playthrough_of'] = len(pt)

    for name, ok in ms + [(f'mission 1: {n}', ok) for n, ok in pt]:
        print(f'  [{"PASS" if ok else "FAIL"}] {name}')
    print(f'milestones {passed}/{len(ms)}   selftest {st_ok}/{st_n}   '
          f'lifted {lh["lifted"]}   errors {lh["errors"]}   '
          f'unimplemented {lh["unimplemented"]}')

    base = {}
    if os.path.exists(BASELINE):
        with open(BASELINE, encoding='utf-8') as fh:
            base = json.load(fh)
    worse = [k for k in ('milestones', 'selftest', 'lifted', 'playthrough')
             if k in result and result[k] < base.get(k, 0)]
    worse += [k for k in ('errors', 'unimplemented') if result[k] > base.get(k, result[k])]
    if a.update:
        if 'playthrough' not in result and 'playthrough' in base:   # keep it
            result['playthrough'], result['playthrough_of'] = base['playthrough'], base['playthrough_of']
        with open(BASELINE, 'w', encoding='utf-8') as fh:
            json.dump(result, fh, indent=1)
        print('baseline updated')
    elif worse:
        print(f'REGRESSION against conformance.json: {", ".join(worse)}')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
