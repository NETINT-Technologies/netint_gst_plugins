if host_machine.system() == 'windows'
  ws2_32_dep = cc.find_library('ws2_32', required : true)
  platform_deps = [ws2_32_dep]
else
  platform_deps = []
endif

ni_plugins_args = ['-DHAVE_CONFIG_H']

niquadra_sources =[
    'gstniquadra.c',
    'gstniquadracontext.c',
    'gstniquadramemory.c',
    'gstniquadradec.c',
    'gstniquadrah264dec.c',
    'gstniquadrah265dec.c',
    'gstniquadravp9dec.c',
    'gstniquadrajpegdec.c',
    'gstniquadraenc.c',
    'gstniquadrah264enc.c',
    'gstniquadrah265enc.c',
    'gstniquadrajpegenc.c',
    'gstniquadraav1enc.c',
    'gstniquadrautils.c',
    'gstniquadrahwdownload.c',
    'gstniquadrahwupload.c',
    'gstniquadrascale.c',
    'gstniquadradrawbox.c',
    'gstniquadrarotate.c',
    'gstniquadracrop.c',
    'gstniquadrapad.c',
    'gstniquadrastack.c',
    'gstniquadraoverlay.c',
    'gstniquadrabgr.c',
    'gstniquadraroi.c',
    'gstnieval.c',
    'gstniquadrabg.c',
    'gstnimetadata.c',
    'gstniquadradelogo.c',
    'gstniquadraflip.c',
    'gstniquadramerge.c',
    'gstniquadraaipre.c',
    'gstniquadrainterleave.c',
    'gstniquadrabaseconvert.c',
    'gstniquadrasplit.c',
    ]

xcoder_dep = dependency('xcoder', required: get_option('niquadra'), version: '=' + meson.project_version())
if not xcoder_dep.found()
  subdir_done()
endif


gstniquadratag = library('gstniquadra',
  niquadra_sources,
  c_args : ni_plugins_args,
  include_directories : [configinc],
  dependencies : [gstbase_dep, gstapp_dep, gstvideo_dep, gstpbutils_dep, gstallocators_dep, gstcodecparsers_dep, math_dep, xcoder_dep] + platform_deps,
  install : true,
  install_dir : plugins_install_dir,
)
pkgconfig.generate(gstniquadratag, install_dir : plugins_pkgconfig_install_dir)
plugins += [gstniquadratag]
