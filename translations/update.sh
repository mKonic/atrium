#!/usr/bin/env bash
# Refresh the translation catalogues from the sources (meson compile
# update-translations): translations/atrium.ts for the shell (Qt Linguist;
# copy it to atrium_<lang>.ts to translate) and po/atrium.pot for
# the compositor's own messages (gettext; po/<lang>.po). Languages to
# build are listed in translations/LINGUAS and po/LINGUAS.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
build=${1:-$src/build}
tools=/usr/lib/qt6/bin
cd "$src"

# Settings' pages, titles, descriptions and choices come from the
# compositor's schema at run time; written out here for lupdate to find.
"$build/src/atrium" --dump-schema | python3 -I translations/schema_strings.py > translations/schema_strings.cpp

qml=$(git ls-files 'shell/*.qml')
cpp=$(git ls-files 'shell/plugin/Atrium/*.cpp')
for ts in translations/atrium.ts translations/atrium_*.ts; do
    [[ -e $ts ]] || continue
    "$tools/lupdate" -silent -locations none -no-obsolete $qml $cpp translations/schema_strings.cpp -ts "$ts"
done
# The compositor's: po/atrium.pot (meson's gettext targets).
meson compile -C "$build" atrium-pot >/dev/null
echo "translations: $(grep -c '<source>' translations/atrium.ts) shell strings, $(grep -c '^msgid ' po/atrium.pot) compositor strings"
