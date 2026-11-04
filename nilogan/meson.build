ni_plugins_args = ['-DHAVE_CONFIG_H']

nilogan_sources =[
  'gstnilogan.c',
  'gstnilogancontext.c',
  'gstniloganutils.c',
  'gstnieval.c',
  'gstnimetadata.c',
  'gstnilogandec.c',
  'gstniloganh264dec.c',
  'gstniloganh265dec.c',
  'gstniloganenc.c',
  'gstniloganh264enc.c',
  'gstniloganh265enc.c',
]

nilogan_dep = []

nilogan_option = get_option('nilogan')
if nilogan_option.disabled()
  subdir_done()
endif

xcoder_logan_dep = dependency('xcoder_logan', required: get_option('nilogan'))
if xcoder_logan_dep.found()
  xcoder_logan_incdir = xcoder_logan_dep.get_variable('includedir')
  xcoder_logan_inc = []
elif get_option('nilogan').enabled()
  error('nilogan plugin enabled but NI xcoder_logan not found: consider setting PKG_CONFIG_PATH')
  subdir_done()
else
  subdir_done()
endif

if xcoder_logan_dep.found()
  gstnilogantag = library('gstnilogan',
                           nilogan_sources,
                           c_args : ni_plugins_args + ['-DGST_USE_UNSTABLE_API'],
                           include_directories : [configinc, xcoder_logan_inc],
                           dependencies : [gstbase_dep, gstvideo_dep, gstpbutils_dep, gstallocators_dep, gstcodecparsers_dep, math_dep, xcoder_logan_dep],
                           install : true,
                           install_dir : plugins_install_dir,
  )
  pkgconfig.generate(gstnilogantag, install_dir : plugins_pkgconfig_install_dir)
  plugins += [gstnilogantag]
endif

