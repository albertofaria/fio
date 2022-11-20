#!/usr/bin/env bash
# ---------------------------------------------------------------------------- #

set -o errexit -o pipefail -o nounset

if (( $# != 1 )); then
	>&2 echo "Usage: $0 <libblkio_dir>"
	exit 2
fi

libblkio_dir="$1"
script_dir="$( dirname "$0" | xargs readlink -e -- )"

set -o xtrace

# build libblkio

cd "${libblkio_dir}"

meson setup build --debug --optimization=3
meson compile -j "$( nproc )" -C build libblkio.so

# build fio

cd "${script_dir}/.."

if [[ ! -e config.log ]]; then
	PKG_CONFIG_PATH="${libblkio_dir}/build/meson-uninstalled" \
		LDFLAGS="-L${libblkio_dir}/build/src" \
		./configure --extra-cflags="-I$1/include"
fi

rm -f fio
make -j "$( nproc )"

# ---------------------------------------------------------------------------- #
