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
numjobs=2
offset_increment=4M
size=4M
runtime=15s
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
ioengine=libblkio
libblkio_driver=io_uring
libblkio_path=/dev/nvme0n1p5

[job1]
offset=0M
libblkio_wait_mode=block
mem=malloc

[job2]
offset=8M
libblkio_wait_mode=eventfd
libblkio_vectored=1
libblkio_write_zeroes_on_trim=1
libblkio_force_enable_completion_eventfd=1

[job3]
offset=16M
libblkio_wait_mode=loop
mem=malloc

[job4]
offset=24M
libblkio_wait_mode=block
hipri
libblkio_vectored=1

[job5]
offset=32M
libblkio_wait_mode=loop
hipri
mem=malloc

[job6]
offset=40M
libblkio_wait_mode=block
thread
libblkio_vectored=1
libblkio_write_zeroes_on_trim=1

[job7]
offset=48M
libblkio_wait_mode=eventfd
thread
mem=malloc

[job8]
offset=56M
libblkio_wait_mode=loop
thread
libblkio_vectored=1
libblkio_force_enable_completion_eventfd=1

[job9]
offset=64M
libblkio_wait_mode=block
hipri
thread
mem=malloc

[job10]
offset=72M
libblkio_wait_mode=loop
hipri
thread
libblkio_vectored=1
libblkio_write_zeroes_on_trim=1

# ---

# [job1]
# blkio_path=/dev/ram0
# mem=malloc

# [job2]
# blkio_path=/dev/ram1
# blkio_wait_mode=block

# [job3]
# blkio_path=/dev/ram2
# blkio_wait_mode=eventfd

# [job4]
# blkio_path=/dev/ram3
# thread
# mem=malloc
# blkio_wait_mode=loop

# [job5]
# blkio_path=/dev/ram0
# mem=malloc
# hipri

# [job6]
# blkio_path=/dev/ram1
# blkio_wait_mode=block
# hipri

# [job7]
# blkio_path=/dev/ram3
# thread
# mem=malloc
# blkio_wait_mode=loop
# hipri
EOF

# run fio

LD_LIBRARY_PATH="${libblkio_dir}/build/src" \
	"${script_dir}/../fio" "${temp_dir}/workload.fio"

# LD_LIBRARY_PATH="${libblkio_dir}/build/src" gdb \
# 	-ex='set confirm on' -ex=run -ex=quit \
# 	--args "${script_dir}/../fio" "${temp_dir}/workload.fio"

# ---------------------------------------------------------------------------- #
