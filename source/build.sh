#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")"
resource_object="$(mktemp)"
trap 'rm -f "$resource_object"' EXIT
"${WINDRES:-i686-w64-mingw32-windres}" --preprocessor="${CXX:-i686-w64-mingw32-g++}" --preprocessor-arg=-E --preprocessor-arg=-xc++ --preprocessor-arg=-DRC_INVOKED -i resources.rc -O coff -o "$resource_object"
"${CXX:-i686-w64-mingw32-g++}" -O2 -std=c++17 -Wall -Wextra -Werror -shared -static -s plugin.cpp "$resource_object" -lwinmm -ldxguid -lstrmiids -ldmoguids -luuid -lole32 -lksuser -Wl,--kill-at -o ../Rebound.SA.asi
