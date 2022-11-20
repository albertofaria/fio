#!/usr/bin/env bash
# ---------------------------------------------------------------------------- #

set -o errexit -o pipefail -o nounset

if (( $# != 1 )); then
	>&2 echo "Usage: $0 <libblkio_dir>"
	exit 2
fi

script_dir="$( dirname "$0" | xargs readlink -e -- )"

modprobe -r nvme
modprobe nvme poll_queues="$( nproc )"  # will use classic polling by default

"${script_dir}/eval.sh" "$1" /dev/nvme0n1 /dev/ng0n1

# ---------------------------------------------------------------------------- #
