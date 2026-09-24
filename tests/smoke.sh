#!/usr/bin/env bash
# End-to-end smoke test: the whole desktop in a vc box (a nested atrium with
# its own seat), driven over IPC. The unit tests cover the logic; this proves
# the pieces start, talk and shut down together, ideally under ASan.
#
#   tests/smoke.sh [BUILD_DIR]      (default: build-asan, else build)
#   ATRIUM_SMOKE_LOG=FILE keeps atrium's log (-d) there.
#
# Needs vc (~/dev/c/vctools), foot and xmessage, and the build's ime_probe and x11_probe. Runs under its own D-Bus
# session so the shell can't take the live session's notification server.
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-}
if [[ -z $build ]]; then
    build=$root/build-asan
    [[ -x $build/src/atrium ]] || build=$root/build
fi
build=$(cd "$build" && pwd)
atrium=$build/src/atrium
ctl=$build/src/atriumctl
box=atrium-smoke
work=$(mktemp -d "${TMPDIR:-/tmp}/atrium-smoke.XXXXXX")
mkdir -p "$work/desk" "$work/state" "$work/cache"
log=$work/atrium.log
failures=0

pass() { printf '  \e[32mok\e[0m   %s\n' "$1"; }
fail() { printf '  \e[31mFAIL\e[0m %s\n' "$1"; failures=$((failures + 1)); }
# check NAME CMD...: passes when CMD succeeds within 10 s.
check() {
    local name=$1; shift
    for _ in $(seq 50); do
        if "$@" >/dev/null 2>&1; then pass "$name"; return 0; fi
        sleep 0.2
    done
    fail "$name"
    ctl windows 2>/dev/null | sed 's/^/         /'
    [[ -s $work/x11.log ]] && sed 's/^/         x11_probe: /' "$work/x11.log"
    # Which step a lost X11 window missed (PLAN: lost X11 window).
    grep 'x11: window' "$log" | tail -12 | sed 's/^/         /'
    return 1
}
ctl() { "$ctl" -s "$sock" "$@"; }
json() { ctl -j "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if ($2) else 1)"; }

cleanup() {
    vc box kill "$box" >/dev/null 2>&1
    [[ -n ${ATRIUM_SMOKE_LOG:-} ]] && cp "$log" "$ATRIUM_SMOKE_LOG"
    rm -rf "$work"
}
trap cleanup EXIT

echo "atrium smoke test ($build)"
vc box start "$box" --size 1280x800 --hide "${ATRIUM_SMOKE_HIDE:-4}" >/dev/null 2>&1 || { echo "vc box start failed"; exit 1; }
vc box exec "$box" -- dbus-run-session sh -c "
    ASAN_OPTIONS=detect_leaks=0 XDG_CACHE_HOME=$work/cache XDG_STATE_HOME=$work/state \
    ATRIUM_DESKTOP_DIR=$work/desk $atrium -d -c $work/registry.db >$log 2>&1
    echo \$? >$work/exit" >/dev/null 2>&1 &

# atrium says where its socket is.
sock=
for _ in $(seq 100); do
    sock=$(sed -n 's/.*ipc: listening on \(.*\)$/\1/p' "$log" 2>/dev/null | head -1)
    [[ -n $sock ]] && break
    sleep 0.2
done
[[ -n $sock ]] && pass "compositor is up ($(ctl version))" || { fail "compositor never opened its socket"; tail -20 "$log"; exit 1; }

check "the bar and the Dock appear" json layers \
    "{'atrium-bar','atrium-dock'} <= {l['namespace'] for l in d if l['mapped']}"

ctl action spawn foot >/dev/null
check "a Wayland window opens (foot)" json windows "any(w['app_id'] == 'foot' for w in d)"
ctl action spawn "xmessage -name smoke -geometry +200+150 'atrium smoke test'" >/dev/null
check "an X11 window opens (xmessage)" json windows "any(w['xwayland'] for w in d)"
check "where it asked to be (-geometry)" json windows "any(w['xwayland'] and w['geometry']['x'] == 200 for w in d)"
ctl action spawn "sh -c '$build/tests/x11_probe splash > $work/x11.log 2>&1; echo exit \$? >> $work/x11.log'" >/dev/null
check "a splash screen opens without taking focus" json windows \
    "any(w['app_id'] == 'x11probe' and not w['focused'] for w in d) and any(w['focused'] for w in d)"

foot=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id']=='foot'][0])")
ctl send "$foot" 2 >/dev/null
check "a window moves to space 2" json windows "any(w['id'] == $foot and w['space'] == '2' for w in d)"
ctl space 2 >/dev/null
check "space 2 is shown" json spaces "any(s['number'] == 2 and s['shown'] and not s['secret'] for s in d)"
ctl space 1 >/dev/null

# Tiling: the windows of the space share the screen, and float again after.
ctl space 2 >/dev/null
ctl action spawn foot >/dev/null
check "a second window on space 2" json windows "sum(w['space'] == '2' for w in d) == 2"
ctl action toggle-tiling >/dev/null
check "the space tiles" json windows "all(w['tiled'] for w in d if w['space'] == '2')"
check "side by side, not overlapping" json windows "(lambda a, b: a['x'] + a['width'] <= b['x'] or b['x'] + b['width'] <= a['x'])(*[w['geometry'] for w in d if w['space'] == '2'])"
ctl action toggle-tiling >/dev/null
check "and floats again" json windows "not any(w['tiled'] for w in d)"

# Dragged hard against the screen's left end, a window rides to the space before.
read -r drag gx gy < <(ctl -j windows | python3 -c "
import json,sys
w=[w for w in json.load(sys.stdin) if w['space'] == '2' and w['focused']][0]; g=w['geometry']
print(w['id'], g['x'] + g['width'] // 2, g['y'] + 12)")
vc in "$box" move "$gx" "$gy" down >/dev/null 2>&1
for _ in $(seq $((gx / 50 + 12))); do vc in "$box" moverel -50 0 >/dev/null 2>&1; done
vc in "$box" up >/dev/null 2>&1
check "a window pushed past the screen's edge moves to the space before" json windows \
    "any(w['id'] == $drag and w['space'] == '1' for w in d)"
ctl space 1 >/dev/null

ctl action overview >/dev/null
sleep 0.5
ctl action overview >/dev/null
check "Mission Control opens and closes" ctl version
ctl action app-expose >/dev/null
sleep 0.5
ctl action app-expose >/dev/null
check "app exposé opens and closes" ctl version

ctl secret smoke >/dev/null
check "a secret space shows" json spaces "any(s['secret'] and s['shown'] for s in d)"
ctl secret smoke >/dev/null
check "and hides again" json spaces "not any(s['secret'] and s['shown'] for s in d)"

ctl set appearance.blur false >/dev/null
check "a setting changes" json get "d.get('appearance.blur') is False"
ctl reset appearance.blur >/dev/null
check "and resets" json get "d.get('appearance.blur') is True"

# windows.fullscreen_space: fullscreen gets a space of its own, and comes home.
ctl set windows.fullscreen_space true >/dev/null
read -r fs home < <(ctl -j windows | python3 -c "
import json,sys
w=[w for w in json.load(sys.stdin) if w['focused']][0]; print(w['id'], w['space'])")
ctl action fullscreen >/dev/null
check "fullscreen, a window gets a space of its own" json windows \
    "any(w['id'] == $fs and w['fullscreen'] and w['space'] != '$home' for w in d)"
ctl action fullscreen >/dev/null
check "and comes back after" json windows "any(w['id'] == $fs and not w['fullscreen'] and w['space'] == '$home' for w in d)"
ctl reset windows.fullscreen_space >/dev/null

# A pointing device's own settings, over the shared ones.
check "the pointer is listed" json devices "len(d) >= 1"
dev=$(ctl -j devices | python3 -c "import json,sys; print(json.load(sys.stdin)[0]['name'])")
ctl device "$dev" speed=0.25 >/dev/null
check "a device keeps a speed of its own" json devices "d[0]['speed'] == 0.25"
ctl device "$dev" speed=null >/dev/null
check "and gives it back" json devices "d[0]['speed'] is None"

# An input method: keys go to it, what it composes lands in the app.
ctl action spawn "foot -a ime-target sh -c 'head -1 > $work/typed'" >/dev/null
check "a text field takes focus" json windows "any(w['app_id'] == 'ime-target' and w['focused'] for w in d)"
ctl action spawn "sh -c '$build/tests/ime_probe > $work/ime.log 2>&1'" >/dev/null
check "an input method starts" grep -q activate "$work/ime.log"
vc in "$box" key a key b >/dev/null 2>&1
check "it types into the app" grep -qx "αIME>" "$work/typed"

# A right-click on a title bar: the window menu, gone again on Escape.
read -r mx my < <(ctl -j windows | python3 -c "
import json,sys
g=[w for w in json.load(sys.stdin) if w['app_id'] == 'foot' and w['space'] == '1'][0]['geometry']
print(max(g['x'], 0) + 60, g['y'] + 12)")
vc in "$box" move "$mx" "$my" >/dev/null 2>&1
vc in "$box" moverel 1 0 >/dev/null 2>&1
vc in "$box" click right >/dev/null 2>&1
check "right-clicking a title bar opens the window menu" json layers \
    "any(l['namespace'] == 'atrium-window-menu' and l['mapped'] for l in d)"
vc in "$box" key Escape >/dev/null 2>&1
check "and Escape closes it" json layers "not any(l['namespace'] == 'atrium-window-menu' and l['mapped'] for l in d)"

# A tab torn out of a window rides the drag (xdg-toplevel-drag).
ctl action spawn "sh -c '$build/tests/drag_probe > $work/drag.log 2>&1'" >/dev/null
check "a window to tear a tab from" grep -qx ready "$work/drag.log"
check "and it is on screen" json windows "any(w['app_id'] == 'drag-probe' for w in d)"
read -r px py < <(ctl -j windows | python3 -c "
import json,sys
g=[w for w in json.load(sys.stdin) if w['app_id'] == 'drag-probe'][0]['geometry']
print(g['x'] + g['width'] // 2, g['y'] + g['height'] // 2)")
vc in "$box" move "$px" "$py" >/dev/null 2>&1
vc in "$box" down >/dev/null 2>&1
check "pressing tears one off" json windows "any(w['app_id'] == 'drag-probe-torn' for w in d)"
for _ in 1 2 3 4 5 6; do vc in "$box" moverel 25 15 >/dev/null 2>&1; done
check "which follows the pointer" json windows \
    "any(w['app_id'] == 'drag-probe-torn' and w['geometry']['x'] == $px + 150 - 20 for w in d)"
vc in "$box" up >/dev/null 2>&1
check "and stays where it was let go" grep -qE "dropped|cancelled" "$work/drag.log"

# An app that asks for its window back gets it where it was (xdg-session-management).
# Off, atrium's own "reopen where it was" can't be what puts it back.
ctl set windows.remember_placement false >/dev/null
ctl action spawn "sh -c 'echo \$\$ > $work/session.pid; exec $build/tests/session_probe > $work/session.log 2>&1'" >/dev/null
check "an app starts a session" grep -q "^created " "$work/session.log"
check "with its window" json windows "any(w['app_id'] == 'session-probe' for w in d)"
sp=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id'] == 'session-probe'][0])")
ctl move "$sp" 130 170 >/dev/null
sleep 1.5  # atrium writes it down once it holds still
kill "$(cat "$work/session.pid")"
check "the app quits" json windows "not any(w['app_id'] == 'session-probe' for w in d)"
sid=$(awk '/^created/ {print $2}' "$work/session.log")
ctl action spawn "sh -c 'echo \$\$ > $work/session.pid; exec $build/tests/session_probe $sid > $work/session2.log 2>&1'" >/dev/null
check "started again, it gets its session back" grep -qx "window restored" "$work/session2.log"
check "and its window where it was" json windows \
    "any(w['app_id'] == 'session-probe' and w['geometry']['x'] == 130 and w['geometry']['y'] == 170 for w in d)"
kill "$(cat "$work/session.pid")"
ctl reset windows.remember_placement >/dev/null

xm=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id'] == 'Xmessage'][0])")
ctl close "$xm" >/dev/null
check "a window closes" json windows "not any(w['id'] == $xm for w in d)"

# A last look for a human, once the animations have finished. The box sits
# on a hidden workspace, where the host hands out frames only when asked:
# the first capture wakes it and may still show the frame before.
# A second screen comes and goes; its windows come back.
ctl output create >/dev/null
check "a second screen is plugged in" json outputs "len(d) == 2"
check "and gets a bar" json layers "sum(l['namespace'] == 'atrium-bar' for l in d) == 2"
check "one Dock: only one screen keeps room for it" json outputs "len({o['usable']['height'] for o in d}) == 2"
x2=$(ctl -j outputs | python3 -c "import json,sys; print(max(o['geometry']['x'] for o in json.load(sys.stdin)))")
out2=$(ctl -j outputs | python3 -c "import json,sys; print(max(json.load(sys.stdin), key=lambda o: o['geometry']['x'])['name'])")
ctl move "$foot" $((x2 + 40)) 200 >/dev/null
check "a window moved onto it joins its space" json windows "any(w['id'] == $foot and w['output'] == '$out2' for w in d)"
# Closed there, an app's window reopens there, whichever screen has focus.
ctl action spawn "foot -a smoke-reopen" >/dev/null
check "another window opens" json windows "any(w['app_id'] == 'smoke-reopen' for w in d)"
re=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id'] == 'smoke-reopen'][0])")
ctl move "$re" $((x2 + 60)) 150 >/dev/null
check "and goes to the second screen" json windows "any(w['id'] == $re and w['output'] == '$out2' for w in d)"
ctl close "$re" >/dev/null
check "and closes there" json windows "not any(w['app_id'] == 'smoke-reopen' for w in d)"
other=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['output'] != '$out2' and not w['minimized']][0])")
ctl focus "$other" >/dev/null
ctl action spawn "foot -a smoke-reopen" >/dev/null
check "reopened, it comes back on the second screen" json windows "any(w['app_id'] == 'smoke-reopen' and w['output'] == '$out2' for w in d)"
ctl output remove "$out2" >/dev/null
check "unplugged, the window comes back" json windows "any(w['id'] == $foot and w['output'] != '$out2' for w in d)"
check "the last screen can't be removed" sh -c "! $ctl -s $sock output remove \$($ctl -s $sock -j outputs | python3 -c 'import json,sys; print(json.load(sys.stdin)[0][\"name\"])')"

vc wait "$box" settle 5 >/dev/null 2>&1
vc in "$box" shot '' "$work/wake.png" >/dev/null 2>&1
sleep 0.5
vc in "$box" shot '' "${ATRIUM_SMOKE_SHOT:-$work/last.png}" >/dev/null 2>&1

ctl action quit >/dev/null
for _ in $(seq 100); do [[ -f $work/exit ]] && break; sleep 0.2; done
code=$(cat "$work/exit" 2>/dev/null || echo "none")
[[ $code == 0 ]] && pass "quits cleanly" || fail "quit: exit status $code"
if grep -q "ERROR: AddressSanitizer\|runtime error:" "$log"; then
    fail "sanitizer report"
    grep -A12 "ERROR: AddressSanitizer\|runtime error:" "$log" | head -40
else
    pass "no sanitizer reports"
fi

if ((failures)); then
    echo "$failures failed; last lines of the log:"
    grep -v DEBUG "$log" | tail -15
    exit 1
fi
echo "all passed"
