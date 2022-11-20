#!/usr/bin/env bash
# ---------------------------------------------------------------------------- #

set -o errexit -o pipefail -o nounset

# ---------------------------------------------------------------------------- #

runs=10

blocksize=512
# queue_depths=( 1 4 16 64 256 )
queue_depths=( 1 64 )

# ramp_time=30s
# runtime=30s
ramp_time=10s
runtime=20s

# shellcheck disable=SC2016
configs=(
	# io_uring, POSIX, no polling

	'fio-io_uring
		--ioengine=io_uring
		--direct=1
		--filename="$block_dev"
		--nonvectored=1
		--registerfiles=1
		'
	'blkio-io_uring
		--ioengine=blkio
		--direct=1
		--libblkio_driver=io_uring
		--libblkio_path="$block_dev"
		--libblkio_num_entries="$qd"
		'

	# io_uring, POSIX, completion polling

	'fio-io_uring-cp
		--ioengine=io_uring
		--direct=1
		--filename="$block_dev"
		--nonvectored=1
		--registerfiles=1
		--hipri=1
		'
	'blkio-io_uring-cp
		--ioengine=blkio
		--direct=1
		--libblkio_driver=io_uring
		--libblkio_path="$block_dev"
		--libblkio_num_entries="$qd"
		--hipri=1
		'

	# io_uring, POSIX, other optimizations

	'fio-io_uring-cp-sp
		--ioengine=io_uring
		--direct=1
		--filename="$block_dev"
		--nonvectored=1
		--registerfiles=1
		--hipri=1
		--sqthread_poll=1
		'
	'fio-io_uring-cp-sp-fb
		--ioengine=io_uring
		--direct=1
		--filename="$block_dev"
		--nonvectored=1
		--registerfiles=1
		--hipri=1
		--sqthread_poll=1
		--fixedbufs=1
		'

	# 'xnvme-io_uring
	# 	--ioengine=xnvme
	# 	--xnvme_async=io_uring
	# 	--direct=1
	# 	--filename="$block_dev"
	# 	'
	# 'xnvme-io_uring-cp
	# 	--ioengine=xnvme
	# 	--xnvme_async=io_uring
	# 	--direct=1
	# 	--filename="$block_dev"
	# 	--hipri=1
	# 	'
	# 'xnvme-io_uring-cp-sp
	# 	--ioengine=xnvme
	# 	--xnvme_async=io_uring
	# 	--direct=1
	# 	--filename="$block_dev"
	# 	--hipri=1
	# 	--sqthread_poll=1
	# 	'

	# io_uring NVMe passthrough

	# 'fio-nvme-io_uring
	# 	--ioengine=io_uring_cmd
	# 	--cmd_type=nvme
	# 	--filename="$char_dev"
	# 	--nonvectored=1
	# 	--registerfiles=1
	# 	'
	# 'fio-nvme-io_uring-cp
	# 	--ioengine=io_uring_cmd
	# 	--cmd_type=nvme
	# 	--filename="$char_dev"
	# 	--nonvectored=1
	# 	--registerfiles=1
	# 	--hipri=1
	# 	'
	# 'fio-nvme-io_uring-cp-sp
	# 	--ioengine=io_uring_cmd
	# 	--cmd_type=nvme
	# 	--filename="$char_dev"
	# 	--nonvectored=1
	# 	--registerfiles=1
	# 	--hipri=1
	# 	--sqthread_poll=1
	# 	'
	# 'fio-nvme-io_uring-cp-sp-fb
	# 	--ioengine=io_uring_cmd
	# 	--cmd_type=nvme
	# 	--filename="$char_dev"
	# 	--nonvectored=1
	# 	--registerfiles=1
	# 	--hipri=1
	# 	--sqthread_poll=1
	# 	--fixedbufs=1
	# 	'

	# 'blkio-nvme-io_uring
	# 	--ioengine=blkio
	# 	--libblkio_driver=nvme-io_uring
	# 	--libblkio_path="$char_dev"
	# 	--libblkio_num_entries="$qd"
	# 	'
	# 'blkio-nvme-io_uring-cp
	# 	--ioengine=blkio
	# 	--libblkio_driver=nvme-io_uring
	# 	--libblkio_path="$char_dev"
	# 	--libblkio_num_entries="$qd"
	# 	--hipri=1
	# 	'
)

# ---------------------------------------------------------------------------- #

script_dir="$( dirname "$0" | xargs readlink -e -- )"

if (( $# != 3 )); then
	>&2 echo "Usage: $0 <libblkio_dir> <block_dev> <char_dev>"
	exit 2
fi

libblkio_dir="$1"
block_dev="$2"
char_dev="$3"

# create temporary directory

temp_dir="$( mktemp -d )"
trap 'rm -fr "${temp_dir}"' EXIT

# create workload file

cat > "${temp_dir}/workload.fio" <<EOF
[global]
numjobs=1
thread
runtime=${runtime}
ramp_time=${ramp_time}
time_based
rw=randread
blocksize=${blocksize}
exitall=1
exitall_on_error
exit_what=all
allow_file_create=0
norandommap
random_generator=lfsr
randrepeat=0
group_reporting
numa_cpu_nodes=0
numa_mem_policy=bind:0
# sqthread_poll_cpu=0  # ensure it's in NUMA node 0; must change if numjobs > 1

[job]
EOF

# run fio

function __print_header() {
	printf '%-27s' ""
	printf '%13s' "$@"
	printf '\n'
}

function __print_separator() {
	printf '%79s\n' "" | tr ' ' '-'
}

function __print_row() {
	printf '%-27s' "$1"
	printf '%13.3f' "${@:2}"
	printf '\n'
}

function __fio() {
	# LD_LIBRARY_PATH="$libblkio_dir/build/src" perf stat -e 'syscalls:sys_enter_io_uring_enter' -- ./fio \
	LD_LIBRARY_PATH="${libblkio_dir}/build/src" \
		numactl --membind 0 --cpunodebind 0 -- "${script_dir}/../fio" \
		"${temp_dir}/workload.fio" \
		--output-format=json \
		--lat_percentiles=1 \
		--slat_percentiles=0 \
		--clat_percentiles=0 \
		--readonly \
		"$@"
}

function __eval() {
	# shellcheck disable=SC2046
	__print_row "$1" $( __fio "${@:2}" | jq '.jobs[0].read | [
		.iops / 1000,
		.lat_ns.mean / 1000,
		.lat_ns.percentile."99.000000" / 1000,
		.lat_ns.percentile."99.900000" / 1000
		][]' )
}

# evaluate

declare -A results

test_total="$(( ${#configs[@]} * ${#queue_depths[@]} * runs ))"
test_i=0

for (( i = 0; i < runs; ++i )); do

	>&2 echo "Run $(( i + 1 ))/$runs..."

	mapfile -d "" -t shuffled_configs < <( shuf -ez "${configs[@]}" )

	for config in "${shuffled_configs[@]}"; do
		elems=( ${config} )
		for qd in "${queue_depths[@]}"; do

			k="qd$qd-${elems[0]}"
			if (( i == 0 )); then
				name="$k"
			else
				name=
			fi

			test_i="$(( test_i + 1 ))"
			>&2 printf "Test %d/%d (%s)...                     \n" \
				"$test_i" "$test_total" "$k"

			r="$( __eval "$name" --iodepth="$qd" $( eval echo ${elems[@]:1} ) )"
			if (( i == 0 )); then
				results[$k]="$r"$'\n'
			else
				results[$k]="${results[$k]}$r"$'\n'
			fi

		done
	done

	>&2 echo

done

# print results

__print_header kop/s mean-us/op 99p-us/op 99.9p-us/op

for config in "${configs[@]}"; do
	elems=( ${config} )
	__print_separator
	for qd in "${queue_depths[@]}"; do
		k="qd$qd-${elems[0]}"
		printf '%s' "${results[$k]}"
	done
done

__print_separator

# ---------------------------------------------------------------------------- #
