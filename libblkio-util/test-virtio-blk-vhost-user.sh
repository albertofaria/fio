#!/usr/bin/env bash
# ---------------------------------------------------------------------------- #

set -o errexit -o pipefail -o nounset

script_dir="$( dirname "$0" | xargs readlink -e -- )"

if (( $# != 1 )); then
	>&2 echo "Usage: $0 <libblkio_dir>"
	exit 2
fi

libblkio_dir="$1"

# create temporary directory

temp_dir="$( mktemp -d )"
trap 'rm -fr "${temp_dir}"' EXIT

# create workload file

cat > "${temp_dir}/workload.fio" <<EOF
[global]
numjobs=1
offset_increment=4M
size=4M
runtime=10s
time_based
rw=randwrite
blocksize=512
iodepth=16
direct=1
exitall=1
exitall_on_error
exit_what=all
allow_file_create=0
verify=crc32c
verify_backlog=1024
verify_fatal=1
verify_state_save=0
group_reporting
thread
ioengine=libblkio
libblkio_driver=virtio-blk-vhost-user
libblkio_path=/home/afaria/downloads/vhost-user-blk.sock
libblkio_vectored

[job1]
libblkio_wait_mode=block

[job2]
libblkio_wait_mode=eventfd

[job3]
libblkio_wait_mode=loop

[job4]
libblkio_wait_mode=block
hipri

[job5]
libblkio_wait_mode=loop
hipri
EOF

# run fio

LD_LIBRARY_PATH="${libblkio_dir}/build/src" \
	"${script_dir}/../fio" "${temp_dir}/workload.fio"

# LD_LIBRARY_PATH="${libblkio_dir}/build/src" gdb \
# 	-ex='set confirm on' -ex=run -ex=quit \
# 	--args "${script_dir}/../fio" "${temp_dir}/workload.fio"

# ---------------------------------------------------------------------------- #
