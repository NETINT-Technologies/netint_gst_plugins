#!/usr/bin/env bash

# Copyright (C) 2023 NETINT Technologies
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

SCRIPT_VERSION="v0.3"

install_after_compile=false
install_prefix=""

# Detect OS
if [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]] || [[ "$MSYSTEM" == "MINGW64" ]]; then
    OS_TYPE="windows"
else
    OS_TYPE="linux"
fi

while [ "$1" != "" ]; do
    case $1 in
        -h | --help) echo "Usage: ./build_ni_gst_plugins.sh [OPTION]";
                     echo "Compile Gstreamer";
                     echo "Example: ./build_ni_gst_plugins.sh";
                     echo;
                     echo "Options:";
                     echo "-h, --help       display this help and exit";
                     echo "-i, --install    install after compile";
                     echo "-p, --prefix PATH";
                     echo "                 install files under PATH";
                     exit 0
        ;;
        -i | --install)  install_after_compile=true
        ;;
        -p | --prefix)
            if [ -z "${2:-}" ]; then
                echo "build_ni_gst_plugins.sh: option '$1' requires a path" >&2
                exit 1
            fi
            install_prefix="$2"
            shift
        ;;
        --prefix=*)
            install_prefix="${1#*=}"
            if [ -z "$install_prefix" ]; then
                echo "build_ni_gst_plugins.sh: option '--prefix' requires a path" >&2
                exit 1
            fi
        ;;
    esac
    shift
done

echo "Compiling NETINT GStreamer plugins for $OS_TYPE..."

meson_framework_patch_option=("-Dni-gst-framework-patch=false")
if [[ -n "${NI_GST_FRAMEWORK_PATCH_OPTION:-}" ]]; then
    meson_framework_patch_option=("${NI_GST_FRAMEWORK_PATCH_OPTION}")
fi

meson_prefix_option=()
if [[ -n "$install_prefix" ]]; then
    meson_prefix_option=("--prefix=${install_prefix}")
fi

# Set up environment and build based on OS
if [[ "$OS_TYPE" == "windows" ]]; then
    export PATH="/mingw64/bin:/usr/bin:/usr/local/bin:$PATH"
    export PKG_CONFIG_PATH="/mingw64/lib/pkgconfig:/mingw64/share/pkgconfig:/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH"
    
    meson setup --wipe build \
        -Dwerror=false \
        -Dc_args="-Wno-incompatible-pointer-types -I/usr/local/include" \
        --wrap-mode=nodownload \
        "${meson_prefix_option[@]}" \
        "${meson_framework_patch_option[@]}" &&
    ninja -C build
    rc=$?
else
    meson setup --wipe build \
        "${meson_prefix_option[@]}" \
        "${meson_framework_patch_option[@]}" &&
    ninja -C build
    rc=$?
fi

if [ $rc -eq 0 ]; then
    echo "Successfully compiled NETINT GStreamer plugins"
else
    echo "Failed to compile NETINT GStreamer plugins"
fi

if [ $rc -eq 0 ] && $install_after_compile; then
    if [[ "$OS_TYPE" == "windows" ]]; then
        ninja -C build install
        rc=$?
    else
        sudo ninja -C build install &&
        sudo ldconfig
        rc=$?
    fi
    
    if [ $rc -eq 0 ]; then
        echo "Successfully installed NETINT GStreamer plugins"
    else
        echo "Failed to install NETINT GStreamer plugins"
    fi
fi

exit $rc