#!/bin/sh
# Installs atrium's own wlroots (subprojects/wlroots) where only atrium
# looks: $libdir/atrium, found through its rpath. Its headers and pkg-config
# file stay out, so it can't clash with a system wlroots.
#   install-wlroots.sh LIBRARY LIBDIR
set -eu
dest="${DESTDIR:-}$2/atrium"
mkdir -p "$dest"
cp -P "$1" "$dest/"
