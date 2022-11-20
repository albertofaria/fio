#!/usr/bin/env bash
# ---------------------------------------------------------------------------- #

set -o errexit -o pipefail -o nounset

if (( $# != 3 )); then
	>&2 echo "Usage: $0 <libblkio_dir> <flamegraph_dir> <target_file>"
	exit 2
fi

libblkio_dir="$1"
flamegraph_dir="$2"
target_file="$3"

# create temporary directory

temp_dir="$( mktemp -d )"
trap 'rm -fr "$temp_dir"' EXIT

# create workload file

cat > "$temp_dir/workload.fio" <<EOF
[global]
numjobs=1
thread
runtime=15s
time_based
rw=randread
blocksize=512
direct=1
exitall=1
exitall_on_error
exit_what=all
allow_file_create=0
norandommap
random_generator=lfsr
randrepeat=0
group_reporting

[job]
EOF

# run fio

function __fio() {
	LD_LIBRARY_PATH="$libblkio_dir/build/src" \
		perf record --call-graph=dwarf --output="$temp_dir/perf.data" \
		./fio \
		"$temp_dir/workload.fio" \
		--output-format=terse \
		--lat_percentiles=1 \
		--slat_percentiles=0 \
		--clat_percentiles=0 \
		--readonly \
		"$@"
}

function __eval() {
	__fio "${@:2}"
	perf script --input="$temp_dir/perf.data" |
		"$flamegraph_dir/stackcollapse-perf.pl" |
		"$flamegraph_dir/flamegraph.pl" > "$1.svg"
}

for qd in 1 4 16 64; do

	# --fixedbufs=1

	__eval "qd$qd-io_uring" --ioengine=io_uring \
		--filename="$target_file" --registerfiles=1 \
		--iodepth="$qd"

	__eval "qd$qd-io_uring-poll" --ioengine=io_uring \
		--filename="$target_file" --registerfiles=1 \
		--iodepth="$qd" \
		--hipri

	__eval "qd$qd-libblkio" --ioengine=libblkio \
		--libblkio_driver=io_uring --libblkio_path="$target_file" \
		--iodepth="$qd" --libblkio_num_descs="$qd"

	__eval "qd$qd-libblkio-poll" --ioengine=libblkio \
		--libblkio_driver=io_uring --libblkio_path="$target_file" \
		--iodepth="$qd" --libblkio_num_descs="$qd" \
		--libblkio_wait_mode=loop

done

# ---------------------------------------------------------------------------- #
