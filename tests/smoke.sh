#!/usr/bin/env bash
# End-to-end smoke test: the whole desktop in a vc box (a nested atrium with
# its own seat), driven over IPC. The unit tests cover the logic; this proves
# the pieces start, talk and shut down together, ideally under ASan.
#
#   tests/smoke.sh [BUILD_DIR]      (default: build-asan, else build)
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
    return 1
}
ctl() { "$ctl" -s "$sock" "$@"; }
json() { ctl -j "$1" | python3 -c "import json,sys; d=json.load(sys.stdin); sys.exit(0 if ($2) else 1)"; }

cleanup() {
    vc box kill "$box" >/dev/null 2>&1
    rm -rf "$work"
}
trap cleanup EXIT

echo "atrium smoke test ($build)"
vc box start "$box" --size 1280x800 --hide "${ATRIUM_SMOKE_HIDE:-4}" >/dev/null 2>&1 || { echo "vc box start failed"; exit 1; }
vc box exec "$box" -- dbus-run-session sh -c "
    ASAN_OPTIONS=detect_leaks=0 XDG_CACHE_HOME=$work/cache XDG_STATE_HOME=$work/state \
    ATRIUM_DESKTOP_DIR=$work/desk $atrium -c $work/registry.db >$log 2>&1
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
ctl action spawn "$build/tests/x11_probe splash" >/dev/null
check "a splash screen opens without taking focus" json windows \
    "any(w['app_id'] == 'x11probe' and not w['focused'] for w in d) and any(w['focused'] for w in d)"

foot=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id']=='foot'][0])")
ctl send "$foot" 2 >/dev/null
check "a window moves to space 2" json windows "any(w['id'] == $foot and w['space'] == '2' for w in d)"
ctl space 2 >/dev/null
check "space 2 is shown" json spaces "any(s['number'] == 2 and s['shown'] and not s['secret'] for s in d)"
ctl space 1 >/dev/null

ctl action overview >/dev/null
sleep 0.5
ctl action overview >/dev/null
check "Mission Control opens and closes" ctl version

ctl secret smoke >/dev/null
check "a secret space shows" json spaces "any(s['secret'] and s['shown'] for s in d)"
ctl secret smoke >/dev/null
check "and hides again" json spaces "not any(s['secret'] and s['shown'] for s in d)"

ctl set appearance.blur false >/dev/null
check "a setting changes" json get "d.get('appearance.blur') is False"
ctl reset appearance.blur >/dev/null
check "and resets" json get "d.get('appearance.blur') is True"

# An input method: keys go to it, what it composes lands in the app.
ctl action spawn "foot -a ime-target sh -c 'head -1 > $work/typed'" >/dev/null
check "a text field takes focus" json windows "any(w['app_id'] == 'ime-target' and w['focused'] for w in d)"
ctl action spawn "sh -c '$build/tests/ime_probe > $work/ime.log 2>&1'" >/dev/null
check "an input method starts" grep -q activate "$work/ime.log"
vc in "$box" key a key b >/dev/null 2>&1
check "it types into the app" grep -qx "αIME>" "$work/typed"

xm=$(ctl -j windows | python3 -c "import json,sys; print([w['id'] for w in json.load(sys.stdin) if w['app_id'] == 'Xmessage'][0])")
ctl close "$xm" >/dev/null
check "a window closes" json windows "not any(w['id'] == $xm for w in d)"

# A last look for a human, once the animations have finished. The box sits
# on a hidden workspace, where the host hands out frames only when asked:
# the first capture wakes it and may still show the frame before.
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
    tail -15 "$log"
    exit 1
fi
echo "all passed"
