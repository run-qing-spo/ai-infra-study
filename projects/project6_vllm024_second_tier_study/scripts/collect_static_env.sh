#!/usr/bin/env bash
#
# Collect a read-only snapshot of a Linux host before running second-tier
# storage experiments.
#
# This script:
#   - writes nothing itself (all output goes to stdout);
#   - installs nothing and changes no kernel/sysfs settings;
#   - does not run a storage workload;
#   - distinguishes an absent virtual file from an empty one.
#
# To persist the output, the caller must explicitly redirect it or use tee.

set -u
set -o pipefail

export LC_ALL=C

readonly COLLECTOR_SCHEMA_VERSION="1"

section() {
    printf '\n\n##### %s #####\n' "$1"
}

item() {
    printf '\n=== %s ===\n' "$1"
}

run_optional() {
    local label=$1
    shift

    item "${label}"
    if ! command -v "$1" >/dev/null 2>&1; then
        printf '<command-unavailable: %s>\n' "$1"
        return 0
    fi

    "$@" 2>&1
    local status=$?
    if (( status != 0 )); then
        printf '<command-exit-status: %d>\n' "${status}"
    fi
}

read_virtual_file() {
    local label=$1
    local path=$2
    local value
    local status

    item "${label} [${path}]"
    if [[ ! -e "${path}" ]]; then
        printf '<absent>\n'
        return 0
    fi

    value=$(cat -- "${path}" 2>&1)
    status=$?
    if (( status != 0 )); then
        printf '<read-error: %d>\n%s\n' "${status}" "${value}"
    elif [[ -n "${value}" ]]; then
        printf '%s\n' "${value}"
    else
        printf '<empty>\n'
    fi
}

collect_cgroup_v2() {
    local relative_path
    local current_dir
    local field

    relative_path=$(awk -F: '$1 == "0" {print $3}' /proc/self/cgroup)
    if [[ -z "${relative_path}" ]]; then
        printf '<no-unified-cgroup-entry>\n'
        return 0
    fi

    if [[ "${relative_path}" == "/" ]]; then
        current_dir="/sys/fs/cgroup"
    else
        current_dir="/sys/fs/cgroup${relative_path}"
    fi

    while true; do
        printf '\n--- cgroup: %s ---\n' "${current_dir}"
        for field in \
            cgroup.controllers \
            cgroup.subtree_control \
            cpu.max \
            cpu.weight \
            cpu.stat \
            memory.max \
            memory.high \
            memory.current \
            cpuset.cpus \
            cpuset.cpus.effective \
            cpuset.mems \
            cpuset.mems.effective \
            io.max \
            io.weight \
            pids.max; do
            read_virtual_file "${field}" "${current_dir}/${field}"
        done

        if [[ "${current_dir}" == "/sys/fs/cgroup" ]]; then
            break
        fi
        current_dir=${current_dir%/*}
        if [[ -z "${current_dir}" ]]; then
            printf '<invalid-cgroup-parent>\n'
            break
        fi
    done
}

collect_block_queues() {
    local device
    local device_type
    local queue_dir
    local field

    if ! command -v lsblk >/dev/null 2>&1; then
        printf '<command-unavailable: lsblk>\n'
        return 0
    fi

    while read -r device device_type; do
        case "${device_type}" in
            disk|raid*|mpath)
                ;;
            *)
                continue
                ;;
        esac

        queue_dir="/sys/class/block/${device}/queue"
        printf '\n--- block device: %s (type=%s) ---\n' \
            "${device}" "${device_type}"
        read_virtual_file "major:minor" "/sys/class/block/${device}/dev"
        read_virtual_file "device model" "/sys/class/block/${device}/device/model"
        read_virtual_file "device vendor" "/sys/class/block/${device}/device/vendor"

        for field in \
            logical_block_size \
            physical_block_size \
            minimum_io_size \
            optimal_io_size \
            rotational \
            scheduler \
            nr_requests \
            max_sectors_kb \
            max_hw_sectors_kb \
            max_segments \
            max_segment_size \
            read_ahead_kb \
            nomerges \
            rq_affinity \
            write_cache \
            fua \
            wbt_lat_usec \
            discard_granularity \
            discard_max_bytes \
            discard_zeroes_data \
            zoned \
            dax; do
            read_virtual_file "${field}" "${queue_dir}/${field}"
        done
    done < <(lsblk -dn -o KNAME,TYPE 2>/dev/null)
}

section "collector"
printf 'collector_schema_version=%s\n' "${COLLECTOR_SCHEMA_VERSION}"
printf 'timestamp_utc=%s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
printf 'effective_uid=%s\n' "$(id -u)"
printf 'effective_user=%s\n' "$(id -un 2>/dev/null || printf '<unknown>')"

section "operating system and virtualization"
read_virtual_file "os-release" "/etc/os-release"
run_optional "kernel" uname -srmo
read_virtual_file "kernel command line" "/proc/cmdline"
run_optional "virtualization" systemd-detect-virt
read_virtual_file "DMI system vendor" "/sys/class/dmi/id/sys_vendor"
read_virtual_file "DMI product name" "/sys/class/dmi/id/product_name"
read_virtual_file "DMI product version" "/sys/class/dmi/id/product_version"

section "CPU, NUMA, memory, and process limits"
run_optional "lscpu" lscpu
run_optional "CPU topology" lscpu -e=CPU,NODE,SOCKET,CORE,ONLINE,MAXMHZ,MINMHZ
run_optional "nproc --all" nproc --all
run_optional "memory (bytes)" free -b
run_optional "memory (human-readable)" free -h
run_optional "NUMA topology" numactl --hardware
run_optional "process resource limits" prlimit --pid "$$"
read_virtual_file "process CPU/memory affinity" "/proc/self/status"
read_virtual_file "SMT active" "/sys/devices/system/cpu/smt/active"
read_virtual_file \
    "CPU 0 frequency governor" \
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
read_virtual_file \
    "Intel turbo disable flag" \
    "/sys/devices/system/cpu/intel_pstate/no_turbo"
read_virtual_file \
    "generic CPU boost flag" \
    "/sys/devices/system/cpu/cpufreq/boost"

section "cgroup hierarchy"
run_optional \
    "cgroup mounts" \
    findmnt -t cgroup,cgroup2 -o TARGET,SOURCE,FSTYPE,OPTIONS
read_virtual_file "current process cgroup" "/proc/self/cgroup"
if [[ -e /sys/fs/cgroup/cgroup.controllers ]]; then
    collect_cgroup_v2
else
    printf '<cgroup-v2-unavailable>\n'
fi

section "GPU and CUDA"
run_optional "nvidia-smi" nvidia-smi
run_optional \
    "GPU identity" \
    nvidia-smi \
    --query-gpu=index,name,pci.bus_id,driver_version,memory.total,pstate \
    --format=csv,noheader
run_optional "GPU topology" nvidia-smi topo -m
read_virtual_file "NVIDIA kernel module" "/proc/driver/nvidia/version"
run_optional "CUDA compiler" nvcc --version

section "filesystem and block topology"
run_optional \
    "block devices" \
    lsblk -b -e 7 \
    -o NAME,KNAME,PATH,MAJ:MIN,TYPE,SIZE,RO,RM,ROTA,FSTYPE,FSVER,MOUNTPOINTS,MODEL,VENDOR,TRAN
run_optional \
    "root mount" \
    findmnt -T / -o TARGET,SOURCE,FSTYPE,OPTIONS
run_optional "root filesystem capacity (bytes)" df -B1 -T /
run_optional "root filesystem capacity (human-readable)" df -hT /
run_optional "PCI devices and drivers" lspci -nnk

root_fstype=$(findmnt -n -o FSTYPE -T / 2>/dev/null || true)
if [[ "${root_fstype}" == "xfs" ]]; then
    run_optional "XFS geometry" xfs_info /
fi

collect_block_queues

section "io_uring and asynchronous I/O support"
read_virtual_file \
    "io_uring disabled policy" \
    "/proc/sys/kernel/io_uring_disabled"
read_virtual_file \
    "io_uring group policy" \
    "/proc/sys/kernel/io_uring_group"
read_virtual_file "AIO maximum requests" "/proc/sys/fs/aio-max-nr"
read_virtual_file "AIO current requests" "/proc/sys/fs/aio-nr"

kernel_config="/boot/config-$(uname -r)"
item "selected kernel configuration [${kernel_config}]"
if [[ -r "${kernel_config}" ]]; then
    grep -E \
        '^(CONFIG_IO_URING|CONFIG_AIO|CONFIG_BLOCK|CONFIG_XFS_FS|CONFIG_EXT4_FS|CONFIG_BTRFS_FS|CONFIG_NUMA|CONFIG_PREEMPT|CONFIG_HZ)=' \
        "${kernel_config}" || printf '<selected-options-absent>\n'
elif [[ -r /proc/config.gz ]] && command -v zgrep >/dev/null 2>&1; then
    zgrep -E \
        '^(CONFIG_IO_URING|CONFIG_AIO|CONFIG_BLOCK|CONFIG_XFS_FS|CONFIG_EXT4_FS|CONFIG_BTRFS_FS|CONFIG_NUMA|CONFIG_PREEMPT|CONFIG_HZ)=' \
        /proc/config.gz || printf '<selected-options-absent>\n'
else
    printf '<kernel-config-unavailable>\n'
fi

section "memory and writeback policy"
for sysctl_file in \
    /proc/sys/vm/dirty_background_bytes \
    /proc/sys/vm/dirty_background_ratio \
    /proc/sys/vm/dirty_bytes \
    /proc/sys/vm/dirty_ratio \
    /proc/sys/vm/dirty_expire_centisecs \
    /proc/sys/vm/dirty_writeback_centisecs \
    /proc/sys/vm/swappiness \
    /proc/sys/vm/nr_hugepages; do
    read_virtual_file "$(basename "${sysctl_file}")" "${sysctl_file}"
done
read_virtual_file \
    "transparent hugepage enabled modes" \
    "/sys/kernel/mm/transparent_hugepage/enabled"
read_virtual_file \
    "transparent hugepage defrag modes" \
    "/sys/kernel/mm/transparent_hugepage/defrag"

section "toolchain and experiment utilities"
for tool in \
    bash \
    python3 \
    conda \
    gcc \
    g++ \
    clang \
    cmake \
    ninja \
    pkg-config \
    fio \
    iostat \
    mpstat \
    numactl \
    nvcc; do
    item "${tool}"
    if command -v "${tool}" >/dev/null 2>&1; then
        printf 'path=%s\n' "$(command -v "${tool}")"
        case "${tool}" in
            bash)
                bash --version 2>&1 | sed -n '1p'
                ;;
            python3)
                python3 --version 2>&1
                ;;
            conda)
                conda --version 2>&1
                ;;
            gcc|g++|clang|cmake)
                "${tool}" --version 2>&1 | sed -n '1p'
                ;;
            ninja)
                ninja --version 2>&1
                ;;
            pkg-config)
                pkg-config --version 2>&1
                ;;
            fio)
                fio --version 2>&1
                ;;
            iostat|mpstat)
                "${tool}" -V 2>&1 | sed -n '1,2p'
                ;;
            numactl)
                numactl --show 2>&1
                ;;
            nvcc)
                nvcc --version 2>&1
                ;;
        esac
    else
        printf '<command-unavailable>\n'
    fi
done

item "selected installed packages"
if command -v dpkg-query >/dev/null 2>&1; then
    dpkg-query -W -f='${binary:Package}\t${Version}\n' 2>/dev/null |
        awk '$1 ~ /^(fio|liburing|libaio|cuda|nvidia|libnvidia|gcc|g\+\+|cmake|ninja|pkg-config)(:|$|-)/'
else
    printf '<command-unavailable: dpkg-query>\n'
fi

section "instantaneous no-load context"
run_optional "uptime and load average" uptime
read_virtual_file "load average" "/proc/loadavg"
read_virtual_file "memory snapshot" "/proc/meminfo"
read_virtual_file "pressure: CPU" "/proc/pressure/cpu"
read_virtual_file "pressure: memory" "/proc/pressure/memory"
read_virtual_file "pressure: I/O" "/proc/pressure/io"

section "collector complete"
printf 'collector_schema_version=%s\n' "${COLLECTOR_SCHEMA_VERSION}"
printf 'timestamp_utc=%s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
