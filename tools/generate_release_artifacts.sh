#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly DEFAULT_BUILD_DIR="${ROOT_DIR}/build"
readonly DEFAULT_SANITIZER_BUILD_DIR="${ROOT_DIR}/build-sanitizer"
readonly ARTIFACT_DIR="${ROOT_DIR}/doc/status/artifacts"
mkdir -p "${ARTIFACT_DIR}"

timestamp_utc() {
  date -u +"%Y-%m-%dT%H:%M:%SZ"
}

host_context_md() {
  cat <<EOF
- Timestamp (UTC): $(timestamp_utc)
- Hostname: $(hostname)
- Kernel: $(uname -srmo)
- CPU cores: $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "unknown")
EOF
}

run_sanitizer_report() {
  local build_dir="${SQLI_SANITIZER_BUILD_DIR:-${DEFAULT_SANITIZER_BUILD_DIR}}"
  local report="${ARTIFACT_DIR}/sanitizer_report.md"
  local log_file="${ARTIFACT_DIR}/sanitizer_ctest.log"

  cmake -S "${ROOT_DIR}" -B "${build_dir}" -DSQLI_ENABLE_SANITIZERS=ON >/dev/null
  cmake --build "${build_dir}" -j >/dev/null
  if ctest --test-dir "${build_dir}" --output-on-failure >"${log_file}" 2>&1; then
    local status="PASS"
  else
    local status="FAIL"
  fi

  cat >"$report" <<EOF
# Sanitizer Verification Report

## Result

- Status: **$status**

## Environment

$(host_context_md)

## Commands

\`\`\`bash
cmake -S . -B build-sanitizer -DSQLI_ENABLE_SANITIZERS=ON
cmake --build build-sanitizer -j
ctest --test-dir build-sanitizer --output-on-failure
\`\`\`

## Output

\`\`\`text
$(cat "${log_file}")
\`\`\`
EOF

  if [[ "${status}" != "PASS" ]]; then
    echo "Sanitizer report failed: ${report}" >&2
    return 1
  fi
  echo "Wrote ${report}"
}

calc_avg_int() {
  awk '{s+=$1; n++} END { if (n==0) print 0; else printf "%.0f", s/n }'
}

calc_p95_int_file() {
  local file="$1"
  local n idx
  n="$(wc -l <"$file" | awk '{print $1}')"
  if [[ "$n" -eq 0 ]]; then
    echo 0
    return
  fi
  idx=$(( (n * 95 + 99) / 100 ))
  if [[ "$idx" -lt 1 ]]; then idx=1; fi
  if [[ "$idx" -gt "$n" ]]; then idx="$n"; fi
  sort -n "$file" | sed -n "${idx}p"
}

calc_min_float() {
  awk 'NR==1{m=$1} NR>1 && $1<m{m=$1} END { if (NR==0) print "0.000"; else printf "%.3f", m }'
}

calc_avg_float() {
  awk '{s+=$1; n++} END { if (n==0) print "0.000"; else printf "%.3f", s/n }'
}

run_performance_report() {
  : "${SQLI_BENCH_HOST:?missing SQLI_BENCH_HOST}"
  : "${SQLI_BENCH_PORT:?missing SQLI_BENCH_PORT}"
  : "${SQLI_BENCH_DB:?missing SQLI_BENCH_DB}"
  : "${SQLI_BENCH_SERVER:?missing SQLI_BENCH_SERVER}"
  : "${SQLI_BENCH_USER:?missing SQLI_BENCH_USER}"
  : "${SQLI_BENCH_PASS:?missing SQLI_BENCH_PASS}"

  local client_locale="${SQLI_CLIENT_LOCALE:-en_US.utf8}"
  local db_locale="${SQLI_DB_LOCALE:-de_DE.CP1252}"
  local build_dir="${SQLI_BUILD_DIR:-${DEFAULT_BUILD_DIR}}"
  local report="${ARTIFACT_DIR}/performance_baseline_report.md"
  local gate_report="${ARTIFACT_DIR}/performance_gate_report.md"
  local work_dir="${ARTIFACT_DIR}/perf_tmp"
  local sqliconn_bin="${build_dir}/sqliconn"
  local export_csv_bin="${build_dir}/sqli_export_csv"
  rm -rf "${work_dir}"
  mkdir -p "${work_dir}"

  cmake -S "${ROOT_DIR}" -B "${build_dir}" >/dev/null
  cmake --build "${build_dir}" -j >/dev/null

  if [[ ! -x "${sqliconn_bin}" ]]; then
    echo "sqliconn binary not found: ${sqliconn_bin}" >&2
    return 1
  fi
  if [[ ! -x "${export_csv_bin}" ]]; then
    echo "sqli_export_csv binary not found: ${export_csv_bin}" >&2
    return 1
  fi

  local cq_file="${work_dir}/connect_query_ms.txt"
  local ft_ms_file="${work_dir}/fetch_ms.txt"
  local ft_mib_file="${work_dir}/fetch_mibps.txt"
  : >"${cq_file}"
  : >"${ft_ms_file}"
  : >"${ft_mib_file}"

  local i
  for i in 1 2 3 4 5; do
    local t0 t1 ms
    t0="$(date +%s%3N)"
    SQLI_LOG_LEVEL=ERROR \
    SQLI_CLIENT_LOCALE="$client_locale" \
    SQLI_DB_LOCALE="$db_locale" \
    "${sqliconn_bin}" \
      "$SQLI_BENCH_HOST" "$SQLI_BENCH_PORT" "$SQLI_BENCH_DB" \
      "$SQLI_BENCH_USER" "$SQLI_BENCH_PASS" "$SQLI_BENCH_SERVER" \
      "SELECT FIRST 1 tabname FROM systables" \
      >"${work_dir}/cq_run_${i}.out" 2>"${work_dir}/cq_run_${i}.err"
    t1="$(date +%s%3N)"
    ms=$((t1 - t0))
    echo "${ms}" >>"${cq_file}"
  done

  for i in 1 2 3 4 5; do
    local csv="${work_dir}/fetch_run_${i}.csv"
    local t0 t1 ms bytes mibps rc
    rm -f "${csv}"
    t0="$(date +%s%3N)"
    if SQLI_LOG_LEVEL=ERROR \
      SQLI_CLIENT_LOCALE="$client_locale" \
      SQLI_DB_LOCALE="$db_locale" \
      timeout 20s "${export_csv_bin}" \
        "$SQLI_BENCH_HOST" "$SQLI_BENCH_PORT" "$SQLI_BENCH_DB" \
        "$SQLI_BENCH_USER" "$SQLI_BENCH_PASS" "TABLE:mtst" "$csv" "$SQLI_BENCH_SERVER" \
        >"${work_dir}/fetch_run_${i}.out" 2>"${work_dir}/fetch_run_${i}.err"; then
      rc=0
    else
      rc=$?
    fi
    t1="$(date +%s%3N)"
    if [[ ${rc} -ne 0 ]]; then
      echo "sqli_export_csv failed in run ${i} with exit code ${rc}" >&2
      return 1
    fi
    ms=$((t1 - t0))
    bytes="$(wc -c <"${csv}" 2>/dev/null || echo 0)"
    mibps="$(awk -v b="$bytes" -v m="$ms" 'BEGIN { if (m<=0) { print "0.000"; } else { printf "%.3f", (b / 1048576.0) / (m / 1000.0); } }')"
    echo "${ms}" >>"${ft_ms_file}"
    echo "${mibps}" >>"${ft_mib_file}"
  done

  local cq_avg cq_p95 ft_ms_avg ft_ms_p95 ft_mib_avg ft_mib_min
  cq_avg="$(calc_avg_int <"${cq_file}")"
  cq_p95="$(calc_p95_int_file "${cq_file}")"
  ft_ms_avg="$(calc_avg_int <"${ft_ms_file}")"
  ft_ms_p95="$(calc_p95_int_file "${ft_ms_file}")"
  ft_mib_avg="$(calc_avg_float <"${ft_mib_file}")"
  ft_mib_min="$(calc_min_float <"${ft_mib_file}")"

  local target_cq_ms target_ft_ms target_ft_mib
  target_cq_ms="$(awk -v p="$cq_p95" 'BEGIN{printf "%.0f", p*1.25}')"
  target_ft_ms="$(awk -v p="$ft_ms_p95" 'BEGIN{printf "%.0f", p*1.25}')"
  target_ft_mib="$(awk -v m="$ft_mib_min" 'BEGIN{printf "%.3f", m*0.80}')"

  # Contractual SLO thresholds (override by environment for stricter profiles).
  local slo_cq_p95_ms="${SQLI_SLO_CQ_P95_MS:-1500}"
  local slo_fetch_p95_ms="${SQLI_SLO_FETCH_P95_MS:-15000}"
  local slo_fetch_min_mibps="${SQLI_SLO_FETCH_MIN_MIBPS:-6.500}"

  local gate_cq gate_fetch_ms gate_fetch_mib overall
  gate_cq="$(awk -v v="$cq_p95" -v t="$slo_cq_p95_ms" 'BEGIN{print (v<=t)?"PASS":"FAIL"}')"
  gate_fetch_ms="$(awk -v v="$ft_ms_p95" -v t="$slo_fetch_p95_ms" 'BEGIN{print (v<=t)?"PASS":"FAIL"}')"
  gate_fetch_mib="$(awk -v v="$ft_mib_min" -v t="$slo_fetch_min_mibps" 'BEGIN{print (v>=t)?"PASS":"FAIL"}')"
  if [[ "$gate_cq" == "PASS" && "$gate_fetch_ms" == "PASS" && "$gate_fetch_mib" == "PASS" ]]; then
    overall="PASS"
  else
    overall="FAIL"
  fi

  cat >"$report" <<EOF
# Performance Baseline Report

## Environment

$(host_context_md)

- Target host: ${SQLI_BENCH_HOST}:${SQLI_BENCH_PORT}
- Target database: ${SQLI_BENCH_DB}
- Target server alias: ${SQLI_BENCH_SERVER}
- Client locale: ${client_locale}
- DB locale: ${db_locale}

## Scenarios (5 runs each)

1. Connect + simple query latency using \`sqliconn\` with:
   - \`SELECT FIRST 1 tabname FROM systables\`
2. Sustained fetch throughput using \`sqli_export_csv\` for 20s with:
   - \`TABLE:mtst\`

## Results

### Connect + Query Latency
- Avg: **${cq_avg} ms**
- P95: **${cq_p95} ms**
- Proposed minimum target (release gate): **P95 <= ${target_cq_ms} ms**

### Sustained Fetch Window (20s)
- Avg run duration: **${ft_ms_avg} ms**
- P95 run duration: **${ft_ms_p95} ms**
- Avg throughput: **${ft_mib_avg} MiB/s**
- Minimum throughput: **${ft_mib_min} MiB/s**
- Proposed minimum target (release gate): **throughput >= ${target_ft_mib} MiB/s**

## Raw Data

### Connect + Query (ms)
\`\`\`text
$(paste -d' ' <(seq 1 5) "${cq_file}" | awk '{printf "run_%d %s ms\n",$1,$2}')
\`\`\`

### Fetch (duration ms, MiB/s)
\`\`\`text
$(paste -d' ' <(seq 1 5) "${ft_ms_file}" "${ft_mib_file}" | awk '{printf "run_%d %s ms %s MiB/s\n",$1,$2,$3}')
\`\`\`
EOF

  cat >"$gate_report" <<EOF
# Performance Gate Report

## Environment

$(host_context_md)

- Target host: ${SQLI_BENCH_HOST}:${SQLI_BENCH_PORT}
- Target database: ${SQLI_BENCH_DB}
- Target server alias: ${SQLI_BENCH_SERVER}
- Client locale: ${client_locale}
- DB locale: ${db_locale}

## Contractual SLO Thresholds

- Connect+Query latency P95: **<= ${slo_cq_p95_ms} ms**
- Fetch run duration P95 (20s window): **<= ${slo_fetch_p95_ms} ms**
- Sustained fetch minimum throughput: **>= ${slo_fetch_min_mibps} MiB/s**

## Measured Results

- Connect+Query latency P95: **${cq_p95} ms** -> **${gate_cq}**
- Fetch run duration P95: **${ft_ms_p95} ms** -> **${gate_fetch_ms}**
- Sustained fetch minimum throughput: **${ft_mib_min} MiB/s** -> **${gate_fetch_mib}**

## Gate Decision

- Overall: **${overall}**

## Source Baseline Report

- \`doc/status/artifacts/performance_baseline_report.md\`
EOF

  rm -rf "${work_dir}"
  echo "Wrote ${report}"
  echo "Wrote ${gate_report}"
  if [[ "${overall}" != "PASS" ]]; then
    echo "Performance gate failed: ${gate_report}" >&2
    return 1
  fi
}

usage() {
  cat <<EOF
Usage:
  $0 sanitizer
  $0 performance
  $0 all

For performance mode, set:
  SQLI_BENCH_HOST SQLI_BENCH_PORT SQLI_BENCH_DB SQLI_BENCH_SERVER SQLI_BENCH_USER SQLI_BENCH_PASS
Optional:
  SQLI_BUILD_DIR SQLI_SANITIZER_BUILD_DIR
  SQLI_CLIENT_LOCALE SQLI_DB_LOCALE
  SQLI_SLO_CQ_P95_MS SQLI_SLO_FETCH_P95_MS SQLI_SLO_FETCH_MIN_MIBPS
EOF
}

main() {
  local mode="${1:-}"
  case "$mode" in
    sanitizer)
      run_sanitizer_report
      ;;
    performance)
      run_performance_report
      ;;
    all)
      run_sanitizer_report
      run_performance_report
      ;;
    *)
      usage
      exit 2
      ;;
  esac
}

main "$@"
