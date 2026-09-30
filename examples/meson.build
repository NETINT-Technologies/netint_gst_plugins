demo_sources = [
  'demo_ni_scale.c', 
  'demo_ni_stack.c',
  'demo_ni_ppu.c',
  'demo_decodebin.c',
  'demo_seek.c'
]

foreach demo_src : demo_sources
  demo_name = demo_src.split('.')[0]
  executable(demo_name,
    demo_src,
    dependencies: gst_dep,
    install: false,
  )
endforeach
