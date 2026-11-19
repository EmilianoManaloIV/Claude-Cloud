# Third-party software in the Tetra Studio downloads

Tetra Studio itself is licensed GPL-3.0-or-later (`LICENSE.txt`). The download
archives also contain the programs and libraries below, unmodified, each under
its own licence. Source code for every one of them is available from the
project named, and for the exact binaries from the distributor named under
"Where the binaries come from".

## Built into the editor

| Component | Licence | Project |
| --- | --- | --- |
| Godot Engine 4.7.2 and the libraries it embeds (FreeType, HarfBuzz, ICU, mbedTLS, zlib, zstd and others; the full list is in the editor under *Help > About > Third-party Licenses*) | MIT, with per-library licences | https://godotengine.org |
| Layer-mode and paint algorithms ported from GIMP 3.2 | GPL-3.0-or-later | https://www.gimp.org |
| Timeline rules ported from Kdenlive 26.08 | GPL-3.0-or-later | https://kdenlive.org |

## Shipped next to the editor

| Component | Where in the archive | Licence | Project |
| --- | --- | --- | --- |
| GEGL (library and operations) | `lib/` (Windows: `bin/`), `lib/gegl-0.4/` | LGPL-3.0-or-later; some operations GPL-3.0-or-later | https://gegl.org |
| babl | `lib/`, `lib/babl-0.1/` | LGPL-3.0-or-later | https://gegl.org/babl |
| GLib, GObject, GIO, GModule | `lib/` | LGPL-2.1-or-later | https://gitlab.gnome.org/GNOME/glib |
| json-glib, gdk-pixbuf, Pango, librsvg | `lib/` | LGPL-2.1-or-later | https://gitlab.gnome.org/GNOME |
| cairo, pixman | `lib/` | LGPL-2.1 or MPL-1.1 (cairo), MIT (pixman) | https://cairographics.org |
| MLT framework, MLT++ and `melt` | `lib/`, `lib/mlt-7/`, `share/mlt-7/` (Windows: `lib/mlt/`, `share/mlt/`), `bin/melt` | LGPL-2.1-or-later; the modules `plusgpl`, `xine`, `resample`, `rubberband`, `vidstab`, `frei0r` wrappers and `melt` as distributed are GPL-2.0-or-later or GPL-3.0-or-later | https://www.mltframework.org |
| FFmpeg (libavcodec, libavformat, libavfilter, libavutil, libswscale, libswresample, libavdevice, libpostproc) | `lib/` | GPL-2.0-or-later / GPL-3.0-or-later as built by the distributor (GPL components such as x264 and x265 enabled) | https://ffmpeg.org |
| Codec and format libraries FFmpeg uses (x264, x265, libvpx, dav1d, SVT-AV1, libaom, opus, vorbis, mp3lame, theora, libwebp, OpenJPEG, libass and others) | `lib/` | GPL-2.0-or-later (x264, x265), BSD, MIT, LGPL or ISC (the others) | see each project |
| frei0r plug-ins | `lib/frei0r-1/` | GPL-2.0-or-later | https://frei0r.dyne.org |
| SDL 2, RtAudio, SoX, Rubber Band, vid.stab, libsamplerate, libebur128, FFTW | `lib/` | zlib (SDL 2), MIT (RtAudio, libebur128), GPL-2.0-or-later / LGPL (SoX), GPL-2.0-or-later (Rubber Band, vid.stab, FFTW), BSD-2-Clause (libsamplerate) | see each project |
| Image libraries GEGL uses (libpng, libjpeg-turbo, libtiff, OpenEXR, LibRaw, lcms2, libspiro, and others) | `lib/` | permissive licences or LGPL | see each project |
| Kdenlive effect, transition and render-preset catalogs | `kdenlive-data/` | GPL-3.0-or-later | https://kdenlive.org |
| Blender 5.3.0 (official portable build, with the libraries and the Python it bundles) | `blender/` | GPL-3.0-or-later; bundled components under their own licences, listed in `blender/license/` | https://www.blender.org |
| Linux only: FreeType, Fontconfig, HarfBuzz, PulseAudio client, JACK client, libva, libvdpau, ICU, libxml2 and further dependencies of the above | `lib/` | permissive licences or LGPL | see each project |
| Windows only: GCC and MinGW-w64 runtime libraries (libstdc++, libgcc, winpthreads) | `bin/` | GPL-3.0-or-later with the GCC Runtime Library Exception; MIT/ZPL (MinGW-w64) | https://www.mingw-w64.org |

Because GPL components are included (FFmpeg as built, frei0r, Blender, the
GIMP and Kdenlive code), the archive as a whole is distributed under the GNU
General Public License, version 3 or later.

## Where the binaries come from

- **Linux archive:** the editor is built from this repository; every other
  library, plug-in and `melt` is copied from the Ubuntu 24.04 packages
  installed on the build machine (`libgegl-0.4-0t64`, `libmlt++7`, `melt`,
  `frei0r-plugins` and their dependencies). Sources: `apt-get source <package>`
  on Ubuntu 24.04, or https://packages.ubuntu.com.
- **Windows archive:** the editor is built from this repository with MSYS2's
  UCRT64 GCC; every DLL, plug-in and `melt.exe` is copied from the MSYS2
  UCRT64 packages (`gegl`, `mlt`, `ffmpeg`, `frei0r-plugins` and their
  dependencies). Sources and build recipes:
  https://github.com/msys2/MINGW-packages.
- **Blender:** the unmodified official build from
  https://download.blender.org/release/. Source: https://projects.blender.org.

The licence texts are in `LICENSE.txt` (GPL-3.0), in `blender/license/`, and in
each project's source distribution.
