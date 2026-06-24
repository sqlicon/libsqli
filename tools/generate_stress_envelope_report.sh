#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARTIFACT_DIR="$ROOT_DIR/doc/status/artifacts"
TMP_DIR="$ARTIFACT_DIR/stress_tmp"
mkdir -p "$ARTIFACT_DIR" "$TMP_DIR"

: "${SQLI_BENCH_HOST:?missing SQLI_BENCH_HOST}"
: "${SQLI_BENCH_PORT:?missing SQLI_BENCH_PORT}"
: "${SQLI_BENCH_DB:?missing SQLI_BENCH_DB}"
: "${SQLI_BENCH_SERVER:?missing SQLI_BENCH_SERVER}"
: "${SQLI_BENCH_USER:?missing SQLI_BENCH_USER}"
: "${SQLI_BENCH_PASS:?missing SQLI_BENCH_PASS}"

client_locale="${SQLI_CLIENT_LOCALE:-en_US.utf8}"
db_locale="${SQLI_DB_LOCALE:-de_DE.CP1252}"
stress_levels="${SQLI_STRESS_LEVELS:-8 16 24 32 48 64}"
iterations="${SQLI_STRESS_ITERATIONS:-300}"
stress_sql="${SQLI_STRESS_SQL:-SELECT FIRST 1 u01_nr FROM uno_msg}"
fetch_seconds="${SQLI_FETCH_SECONDS:-60}"

report="$ARTIFACT_DIR/stress_envelope_report.md"
stress_log="$TMP_DIR/stress_runs.log"
fetch_log="$TMP_DIR/fetch_runs.log"
: >"$stress_log"
: >"$fetch_log"

run_stress_case() {
  local threads="$1"
  local out
  out="$(
    LSAN_OPTIONS=detect_leaks=0 \
    SQLI_LOG_LEVEL=ERROR \
    SQLI_CLIENT_LOCALE="$client_locale" \
    SQLI_DB_LOCALE="$db_locale" \
    "$ROOT_DIR/build/sqli_stress" \
      "$SQLI_BENCH_HOST" "$SQLI_BENCH_PORT" "$SQLI_BENCH_DB" \
      "$SQLI_BENCH_USER" "$SQLI_BENCH_PASS" \
      "$threads" "$iterations" "$threads" "$stress_sql" 2>&1 || true
  )"
  echo "$out" >>"$stress_log"
  echo "$out"
}

run_fetch_loop() {
  local workers="$1"
  local tag="$2"
  local loop_dir="$TMP_DIR/$tag"
  rm -rf "$loop_dir"
  mkdir -p "$loop_dir"

  local start end elapsed total_bytes files mibps
  start="$(date +%s)"
  if [[ "$workers" -eq 1 ]]; then
    timeout "${fetch_seconds}s" bash -c '
      i=0
      while true; do
        i=$((i+1))
        out="'"$loop_dir"'/w1_${i}.csv"
        LSAN_OPTIONS=detect_leaks=0 \
        SQLI_LOG_LEVEL=ERROR \
        SQLI_CLIENT_LOCALE="'"$client_locale"'" \
        SQLI_DB_LOCALE="'"$db_locale"'" \
        timeout 25s "'"$ROOT_DIR"'/build/sqli_export_csv" \
          "'"$SQLI_BENCH_HOST"'" "'"$SQLI_BENCH_PORT"'" "'"$SQLI_BENCH_DB"'" \
          "'"$SQLI_BENCH_USER"'" "'"$SQLI_BENCH_PASS"'" "TABLE:mtst" "$out" "'"$SQLI_BENCH_SERVER"'" \
          >/dev/null 2>&1 || true
      done
    ' || true
  else
    timeout "${fetch_seconds}s" bash -c '
      i=0
      while true; do
        i=$((i+1))
        out1="'"$loop_dir"'/w1_${i}.csv"
        out2="'"$loop_dir"'/w2_${i}.csv"
        LSAN_OPTIONS=detect_leaks=0 \
        SQLI_LOG_LEVEL=ERROR \
        SQLI_CLIENT_LOCALE="'"$client_locale"'" \
        SQLI_DB_LOCALE="'"$db_locale"'" \
        timeout 25s "'"$ROOT_DIR"'/build/sqli_export_csv" \
          "'"$SQLI_BENCH_HOST"'" "'"$SQLI_BENCH_PORT"'" "'"$SQLI_BENCH_DB"'" \
          "'"$SQLI_BENCH_USER"'" "'"$SQLI_BENCH_PASS"'" "TABLE:mtst" "$out1" "'"$SQLI_BENCH_SERVER"'" \
          >/dev/null 2>&1 &
        p1=$!
        LSAN_OPTIONS=detect_leaks=0 \
        SQLI_LOG_LEVEL=ERROR \
        SQLI_CLIENT_LOCALE="'"$client_locale"'" \
        SQLI_DB_LOCALE="'"$db_locale"'" \
        timeout 25s "'"$ROOT_DIR"'/build/sqli_export_csv" \
          "'"$SQLI_BENCH_HOST"'" "'"$SQLI_BENCH_PORT"'" "'"$SQLI_BENCH_DB"'" \
          "'"$SQLI_BENCH_USER"'" "'"$SQLI_BENCH_PASS"'" "TABLE:mtst" "$out2" "'"$SQLI_BENCH_SERVER"'" \
          >/dev/null 2>&1 &
        p2=$!
        wait "$p1" || true
        wait "$p2" || true
      done
    ' || true
  fi
  end="$(date +%s)"
  elapsed=$((end - start))
  files="$(find "$loop_dir" -type f -name '*.csv' | wc -l | awk '{print $1}')"
  total_bytes="$(find "$loop_dir" -type f -name '*.csv' -printf '%s\n' | awk '{s+=$1} END{print s+0}')"
  mibps="$(awk -v b="$total_bytes" -v s="$elapsed" 'BEGIN{ if (s<=0) print "0.000"; else printf "%.3f", (b/1048576.0)/s }')"
  echo "fetch_result tag=$tag workers=$workers elapsed_s=$elapsed files=$files bytes=$total_bytes mibps=$mibps" | tee -a "$fetch_log"
}

# Ensure binaries are built.
cmake -S "$ROOT_DIR/libsqli" -B "$ROOT_DIR/build" >/dev/null
cmake --build "$ROOT_DIR/build" -j >/dev/null

declare -a stress_rows
max_stable_threads=0
overall="PASS"

for t in $stress_levels; do
  line="$(run_stress_case "$t" | tail -n 1)"
  if [[ "$line" != stress_result* ]]; then
    stress_rows+=("| $t | FAIL | n/a | n/a | n/a | n/a | malformed output |")
    overall="FAIL"
    continue
  fi

  elapsed_ms="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^elapsed_ms=/){split($i,a,"="); print a[2]}}' <<<"$line")"
  ok="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^ok=/){split($i,a,"="); print a[2]}}' <<<"$line")"
  failed="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^failed=/){split($i,a,"="); print a[2]}}' <<<"$line")"
  auth="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^auth=/){split($i,a,"="); print a[2]}}' <<<"$line")"
  network="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^network=/){split($i,a,"="); print a[2]}}' <<<"$line")"
  qps="$(awk -v o="$ok" -v e="$elapsed_ms" 'BEGIN{ if (e<=0) print "0.00"; else printf "%.2f", o/(e/1000.0) }')"

  if [[ "$failed" == "0" ]]; then
    status="PASS"
    if [[ "$t" -gt "$max_stable_threads" ]]; then
      max_stable_threads="$t"
    fi
  else
    status="FAIL"
    overall="FAIL"
  fi
  stress_rows+=("| $t | $status | $elapsed_ms | $ok | $failed | $qps | auth=$auth network=$network |")
done

run_fetch_loop 1 "fetch_1w_${fetch_seconds}s"
run_fetch_loop 2 "fetch_2w_${fetch_seconds}s"

f1_line="$(grep 'tag=fetch_1w_' "$fetch_log" | tail -n 1 || true)"
f2_line="$(grep 'tag=fetch_2w_' "$fetch_log" | tail -n 1 || true)"
f1_mibps="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^mibps=/){split($i,a,"="); print a[2]}}' <<<"$f1_line")"
f2_mibps="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^mibps=/){split($i,a,"="); print a[2]}}' <<<"$f2_line")"
f1_files="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^files=/){split($i,a,"="); print a[2]}}' <<<"$f1_line")"
f2_files="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^files=/){split($i,a,"="); print a[2]}}' <<<"$f2_line")"

if [[ -z "$f1_line" || -z "$f2_line" ]]; then
  overall="FAIL"
fi

cat >"$report" <<EOF
# Stress Envelope Report

- Timestamp (UTC): $(date -u +"%Y-%m-%dT%H:%M:%SZ")
- Target: ${SQLI_BENCH_HOST}:${SQLI_BENCH_PORT} / ${SQLI_BENCH_DB} (server=${SQLI_BENCH_SERVER})
- Locales: client=${client_locale}, db=${db_locale}
- Stress SQL: \`${stress_sql}\`
- Iterations per thread: ${iterations}

## Concurrent Query Envelope

| Threads | Status | Elapsed ms | OK | Failed | QPS | Notes |
|---|---|---:|---:|---:|---:|---|
$(printf '%s\n' "${stress_rows[@]}")

## Long-Running Fetch Envelope

- Single-worker fetch (${fetch_seconds}s): files=${f1_files:-0}, throughput=${f1_mibps:-0.000} MiB/s
- Two-worker fetch (${fetch_seconds}s): files=${f2_files:-0}, throughput=${f2_mibps:-0.000} MiB/s

## Envelope Decision

- Maximum stable concurrent query threads observed: **${max_stable_threads}**
- Overall gate: **${overall}**

## Raw Artifacts

- \`doc/status/artifacts/stress_tmp/stress_runs.log\`
- \`doc/status/artifacts/stress_tmp/fetch_runs.log\`
EOF

echo "Wrote $report"
if [[ "$overall" != "PASS" ]]; then
  echo "Stress envelope failed: $report" >&2
  exit 1
fi
