# NETINT Quadra VPU GStreamer Plugins

## Build and install

GStreamer 1.22.2 or later (including its development packages), Meson 0.62 or
later, Ninja, Git, and C/C++ compilers are required.

```bash
meson setup build -Dniquadra=enabled -Dnilogan=disabled
meson compile -C build
sudo meson install -C build
sudo ldconfig # Linux: refresh the shared-library cache
```

Alternatively, run `./build_ni_gst_plugins.sh -h` for the build script's options.

## libxcoder dependency

Meson prefers an installed `xcoder` dependency. When it is missing,
`subprojects/libxcoder.wrap` automatically clones and builds libxcoder from
[netint_libxcoder](https://github.com/NETINT-Technologies/netint_libxcoder).
The wrap tracks its `main` branch. The libxcoder Meson support must be merged
there before the GitHub fallback can build it. Change the wrap's `revision` to
select another branch, tag, or commit with Meson support.

If an installed libxcoder is incompatible, explicitly select the fallback:

```bash
meson setup build --force-fallback-for=xcoder -Dniquadra=enabled -Dnilogan=disabled
```

Use `--wrap-mode=nodownload` to prohibit downloads. Logan remains an optional
installed dependency (`xcoder_logan`); it is not downloaded by this wrap.

The plugin and libxcoder Meson project versions are independent of the required
libxcoder product revision. Compatibility is checked against the first three
characters of `NI_XCODER_REVISION` from the selected dependency's `ni_defs.h`.
The required prefix is `590`, configured by `xcoder_required_revision` in
`meson.build`. Updating a wrap revision does not change this requirement.
