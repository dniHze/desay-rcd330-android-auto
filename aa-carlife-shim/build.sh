#!/usr/bin/env bash
# Build aa-carlife-shim and cl_probe for the head unit (armhf).
#
# Run inside the arm64 build container, like aa-bridge:
#   docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest bash /w/aa-carlife-shim/build.sh
#
# CROSS MODEL (same as aa-bridge/build.sh):
#   * x-tools gcc 6.5 with its own glibc 2.12 sysroot; nothing is linked from the device.
#   * -D_GLIBCXX_USE_CXX11_ABI=0, -static-libstdc++ -static-libgcc: no GLIBCXX_ demand.
#   * RCF is compiled in from source (RCF 2.1.0.0, runtime version 12, the same as the
#     device's libRCF.so.2.2.0.0), with boost 1.56 headers (the device's boost version).
#     So no vendor header or library is needed and the device C++ ABI does not matter.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "${HERE}/.." && pwd)"
XT="${WORK}/.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/bin/arm-cortex_a15-linux-gnueabihf"
CXX="${CXX:-${XT}-g++}"
READELF="${READELF:-${XT}-readelf}"
STRIP="${STRIP:-${XT}-strip}"
OBJDUMP="${OBJDUMP:-${XT}-objdump}"
SYSROOT="${SYSROOT:-${WORK}/.scratch/sysroot}"
RCF_DIR="${RCF_DIR:-${WORK}/.scratch/rcf-dl/src/RCF-2.1.0.0}"
BOOST_DIR="${BOOST_DIR:-${WORK}/.scratch/boost-dl/boost_1_56_0}"
OUT="${HERE}/out"

[ -x "${CXX}" ] || { echo "CXX not found/executable: ${CXX}" >&2; exit 1; }
[ -f "${RCF_DIR}/src/RCF/RCF.cpp" ] || { echo "RCF source not found: ${RCF_DIR}" >&2; exit 1; }
[ -f "${BOOST_DIR}/boost/version.hpp" ] || { echo "boost headers not found: ${BOOST_DIR}" >&2; exit 1; }
[ -e "${SYSROOT}/lib/ld-linux-armhf.so.3" ] || { echo "device loader not found under ${SYSROOT}" >&2; exit 1; }
mkdir -p "${OUT}"

# -funwind-tables: every function can be unwound, for the shim's crash backtrace (onCrash).
CXXFLAGS="-std=gnu++11 -O2 -pthread -D_GLIBCXX_USE_CXX11_ABI=0 -Wno-deprecated-declarations -funwind-tables"
# Features we do not use are compiled out. SF + TCP + server + pub/sub stay. HTTP and UDP
# stay on because RCF 2.1's RcfServer.cpp does not link without them (they need no extra
# library). LEGACY stays on (default): it only adds handling for older runtime versions.
# NDEBUG: RCF's release build. Its debug asserts abort the process where the release build
# throws (an event id without a servant method aborted the shim, step 3 runs 34-36).
RCF_DEFS="-DNDEBUG -DRCF_FEATURE_SF=1 -DRCF_FEATURE_BOOST_SERIALIZATION=0 -DRCF_FEATURE_TCP=1
          -DRCF_FEATURE_SERVER=1 -DRCF_FEATURE_PUBSUB=1
          -DRCF_FEATURE_ZLIB=0 -DRCF_FEATURE_OPENSSL=0 -DRCF_FEATURE_SSPI=0
          -DRCF_FEATURE_FILETRANSFER=0
          -DRCF_FEATURE_NAMEDPIPE=0 -DRCF_FEATURE_LOCALSOCKET=0 -DRCF_FEATURE_JSON=0
          -DRCF_FEATURE_PROTOBUF=0 -DRCF_FEATURE_IPV6=0 -DRCF_FEATURE_CUSTOM_ALLOCATOR=0"
# boost/version.hpp first in every file: RCF/Config.hpp tests BOOST_VERSION without including
# it, so a file that does not include it first (all of ours) got FutureImpl's destructor as
# noexcept while RCF.cpp got noexcept(false). The mismatch made every RCF error thrown from
# that destructor (a void call, createSubscription) end the process, whatever caught it.
INCLUDES="-include boost/version.hpp -I${HERE}/src -I${RCF_DIR}/include -isystem ${BOOST_DIR}"
LDFLAGS="-static-libstdc++ -static-libgcc -pthread"
# Static libstdc++ uses __tls_get_addr. Left to the toolchain's glibc 2.12 libc.so script it
# resolves to "ld-linux.so.3", which on a current glibc maps the loader a second time and
# aborts. Linking the device's own loader (SONAME ld-linux-armhf.so.3) records the same
# NEEDED name as every stock binary and aa-bridge. It must also come LAST in NEEDED, after
# libc, as in the stock binaries: the device's glibc 2.15 loader asserts in dl-minimal.c
# (realloc) when it is listed before libc. libm and libc go in as files because the g++
# driver moves -lm/-lc to its own position, after anything given here.
TC_LIB="$("${CXX}" -print-sysroot)/lib"
LIBS="-lrt -ldl ${TC_LIB}/libpthread.so.0 ${TC_LIB}/libm.so.6 ${TC_LIB}/libc.so.6 ${SYSROOT}/lib/ld-linux-armhf.so.3"

abi_gate() {
    local bin="$1"
    local interp maxglibc maxglibcxx
    interp="$("${READELF}" -l "${bin}" | grep -o '/lib/ld[^]]*' | head -1 || true)"
    maxglibc="$("${READELF}" -V "${bin}" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 || true)"
    maxglibcxx="$("${READELF}" -V "${bin}" | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1 || true)"
    echo "---- ABI gate: $(basename "${bin}") ----"
    echo "interp:    ${interp:-<none>}   (must be /lib/ld-linux-armhf.so.3)"
    echo "max GLIBC: ${maxglibc:-<none>}   (must be <= GLIBC_2.15)"
    echo "GLIBCXX:   ${maxglibcxx:-(none / static)}"
    echo "NEEDED:    $("${READELF}" -d "${bin}" | awk '/NEEDED/ {gsub(/[\[\]]/, "", $5); printf "%s ", $5}')"
    [ "${interp}" = "/lib/ld-linux-armhf.so.3" ] || { echo "ABI gate FAILED: interpreter" >&2; exit 1; }
    [ -z "${maxglibcxx}" ] || { echo "ABI gate FAILED: GLIBCXX dependency" >&2; exit 1; }
    if "${READELF}" -d "${bin}" | grep -q '\[ld-linux\.so\.3\]'; then
        echo "ABI gate FAILED: NEEDED ld-linux.so.3 (stock binaries use ld-linux-armhf.so.3)" >&2; exit 1
    fi
    if [ "$("${READELF}" -d "${bin}" | awk '/NEEDED/ {n=$5} END {print n}')" != "[ld-linux-armhf.so.3]" ]; then
        echo "ABI gate FAILED: ld-linux-armhf.so.3 must be the last NEEDED entry, after libc" >&2; exit 1
    fi
    if [ -n "${maxglibc}" ] && [ "$(printf '%s\nGLIBC_2.15\n' "${maxglibc}" | sort -V | tail -1)" != "GLIBC_2.15" ]; then
        echo "ABI gate FAILED: needs ${maxglibc}" >&2; exit 1
    fi
}

echo "==> RCF $(sed -n 's/.*RCF_VERSION_STR *"\(.*\)".*/\1/p' "${RCF_DIR}/include/RCF/BuildVersion.hpp" 2>/dev/null || true) from ${RCF_DIR}"
# One change to RCF's sources, in a copy (the download stays as it is): its timeouts measure
# time with gettimeofday() (src/RCF/util/Platform.cpp, getCurrentTimeMs), and the unit's wall
# clock steps by hours early in every boot, which ended RCF calls in flight at once. The copy
# reads CLOCK_MONOTONIC instead. Only differences of that clock are used (Timer, timeouts, ping
# stamps), never the wall time, so nothing else changes. (RCF's files have CRLF line ends.)
RCF_SRC="$(mktemp -d)"
trap 'rm -rf "${RCF_SRC}"' EXIT
cp -R "${RCF_DIR}/src/RCF" "${RCF_DIR}/src/SF" "${RCF_SRC}/"
PLATFORM="${RCF_SRC}/RCF/util/Platform.cpp"
sed -i -e 's|gettimeofday(&start, NULL);|rcfMonotonicTimeval(\&start);|' \
       -e 's|gettimeofday(&now, NULL);|rcfMonotonicTimeval(\&now);|' \
       -e 's|^#include <boost/cstdint.hpp>|#include <boost/cstdint.hpp>\
#include <sys/time.h>\
#include <time.h>\
static void rcfMonotonicTimeval(struct timeval* tv) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, \&ts); tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000; }|' \
       "${PLATFORM}"
if [ "$(grep -c 'rcfMonotonicTimeval' "${PLATFORM}")" -ne 3 ] || grep -q 'gettimeofday(&start\|gettimeofday(&now' "${PLATFORM}"; then
    echo "RCF patch FAILED: Platform.cpp's getCurrentTimeMs still reads the wall clock" >&2
    exit 1
fi
# shellcheck disable=SC2086
"${CXX}" ${CXXFLAGS} ${RCF_DEFS} ${INCLUDES} -w -c "${RCF_SRC}/RCF/RCF.cpp" -o "${OUT}/RCF.o"

# The shim's build id, in its "starting:" line and crash reports: the md5 of its sources (there
# is no version control to take it from); AA_VERSION overrides.
AA_VERSION="${AA_VERSION:-src-$(cat "${HERE}"/src/*.cpp "${HERE}"/src/*.hpp | md5sum | cut -c1-8)}"
echo "==> build id ${AA_VERSION}"

for prog in shim cl_probe fake_receiver fake_ilm fake_source fake_can fake_bt fake_power fake_syslog; do
    extra=""
    case "${prog}" in
        shim)          name=aa-carlife-shim; srcs="${HERE}/src/shim.cpp ${OUT}/RCF.o"; extra="-DAA_VERSION=\"${AA_VERSION}\"" ;;
        cl_probe)      name=cl_probe;        srcs="${HERE}/src/cl_probe.cpp ${OUT}/RCF.o" ;;
        fake_receiver) name=fake_receiver;   srcs="${HERE}/tools/fake_receiver.cpp" ;;
        fake_ilm)      name=fake_ilm;        srcs="${HERE}/tools/fake_ilm.cpp ${OUT}/RCF.o" ;;
        fake_source)   name=fake_source;     srcs="${HERE}/tools/fake_source.cpp ${OUT}/RCF.o" ;;
        fake_can)      name=fake_can;        srcs="${HERE}/tools/fake_can.cpp ${OUT}/RCF.o" ;;
        fake_bt)       name=fake_bt;         srcs="${HERE}/tools/fake_bt.cpp ${OUT}/RCF.o" ;;
        fake_power)    name=fake_power;      srcs="${HERE}/tools/fake_power.cpp ${OUT}/RCF.o" ;;
        fake_syslog)   name=fake_syslog;     srcs="${HERE}/tools/fake_syslog.cpp" ;;
    esac
    echo "==> ${name}"
    # shellcheck disable=SC2086
    "${CXX}" ${CXXFLAGS} ${RCF_DEFS} ${INCLUDES} -Wall ${extra} ${srcs} ${LDFLAGS} ${LIBS} \
        -o "${OUT}/${name}.unstripped.armhf"
    "${STRIP}" -o "${OUT}/${name}.armhf" "${OUT}/${name}.unstripped.armhf"
    abi_gate "${OUT}/${name}.armhf"
done

# The method IDs our build sends must equal the stock table (saved as
# tools/stock_ids.txt). Read them back from unoptimised client stubs of both
# interfaces, the same way they were read from the stock libcarlife.so and carlifeservice.
echo "==> method-ID check against tools/stock_ids.txt"
# shellcheck disable=SC2086
"${CXX}" ${CXXFLAGS/-O2/-O0} -fno-inline ${RCF_DEFS} ${INCLUDES} -w \
    -c "${HERE}/tools/idcheck.cpp" -o "${OUT}/idcheck.o"
"${OBJDUMP}" -dr "${OUT}/idcheck.o" > "${OUT}/idcheck.dis"
CXXFILT="${XT}-c++filt" python3 -I "${HERE}/tools/rcf_ids.py" "${OUT}/idcheck.dis" > "${OUT}/our_ids.txt"
if diff -u "${HERE}/tools/stock_ids.txt" "${OUT}/our_ids.txt"; then
    echo "method IDs: $(wc -l < "${OUT}/our_ids.txt") of $(wc -l < "${HERE}/tools/stock_ids.txt") identical to stock"
else
    echo "method-ID check FAILED: the interface order or types differ from stock" >&2
    exit 1
fi
rm -f "${OUT}/idcheck.o" "${OUT}/idcheck.dis" "${OUT}"/idcheck_*.o

echo "built into ${OUT}"
ls -l "${OUT}"/*.armhf
