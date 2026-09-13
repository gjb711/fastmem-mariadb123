#!/usr/bin/env bash
#
# FASTMEM - build & install script (source distribution)
#
# The repository holds only sources plus this script.  The script clones
# the requested MariaDB server source, drops storage/fastmem/ into it and
# builds the plugin with the server's own cmake - no prebuilt binaries,
# no GLIBC/ABI compatibility concerns, works on any distribution that can
# build MariaDB itself (tested target: Linux, macOS, Windows/Git-Bash).
#
# Usage:
#   ./install.sh --mariadb-version 12.3.3            # build latest 12.3.x
#   ./install.sh --detect                            # auto-detect the local server version
#   ./install.sh --mariadb-version 12.4 --jobs 8     # LTS branch, parallel
#   ./install.sh --source-dir ~/mariadb-server       # reuse an existing tree
#   ./install.sh --help
#
# Options:
#   --detect                Read the version of the local mariadbd/mysqld and
#                           build exactly that version (no guessing needed).
#   --mariadb-version <v>   MariaDB version/branch to fetch.  Tries the
#                           release tag "maria-<v>" / "mariadb-<v>" first,
#                           then the branch "<v>" (default: 12.3.3)
#   --branch <name>         Fetch a specific branch/tag (overrides the
#                           version guessing; e.g. --branch 11.4)
#   --source-dir <dir>      Do not clone: use an existing MariaDB source
#                           tree (it will be modified: storage/fastmem is
#                           created inside it)
#   --build-dir <dir>       CMake build directory (default: <src>/build_fastmem)
#   --jobs <n>              Parallel build jobs (default: nproc/cores)
#   --plugin-dir <dir>      Copy the built plugin here (default: auto-detect
#                           via mariadbd/mysql_config, otherwise print hint)
#   --extra-cmake-args ".." Extra arguments passed to the cmake configure
#   --dry-run               Print what would happen, do nothing
#   -h, --help              This help
#
# After a successful build the plugin still needs to be loaded, e.g. in
# my.cnf under [mysqld]:
#   plugin-load-add=ha_fastmem
# then restart MariaDB.

set -euo pipefail

# ---------------------------------------------------------------------------
# defaults & argument parsing
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="12.3.3"
BRANCH=""
DETECT=0
SOURCE_DIR=""
BUILD_DIR=""
JOBS=""
PLUGIN_DIR=""
EXTRA_CMAKE=""
DRY_RUN=0

usage() { sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --detect)          DETECT=1; shift ;;
    --mariadb-version) VERSION="${2:?--mariadb-version needs a value}"; shift 2 ;;
    --branch)          BRANCH="${2:?--branch needs a value}"; shift 2 ;;
    --source-dir)      SOURCE_DIR="${2:?--source-dir needs a value}"; shift 2 ;;
    --build-dir)       BUILD_DIR="${2:?--build-dir needs a value}"; shift 2 ;;
    --jobs)            JOBS="${2:?--jobs needs a value}"; shift 2 ;;
    --plugin-dir)      PLUGIN_DIR="${2:?--plugin-dir needs a value}"; shift 2 ;;
    --extra-cmake-args) EXTRA_CMAKE="${2:?--extra-cmake-args needs a value}"; shift 2 ;;
    --dry-run)         DRY_RUN=1; shift ;;
    -h|--help)         usage ;;
    *) echo "unknown option: $1" >&2; usage ;;
  esac
done

# ---------------------------------------------------------------------------
# --detect: resolve the local server's exact version
# ---------------------------------------------------------------------------

if [[ "$DETECT" == "1" ]]; then
  [[ -n "$BRANCH" ]] && { echo "ERROR: --detect conflicts with --branch." >&2; exit 1; }
  [[ -n "$SOURCE_DIR" ]] && { echo "ERROR: --detect conflicts with --source-dir." >&2; exit 1; }
  DETECTED=""
  # Look on PATH first, then probe well-known install locations (BaoTa
  # panel, package installs, etc.) where mariadbd is not on PATH.
  declare -a CANDIDATES
  for c in mariadbd mysqld; do command -v "$c" >/dev/null 2>&1 && CANDIDATES+=("$(command -v "$c")"); done
  for p in /usr/sbin/mariadbd /usr/sbin/mysqld /usr/local/mariadb/bin/mariadbd \
           /usr/local/mysql/bin/mariadbd /usr/local/mysql/bin/mysqld \
           /www/server/mysql/bin/mariadbd /www/server/mysql/bin/mysqld \
           /opt/lampp/sbin/mariadbd /opt/lampp/sbin/mysqld; do
    [[ -x "$p" ]] && CANDIDATES+=("$p")
  done
  for bin in "${CANDIDATES[@]}"; do
    DETECTED="$("$bin" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -n1)"
    if [[ -n "$DETECTED" ]]; then
      echo ">> --detect: $bin -> MariaDB $DETECTED"
      break
    fi
  done
  if [[ -z "$DETECTED" ]]; then
    echo "ERROR: --detect could not find a server version." >&2
    echo "       Install MariaDB first, or pass --mariadb-version <v> explicitly." >&2
    echo "       If your server binary is somewhere unusual, set PATH so that" >&2
    echo "       'mariadbd --version' works, then retry." >&2
    exit 1
  fi
  VERSION="$DETECTED"
fi

# ---------------------------------------------------------------------------
# decide which refs to fetch (shared by dry-run and the real run)
# ---------------------------------------------------------------------------

if [[ -n "$BRANCH" ]]; then
  REFS=("$BRANCH")
elif [[ -z "$SOURCE_DIR" ]]; then
  # MariaDB tags releases both as "maria-<v>" (12.x era) and as
  # "mariadb-<v>" (11.x era); try both, then the branch.
  REFS=("maria-${VERSION}" "mariadb-${VERSION}" "${VERSION}")
  # "12.3.3" -> also try the maintenance branch "12.3": MariaDB releases
  # are cut from X.Y branches, so patch versions may not have their own
  # tag/branch on GitHub.
  if [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    REFS+=("${VERSION%.*}")
  fi
fi

# ---------------------------------------------------------------------------
# dry-run: print the plan and stop (before any toolchain checks)
# ---------------------------------------------------------------------------

if [[ "$DRY_RUN" == "1" ]]; then
  if [[ -n "$SOURCE_DIR" ]]; then
    echo ">> plan: reuse source tree $SOURCE_DIR (storage/fastmem will be created inside it)"
  else
    echo ">> plan: git clone --depth 1 --branch <ref> https://github.com/MariaDB/server.git"
    echo "         refs to try (in order): ${REFS[*]}"
  fi
  NCPU=4
  [[ "$(uname)" == "Darwin" ]] && NCPU="$(sysctl -n hw.ncpu)"
  command -v nproc >/dev/null 2>&1 && NCPU="$(nproc)"
  [[ -n "$JOBS" ]] && NCPU="$JOBS"
  echo ">> plan: copy $(basename "$SCRIPT_DIR") -> <src>/storage/fastmem"
  echo ">> plan: cmake -S <src> -B <build> (-DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_WSREP=OFF"
  echo "                     -DWITH_SSL=system -DPLUGIN_ROCKSDB=NO -DPLUGIN_MROONGA=NO)"
  [[ -n "$EXTRA_CMAKE" ]] && echo "         extra args: $EXTRA_CMAKE"
  echo ">> plan: cmake --build <build> --target fastmem -- -j${NCPU}"
  echo ">> dry-run finished - nothing was modified."
  exit 0
fi

# ---------------------------------------------------------------------------
# toolchain checks
# ---------------------------------------------------------------------------

need() { command -v "$1" >/dev/null 2>&1 || { echo "ERROR: '$1' not found in PATH (required)." >&2; MISSING=1; }; }
MISSING=0
need git
need cmake
need make
need bison
need perl
if [[ "$(command -v cc gcc clang cl 2>/dev/null | head -n1)" == "" ]]; then
  echo "ERROR: no C/C++ compiler found (need cc/gcc/clang or MSVC cl)." >&2
  MISSING=1
fi
if [[ "$(command -v pkg-config 2>/dev/null)" == "" && "$MISSING" == "0" ]]; then
  echo "WARN: pkg-config not found; MariaDB build may need it (libssl etc)." >&2
  echo "      On Debian/Ubuntu: apt install pkg-config libssl-dev libpcre2-dev" >&2
  echo "      On Fedora:        dnf install pkgconf-pkg-config openssl-devel pcre2-devel" >&2
  echo "      On macOS:         brew install pkg-config openssl pcre2" >&2
fi
if [[ "$MISSING" == "1" ]]; then
  echo "Install missing tools first (see WARN above for the usual package names)." >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# locate / clone the MariaDB source tree
# ---------------------------------------------------------------------------

if [[ -n "$SOURCE_DIR" ]]; then
  SRC="$SOURCE_DIR"
  [[ -d "$SRC" ]] || { echo "ERROR: --source-dir '$SRC' does not exist." >&2; exit 1; }
  echo ">> reusing source tree: $SRC"
else
  SRC="mariadb-server-${VERSION}"
  if [[ -d "$SRC/.git" ]]; then
    if [[ -f "$SRC/CMakeLists.txt" ]]; then
      echo ">> directory '$SRC' already has a checkout - reusing it."
      echo "   (remove it or pass --source-dir to use another tree)"
    else
      echo ">> '$SRC' exists but is incomplete (no CMakeLists.txt) -"
      echo "   removing it and cloning again."
      rm -rf "$SRC"
    fi
  fi
  if [[ ! -d "$SRC/.git" ]]; then
    CLONED=0
    for ref in "${REFS[@]}"; do
      echo ">> git clone --depth 1 --branch $ref https://github.com/MariaDB/server.git"
      if git clone --depth 1 --branch "$ref" \
          https://github.com/MariaDB/server.git "$SRC"; then
        CLONED=1; break
      fi
      echo "   ref '$ref' not found, trying next..."
    done
    if [[ "$CLONED" != "1" ]]; then
      # GitHub may be unreachable (e.g. mainland China) or the refs missing.
      # Fall back to the official release source tarball from
      # archive.mariadb.org (it ships all submodules, incl. libmariadb,
      # so no git fetch is needed afterwards).
      TB="mariadb-${VERSION}.tar.gz"
      URL="https://archive.mariadb.org/mariadb-${VERSION}/source/${TB}"
      echo ">> git fetch failed; falling back to the official source tarball:"
      echo "   $URL"
      rm -rf "$SRC"; mkdir -p "$SRC"
      if command -v curl >/dev/null 2>&1; then
        curl -L --fail --retry 3 -o "$TB" "$URL"
      elif command -v wget >/dev/null 2>&1; then
        wget -O "$TB" "$URL"
      else
        echo "ERROR: neither curl nor wget available to fetch the tarball." >&2
        echo "       Install curl/wget or provide the source tree via --source-dir." >&2
        exit 1
      fi
      tar -xzf "$TB" -C "$SRC" --strip-components=1 || { echo "ERROR: tarball extract failed." >&2; exit 1; }
      rm -f "$TB"
      echo ">> extracted $URL -> $SRC"
      echo "   (using the release tarball - exact version source, submodules bundled)"
    fi
  fi
fi

# ---------------------------------------------------------------------------
# drop storage/fastmem/ into the tree
# ---------------------------------------------------------------------------

DST="$SRC/storage/fastmem"
echo ">> copying engine sources: $(basename "$SCRIPT_DIR") -> $DST"
rm -rf "$DST"
mkdir -p "$DST"
# Copy the repository contents, excluding the local git dir, the freshly
# cloned server tree and any previous build dir (they must not be
# re-packaged into the engine directory).
(cd "$SCRIPT_DIR" && tar --exclude=.git --exclude='mariadb-server*' \
    --exclude='build_fastmem*' -cf - .) | tar -C "$DST" -xf -
[[ -f "$DST/CMakeLists.txt" ]] || { echo "ERROR: CMakeLists.txt missing after copy." >&2; exit 1; }

# ---------------------------------------------------------------------------
# MariaDB sources use git submodules.  Configure would fetch ALL of them
# (rocksdb, columnstore, wsrep-lib, wolfssl, ...) over the network - and
# aborts if any fails.  We fetch only the required one (MariaDB
# Connector/C) ourselves and tell cmake to leave the rest alone.
# ---------------------------------------------------------------------------

if [[ -d "$SRC/.git" ]]; then
  if [[ ! -f "$SRC/libmariadb/CMakeLists.txt" ]]; then
    echo ">> fetching required submodule: libmariadb (MariaDB Connector/C)"
    git -C "$SRC" submodule update --init --depth 1 libmariadb
  fi
  git -C "$SRC" config cmake.update-submodules no
  echo ">> submodules: libmariadb ready, other submodules disabled (cmake.update-submodules=no)"
fi

# ---------------------------------------------------------------------------
# cmake configure + build
# ---------------------------------------------------------------------------

BUILD_DIR="${BUILD_DIR:-$SRC/build_fastmem}"
if [[ -z "$JOBS" ]]; then
  if [[ "$(uname)" == "Darwin" ]]; then
    JOBS="$(sysctl -n hw.ncpu)"
  elif command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  else
    JOBS=4
  fi
fi

CMAKE="$({ command -v cmake3 || command -v cmake; } 2>/dev/null | head -n1)"
echo ">> cmake: $CMAKE"
echo ">> configure: cmake -S $SRC -B $BUILD_DIR [RelWithDebInfo]"

# Platform-neutral configure options.  WITH_SSL=system keeps the build
# independent from bundled downloads; if the system has no OpenSSL dev
# files, swap to --extra-cmake-args "-DWITH_SSL=bundled" (needs network).
CMAKE_ARGS=(
  -S "$SRC"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DWITH_WSREP=OFF
  -DWITH_SSL=system
  -DPLUGIN_ROCKSDB=NO
  -DPLUGIN_MROONGA=NO
  -DPLUGIN_OQGRAPH=NO
  -DPLUGIN_SPIDER=NO
  -DPLUGIN_TOKUDB=NO
  -DPLUGIN_CONNECT=NO
  -DPLUGIN_FEDERATED=NO
  -DPLUGIN_COLUMNSTORE=NO
  -DPLUGIN_DUCKDB=NO
  -DPLUGIN_S3=NO
  -DWITH_UNIT_TESTS=OFF
)
if [[ -n "$EXTRA_CMAKE" ]]; then
  # shellcheck disable=SC2206
  CMAKE_ARGS+=($EXTRA_CMAKE)
fi

echo ">> running cmake (first configure is the slow step)..."
"$CMAKE" "${CMAKE_ARGS[@]}"

echo ">> building target: fastmem (jobs=$JOBS)"
"$CMAKE" --build "$BUILD_DIR" --config RelWithDebInfo --target fastmem -- -j"$JOBS"

# ---------------------------------------------------------------------------
# locate the plugin and install it
# ---------------------------------------------------------------------------

case "$(uname)" in
  Darwin) PLUGIN_CANDIDATES=("$BUILD_DIR/storage/fastmem/ha_fastmem.dylib" \
                             "$BUILD_DIR/storage/fastmem/RelWithDebInfo/ha_fastmem.dylib") ;;
  MINGW*|MSYS*|CYGWIN*) PLUGIN_CANDIDATES=("$BUILD_DIR/storage/fastmem/ha_fastmem.dll" \
                                           "$BUILD_DIR/storage/fastmem/RelWithDebInfo/ha_fastmem.dll") ;;
  *) PLUGIN_CANDIDATES=("$BUILD_DIR/storage/fastmem/ha_fastmem.so" \
                        "$BUILD_DIR/storage/fastmem/RelWithDebInfo/ha_fastmem.so") ;;
esac

PLUGIN_SRC=""
for c in "${PLUGIN_CANDIDATES[@]}"; do
  [[ -f "$c" ]] && { PLUGIN_SRC="$c"; break; }
done
if [[ -z "$PLUGIN_SRC" ]]; then
  echo "WARN: plugin binary not found at the usual locations;" >&2
  echo "      look for ha_fastmem.so/.dylib/.dll under $BUILD_DIR/storage/fastmem" >&2
  echo "      and copy it into your MariaDB plugin dir manually." >&2
  exit 1
fi
echo ">> built plugin: $PLUGIN_SRC"

if [[ -z "$PLUGIN_DIR" ]]; then
  # Try the server itself; fall back to mysql_config / mariadb_config.
  for cand in mariadbd mysqld mariadb_config mysql_config; do
    if command -v "$cand" >/dev/null 2>&1; then
      if [[ "$cand" == mariadbd || "$cand" == mysqld ]]; then
        PDIR="$( "$cand" --no-defaults --verbose --help 2>/dev/null | \
                 awk '/plugin-dir/{print $2; exit}' )"
      else
        PDIR="$("$cand" --plugindir 2>/dev/null)"
      fi
      [[ -n "$PDIR" ]] && { PLUGIN_DIR="$PDIR"; break; }
    fi
  done
fi

if [[ -n "$PLUGIN_DIR" ]]; then
  mkdir -p "$PLUGIN_DIR"
  cp -v "$PLUGIN_SRC" "$PLUGIN_DIR/"
  echo ">> installed: $PLUGIN_DIR/$(basename "$PLUGIN_SRC")"
else
  echo ">> could not auto-detect the plugin directory."
  echo "   Copy it manually, e.g.:"
  echo "     sudo cp $PLUGIN_SRC /usr/lib/mysql/plugin/"
  echo "   then add to my.cnf [mysqld]:  plugin-load-add=ha_fastmem"
fi

cat <<'EOF'

-------------------------------------------------------------------------
FASTMEM build finished.

Next steps:
  1) Enable the plugin in my.cnf under [mysqld]:
       plugin-load-add=ha_fastmem
     (on Windows: put the same line in my.ini)
     NOTE: since v1.1 FASTMEM declares GAMMA maturity and passes the
     default maturity gate of MariaDB >= 11.x servers. Only an old
     1.0 (BETA) build could be refused ("Loading of beta plugin ...
     prohibited") - add plugin-maturity=beta for that, or upgrade.
     Either way never INSTALL PLUGIN a plugin already loaded via
     plugin-load-add (double registration can crash the server).
  2) Restart MariaDB, then verify:
       SHOW ENGINES;            -- FASTMEM should be listed
  3) Usage is identical to MEMORY (heap) tables:
       CREATE TABLE t (
         id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
         v  BIGINT NOT NULL
       ) ENGINE=FASTMEM;
-------------------------------------------------------------------------
EOF