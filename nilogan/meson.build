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

xcoder_logan_dep = dependency('xcoder_logan', required: get_option('nilogan'))
if not xcoder_logan_dep.found()
  subdir_done()
endif

gstnilogantag = library('gstnilogan',
                         nilogan_sources,
                         c_args : ni_plugins_args + ['-DGST_USE_UNSTABLE_API'],
                         include_directories : [configinc],
                         dependencies : [gstbase_dep, gstvideo_dep, gstpbutils_dep, gstallocators_dep, gstcodecparsers_dep, math_dep, xcoder_logan_dep],
                         install : true,
                         install_dir : plugins_install_dir,
)
pkgconfig.generate(gstnilogantag, install_dir : plugins_pkgconfig_install_dir)
plugins += [gstnilogantag]
