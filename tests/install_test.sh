#!/usr/bin/env bash
# install.sh's look at the machine, against made-up machines: sysfs, /etc and
# desktop files under a temporary root, pacman and systemctl answered here.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

export ATRIUM_INSTALL_LIB=1 ATRIUM_SYS=$root/sys ATRIUM_ETC=$root/etc ATRIUM_APPS=$root/apps ATRIUM_MODULES=$root/modules
# shellcheck source=../install.sh
. "$here/../install.sh"
XDG_DATA_HOME=$root/home

failed=0 ran=0
check() {
    local name=$1 want=$2 got=$3
    ((ran++))
    if [[ $got != "$want" ]]; then
        printf 'FAIL %s\n  want: %q\n  got:  %q\n' "$name" "$want" "$got"
        ((failed++))
    fi
}
yes_no() { "$@" && echo yes || echo no; }

reset() {
    rm -rf "$root"/{sys,etc,apps,modules,home}
    mkdir -p "$root"/{sys/bus/pci/devices,sys/class/power_supply,etc/systemd/system,apps,modules,home}
}
gpu() {  # gpu NAME VENDOR DEVICE [CLASS]
    local d=$root/sys/bus/pci/devices/$1
    mkdir -p "$d"
    echo "$2" >"$d/vendor"
    echo "$3" >"$d/device"
    echo "${4:-0x030000}" >"$d/class"
}
os() { printf '%s\n' "$@" >"$root/etc/os-release"; }

# --- distributions
reset
os 'ID=arch' 'PRETTY_NAME="Arch Linux"'
check "arch" yes "$(yes_no is_arch)"
os 'ID=cachyos' 'ID_LIKE=arch'
check "cachyos (like arch)" yes "$(yes_no is_arch)"
os 'ID=manjaro' 'ID_LIKE="arch"'
check "manjaro (quoted)" yes "$(yes_no is_arch)"
os 'ID=ubuntu' 'ID_LIKE=debian'
check "ubuntu" no "$(yes_no is_arch)"
os 'ID=archcraft' 'ID_LIKE=archlinux'
check "a like that only starts with arch" no "$(yes_no is_arch)"
rm "$root/etc/os-release"
check "no os-release" no "$(yes_no is_arch)"

# --- graphics
reset
gpu 0000:01:00.0 0x10de 0x2684
gpu 0000:00:02.0 0x8086 0xa780
gpu 0000:00:1f.3 0x8086 0x7a50 0x040300  # sound: not graphics
check "gpus" $'intel\nnvidia' "$(gpus)"
check "Ada: open" open "$(nvidia_driver)"
reset
gpu 0000:01:00.0 0x10de 0x1b80
check "Pascal: 580xx" 580xx "$(nvidia_driver)"
reset
gpu 0000:01:00.0 0x10de 0x1e04
check "first Turing: open" open "$(nvidia_driver)"
reset
gpu 0000:01:00.0 0x10de 0x1340
check "first Maxwell: 580xx" 580xx "$(nvidia_driver)"
reset
gpu 0000:01:00.0 0x10de 0x1180
check "Kepler: legacy" legacy "$(nvidia_driver)"
reset
gpu 0000:01:00.0 0x10de 0x1180
gpu 0000:02:00.0 0x10de 0x1b80
gpu 0000:03:00.0 0x10de 0x2684
check "the newest decides" open "$(nvidia_driver)"
reset
gpu 0000:00:02.0 0x1002 0x15bf
check "amd, no nvidia driver" amd/ "$(gpus)/$(nvidia_driver)"
reset
check "no gpus" "" "$(gpus)"

# --- laptop, bluetooth
reset
mkdir -p "$root/sys/class/power_supply/AC" "$root/sys/class/power_supply/BAT0"
echo Mains >"$root/sys/class/power_supply/AC/type"
check "mains only" no "$(yes_no has_battery)"
echo Battery >"$root/sys/class/power_supply/BAT0/type"
check "battery" yes "$(yes_no has_battery)"
check "no bluetooth" no "$(yes_no has_bluetooth)"
mkdir -p "$root/sys/class/bluetooth"
check "bluetooth class, no adapter" no "$(yes_no has_bluetooth)"
mkdir -p "$root/sys/class/bluetooth/hci0"
check "an adapter" yes "$(yes_no has_bluetooth)"

# --- login screen
reset
check "no display manager" "" "$(current_dm)"
ln -s /usr/lib/systemd/system/sddm.service "$root/etc/systemd/system/display-manager.service"
check "sddm" sddm "$(current_dm)"
ln -sf /usr/lib/systemd/system/atrium-login.service "$root/etc/systemd/system/display-manager.service"
check "atrium-login" atrium-login "$(current_dm)"

# --- network
enabled_units=()
unit_enabled() { [[ " ${enabled_units[*]} " == *" $1 "* ]]; }
enabled_units=(systemd-networkd.service)
check "networkd" systemd-networkd "$(other_network)"
enabled_units=(systemd-networkd.service iwd.service)
check "networkd and iwd" $'systemd-networkd\niwd' "$(other_network)"
enabled_units=(NetworkManager.service iwd.service)
check "NetworkManager already (with iwd as its backend)" "" "$(other_network)"
enabled_units=()
check "nothing" "" "$(other_network)"

# --- apps
reset
app() { printf '[Desktop Entry]\nName=%s\nCategories=%s\n%s' "$1" "$2" "${3:-}" >"$root/apps/$1.desktop"; }
check "no browser" no "$(yes_no has_app WebBrowser)"
app firefox 'Network;WebBrowser;'
check "browser" yes "$(yes_no has_app WebBrowser)"
app hidden 'Utility;TextEditor;' 'NoDisplay=true'
check "hidden editor doesn't count" no "$(yes_no has_app TextEditor)"
app mousepad 'Utility;TextEditor'
check "editor, category last" yes "$(yes_no has_app TextEditor)"
app notfm 'System;FileManagerish;'
check "a category is a whole word" no "$(yes_no has_app FileManager)"
mkdir -p "$root/home/applications"
printf '[Desktop Entry]\nName=nemo\nCategories=System;FileManager;\n' >"$root/home/applications/nemo.desktop"
check "file manager in the user's own apps" yes "$(yes_no has_app FileManager)"

# --- kernel headers
reset
mkdir -p "$root/modules/6.17.1-arch1-1" "$root/modules/6.12.50-1-lts" "$root/modules/extramodules-6.17"
echo linux >"$root/modules/6.17.1-arch1-1/pkgbase"
echo linux-lts >"$root/modules/6.12.50-1-lts/pkgbase"
check "headers" $'linux-headers\nlinux-lts-headers' "$(kernel_headers)"

# --- the release's asset
compact='{"tag_name":"v0.2.0","name":"atrium 0.2.0","assets":[{"url":"https://api.github.com/x/1","name":"notes.txt","digest":"sha256:aaaa","browser_download_url":"https://github.com/mKonic/atrium/releases/download/v0.2.0/notes.txt"},{"url":"https://api.github.com/x/2","name":"atrium-git-0.2.0-1-x86_64.pkg.tar.zst","uploader":{"login":"github-actions[bot]"},"digest":"sha256:0123abcd","browser_download_url":"https://github.com/mKonic/atrium/releases/download/v0.2.0/atrium-git-0.2.0-1-x86_64.pkg.tar.zst"}]}'
check "compact JSON" $'https://github.com/mKonic/atrium/releases/download/v0.2.0/atrium-git-0.2.0-1-x86_64.pkg.tar.zst\t0123abcd' "$(asset_from_json <<<"$compact")"
pretty='{
  "tag_name": "v0.2.0",
  "name": "atrium 0.2.0",
  "assets": [
    {
      "name": "atrium-git-0.2.0-1-x86_64.pkg.tar.zst",
      "digest": null,
      "browser_download_url": "https://github.com/mKonic/atrium/releases/download/v0.2.0/atrium-git-0.2.0-1-x86_64.pkg.tar.zst"
    }
  ]
}'
check "pretty JSON, no digest" $'https://github.com/mKonic/atrium/releases/download/v0.2.0/atrium-git-0.2.0-1-x86_64.pkg.tar.zst\t' "$(asset_from_json <<<"$pretty")"
older='{"assets":[{"name":"notes.txt","digest":"sha256:aaaa","browser_download_url":"https://example.org/notes.txt"},{"name":"atrium-git-0.1.0-1-x86_64.pkg.tar.zst","browser_download_url":"https://example.org/atrium-git-0.1.0-1-x86_64.pkg.tar.zst"}]}'
check "another file's digest isn't the package's" $'https://example.org/atrium-git-0.1.0-1-x86_64.pkg.tar.zst\t' "$(asset_from_json <<<"$older")"
check "no package asset" fail "$(asset_from_json <<<'{"assets":[]}' || echo fail)"

# --- asking (the prompt has no newline: the answer is its last word)
answer() { local out; out=$(yes_no ask "$@" 2>/dev/null); echo "${out##*[[:space:]]}"; }
YES=0
check "Enter takes the default (yes)" yes "$(answer Q y <<<'')"
check "Enter takes the default (no)" no "$(answer Q n <<<'')"
check "n" no "$(answer Q y <<<'n')"
check "YES, after a wrong answer" yes "$(answer Q n <<<$'maybe\nYES')"
check "end of input takes the default" no "$(answer Q n </dev/null)"
YES=1
check "--yes takes the default" yes "$(answer Q y </dev/null)"
check "--yes takes a no default" no "$(answer Q n </dev/null)"

echo "$((ran - failed))/$ran passed"
((failed == 0))
