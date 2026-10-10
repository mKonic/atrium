# The compositor's own messages (i18n.hpp: tr, trf) through gettext: <lang>.po
# for each language in LINGUAS; `meson compile atrium-pot` refreshes atrium.pot.
i18n = import('i18n')
i18n.gettext(meson.project_name(),
  args: ['--keyword=tr', '--keyword=trf', '--from-code=UTF-8', '--package-name=atrium'],
  preset: 'glib')
