#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# FFmpeg links configure probes through the compiler driver, not raw ld (host builds).
# Usage: patch-host-ffmpeg.py <eden source> <build dir>; run from the repository root.
import pathlib, sys, tarfile
# The pristine file, from the archive the tree was unpacked from (.unpacked-from: <archive>+<patch>).
archive = pathlib.Path('.deps') / (pathlib.Path(sys.argv[1]) / '.unpacked-from').read_text().split('+')[0].strip()
with tarfile.open(archive) as tar:
    member = next(m for m in tar.getmembers() if m.name.split('/', 1)[-1] == 'externals/ffmpeg/CMakeLists.txt')
    text = tar.extractfile(member).read().decode()
for old, new in (
    ('--ld=${CMAKE_LINKER}', '--ld=${CMAKE_C_COMPILER}'),
    ('elseif (NOT (CMAKE_HOST_SYSTEM_PROCESSOR MATCHES CMAKE_SYSTEM_PROCESSOR\n    AND CMAKE_HOST_SYSTEM_NAME MATCHES CMAKE_SYSTEM_NAME))', 'elseif (CMAKE_CROSSCOMPILING)'),
    ('set(FFmpeg_MAKE_ARGS -j${SYSTEM_THREADS})', 'set(FFmpeg_MAKE_ARGS -j6)'),
    # AV1 movies (Switch 2 title screens) through the system's dav1d, as the PS5 build does.
    ('                --enable-decoder=vp9\n',
     '                --enable-decoder=vp9\n                --enable-libdav1d\n                --enable-decoder=libdav1d\n'),
    ('set(FFmpeg_LIBRARIES ${FFmpeg_BUILD_LIBRARIES} ${FFmpeg_HWACCEL_LIBRARIES}\n',
     'set(FFmpeg_LIBRARIES ${FFmpeg_BUILD_LIBRARIES} ${FFmpeg_HWACCEL_LIBRARIES} dav1d\n'),
):
    assert text.count(old) == 1, old
    text = text.replace(old, new)
target = pathlib.Path(sys.argv[1]) / 'externals/ffmpeg/CMakeLists.txt'
if target.read_text() != text:
    target.write_text(text)
    (pathlib.Path(sys.argv[2]) / '_deps/ffmpeg-build/Makefile').unlink(missing_ok=True)
    # The build step runs only for a missing library: drop them so the new configuration is built.
    for lib in ('libavcodec', 'libavfilter', 'libavutil', 'libswscale'):
        (pathlib.Path(sys.argv[2]) / f'_deps/ffmpeg-build/{lib}/{lib}.a').unlink(missing_ok=True)
