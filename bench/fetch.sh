#!/bin/sh
# Fetch and build the allocators and workloads used by bench/run.py, at
# pinned versions, into the directory given as $1 (default build/ext).
# Nothing fetched here is committed to this repository.
#
#   mimalloc       v2.2.4  (commit 00d07c4c15a04a4cb68d3aeeecbd8d6ae80bb0b7)
#   jemalloc       5.3.0   (release tarball, sha256 checked)
#   mimalloc-bench 69c41ed7e6419fb5d362eb04ba34b850ec89dc8e (workload sources only)
set -eu
EXT=${1:-build/ext}
JOBS=${JOBS:-4}
mkdir -p "$EXT"
cd "$EXT"

MI_COMMIT=00d07c4c15a04a4cb68d3aeeecbd8d6ae80bb0b7
MB_COMMIT=69c41ed7e6419fb5d362eb04ba34b850ec89dc8e
JE_VER=5.3.0
JE_SHA=2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa

fetch_git() { # dir url commit
  if [ ! -d "$1/.git" ]; then
    git init -q "$1"
    git -C "$1" remote add origin "$2"
  fi
  if [ "$(git -C "$1" rev-parse HEAD 2>/dev/null || true)" != "$3" ]; then
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q FETCH_HEAD
  fi
}

case "$(uname -s)" in
  Darwin) SO=dylib ;;
  *) SO=so ;;
esac

# ---- mimalloc
fetch_git mimalloc https://github.com/microsoft/mimalloc "$MI_COMMIT"
if [ ! -f mimalloc/out/libmimalloc.$SO ]; then
  cmake -S mimalloc -B mimalloc/out -DCMAKE_BUILD_TYPE=Release -DMI_BUILD_TESTS=OFF \
    -DMI_BUILD_OBJECT=OFF >/dev/null
  cmake --build mimalloc/out -j "$JOBS" >/dev/null
fi

# ---- jemalloc
if [ ! -d jemalloc-$JE_VER ]; then
  curl -sSL -o je.tar.bz2 \
    https://github.com/jemalloc/jemalloc/releases/download/$JE_VER/jemalloc-$JE_VER.tar.bz2
  if command -v shasum >/dev/null 2>&1; then got=$(shasum -a 256 je.tar.bz2 | cut -d' ' -f1)
  else got=$(sha256sum je.tar.bz2 | cut -d' ' -f1); fi
  [ "$got" = "$JE_SHA" ] || { echo "jemalloc checksum mismatch: $got" >&2; exit 1; }
  tar xjf je.tar.bz2 && rm je.tar.bz2
fi
if [ ! -f jemalloc-$JE_VER/lib/libjemalloc.$SO ]; then
  (cd jemalloc-$JE_VER && ./configure --disable-cxx >/dev/null && make -j "$JOBS" >/dev/null 2>&1)
fi

# ---- workloads (sources from mimalloc-bench, built with our own flags)
fetch_git mimalloc-bench https://github.com/daanx/mimalloc-bench "$MB_COMMIT"
if [ ! -f mimalloc-bench/out/larson ]; then
  cmake -S mimalloc-bench/bench -B mimalloc-bench/out -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build mimalloc-bench/out -j "$JOBS" >/dev/null 2>&1 || true
fi
ls mimalloc/out/libmimalloc.$SO jemalloc-$JE_VER/lib/libjemalloc.$SO
ls mimalloc-bench/out | head -40
