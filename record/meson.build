# atrium-record: Record Screen's recorder (see main.cpp).
record_core = files('record_core.cpp')
executable('atrium-record', 'main.cpp', record_core,
  dependencies: [json, dependency('libpipewire-0.3'), dependency('libavcodec'), dependency('libavformat'),
    dependency('libavutil'), dependency('libswscale')],
  install: true,
)
