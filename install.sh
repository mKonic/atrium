#!/usr/bin/env bash
# atrium's installer: looks at the machine, asks what it can't tell, installs
# the latest release and switches the login screen over.
#
#   curl -fsSL https://raw.githubusercontent.com/mKonic/atrium/master/install.sh | bash
#
#   --yes            take every default without asking
#   --package FILE   install this package instead of the latest release
#   --version vX.Y.Z install that release instead of the latest
#
# Run again to update or to change an answer: it only does what isn't done.

set -uo pipefail

REPO=mKonic/atrium
SYS=${ATRIUM_SYS:-/sys}
ETC=${ATRIUM_ETC:-/etc}
APPS=${ATRIUM_APPS:-/usr/share/applications}
MODULES=${ATRIUM_MODULES:-/usr/lib/modules}

YES=0
PACKAGE=
VERSION=
TMP=

# --- talking ----------------------------------------------------------------------------------

if [[ -t 1 ]]; then
    bold=$'\e[1m' dim=$'\e[2m' blue=$'\e[34m' red=$'\e[31m' plain=$'\e[0m'
else
    bold= dim= blue= red= plain=
fi

say() { printf '%s\n' "$*"; }
step() { printf '\n%s%s%s\n' "$bold" "$*" "$plain"; }
note() { printf '  %s%s%s\n' "$dim" "$*" "$plain"; }
fail() {
    printf '%s%s%s\n' "$red" "$*" "$plain" >&2
    exit 1
}

# ask "Question" y|n: true for yes. The default is taken with --yes or Enter.
ask() {
    local question=$1 default=$2 hint answer
    [[ $default == y ]] && hint="Y/n" || hint="y/N"
    if ((YES)); then
        printf '%s?%s %s [%s] %s\n' "$blue" "$plain" "$question" "$hint" "$default"
        [[ $default == y ]]
        return
    fi
    while true; do
        printf '%s?%s %s [%s] ' "$blue" "$plain" "$question" "$hint"
        read -r answer || answer=
        case "${answer,,}" in
            "") [[ $default == y ]]; return ;;
            y | yes) return 0 ;;
            n | no) return 1 ;;
        esac
    done
}

# --- the machine ------------------------------------------------------------------------------

# Arch or a distribution built on it (CachyOS, EndeavourOS, Manjaro...).
is_arch() {
    local id= like=
    [[ -r $ETC/os-release ]] || return 1
    id=$(. "$ETC/os-release" && echo "${ID:-}")
    like=$(. "$ETC/os-release" && echo "${ID_LIKE:-}")
    [[ $id == arch || " $like " == *" arch "* ]]
}

distro_name() {
    (. "$ETC/os-release" 2>/dev/null && echo "${PRETTY_NAME:-${NAME:-Linux}}")
}

# The display controllers' vendors, one per line: nvidia, amd, intel or other.
# sysfs, not lspci: lspci wakes a sleeping GPU (and may not be installed).
gpus() {
    local dev vendor
    for dev in "$SYS"/bus/pci/devices/*; do
        [[ -r $dev/class && $(<"$dev/class") == 0x03* ]] || continue
        vendor=$(<"$dev/vendor")
        case "$vendor" in
            0x10de) echo nvidia ;;
            0x1002) echo amd ;;
            0x8086) echo intel ;;
            *) echo other ;;
        esac
    done | sort -u
}

# Which NVIDIA driver the newest NVIDIA GPU takes: open (Turing and newer, with
# GSP firmware), 580xx (Maxwell to Volta), legacy (older), or nothing.
# Turing's device IDs start at 0x1e00, Maxwell's at 0x1340 (as Omarchy tells them).
nvidia_driver() {
    local dev id best=
    for dev in "$SYS"/bus/pci/devices/*; do
        [[ -r $dev/class && $(<"$dev/class") == 0x03* && $(<"$dev/vendor") == 0x10de ]] || continue
        id=$(($(<"$dev/device")))
        if ((id >= 0x1e00)); then
            best=open
        elif ((id >= 0x1340)); then
            [[ $best == open ]] || best=580xx
        else
            [[ -n $best ]] || best=legacy
        fi
    done
    echo "$best"
}

installed() { pacman -Qq "$1" &>/dev/null; }
available() { pacman -Si "$1" &>/dev/null; }

# An NVIDIA driver from any branch.
has_nvidia_driver() {
    pacman -Qqs '^nvidia(-open)?(-[0-9]+xx)?(-dkms)?$' &>/dev/null || pacman -Qqs '^nvidia(-[0-9]+xx)?-utils$' &>/dev/null
}

# Headers for every installed kernel, which DKMS builds the driver against.
kernel_headers() {
    local base
    for base in "$MODULES"/*/pkgbase; do
        [[ -r $base ]] && echo "$(<"$base")-headers"
    done | sort -u
}

has_battery() {
    local p
    for p in "$SYS"/class/power_supply/*; do
        [[ -r $p/type && $(<"$p/type") == Battery ]] && return 0
    done
    return 1
}

has_bluetooth() { [[ -d $SYS/class/bluetooth ]] && [[ -n $(ls -A "$SYS/class/bluetooth" 2>/dev/null) ]]; }

# The display manager systemd starts at boot (display-manager.service's target), or nothing.
current_dm() {
    local link
    link=$(readlink "$ETC/systemd/system/display-manager.service" 2>/dev/null) || return 0
    link=${link##*/}
    echo "${link%.service}"
}

unit_enabled() { systemctl is-enabled -q "$1" 2>/dev/null; }

# What runs the network now, when it isn't NetworkManager: systemd-networkd, iwd, dhcpcd, connman.
other_network() {
    local u
    unit_enabled NetworkManager.service && return 0
    for u in systemd-networkd iwd dhcpcd connman; do
        unit_enabled "$u.service" && echo "$u"
    done
}

# An installed app in a desktop category (TerminalEmulator, FileManager, WebBrowser, TextEditor).
has_app() {
    local category=$1 dir file
    for dir in $APPS ${XDG_DATA_HOME:-$HOME/.local/share}/applications /var/lib/flatpak/exports/share/applications; do
        for file in "$dir"/*.desktop; do
            [[ -r $file ]] || continue
            grep -q "^Categories=.*\b$category\b" "$file" && ! grep -q '^NoDisplay=true' "$file" && return 0
        done
    done
    return 1
}

# atrium starts any of these (src/terminal.hpp), or what xdg-terminal-exec names.
has_terminal() {
    local t
    for t in xdg-terminal-exec ghostty kitty foot alacritty wezterm konsole ptyxis gnome-terminal xfce4-terminal xterm; do
        command -v "$t" &>/dev/null && return 0
    done
    return 1
}

# --- the release ------------------------------------------------------------------------------

# The release's package: its URL and sha256 (GitHub's digest), tab-separated.
release_asset() {
    local api="https://api.github.com/repos/$REPO/releases/latest" json
    [[ -n $VERSION ]] && api="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
    json=$(curl -fsSL -H 'Accept: application/vnd.github+json' "$api") || return 1
    printf '%s' "$json" | asset_from_json
}

# From a release's JSON: the .pkg.tar.zst asset's URL and digest. jq isn't on a
# fresh system, so this reads GitHub's one-field-per-line layout.
asset_from_json() {
    local line url= digest= name=
    while IFS= read -r line; do
        case "$line" in
            *'"name":'*) name=$(sed -E 's/.*"name": *"([^"]*)".*/\1/' <<<"$line") ;;
            *'"digest":'*) [[ $name == *.pkg.tar.zst ]] && digest=$(sed -E 's/.*"digest": *"sha256:([0-9a-f]+)".*/\1/; t; s/.*//' <<<"$line") ;;
            *'"browser_download_url":'*)
                if [[ $line == *.pkg.tar.zst\"* ]]; then
                    url=$(sed -E 's/.*"browser_download_url": *"([^"]*)".*/\1/' <<<"$line")
                    break
                fi
                ;;
        esac
    done < <(sed 's/,"/,\n"/g; s/{"/{\n"/g')
    [[ -n $url ]] || return 1
    printf '%s\t%s\n' "$url" "$digest"
}

usage() {
    cat <<EOF
atrium's installer: looks at the machine, asks what it can't tell, installs
the latest release and switches the login screen over.

  --yes            take every default without asking
  --package FILE   install this package instead of the latest release
  --version vX.Y.Z install that release instead of the latest
EOF
}

# --- installing -------------------------------------------------------------------------------

# pacman, once more if it fails: a mirror that stalls mid-download is the usual reason.
pacman_twice() {
    sudo pacman "$@" && return 0
    note "pacman failed; trying once more."
    sudo pacman "$@"
}

main() {
    while (($#)); do
        case "$1" in
            -y | --yes) YES=1 ;;
            --package) PACKAGE=${2:-}; shift ;;
            --version) VERSION=${2:-}; shift ;;
            -h | --help) usage; exit 0 ;;
            *) fail "Unknown option: $1" ;;
        esac
        shift
    done

    # Piped into bash, stdin is the script itself: answers come from the terminal.
    if [[ ! -t 0 ]]; then
        if { : </dev/tty; } 2>/dev/null; then
            exec </dev/tty
        elif ((!YES)); then
            fail "No terminal to ask in: run it in one, or with --yes for the defaults."
        else
            exec </dev/null
        fi
    fi

    say "${bold}atrium${plain}, a floating-first Wayland desktop"

    # --- can it go here at all
    ((EUID != 0)) || fail "Run this as yourself, not root: it asks for sudo when it needs it."
    is_arch && command -v pacman &>/dev/null || fail "atrium installs on Arch Linux and distributions based on it ($(distro_name) isn't)."
    [[ $(uname -m) == x86_64 ]] || fail "atrium's packages are for x86_64; this is $(uname -m)."
    command -v sudo &>/dev/null || fail "sudo isn't installed: as root, run 'pacman -S sudo' and add yourself to the wheel group."
    if [[ -n $PACKAGE ]]; then
        [[ -r $PACKAGE ]] || fail "Can't read $PACKAGE."
    else
        command -v curl &>/dev/null || fail "curl isn't installed: sudo pacman -S curl"
    fi

    # --- what's here
    step "This computer"
    local -a gpu_list=() others=()
    mapfile -t gpu_list < <(gpus)
    mapfile -t others < <(other_network)
    local dm laptop=0 bluetooth=0 driver=
    dm=$(current_dm)
    has_battery && laptop=1
    has_bluetooth && bluetooth=1
    [[ " ${gpu_list[*]} " == *" nvidia "* ]] && driver=$(nvidia_driver)
    note "$(distro_name), $( ((laptop)) && echo laptop || echo desktop)"
    note "Graphics: ${gpu_list[*]:-none found}"
    note "Login screen: ${dm:-none (boots to a console)}"
    note "Network: $( ((${#others[@]})) && echo "${others[*]}" || { unit_enabled NetworkManager.service && echo NetworkManager || echo "nothing enabled"; })"
    installed atrium-git && note "atrium: $(pacman -Q atrium-git | cut -d' ' -f2) installed (this updates it)"

    # --- what to do: packages to add and changes to make, asked for first and done together
    local -a packages=() enable=() disable=()
    local use_login=0 use_nm=0 printing=0

    step "Questions"
    if [[ $dm == atrium-login ]]; then
        use_login=1
    elif [[ -n $dm ]]; then
        if ask "Use atrium's login screen in place of $dm" y; then
            use_login=1
        else
            note "atrium will be in $dm's list of sessions."
        fi
    elif ask "Start atrium's login screen when the computer starts" y; then
        use_login=1
    fi

    if ((${#others[@]})); then
        note "atrium's Wi-Fi, network and VPN settings work through NetworkManager; ${others[*]} runs the network now."
        [[ " ${others[*]} " == *" iwd "* ]] && note "Wi-Fi passwords saved in iwd are asked for again the first time."
        if ask "Let NetworkManager run the network (from the next start)" y; then
            use_nm=1
        fi
    elif ! unit_enabled NetworkManager.service; then
        use_nm=1
    fi

    if [[ -n $driver ]] && ! has_nvidia_driver; then
        case "$driver" in
            open | 580xx)
                local -a nv=(nvidia-utils)
                [[ $driver == open ]] && nv=(nvidia-open-dkms nvidia-utils) || nv=(nvidia-580xx-dkms nvidia-580xx-utils)
                if available "${nv[0]}"; then
                    if ask "Install NVIDIA's driver (${nv[0]})" y; then
                        packages+=("${nv[@]}")
                        local h
                        while read -r h; do available "$h" && packages+=("$h"); done < <(kernel_headers)
                    fi
                else
                    note "This NVIDIA GPU needs ${nv[0]}, which isn't in your repositories (it's in the AUR):"
                    note "https://wiki.archlinux.org/title/NVIDIA"
                fi
                ;;
            legacy)
                note "This NVIDIA GPU is older than any driver atrium works with; see https://wiki.archlinux.org/title/NVIDIA"
                ;;
        esac
    fi

    if ! has_terminal && ask "No terminal found: install Ghostty" y; then packages+=(ghostty); fi
    if ! has_app FileManager && ask "No file manager found: install Dolphin" y; then packages+=(dolphin); fi
    if ! has_app WebBrowser && ask "No web browser found: install Firefox" y; then packages+=(firefox); fi
    if ! has_app TextEditor && ask "No text editor found: install Kate" y; then packages+=(kate); fi

    if ! installed cups && ask "Set up printing (CUPS)" n; then
        packages+=(cups)
        printing=1
    fi
    if ((laptop)) && ! installed fprintd && ask "Unlock with a fingerprint reader (fprintd)" n; then packages+=(fprintd); fi
    if ! installed fcitx5 && ask "Type Chinese, Japanese or Korean (fcitx5 and CJK fonts)" n; then
        packages+=(fcitx5 fcitx5-qt fcitx5-gtk fcitx5-configtool noto-fonts-cjk)
    fi

    # --- the package
    step "atrium"
    local file sum=
    # Global: the EXIT trap runs after main's locals are gone.
    TMP=$(mktemp -d) || fail "No temporary directory."
    trap 'rm -rf "$TMP"' EXIT
    if [[ -n $PACKAGE ]]; then
        file=$PACKAGE
        note "From $file"
    else
        local asset url
        asset=$(release_asset) || fail "Couldn't find atrium's ${VERSION:-latest} release on GitHub (https://github.com/$REPO/releases)."
        url=${asset%%$'\t'*}
        sum=${asset#*$'\t'}
        file=$TMP/${url##*/}
        note "Downloading ${url##*/}"
        curl -fL --progress-bar -o "$file" "$url" || fail "The download failed."
        if [[ -n $sum ]]; then
            [[ $(sha256sum "$file" | cut -d' ' -f1) == "$sum" ]] || fail "The download doesn't match its checksum; nothing was installed."
            note "Checksum matches."
        fi
    fi

    # --- the plan, then doing it
    ((use_login)) && [[ $dm != atrium-login ]] && { [[ -n $dm ]] && disable+=("$dm.service"); enable+=(atrium-login.service); }
    if ((use_nm)); then
        local o
        for o in "${others[@]}"; do disable+=("$o.service"); done
        [[ " ${others[*]} " == *" systemd-networkd "* ]] && disable+=(systemd-networkd.socket systemd-networkd-wait-online.service)
        enable+=(NetworkManager.service)
    fi
    ((bluetooth)) && ! unit_enabled bluetooth.service && enable+=(bluetooth.service)
    ! unit_enabled power-profiles-daemon.service && enable+=(power-profiles-daemon.service)
    ((printing)) && enable+=(cups.socket)

    step "About to"
    note "update the system and install atrium${packages[*]:+ with ${packages[*]}}"
    ((${#disable[@]})) && note "turn off at start: ${disable[*]}"
    ((${#enable[@]})) && note "turn on at start: ${enable[*]}"
    ask "Go ahead" y || fail "Nothing was changed."

    # Everything at once, as pacman wants it: no partial upgrades.
    step "Installing"
    pacman_twice -Syu --needed --noconfirm "${packages[@]}" || fail "pacman couldn't update the system; nothing of atrium's was installed. Run this again to retry."
    pacman_twice -U --needed --noconfirm "$file" || fail "pacman couldn't install atrium. Run this again to retry."

    step "Setting up"
    if ((${#disable[@]})); then
        sudo systemctl disable "${disable[@]}" 2>/dev/null
    fi
    if ((${#enable[@]})); then
        sudo systemctl enable "${enable[@]}" || fail "Couldn't turn on ${enable[*]}."
    fi
    if ((use_login)) && [[ $(systemctl get-default) != graphical.target ]]; then
        sudo systemctl set-default graphical.target
    fi

    step "Done"
    if ((use_login)); then
        note "atrium's login screen comes up at the next start."
    else
        note "Pick atrium in your login screen's list of sessions."
    fi
    if ((!YES)) && ask "Restart now" n; then
        sudo systemctl reboot
    fi
}

# Sourced by tests/install_test.sh for its functions alone.
[[ -n ${ATRIUM_INSTALL_LIB:-} ]] || { main "$@"; exit; }
