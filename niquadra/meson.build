if host_machine.system() == 'windows'
  ws2_32_dep = cc.find_library('ws2_32', required : true)
  platform_deps = [ws2_32_dep]
else
  platform_deps = []
endif

ni_plugins_args = ['-DHAVE_CONFIG_H']

math_dep = cc.find_library('m', required: false)
if not math_dep.found()
  math_dep = dependency('', required: false)
endif

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

niquadra_dep = []

niquadra_option = get_option('niquadra')
if niquadra_option.disabled()
  subdir_done()
endif

xcoder_dep = dependency('xcoder', required: get_option('niquadra'))
if xcoder_dep.found()
  xcoder_incdir = xcoder_dep.get_variable('includedir')
  xcoder_inc = []
elif get_option('niquadra').enabled()
  error('niquadra plugin enabled but NI xcoder not found: consider setting PKG_CONFIG_PATH')
  subdir_done()
else
  subdir_done()
endif

xcoder_revision = cc.get_define('NI_XCODER_REVISION',
  prefix : '#include <ni_defs.h>',
  dependencies : xcoder_dep).replace('"', '')
if ni_gst_revision.substring(0, 3) != xcoder_revision.substring(0, 3)
  revision_mismatch_msg = 'NI_GST_REVISION (@0@) does not match NI_XCODER_REVISION (@1@)'.format(
    ni_gst_revision, xcoder_revision)
  if niquadra_option.enabled()
    error('niquadra plugin enabled but ' + revision_mismatch_msg)
  else
    warning('niquadra plugin disabled: ' + revision_mismatch_msg)
    subdir_done()
  endif
endif

if xcoder_dep.found()
  gstniquadratag = library('gstniquadra',
    niquadra_sources,
    c_args : ni_plugins_args,
    include_directories : [configinc, xcoder_inc],
    dependencies : [gstbase_dep, gstapp_dep, gstvideo_dep, gstpbutils_dep, gstallocators_dep, gstcodecparsers_dep, math_dep, xcoder_dep] + platform_deps,
    install : true,
    install_dir : plugins_install_dir,
  )
  pkgconfig.generate(gstniquadratag, install_dir : plugins_pkgconfig_install_dir)
  plugins += [gstniquadratag]
endif
