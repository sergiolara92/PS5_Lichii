#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepares everything the PS5 build needs besides Eden itself (make prepare), after the pinned
# inputs are in place (make deps): this checkout's build cache, Eden's source tree, the Payload
# SDK, FFmpeg for the PS5, the native packaging tool, the runtime libc.prx, the libSceAgcDriver
# link stub and the isolated RADV driver. Each step is skipped when its result already exists.
set -euo pipefail
export PATH=${PROSPERO_TOOLCHAIN_BIN:+$PROSPERO_TOOLCHAIN_BIN:}/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
step() { echo "== $*"; }
# The sibling checkouts (tools/deps.json): beside this repository unless PROSPERO_SIBLINGS names their folder.
siblings=${PROSPERO_SIBLINGS:-..}

# Thousands of small files: keep the build cache on a native Linux filesystem, one per checkout.
cache_parent="${XDG_CACHE_HOME:-$HOME/.cache}"
if [[ ! -f .local/headless-cache ]] || [[ ! -d "$(cat .local/headless-cache)" ]]; then
    scratch="$cache_parent/ps5-eden-headless.$(printf '%s' "$root" | sha256sum | cut -c1-12)"
    mkdir -p .local "$scratch"
    printf '%s\n' "$root" > "$scratch/owner"
    printf '%s\n' "$scratch" > .local/headless-cache
fi
scratch=$(cat .local/headless-cache)
[[ "$(cat "$scratch/owner")" == "$root" ]] || { echo "Build cache $scratch belongs to another checkout" >&2; exit 1; }

eden="$scratch/source"
# The pinned source (Frizti/Fritchii, tools/deps.json) and its complete pinned-to-final PS5 patch
# (patches/fritchii; ours on top in <patch>.d). The patch is part of the tree's identity, so a
# changed patch unpacks the archive again. EDEN_ARCHIVE/EDEN_COMMIT/EDEN_SOURCE_PATCH override them.
eden_archive=${EDEN_ARCHIVE:-fritchii-1a7a3c0.tar.gz}
eden_commit=${EDEN_COMMIT:-1a7a3c03c5955c7e6a35b447dbd852a4c4a875b5}
EDEN_SOURCE_PATCH=${EDEN_SOURCE_PATCH:-$root/patches/fritchii/fritchii-1a7a3c0-ps5.patch}
eden_id="$eden_archive+$(basename "$EDEN_SOURCE_PATCH"):$(sha256sum < "$EDEN_SOURCE_PATCH" | cut -c1-12)"
# A tree unpacked from another archive is the source of another Eden revision: it is unpacked
# again (Eden's package cache, downloads only, is kept). A tree left from before an update of
# the pin was built as if it were the new one, and what Eden had changed went unseen.
if [[ ! -f $eden/CMakeLists.txt || $(cat "$eden/.unpacked-from" 2>/dev/null) != "$eden_id" ]]; then
    step "Eden source"
    # Unpacked and patched beside the old tree, then synced into it by content: a file that comes
    # out the same keeps its timestamp, so a rebuilt tree recompiles only what really changed.
    # A changed file is written now (rsync without -t; tar -m below for the same reason): with the
    # archive's old times a file a removed patch had changed looked older than its object, which
    # was not rebuilt (a link then failed on the patch's code).
    stage="$scratch/source.next"
    rm -rf "$stage"
    mkdir -p "$stage"
    tar -xzf ".deps/$eden_archive" -m --strip-components=1 -C "$stage"
    step "Eden source patch $(basename "$EDEN_SOURCE_PATCH")"
    patch -d "$stage" -p1 -s -f < "$EDEN_SOURCE_PATCH" ||
        { echo "$EDEN_SOURCE_PATCH does not apply to the pinned Eden" >&2; exit 1; }
    # The patch series too, so files it changes are compared in their final form.
    # In the order the old tree applied them (a patch regenerated from the tree carries the
    # context of those applied before it), then the new ones.
    : > "$stage/.applied-patches"
    series="${EDEN_SOURCE_PATCH%.patch}.d"
    mapfile -t ordered < <({ cut -d' ' -f1 "$eden/.applied-patches" 2>/dev/null; ls "$series"; } |
        awk '!seen[$0]++' | grep '\.diff$')
    for name in "${ordered[@]}"; do
        eden_patch="$series/$name"
        [[ -f $eden_patch ]] || continue
        step "Eden patch $(basename "$eden_patch")"
        patch -d "$stage" -p1 -N -s < "$eden_patch" ||
            { echo "$eden_patch does not apply on top of $EDEN_SOURCE_PATCH" >&2; exit 1; }
        printf '%s\n' "$(basename "$eden_patch") $(sha256sum < "$eden_patch" | cut -c1-12)" >> "$stage/.applied-patches"
    done
    printf '%s\n' "$eden_id" > "$stage/.unpacked-from"
    mkdir -p "$eden"
    rsync -rlp --checksum --delete --exclude=/.cache "$stage/" "$eden/"
    rm -rf "$stage"
fi
printf '%s\n' "$eden_commit" > "$eden/GIT-COMMIT"
printf '%s\n' 'ps5-headless' > "$eden/GIT-REFSPEC"
# Our own changes on top of the source patch (<patch>.d/*.diff, in name order), each applied once,
# so a change to one recompiles only the files it touches instead of unpacking the whole tree again.
# The tree records which were applied (a later patch may change the lines of an earlier one, so the
# earlier one no longer reverses); a recorded patch whose content changed needs a new tree.
command -v patch >/dev/null || { echo "patch is required (apt install patch)" >&2; exit 1; }
applied="$eden/.applied-patches"
touch "$applied"
for eden_patch in "${EDEN_SOURCE_PATCH%.patch}".d/*.diff; do
    [[ -f $eden_patch ]] || continue
    name=$(basename "$eden_patch")
    id="$name $(sha256sum < "$eden_patch" | cut -c1-12)"
    if grep -qxF "$id" "$applied"; then continue; fi
    if grep -q "^$name " "$applied"; then
        echo "$name changed after it was applied: delete $eden/.unpacked-from to rebuild the tree" >&2; exit 1
    fi
    step "Eden patch $name"
    patch -d "$eden" -p1 -N -s < "$eden_patch" || { echo "$eden_patch does not apply on top of $EDEN_SOURCE_PATCH" >&2; exit 1; }
    printf '%s\n' "$id" >> "$applied"
done

# Optional: seed Eden's package cache from another checkout's (CI reuses a development cache).
if [[ -n ${EDEN_CPM_CACHE_SEED:-} && ! -d $eden/.cache/cpm ]]; then
    step "Eden package cache from $EDEN_CPM_CACHE_SEED"
    mkdir -p "$eden/.cache"
    cp -a "$EDEN_CPM_CACHE_SEED" "$eden/.cache/cpm"
fi
# The FFmpeg source Eden pins, where Eden's package cache keeps it.
ffmpeg_source="$eden/.cache/cpm/ffmpeg/c7b5f1537d"
if [[ ! -f $ffmpeg_source/configure ]]; then
    step "FFmpeg source"
    mkdir -p "$(dirname "$ffmpeg_source")"
    cp -a .deps/ffmpeg-c7b5f1537d "$ffmpeg_source"
fi

if [[ ! -f $scratch/sdk/.complete ]]; then
    step "Payload SDK"
    mkdir -p "$scratch/sdk"
    cp -a "$siblings"/ps5-native-app-boilerplate/.deps/native/ps5-payload-sdk/target "$scratch/sdk/"
    touch "$scratch/sdk/.complete"
fi
# .with-dav1d: built with the AV1 decoder (an older cache rebuilds once).
if [[ ! -f $scratch/ffmpeg-native/install/lib/libavcodec.a || ! -f $scratch/ffmpeg-native/install/.with-dav1d ]]; then
    step "FFmpeg for the PS5"
    bash tools/build-headless-ffmpeg.sh
fi
step "Native packaging tool"
bash tools/build-native-tool.sh
# The app's runtime libc.prx: the boilerplate's clean-room build, checked against its recorded digest.
if [[ ! -f $siblings/ps5-native-app-boilerplate/runtime/libc.prx ]]; then
    step "Runtime libc.prx"
    bash "$siblings"/ps5-native-app-boilerplate/tools/rebuild-libc.sh
fi
step "libSceAgcDriver link stub"
bash tools/build-agc-driver-stub.sh
bash tools/build-common-dialog-stub.sh
# The driver's display code carries this repository's adaptation (tools/patch-radv-wsi.py): a
# driver built with another version of it is built again (only the changed file compiles).
# So is a driver built from another Mesa revision than tools/deps.json pins.
radv_release=$siblings/mihawk-vulkan-review/.deps/native/radv-release
mesa_pin=$(python3 -c 'import json; print(next(i["commit"] for i in json.load(open("tools/deps.json"))["items"] if i["name"] == "ps5-mesa"))')
if [[ -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
    built=$(sed -n 's/^revision: //p' "$radv_release/PROVENANCE.txt" 2>/dev/null || true)
    if [[ $built != "$mesa_pin" ]]; then
        step "RADV: built from Mesa ${built:0:12}, pinned ${mesa_pin:0:12}"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    elif ! python3 -B tools/patch-radv-wsi.py --check "$radv_release/EDEN_WSI_SHA256" ||
         ! python3 -B tools/patch-radv-gs-compute.py --check "$radv_release/EDEN_GS_SHA256" ||
         ! python3 -B tools/patch-radv-disk-cache.py --check "$radv_release/EDEN_DISK_CACHE_SHA256" ||
         ! python3 -B tools/patch-radv-transfer-split.py --check "$radv_release/EDEN_TRANSFER_SPLIT_SHA256"; then
        step "RADV: this repository's adaptation of the driver changed"
        bash tools/build-radv-dependencies.sh
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    fi
fi
# The isolated copy is made once from the driver archive: made from another archive (the driver was
# rebuilt at a new pin since), it would keep linking the old driver into every build.
if [[ -f build/radv-isolated/libvulkan_radeon.ps5.a && -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
    isolated_from=$(python3 -c 'import json; print(json.load(open("build/radv-isolated/manifest.json")).get("source_sha256", ""))' 2>/dev/null || true)
    if [[ $isolated_from != "$(sha256sum < "$radv_release/lib/libvulkan_radeon.ps5.a" | cut -d' ' -f1)" ]]; then
        step "RADV: the isolated copy is from another driver archive"
        rm -f build/radv-isolated/libvulkan_radeon.ps5.a
    fi
fi
if [[ ! -f build/radv-isolated/libvulkan_radeon.ps5.a ]]; then
    if [[ ! -f $radv_release/lib/libvulkan_radeon.ps5.a ]]; then
        step "RADV (Mesa) for the PS5 - this takes a while"
        bash tools/build-radv-dependencies.sh
    fi
    step "Isolating RADV beside the OpenGL Mesa"
    python3 -B tools/isolate-radv.py
fi
step "Prepared: $scratch"
