#!/bin/bash
# Builds Wheelio's engine: CodeWeavers' Wine from CrossOver's source release
# with engine/patches applied and Valve's lsteamclient and Proton's steam
# helper built in as Wine modules, plus what it needs at run time: MoltenVK,
# DXVK and DXMT for graphics, GStreamer for video, Wine Mono and Wine Gecko.
# The result is wheelio-engine-<crossover version>-<revision>-<arch>.tar.xz.
#
#   engine/build.sh
#
# WHEELIO_ENGINE_ARCH  x86_64 (the default): Wine for Intel Macs and Rosetta;
#                      arm64: Wine that runs natively on Apple silicon, for
#                      Windows code run by an emulator such as FEX
# WHEELIO_ENGINE_WORK  downloads, sources and build trees (default: build/engine)
# WHEELIO_ENGINE_OUT   where the package goes (default: the work folder)
#
# Needs Xcode and Homebrew's mingw-w64, bison, pkgconf, cmake, xz, meson,
# ninja, nasm and glslang; x86_64 also needs Rosetta, and arm64 an Apple
# silicon Mac (it uses llvm-mingw, which it downloads). Reruns are incremental.
set -euo pipefail

# Bump when anything in engine/ changes what ends up in the package.
ENGINE_REVISION=5

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
work=${WHEELIO_ENGINE_WORK:-$repo/build/engine}
mkdir -p "$work"
work=$(cd "$work" && pwd)
out=${WHEELIO_ENGINE_OUT:-$work}
# Wine's build can't handle spaces in paths, so such a folder is reached
# through a symlink whose name stays the same from one build to the next.
case $work in
    *" "*)
        link=${TMPDIR:-/tmp}/wheelio-engine-$(printf '%s' "$work" | shasum | cut -c1-12)
        ln -sfn "$work" "$link"
        work=$link
        ;;
esac

arch=${WHEELIO_ENGINE_ARCH:-x86_64}
# What follows the architecture: the macOS triple (for configure) and cpu
# (for clang, meson and FFmpeg), Wine's Windows architectures and the folders
# it keeps their DLLs in.
case $arch in
    x86_64)
        host=x86_64-apple-darwin
        macos_cpu=x86_64
        meson_cpu_family=x86_64
        wine_archs=i386,x86_64
        wine_unix=x86_64-unix
        ;;
    arm64)
        host=aarch64-apple-darwin
        macos_cpu=arm64
        meson_cpu_family=aarch64
        # 64-bit Windows code is x86_64 run through ARM64EC, 32-bit through WoW64.
        wine_archs=i386,aarch64,arm64ec
        wine_unix=aarch64-unix
        ;;
    *)
        echo "engine: WHEELIO_ENGINE_ARCH is $arch; it can be x86_64 or arm64" >&2
        exit 1
        ;;
esac

# Downloads and sources are shared; what's built is kept per architecture.
cache=$work/cache
src=$work/src
logs=$work/$arch/logs
deps=$work/$arch/deps
stage=$work/$arch/stage
mkdir -p "$cache" "$logs" "$src" "$deps"

jobs=$(sysctl -n hw.ncpu)
export MACOSX_DEPLOYMENT_TARGET=15.0
build=$(uname -m)-apple-darwin
[ "${build%%-*}" = arm64 ] && build=aarch64-apple-darwin
export CC="clang -arch $macos_cpu" CXX="clang++ -arch $macos_cpu"
# Only our own libraries, never Homebrew's.
export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig
unset PKG_CONFIG_PATH
if command -v brew >/dev/null; then
    PATH="$(brew --prefix bison)/bin:$PATH"
fi

fail() {
    echo "engine: $*" >&2
    exit 1
}

# Runs a step quietly in a subshell, logging to logs/<name>.log, and shows the
# log's tail if it fails. Started with & so that set -e still holds inside.
step() {
    local name=$1
    shift
    ("$@") >"$logs/$name.log" 2>&1 &
    if ! wait $!; then
        echo "engine: $name failed, last lines of $logs/$name.log:" >&2
        tail -n 40 "$logs/$name.log" >&2
        exit 1
    fi
}

for tool in x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc bison flex pkg-config cmake xz git \
    meson ninja nasm glslangValidator xcodebuild; do
    command -v "$tool" >/dev/null || fail "missing $tool (brew install mingw-w64 bison pkgconf cmake xz meson ninja nasm glslang)"
done
case $(bison --version | head -n 1) in
    *" 2."*) fail "bison is too old, put Homebrew's bison first on PATH" ;;
esac
if [ "$arch" = x86_64 ]; then
    arch -x86_64 /usr/bin/true 2>/dev/null || fail "Rosetta is not installed (softwareupdate --install-rosetta)"
elif [ "$build" != "$host" ]; then
    fail "the arm64 engine is built on an Apple silicon Mac"
fi

# --- Inputs ----------------------------------------------------------------

# shellcheck source=engine/inputs.sh
source "$here/inputs.sh"

crossover_tar=$(fetch crossover-sources)
crossover_version=$(basename "$crossover_tar" .tar.gz)
crossover_version=${crossover_version##*crossover-sources-}
step fetch-proton fetch_proton

# --- Sources ---------------------------------------------------------------

# Puts Wine's tree together in a scratch folder, then copies over only the
# files that changed, so make rebuilds no more than it has to.
assemble_wine() {
    local new=$src/wine.new patch name
    rm -rf "$new"
    mkdir -p "$new"
    tar -xzf "$crossover_tar" -C "$new" --strip-components 2 sources/wine
    rsync -r "$cache/proton/lsteamclient/" "$new/dlls/lsteamclient/"
    rsync -r "$cache/proton/steam_helper/" "$new/programs/steam.exe/"
    for patch in "$here"/patches/*.patch; do
        patch -d "$new" -p1 -s -N --no-backup-if-mismatch <"$patch" || { echo "$(basename "$patch") does not apply"; return 1; }
    done
    mkdir -p "$src/wine"
    rsync -rlc --delete "$new/" "$src/wine/"
    rm -rf "$new"
}
step sources assemble_wine

# --- Libraries -------------------------------------------------------------

# Each library is built once per source release into deps/. Wine loads them
# by name, so their install names carry no build paths; engine/run points
# DYLD_FALLBACK_LIBRARY_PATH at the engine's lib folder.
built() {
    [ -f "$deps/.built-$1-$(deps_digest)" ]
}
mark_built() {
    touch "$deps/.built-$1-$(deps_digest)"
}
deps_digest() {
    {
        for name in crossover-sources nettle sdl2 cereal vulkan-headers glslang spirv-tools \
            spirv-headers gvdb proxy-libintl ffmpeg; do
            upstream "$name" 2
        done
        cat "$here"/patches/glib/*.patch "$here"/patches/dxvk/*.patch
    } | shasum | cut -c1-12
}

# Copies a component's licence files into deps/licenses/<name>.
keep_licences() {
    local name=$1
    shift
    mkdir -p "$deps/licenses/$name"
    cp "$@" "$deps/licenses/$name/"
}

# Copies a git input's files (from fetch_git) into a folder. Assigned first, so
# a failed fetch stops the step rather than copying from nowhere.
copy_git_input() {
    local from
    from=$(fetch_git "$1")
    [ -d "$from" ] || return 1
    rsync -a --exclude .git "$from/" "$2/"
}

apply_patches() {
    local dir=$1 patch
    shift
    for patch in "$@"; do
        patch -d "$dir" -p1 -s -N --no-backup-if-mismatch <"$patch" || { echo "$(basename "$patch") does not apply"; return 1; }
    done
}

extract_crossover_source() {
    rm -rf "$work/$arch/deps-build/$1"
    mkdir -p "$work/$arch/deps-build/$1"
    tar -xzf "$crossover_tar" -C "$work/$arch/deps-build/$1" --strip-components "$2" "$3"
}

autotools_flags=(--host="$host" --build="$build" --prefix="$deps")

build_gmp() {
    extract_crossover_source gmp 3 sources/gnutls/gmp
    cd "$work/$arch/deps-build/gmp"
    ./configure "${autotools_flags[@]}" --disable-shared --enable-static --with-pic
    make -j"$jobs"
    make install
}

build_nettle() {
    rm -rf "$work/$arch/deps-build/nettle"
    mkdir -p "$work/$arch/deps-build/nettle"
    tar -xzf "$(fetch nettle)" -C "$work/$arch/deps-build/nettle" --strip-components 1
    cd "$work/$arch/deps-build/nettle"
    ./configure "${autotools_flags[@]}" --disable-shared --enable-static --with-pic \
        --disable-documentation --disable-openssl \
        CPPFLAGS="-I$deps/include" LDFLAGS="-L$deps/lib"
    make -j"$jobs"
    make install
}

# The *_LIBS are single words because GnuTLS's configure tests them unquoted.
build_gnutls() {
    extract_crossover_source gnutls 3 sources/gnutls/gnutls
    cd "$work/$arch/deps-build/gnutls"
    ./configure "${autotools_flags[@]}" --enable-shared --disable-static \
        --with-included-libtasn1 --with-included-unistring --without-p11-kit \
        --without-idn --without-brotli --without-zstd --without-tpm --without-tpm2 \
        --disable-tools --disable-doc --disable-tests --disable-cxx --disable-nls \
        --disable-libdane --disable-guile \
        --disable-maintainer-mode \
        CPPFLAGS="-I$deps/include" LDFLAGS="-L$deps/lib" \
        NETTLE_LIBS=-lnettle HOGWEED_LIBS=-lhogweed GMP_LIBS=-lgmp
    make -j"$jobs"
    make install
    install_name_tool -id @rpath/libgnutls.30.dylib "$deps/lib/libgnutls.30.dylib"
}

cmake_flags=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$macos_cpu"
    -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_NAME_DIR=@rpath
    -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew)

build_freetype() {
    extract_crossover_source freetype 2 sources/freetype
    cmake -S "$work/$arch/deps-build/freetype" -B "$work/$arch/deps-build/freetype/build" "${cmake_flags[@]}" \
        -DBUILD_SHARED_LIBS=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "$work/$arch/deps-build/freetype/build" -j "$jobs"
    cmake --install "$work/$arch/deps-build/freetype/build"
}

build_sdl2() {
    rm -rf "$work/$arch/deps-build/sdl2"
    mkdir -p "$work/$arch/deps-build/sdl2"
    tar -xzf "$(fetch sdl2)" -C "$work/$arch/deps-build/sdl2" --strip-components 1
    cmake -S "$work/$arch/deps-build/sdl2" -B "$work/$arch/deps-build/sdl2/build" "${cmake_flags[@]}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
    cmake --build "$work/$arch/deps-build/sdl2/build" -j "$jobs"
    cmake --install "$work/$arch/deps-build/sdl2/build"
}

# MoltenVK, CodeWeavers' Vulkan on Metal, and the externals its
# ExternalRevisions name, built the way its own fetchDependencies and Makefile
# do but from pinned sources. Universal, thinned to the engine's architecture.
build_moltenvk() {
    local dir=$work/$arch/deps-build/moltenvk known pair name folder
    extract_crossover_source moltenvk 2 sources/moltenvk
    cd "$dir"
    for pair in cereal:cereal vulkan-headers:Vulkan-Headers glslang:glslang; do
        name=${pair%%:*}
        folder=${pair#*:}
        [ "$(cat "ExternalRevisions/${folder}_repo_revision")" = "$(upstream "$name" 2)" ] ||
            { echo "UPSTREAM.txt's $name isn't the commit CrossOver's MoltenVK names"; return 1; }
        copy_git_input "$name" "External/$folder"
    done
    known=$(python3 -I -c 'import json, sys; print(" ".join(c["commit"] for c in json.load(open(sys.argv[1]))["commits"][:2]))' External/glslang/known_good.json)
    [ "$known" = "$(upstream spirv-tools 2) $(upstream spirv-headers 2)" ] ||
        { echo "UPSTREAM.txt's SPIRV-Tools or SPIRV-Headers isn't the commit glslang names"; return 1; }
    copy_git_input spirv-tools External/glslang/External/spirv-tools
    copy_git_input spirv-headers External/glslang/External/spirv-tools/external/spirv-headers
    unzip -o -q -d External/glslang/External/spirv-tools Templates/spirv-tools/build.zip
    (cd External/glslang && python3 -I ./build_info.py . -i build_info.h.tmpl -o build/include/glslang/build_info.h)
    # xcodebuild would take CC and CXX as compiler paths.
    # shellcheck disable=SC2016  # $(inherited) is Xcode's, not the shell's
    env -u CC -u CXX xcodebuild -quiet -project ExternalDependencies.xcodeproj -scheme ExternalDependencies-macOS \
        -destination generic/platform=macOS -configuration Release \
        -derivedDataPath "$work/$arch/deps-build/moltenvk-derived" \
        GCC_PREPROCESSOR_DEFINITIONS='NDEBUG $(inherited)' MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" build
    env -u CC -u CXX xcodebuild -quiet -project MoltenVKPackaging.xcodeproj -scheme "MoltenVK Package (macOS only)" \
        -destination generic/platform=macOS -configuration Release -derivedDataPath build \
        MACOSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" build
    mkdir -p "$deps/lib"
    lipo -thin "$macos_cpu" -output "$deps/lib/libMoltenVK.dylib" Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
    keep_licences moltenvk LICENSE
    keep_licences spirv-cross External/SPIRV-Cross/LICENSE
    keep_licences glslang External/glslang/LICENSE.txt
    keep_licences spirv-tools External/glslang/External/spirv-tools/LICENSE
    keep_licences cereal External/cereal/LICENSE
}

# meson's cross file for the engine's architecture.
meson_cross=$work/$arch/darwin.ini
cat >"$meson_cross" <<EOF
[binaries]
c = ['clang', '-arch', '$macos_cpu']
cpp = ['clang++', '-arch', '$macos_cpu']
objc = ['clang', '-arch', '$macos_cpu']
objcpp = ['clang++', '-arch', '$macos_cpu']
ar = 'ar'
strip = 'strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'darwin'
cpu_family = '$meson_cpu_family'
cpu = '$macos_cpu'
endian = 'little'
EOF
# Wrap files' downloads (PCRE2 for GLib), checked against their hashes.
export MESON_PACKAGE_CACHE_DIR=$cache/meson
mkdir -p "$MESON_PACKAGE_CACHE_DIR"
# The libraries below link to each other by @rpath, so each looks next to
# itself, and GStreamer's plugins one folder up.
rpath_ldflags="-Wl,-rpath,@loader_path -Wl,-rpath,@loader_path/.."

meson_build() {
    local name=$1 source=$2
    shift 2
    LDFLAGS=$rpath_ldflags meson setup "$work/$arch/deps-build/$name-build" "$source" --cross-file "$meson_cross" \
        --prefix "$deps" --buildtype release "$@"
    ninja -C "$work/$arch/deps-build/$name-build"
    ninja -C "$work/$arch/deps-build/$name-build" install
}

# GLib, for GStreamer. macOS has libffi but no pkg-config file for it.
build_glib() {
    local dir=$work/$arch/deps-build/glib sdk
    extract_crossover_source glib 2 sources/glib
    apply_patches "$dir" "$here"/patches/glib/*.patch
    rmdir "$dir/subprojects/gvdb"
    copy_git_input gvdb "$dir/subprojects/gvdb"
    copy_git_input proxy-libintl "$dir/subprojects/proxy-libintl"
    sdk=$(xcrun --show-sdk-path)
    mkdir -p "$deps/lib/pkgconfig"
    printf 'Name: libffi\nDescription: macOS libffi\nVersion: 3.4.0\nCflags: -I%s/usr/include/ffi\nLibs: -lffi\n' \
        "$sdk" >"$deps/lib/pkgconfig/libffi.pc"
    meson_build glib "$dir" -Ddefault_library=shared -Dtests=false -Dinstalled_tests=false -Dnls=disabled \
        -Dselinux=disabled -Dxattr=false -Dlibmount=disabled -Dman=false -Ddtrace=false \
        -Dsystemtap=false -Dsysprof=disabled -Dglib_debug=disabled -Dgtk_doc=false \
        -Dlibelf=disabled -Dbsymbolic_functions=false
    keep_licences glib "$dir/COPYING"
    keep_licences pcre2 "$dir"/subprojects/pcre2-*/LICENCE
    keep_licences proxy-libintl "$dir/subprojects/proxy-libintl/COPYING"
}

# FFmpeg with only the decoders and parsers games' video and audio need;
# GStreamer does the demuxing.
build_ffmpeg() {
    local dir=$work/$arch/deps-build/ffmpeg
    rm -rf "$dir"
    mkdir -p "$dir"
    tar -xJf "$(fetch ffmpeg)" -C "$dir" --strip-components 1
    cd "$dir"
    ./configure --prefix="$deps" --enable-cross-compile --arch="$meson_cpu_family" --target-os=darwin \
        --cc="$CC" --cxx="$CXX" --enable-shared --disable-static --enable-pic \
        --install-name-dir=@rpath --extra-ldflags="-Wl,-rpath,@loader_path" \
        --disable-programs --disable-doc --disable-network --disable-avdevice --disable-swscale \
        --disable-swresample --disable-postproc --disable-autodetect --disable-everything \
        --enable-decoder=h264,aac,aac_latm,mp3float,mp3,mp2float,wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro,wmalossless,wmavoice,vp8,vp9,vorbis,opus,theora,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,mpeg1video,mpeg2video,flac,adpcm_ms,adpcm_ima_wav \
        --enable-parser=h264,aac,aac_latm,mpegaudio,vc1,vp8,vp9,mpeg4video,mpegvideo,vorbis,opus,flac
    make -j"$jobs"
    make install
    keep_licences ffmpeg COPYING.LGPLv2.1 LICENSE.md
}

# GStreamer with what Wine's winegstreamer uses and the demuxers for MP4,
# WebM, ASF (WMV/WMA), MPEG and WAV; FFmpeg decodes.
build_gstreamer() {
    local dir=$work/$arch/deps-build/gstreamer project
    extract_crossover_source gstreamer 3 sources/gstreamer/subprojects
    local common=(-Dauto_features=disabled -Ddoc=disabled -Dtests=disabled)
    meson_build gstreamer "$dir/gstreamer" "${common[@]}" -Dnls=disabled -Dexamples=disabled \
        -Dtools=enabled -Dintrospection=disabled -Dcheck=enabled -Dcoretracers=disabled \
        -Dlibunwind=disabled -Dlibdw=disabled -Dptp-helper=disabled -Dbash-completion=disabled
    meson_build gst-plugins-base "$dir/gst-plugins-base" "${common[@]}" -Dnls=disabled \
        -Dexamples=disabled -Dintrospection=disabled -Dorc=disabled -Dplayback=enabled \
        -Dtypefind=enabled -Daudioconvert=enabled -Daudioresample=enabled \
        -Dvideoconvertscale=enabled -Dapp=enabled
    meson_build gst-plugins-good "$dir/gst-plugins-good" "${common[@]}" -Dnls=disabled \
        -Dexamples=disabled -Dorc=disabled -Disomp4=enabled -Dmatroska=enabled \
        -Daudioparsers=enabled -Ddeinterlace=enabled -Dvideofilter=enabled -Dwavparse=enabled \
        -Did3demux=enabled
    meson_build gst-plugins-ugly "$dir/gst-plugins-ugly" "${common[@]}" -Dnls=disabled -Dorc=disabled \
        -Dasfdemux=enabled
    meson_build gst-plugins-bad "$dir/gst-plugins-bad" "${common[@]}" -Dnls=disabled \
        -Dexamples=disabled -Dorc=disabled -Dintrospection=disabled -Dvideoparsers=enabled \
        -Dmpegdemux=enabled -Dmpegtsdemux=enabled
    meson_build gst-libav "$dir/gst-libav" -Ddoc=disabled -Dtests=disabled
    for project in gstreamer gst-plugins-base gst-plugins-good gst-plugins-ugly gst-plugins-bad gst-libav; do
        keep_licences "$project" "$dir/$project/COPYING"
    done
}

# meson gives the libraries it installs their full path in deps/ as their
# name; the engine needs them found next to whatever loads them, by @rpath.
use_rpath_names() {
    local file dep
    for file in "$deps"/lib/*.dylib "$deps"/lib/gstreamer-1.0/*.dylib; do
        [ -L "$file" ] && continue
        case $(otool -D "$file" | sed -n 2p) in
            "$deps"/lib/*) install_name_tool -id "@rpath/$(basename "$(otool -D "$file" | sed -n 2p)")" "$file" ;;
        esac
        for dep in $(otool -L "$file" | awk 'NR > 1 { print $1 }' | grep "^$deps/lib/"); do
            install_name_tool -change "$dep" "@rpath/$(basename "$dep")" "$file"
        done
        delete_absolute_rpaths "$file"
    done
}

# meson and GLib's pkg-config files also add deps/ itself as a search path,
# which only this Mac has.
delete_absolute_rpaths() {
    local path
    for path in $(otool -l "$1" | awk '$1 == "path" && $2 ~ /^\// { print $2 }'); do
        install_name_tool -delete_rpath "$path" "$1"
    done
}

for lib in gmp nettle gnutls freetype sdl2 moltenvk glib ffmpeg gstreamer; do
    if ! built "$lib"; then
        step "$lib" "build_$lib"
        mark_built "$lib"
    fi
done
step rpath-names use_rpath_names
rm -rf "$work/$arch/deps-build"

# --- Wine ------------------------------------------------------------------

wine_build=$work/$arch/wine-build
wine_tools=$work/$arch/wine-tools

# On Apple silicon, x86_64 Wine is cross-compiled with this Mac's own
# compilers, which is quicker than compiling under Rosetta. Its build tools
# (winebuild, widl, ...) have to come from a build that isn't cross-compiled,
# so they are built first, under Rosetta, where an x86_64 configure needs no
# extra compilers. A build for the Mac it runs on makes its own.
build_wine_tools() {
    mkdir -p "$wine_tools"
    cd "$wine_tools"
    if [ ! -f Makefile ]; then
        # sfnt2fon needs FreeType to build Wine's bitmap fonts.
        arch -x86_64 "$src/wine/configure" --build="$host" --host="$host" --enable-win64 \
            --disable-tests --without-x --without-gnutls --without-sdl --without-vulkan \
            --without-opencl PKG_CONFIG=pkg-config LDFLAGS="-Wl,-rpath,$deps/lib"
    fi
    arch -x86_64 make -j"$jobs" __tooldeps__
}
if [ "$build" != "$host" ]; then
    step wine-tools build_wine_tools
    winebuild_tool=$wine_tools/tools/winebuild/winebuild
    wine_tools_flag=(--with-wine-tools="$wine_tools")
else
    winebuild_tool=$wine_build/tools/winebuild/winebuild
    wine_tools_flag=()
fi

# CrossOver's DXVK, for Direct3D 9 to 11 on Vulkan, marked as Wine builtins so
# the launcher can put them first on Wine's DLL path when asked for.
build_dxvk() {
    local dir=$work/$arch/deps-build/dxvk bits pe_arch dll
    extract_crossover_source dxvk 2 sources/dxvk
    apply_patches "$dir" "$here"/patches/dxvk/*.patch
    cd "$dir"
    for bits in 64 32; do
        [ "$bits" = 64 ] && pe_arch=x86_64 || pe_arch=i386
        # GCC 13 and later no longer include <cstdint> indirectly.
        meson setup "build$bits" --cross-file "build-win$bits.txt" --buildtype release --strip \
            --prefix / -Denable_tests=false -Dcpp_args="-include cstdint"
        ninja -C "build$bits"
        DESTDIR="$dir/out$bits" meson install -C "build$bits"
        rm -rf "$deps/dxvk/$pe_arch-windows"
        mkdir -p "$deps/dxvk/$pe_arch-windows"
        for dll in "out$bits"/bin/*.dll; do
            cp "$dll" "$deps/dxvk/$pe_arch-windows/"
            "$winebuild_tool" --builtin "$deps/dxvk/$pe_arch-windows/$(basename "$dll")"
        done
    done
    keep_licences dxvk LICENSE
}

# --- Wine ------------------------------------------------------------------

# Reference: Gcenx's macOS Wine builds. Everything Linux-only or not shipped is off.
wine_flags=(--prefix="$stage" --host="$host" --build="$build" "${wine_tools_flag[@]}"
    --enable-archs="$wine_archs" --disable-tests
    --with-coreaudio --with-freetype --with-gnutls --with-sdl --with-opencl --with-pcap
    --without-alsa --without-capi --without-dbus --without-ffmpeg --without-fontconfig
    --without-gettext --without-gettextpo --without-gphoto --without-gssapi --with-gstreamer
    --without-inotify --without-krb5 --without-netapi --without-oss --without-pulse
    --without-sane --without-udev --without-usb --without-v4l2 --without-wayland --without-x
    # Wine loads MoltenVK as its Vulkan library, as CrossOver does; no loader.
    --with-vulkan ac_cv_lib_soname_vulkan=libMoltenVK.dylib
    # winegstreamer links GStreamer, which sits in lib/, two folders up.
    PKG_CONFIG=pkg-config CPPFLAGS="-I$deps/include" LDFLAGS="-L$deps/lib -Wl,-rpath,@loader_path/../.."
    # Keep this machine's paths out of the binaries (assertion messages, debug info).
    CFLAGS="-g -O2 -ffile-prefix-map=$work/=" CXXFLAGS="-g -O2 -ffile-prefix-map=$work/="
    CROSSCFLAGS="-O2 -ffile-prefix-map=$work/=")

if [ "$arch" = x86_64 ]; then
    wine_flags+=(--enable-win64)
else
    # Wine's ARM64EC and arm64 parts need clang's PE compilers (llvm-mingw).
    llvm_mingw=$cache/$(basename "$(upstream llvm-mingw 3)" .tar.xz)
    [ -x "$llvm_mingw/bin/arm64ec-w64-mingw32-clang" ] || tar -xJf "$(fetch llvm-mingw)" -C "$cache"
    PATH=$llvm_mingw/bin:$PATH
    wine_flags+=(--with-mingw=llvm-mingw)
fi

configure_wine() {
    mkdir -p "$wine_build"
    cd "$wine_build"
    "$src/wine/configure" "${wine_flags[@]}"
    grep -q "^host_cpu = ${host%%-*}" Makefile || { echo "configure did not take the ${host%%-*} host"; return 1; }
    printf '%s\n' "${wine_flags[@]}" >.wheelio-configure
}

if [ ! -f "$wine_build/Makefile" ] || ! printf '%s\n' "${wine_flags[@]}" | cmp -s - "$wine_build/.wheelio-configure"; then
    step wine-configure configure_wine
fi
step wine-build make -C "$wine_build" -j"$jobs"

# CrossOver's DXVK is built for x86 Windows only, so only the x86_64 engine
# has it.
if [ "$arch" = x86_64 ] && ! built dxvk; then
    step dxvk build_dxvk
    mark_built dxvk
fi
rm -rf "$work/$arch/deps-build"

# --- Package ---------------------------------------------------------------

package() {
    local dxmt_dir lib
    rm -rf "$stage"
    make -C "$wine_build" -j"$jobs" install-lib
    rm -rf "$stage/share/man"

    # GStreamer's plugins, the libraries Wine loads by name, and whatever
    # those and winegstreamer link to, so nothing unused comes along.
    mkdir -p "$stage/lib/gstreamer-1.0"
    cp "$deps"/lib/gstreamer-1.0/*.dylib "$stage/lib/gstreamer-1.0/"
    for lib in libfreetype.6.dylib libgnutls.30.dylib libSDL2-2.0.0.dylib libMoltenVK.dylib; do
        cp "$deps/lib/$lib" "$stage/lib/"
    done
    copy_linked_libraries "$stage"/lib/gstreamer-1.0/*.dylib "$stage/lib/wine/$wine_unix/winegstreamer.so"
    delete_absolute_rpaths "$stage/lib/wine/$wine_unix/winegstreamer.so"
    [ ! -d "$deps/dxvk" ] || cp -R "$deps/dxvk" "$stage/lib/dxvk"

    # Wine finds these in its data folder and uses them in place, so prefixes
    # neither download nor copy them.
    mkdir -p "$stage/share/wine/mono" "$stage/share/wine/gecko"
    tar -xJf "$(fetch wine-mono)" -C "$stage/share/wine/mono"
    tar -xJf "$(fetch wine-gecko-x86)" -C "$stage/share/wine/gecko"
    tar -xJf "$(fetch wine-gecko-x86_64)" -C "$stage/share/wine/gecko"

    # DXMT's releases are for x86_64 Wine only.
    if [ "$arch" = x86_64 ]; then
        dxmt_dir=$stage/lib/dxmt
        mkdir -p "$dxmt_dir"
        tar -xzf "$(fetch dxmt)" -C "$dxmt_dir" --strip-components 1
    fi

    # Prefixes link to Wine's built-in DLLs; read-only, a Windows installer
    # writing through a link fails rather than changing the engine.
    find "$stage/lib/wine" -type f -name "*.*" ! -name "*.so" -exec chmod a-w {} +

    cp "$here/run" "$stage/run"
    chmod +x "$stage/run"
    echo "$crossover_version-$ENGINE_REVISION" >"$stage/VERSION"

    licences "$stage/share/licenses"
}

# Copies into the engine's lib/ every library from deps/ the given files link
# to by @rpath, and those libraries' own, until nothing is missing.
copy_linked_libraries() {
    local file name queue=("$@")
    while [ ${#queue[@]} -gt 0 ]; do
        file=${queue[0]}
        queue=("${queue[@]:1}")
        for name in $(otool -L "$file" | awk 'NR > 1 && $1 ~ /^@rpath\// { sub("@rpath/", "", $1); print $1 }'); do
            [ -e "$stage/lib/$name" ] || [ ! -e "$deps/lib/$name" ] && continue
            cp "$deps/lib/$name" "$stage/lib/$name"
            queue+=("$stage/lib/$name")
        done
    done
}

# Every component's licence, as its terms ask.
licences() {
    local to=$1
    mkdir -p "$to"/{wine,lsteamclient,steam-helper,freetype,gnutls,nettle,gmp,sdl2,wine-mono,wine-gecko}
    [ "$arch" != x86_64 ] || mkdir -p "$to/dxmt"
    cp -R "$deps"/licenses/* "$to/"
    cp "$(fetch wine-mono-license)" "$to/wine-mono/COPYING"
    cp "$(fetch wine-gecko-license)" "$to/wine-gecko/LICENSE"
    cp "$(fetch wine-gecko-licenses)" "$to/wine-gecko/license.html"
    cp "$src/wine/LICENSE" "$src/wine/COPYING.LIB" "$to/wine/"
    cp "$src/wine/dlls/lsteamclient/LICENSE" "$to/lsteamclient/"
    cp "$cache/proton/LICENSE" "$to/steam-helper/"
    [ "$arch" != x86_64 ] || cp "$(fetch dxmt-license)" "$to/dxmt/LICENSE"
    tar -xOzf "$crossover_tar" sources/freetype/LICENSE.TXT >"$to/freetype/LICENSE.TXT"
    tar -xOzf "$crossover_tar" sources/freetype/docs/FTL.TXT >"$to/freetype/FTL.TXT"
    tar -xOzf "$crossover_tar" sources/freetype/docs/GPLv2.TXT >"$to/freetype/GPLv2.TXT"
    tar -xOzf "$crossover_tar" sources/gnutls/gnutls/LICENSE >"$to/gnutls/LICENSE"
    tar -xOzf "$(fetch nettle)" "nettle-*/COPYING.LESSERv3" >"$to/nettle/COPYING.LESSERv3"
    tar -xOzf "$crossover_tar" sources/gnutls/gmp/COPYING.LESSERv3 >"$to/gmp/COPYING.LESSERv3"
    tar -xOzf "$(fetch sdl2)" "SDL2-*/LICENSE.txt" >"$to/sdl2/LICENSE.txt"
}

tarball() {
    mkdir -p "$out"
    tar -C "$stage" --uid 0 --gid 0 -cf - . | xz -T0 -9 >"$out/$1.part"
    mv "$out/$1.part" "$out/$1"
}

step package package
name=wheelio-engine-$crossover_version-$ENGINE_REVISION-$arch.tar.xz
step tarball tarball "$name"
echo "$out/$name"
