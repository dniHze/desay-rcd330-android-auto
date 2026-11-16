#!/usr/bin/env bash
# Build our libUIPublic.so for the head unit (armhf), in the toolchain container:
#   docker run --rm --platform linux/arm64 -v "$PWD":/w desay-aa:latest \
#     bash /w/i18n/lib/build.sh <table .inc> <out .so> [plain|aa] [extra compiler flags...]
#
# With -DAA_SETTINGS='"<settings .inc>"' (i18n/gen_settings.py, from the base's setting app), the
# Settings app's language list offers every language; without it the list stays stock. With
# -DAA_LITERALS='"<literals .inc>"' (i18n/literals.py, from the base's apps), the apps' own texts
# follow the language; without it they stay English.
#
# <table .inc> comes from i18n/gen_table.py (the phrase book of one base, stock or rebranded);
# "aa" also renames CarLife in the other languages (AA_REBRAND). i18n/build.py runs this for
# every base and variant.
#
# The library replaces the stock one in every UI app, so it links as the stock one does:
#   * libstdc++ dynamically, the unit's own (6.0.17, up to GLIBCXX_3.4.17): string_replace
#     returns a std::string (the old COW ABI) that the app destroys, so the string's empty
#     representation must be the app's;
#   * App(), SVP_GetLanguage() and SVP_ChangeLanguage() stay undefined: the app process has them
#     (libUIFrame-kanzi, libapp), exactly as for the stock library;
#   * only the five stock exports are visible.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(cd "${HERE}/../.." && pwd)"
XT="${WORK}/.scratch/x-tools/arm-cortex_a15-linux-gnueabihf/bin/arm-cortex_a15-linux-gnueabihf"
CXX="${CXX:-${XT}-g++}"
READELF="${READELF:-${XT}-readelf}"
STRIP="${STRIP:-${XT}-strip}"
SYSROOT="${SYSROOT:-${WORK}/.scratch/sysroot}"
[ $# -ge 2 ] || { sed -n '2,6p' "$0"; exit 2; }
TABLE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$2"
VARIANT="${3:-plain}"
shift $(($# >= 3 ? 3 : 2))
DEFS=()
[ "${VARIANT}" = aa ] && DEFS+=(-DAA_REBRAND)
[ -f "${SYSROOT}/lib/libstdc++.so.6" ] || { echo "the unit's libstdc++ not found under ${SYSROOT}" >&2; exit 1; }

"${CXX}" -std=gnu++11 -O2 -fPIC -shared -fvisibility=hidden -D_GLIBCXX_USE_CXX11_ABI=0 \
    -Wall -Wextra -Wno-unused-parameter -DAA_TABLE="\"${TABLE}\"" "${DEFS[@]}" "$@" \
    "${HERE}/uipublic.cpp" "${HERE}/settings.cpp" "${HERE}/literals.cpp" "${HERE}/bluetooth.cpp" \
    "${HERE}/appearance.cpp" -lpthread \
    -Wl,-soname,libUIPublic.so -Wl,--allow-shlib-undefined -Wl,--hash-style=gnu -Wl,--build-id=none \
    -nodefaultlibs "${SYSROOT}/lib/libstdc++.so.6" "${SYSROOT}/lib/libgcc_s.so.1" -ldl -lc -lgcc \
    -o "${OUT}.tmp"
"${STRIP}" --strip-unneeded -o "${OUT}" "${OUT}.tmp"
rm -f "${OUT}.tmp"

# The gate: the stock library's exports (a test build, -DAA_LANG_TEST, also shows aa_lang_* and
# aa_test_*),
# nothing the unit's libraries cannot give.
exports="$("${READELF}" --dyn-syms -W "${OUT}" | awk '$7 != "UND" && $5 == "GLOBAL" && ($4 == "FUNC" || $4 == "OBJECT") {print $8}' |
    { if [[ " $* " == *" -DAA_LANG_TEST "* ]]; then grep -v -E '^_Z[0-9]*aa_(lang|test)_'; else cat; fi; } | sort | tr '\n' ' ')"
want="_Z11GetVWStringi _Z13GetVWStringExi _Z14GetStringItemsv _Z14string_replaceRKSsS0_S0_ _fini _init s_Items "
[ "${exports}" = "${want}" ] || { echo "export gate FAILED: ${exports}" >&2; exit 1; }
maxglibcxx="$("${READELF}" -V "${OUT}" | grep -oE 'GLIBCXX_[0-9.]+' | sort -V | tail -1 || true)"
maxglibc="$("${READELF}" -V "${OUT}" | grep -oE 'GLIBC_[0-9.]+' | sort -V | tail -1 || true)"
[ "$(printf '%s\nGLIBCXX_3.4.17\n' "${maxglibcxx}" | sort -V | tail -1)" = GLIBCXX_3.4.17 ] ||
    { echo "ABI gate FAILED: needs ${maxglibcxx}" >&2; exit 1; }
[ "$(printf '%s\nGLIBC_2.15\n' "${maxglibc}" | sort -V | tail -1)" = GLIBC_2.15 ] ||
    { echo "ABI gate FAILED: needs ${maxglibc}" >&2; exit 1; }
soname="$("${READELF}" -d "${OUT}" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
[ "${soname}" = libUIPublic.so ] || { echo "SONAME gate FAILED: ${soname}" >&2; exit 1; }
echo "$(basename "${OUT}") (${VARIANT}): exports ok, ${maxglibcxx}, ${maxglibc}, NEEDED $("${READELF}" -d "${OUT}" |
    awk '/NEEDED/ {gsub(/[\[\]]/, "", $5); printf "%s ", $5}')"
