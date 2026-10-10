# The shell's translations (Qt): atrium_<lang>.ts for each language in
# LINGUAS, compiled beside the shell; `meson compile update-translations`
# refreshes atrium.ts (the catalogue to copy for a new language) and po/.
fs = import('fs')
lrelease = find_program('lrelease', '/usr/lib/qt6/bin/lrelease', required: false)
foreach lang : fs.read('LINGUAS').split()
  if lrelease.found()
    custom_target('atrium_' + lang + '.qm',
      input: 'atrium_' + lang + '.ts',
      output: 'atrium_' + lang + '.qm',
      command: [lrelease, '-silent', '@INPUT@', '-qm', '@OUTPUT@'],
      install: true,
      install_dir: get_option('datadir') / 'atrium' / 'translations')
  endif
endforeach
run_target('update-translations', command: [files('update.sh'), meson.project_build_root()])
