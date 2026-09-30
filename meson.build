project('gst-niquadra', 'c',
  version : '5.8.2',
  meson_version : '>= 0.62',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

ni_gst_revision = meson.project_version().replace('.', '')

# GStreamer dependencies - require version 1.22.2 and above
gst_req = '>= 1.22.2'
gst_dep = dependency('gstreamer-1.0', version : gst_req)
gstbase_dep = dependency('gstreamer-base-1.0', version : gst_req)
gstapp_dep = dependency('gstreamer-app-1.0', version : gst_req)
gstvideo_dep = dependency('gstreamer-video-1.0', version : gst_req)
gstpbutils_dep = dependency('gstreamer-pbutils-1.0', version : gst_req)
gstallocators_dep = dependency('gstreamer-allocators-1.0', version : gst_req)
gstcodecparsers_dep = dependency('gstreamer-codecparsers-1.0', version : gst_req)

# Math library dependency
cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required : false)

# NETINT-patched GStreamer framework support
ni_gst_framework_patch = get_option('ni-gst-framework-patch')
if ni_gst_framework_patch
  has_ni_gst_video_formats = cc.compiles(
    '''
    #include <gst/video/video-format.h>

    int main (void)
    {
      GstVideoFormat format_8bit = GST_VIDEO_FORMAT_NI_QUAD_8_4L4;
      GstVideoFormat format_10bit = GST_VIDEO_FORMAT_NI_QUAD_10_4L4;
      return format_8bit == format_10bit;
    }
    ''',
    dependencies : gstvideo_dep,
    name : 'NETINT GStreamer framework video formats')

  if not has_ni_gst_video_formats
    error('ni-gst-framework-patch requires a NETINT-patched GStreamer framework')
  endif
endif

# Configuration data
cdata = configuration_data()
cdata.set_quoted('VERSION', meson.project_version())
cdata.set_quoted('PACKAGE', 'gst-niquadra')
cdata.set_quoted('GST_PACKAGE_NAME', 'GStreamer NiQuadra Plugin')
cdata.set_quoted('GST_PACKAGE_ORIGIN', get_option('package-origin'))
cdata.set_quoted('NI_GST_REVISION', ni_gst_revision)
cdata.set('GST_USE_UNSTABLE_API', true)
cdata.set10('ENABLE_NI_GST_FRAMEWORK_PATCH', ni_gst_framework_patch)

# Generate config.h
configure_file(output : 'config.h', configuration : cdata)

# Include directories and compile arguments
configinc = include_directories('.')

# Installation directory
pkgconfig = import('pkgconfig')
plugins_install_dir = join_paths(get_option('libdir'), 'gstreamer-1.0')
plugins_pkgconfig_install_dir = join_paths(plugins_install_dir, 'pkgconfig')

plugins = []

subdir('nilogan')
subdir('niquadra')

if not get_option('examples').disabled()
  subdir('examples')
endif
