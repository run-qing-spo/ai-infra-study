#!/usr/bin/env bash
#
# Read-only, cloud-aware runtime evidence collector.
#
# Usage:
#   collect_cloud_runtime.sh EXPERIMENT_PATH [SERVICE_PID]
#
# The script writes only to stdout. It deliberately reports guest-visible facts
# without claiming that virtual devices describe the physical host.

set -u
set -o pipefail

export LC_ALL=C

readonly COLLECTOR_SCHEMA_VERSION="2"
readonly EXPERIMENT_PATH=${1:?"experiment path is required"}
readonly REQUESTED_SERVICE_PID=${2:-}

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

read_optional() {
    local label=$1
    local path=$2
    local value
    local status

    item "${label} [${path}]"
    if [[ ! -e "${path}" ]]; then
        printf '<absent>\n'
        return 0
    fi
    if [[ ! -r "${path}" ]]; then
        printf '<unreadable>\n'
        return 0
    fi

    value=$(cat -- "${path}" 2>&1)
    status=$?
    if (( status != 0 )); then
        printf '<read-error:%d>\n%s\n' "${status}" "${value}"
    elif [[ -n "${value}" ]]; then
        printf '%s\n' "${value}"
    else
        printf '<empty>\n'
    fi
}

resolve_cgroup_dir() {
    local pid=$1
    local relative

    relative=$(awk -F: '$1 == "0" {print $3}' "/proc/${pid}/cgroup" 2>/dev/null)
    if [[ -z "${relative}" ]]; then
        return 1
    fi
    if [[ "${relative}" == "/" ]]; then
        printf '/sys/fs/cgroup\n'
    else
        printf '/sys/fs/cgroup%s\n' "${relative}"
    fi
}

collect_cgroup_chain() {
    local pid=$1
    local current
    local field
    local value
    local status

    current=$(resolve_cgroup_dir "${pid}") || {
        printf '<no-cgroup-v2-entry-for-pid:%s>\n' "${pid}"
        return 0
    }

    while true; do
        printf '\n--- cgroup scope: %s ---\n' "${current}"
        for field in \
            cgroup.controllers \
            cgroup.subtree_control \
            cpu.max \
            cpu.weight \
            cpu.stat \
            cpuset.cpus \
            cpuset.cpus.effective \
            cpuset.mems \
            cpuset.mems.effective \
            memory.max \
            memory.high \
            memory.current \
            memory.events \
            memory.stat \
            io.max \
            io.weight \
            io.stat \
            pids.max \
            pids.current; do
            if [[ -e "${current}/${field}" ]]; then
                printf '%s=' "${field}"
                if [[ -r "${current}/${field}" ]]; then
                    value=$(cat -- "${current}/${field}" 2>&1)
                    status=$?
                    if (( status != 0 )); then
                        printf '<read-error:%d> %s\n' "${status}" "${value}"
                    elif [[ -n "${value}" ]]; then
                        printf '%s\n' "${value}"
                    else
                        printf '<empty>\n'
                    fi
                else
                    printf '<unreadable>\n'
                fi
            else
                printf '%s=<absent>\n' "${field}"
            fi
        done

        if [[ "${current}" == "/sys/fs/cgroup" ]]; then
            break
        fi
        current=${current%/*}
        if [[ -z "${current}" ]]; then
            printf '<invalid-cgroup-parent>\n'
            break
        fi
    done
}

discover_service_pid() {
    local pid

    if [[ -n "${REQUESTED_SERVICE_PID}" ]]; then
        if [[ -d "/proc/${REQUESTED_SERVICE_PID}" ]]; then
            printf '%s\n' "${REQUESTED_SERVICE_PID}"
        fi
        return 0
    fi

    while read -r pid; do
        [[ -n "${pid}" ]] || continue
        if [[ -r "/proc/${pid}/cmdline" ]]; then
            printf '%s\n' "${pid}"
            return 0
        fi
    done < <(pgrep -f '[v]llm serve|[E]ngineCore' 2>/dev/null || true)
}

section "collector"
printf 'collector_schema_version=%s\n' "${COLLECTOR_SCHEMA_VERSION}"
printf 'timestamp_utc=%s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
printf 'collector_pid=%s\n' "$$"
printf 'experiment_path=%s\n' "${EXPERIMENT_PATH}"

section "virtualization and evidence boundary"
run_optional "virtualization" systemd-detect-virt
read_optional "DMI system vendor" "/sys/class/dmi/id/sys_vendor"
read_optional "DMI product name" "/sys/class/dmi/id/product_name"
read_optional "DMI product version" "/sys/class/dmi/id/product_version"
read_optional "kernel command line" "/proc/cmdline"
run_optional "CPU virtualization flags" sh -c \
    "grep -m1 -Eo '(^| )[a-z_]*(hypervisor|vmx|svm)[a-z_]*( |$)' /proc/cpuinfo || true"
run_optional "namespace identities for PID 1 and collector" sh -c \
    "ls -l /proc/1/ns /proc/$$/ns"

section "experiment path resolution"
if [[ ! -e "${EXPERIMENT_PATH}" ]]; then
    printf '<experiment-path-absent>\n'
else
    run_optional "canonical path" realpath -- "${EXPERIMENT_PATH}"
    run_optional "path components" namei -l -- "${EXPERIMENT_PATH}"
    run_optional \
        "path stat" \
        stat -Lc \
        'path=%n mode=%A uid=%u gid=%g device_hex=%D inode=%i block_size=%o' \
        -- "${EXPERIMENT_PATH}"
    run_optional \
        "resolved mount" \
        findmnt -T "${EXPERIMENT_PATH}" \
        -o TARGET,SOURCE,FSTYPE,OPTIONS,FSROOT,MAJ:MIN
    run_optional \
        "capacity bytes" \
        df -B1 -T -- "${EXPERIMENT_PATH}"
    run_optional \
        "capacity human" \
        df -hT -- "${EXPERIMENT_PATH}"
    run_optional \
        "mountinfo match" \
        findmnt -T "${EXPERIMENT_PATH}" -o +PROPAGATION
fi
run_optional \
    "guest-visible block topology" \
    lsblk -b -e 7 \
    -o NAME,KNAME,PATH,MAJ:MIN,TYPE,SIZE,RO,ROTA,FSTYPE,FSVER,MOUNTPOINTS,MODEL,VENDOR,TRAN
read_optional "guest-visible software RAID" "/proc/mdstat"

section "service process discovery"
service_pid=$(discover_service_pid)
if [[ -z "${service_pid}" ]]; then
    printf 'service_pid=<none>\n'
    printf 'service_pid_status=NO_RUNNING_VLLM_PROCESS\n'
else
    printf 'service_pid=%s\n' "${service_pid}"
    printf 'service_pid_status=FOUND\n'
    run_optional \
        "service process" \
        ps -p "${service_pid}" \
        -o pid,ppid,user,etimes,stat,psr,ni,comm,args
    read_optional "service command line" "/proc/${service_pid}/cmdline"
    read_optional "service status" "/proc/${service_pid}/status"
    read_optional "service limits" "/proc/${service_pid}/limits"
    read_optional "service cgroup membership" "/proc/${service_pid}/cgroup"
    run_optional "service affinity" taskset -pc "${service_pid}"
    item "service cgroup effective limits and accounting"
    collect_cgroup_chain "${service_pid}"
fi

section "collector process visibility reference"
printf 'reference_pid=%s\n' "$$"
read_optional "reference cgroup membership" "/proc/$$/cgroup"
read_optional "reference status" "/proc/$$/status"
read_optional "reference limits" "/proc/$$/limits"
item "reference cgroup effective limits and accounting"
collect_cgroup_chain "$$"

section "GPU presentation"
run_optional \
    "GPU identity and presentation" \
    nvidia-smi \
    --query-gpu=index,name,uuid,pci.bus_id,driver_version,memory.total,memory.used,pstate,mig.mode.current \
    --format=csv,noheader
run_optional "GPU topology" nvidia-smi topo -m
run_optional "guest-visible PCI display devices" sh -c \
    "lspci -nnk | grep -A4 -E 'VGA|3D controller|Display' || true"

section "vLLM and CUDA runtime"
run_optional "Python executable" sh -c \
    "command -v python3; python3 --version"
item "vLLM import"
python3 - <<'PY'
import json
import sys

result = {
    "python_executable": sys.executable,
    "python_version": sys.version,
}
try:
    import vllm
except Exception as exc:
    result.update(
        {
            "vllm_import": "FAIL",
            "exception_type": type(exc).__name__,
            "exception": str(exc),
        }
    )
else:
    result.update(
        {
            "vllm_import": "PASS",
            "vllm_version": getattr(vllm, "__version__", "<missing>"),
            "vllm_path": getattr(vllm, "__file__", "<missing>"),
        }
    )
print(json.dumps(result, sort_keys=True))
PY
printf 'vllm_import_exit_status=%s\n' "$?"

item "PyTorch CUDA smoke"
python3 - <<'PY'
import json

result = {}
try:
    import torch

    result.update(
        {
            "torch_import": "PASS",
            "torch_version": torch.__version__,
            "torch_cuda_runtime": torch.version.cuda,
            "cuda_available": torch.cuda.is_available(),
            "cuda_device_count": torch.cuda.device_count(),
        }
    )
    if not torch.cuda.is_available():
        result["gpu_smoke"] = "FAIL"
    else:
        x = torch.arange(1 << 20, device="cuda", dtype=torch.int64)
        observed = int(x.sum().item())
        expected = ((1 << 20) - 1) * (1 << 20) // 2
        torch.cuda.synchronize()
        result.update(
            {
                "device_name": torch.cuda.get_device_name(0),
                "gpu_smoke": "PASS" if observed == expected else "FAIL",
                "checksum_observed": observed,
                "checksum_expected": expected,
            }
        )
except Exception as exc:
    result.update(
        {
            "torch_import_or_smoke": "FAIL",
            "exception_type": type(exc).__name__,
            "exception": str(exc),
        }
    )
print(json.dumps(result, sort_keys=True))
PY
printf 'torch_smoke_exit_status=%s\n' "$?"

section "cloud-sensitive instantaneous context"
read_optional "CPU accounting including steal" "/proc/stat"
read_optional "load average" "/proc/loadavg"
read_optional "virtual memory counters" "/proc/vmstat"
read_optional "pressure CPU" "/proc/pressure/cpu"
read_optional "pressure memory" "/proc/pressure/memory"
read_optional "pressure I/O" "/proc/pressure/io"
run_optional "uptime" uptime

section "collector complete"
printf 'collector_schema_version=%s\n' "${COLLECTOR_SCHEMA_VERSION}"
printf 'timestamp_utc=%s\n' "$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
