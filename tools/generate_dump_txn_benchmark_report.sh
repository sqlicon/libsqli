#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARTIFACT_DIR="$ROOT_DIR/doc/status/artifacts"
TMP_DIR="$ARTIFACT_DIR/dump_txn_tmp"
mkdir -p "$ARTIFACT_DIR" "$TMP_DIR"

: "${SQLI_BENCH_HOST:?missing SQLI_BENCH_HOST}"
: "${SQLI_BENCH_PORT:?missing SQLI_BENCH_PORT}"
: "${SQLI_BENCH_SERVER:?missing SQLI_BENCH_SERVER}"
: "${SQLI_BENCH_USER:?missing SQLI_BENCH_USER}"
: "${SQLI_BENCH_PASS:?missing SQLI_BENCH_PASS}"

bench_db="${SQLI_BENCH_DB:-unorech}"
runs="${SQLI_DUMP_BENCH_RUNS:-5}"
client_locale="${SQLI_CLIENT_LOCALE:-en_US.utf8}"
db_locale="${SQLI_DB_LOCALE:-de_DE.CP1252}"

sqlicon="$ROOT_DIR/build/sqlicon"
if [[ ! -x "$sqlicon" ]]; then
  echo "sqlicon binary not found: $sqlicon" >&2
  exit 2
fi

table_csv="$TMP_DIR/table_list.csv"
table_list="$TMP_DIR/table_list.txt"
cat >"$TMP_DIR/list_tables.sql" <<'EOF'
.mode csv
.headers off
SELECT tabname
  FROM systables
 WHERE tabid >= 100
   AND tabtype = 'T'
   AND owner = USER
 ORDER BY tabname;
EOF

SQLI_LOG_LEVEL=ERROR \
SQLI_CLIENT_LOCALE="$client_locale" \
SQLI_DB_LOCALE="$db_locale" \
"$sqlicon" \
  --host "$SQLI_BENCH_HOST" \
  --port "$SQLI_BENCH_PORT" \
  --server "$SQLI_BENCH_SERVER" \
  --database "$bench_db" \
  --user "$SQLI_BENCH_USER" \
  --password "$SQLI_BENCH_PASS" \
  -f "$TMP_DIR/list_tables.sql" >"$table_csv"

grep -E '^[A-Za-z_][A-Za-z0-9_]*$' "$table_csv" >"$table_list" || true
table_count="$(wc -l <"$table_list" | awk '{print $1}')"
if [[ "$table_count" -eq 0 ]]; then
  echo "no dumpable tables found in $bench_db" >&2
  exit 1
fi

single_log="$TMP_DIR/single_tx_runs.log"
legacy_log="$TMP_DIR/per_table_tx_runs.log"
: >"$single_log"
: >"$legacy_log"

run_single_tx() {
  local i="$1"
  local dump_file="$TMP_DIR/single_tx_${i}.sql"
  local script="$TMP_DIR/single_tx_${i}.sqlicon"
  cat >"$script" <<EOF
.bail on
.once $dump_file
.dump
EOF
  local start_ms end_ms elapsed_ms bytes
  start_ms="$(date +%s%3N)"
  SQLI_LOG_LEVEL=ERROR \
  SQLI_CLIENT_LOCALE="$client_locale" \
  SQLI_DB_LOCALE="$db_locale" \
  "$sqlicon" \
    --host "$SQLI_BENCH_HOST" \
    --port "$SQLI_BENCH_PORT" \
    --server "$SQLI_BENCH_SERVER" \
    --database "$bench_db" \
    --user "$SQLI_BENCH_USER" \
    --password "$SQLI_BENCH_PASS" \
    -f "$script" >/dev/null
  end_ms="$(date +%s%3N)"
  elapsed_ms=$((end_ms - start_ms))
  bytes="$(wc -c <"$dump_file" | awk '{print $1}')"
  echo "run=$i elapsed_ms=$elapsed_ms bytes=$bytes" >>"$single_log"
}

run_per_table_tx() {
  local i="$1"
  local dump_file="$TMP_DIR/per_table_tx_${i}.sql"
  local script="$TMP_DIR/per_table_tx_${i}.sqlicon"
  {
    echo ".bail on"
    echo ".once $dump_file"
    while IFS= read -r t; do
      echo ".dump $t"
    done <"$table_list"
  } >"$script"

  local start_ms end_ms elapsed_ms bytes
  start_ms="$(date +%s%3N)"
  SQLI_LOG_LEVEL=ERROR \
  SQLI_CLIENT_LOCALE="$client_locale" \
  SQLI_DB_LOCALE="$db_locale" \
  "$sqlicon" \
    --host "$SQLI_BENCH_HOST" \
    --port "$SQLI_BENCH_PORT" \
    --server "$SQLI_BENCH_SERVER" \
    --database "$bench_db" \
    --user "$SQLI_BENCH_USER" \
    --password "$SQLI_BENCH_PASS" \
    -f "$script" >/dev/null
  end_ms="$(date +%s%3N)"
  elapsed_ms=$((end_ms - start_ms))
  bytes="$(wc -c <"$dump_file" | awk '{print $1}')"
  echo "run=$i elapsed_ms=$elapsed_ms bytes=$bytes" >>"$legacy_log"
}

for i in $(seq 1 "$runs"); do
  run_single_tx "$i"
done
for i in $(seq 1 "$runs"); do
  run_per_table_tx "$i"
done

calc_stats() {
  local file="$1"
  local avg_ms p95_ms avg_mib
  avg_ms="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^elapsed_ms=/){split($i,a,"="); s+=a[2]; n++}} END {if(n==0) print 0; else printf "%.0f", s/n}' "$file")"
  p95_ms="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^elapsed_ms=/){split($i,a,"="); print a[2]}}' "$file" | sort -n | awk 'BEGIN{n=0} {a[++n]=$1} END{if(n==0){print 0}else{idx=int((95*n+99)/100); if(idx<1) idx=1; if(idx>n) idx=n; print a[idx]}}')"
  avg_mib="$(awk '{for(i=1;i<=NF;i++) if($i ~ /^bytes=/){split($i,a,"="); s+=a[2]; n++}} END {if(n==0) print "0.000"; else printf "%.3f", (s/n)/1048576.0}' "$file")"
  echo "$avg_ms;$p95_ms;$avg_mib"
}

single_stats="$(calc_stats "$single_log")"
legacy_stats="$(calc_stats "$legacy_log")"
single_avg_ms="${single_stats%%;*}"
rest="${single_stats#*;}"
single_p95_ms="${rest%%;*}"
single_avg_mib="${rest##*;}"
legacy_avg_ms="${legacy_stats%%;*}"
rest="${legacy_stats#*;}"
legacy_p95_ms="${rest%%;*}"
legacy_avg_mib="${rest##*;}"

delta_pct="$(awk -v a="$legacy_avg_ms" -v b="$single_avg_ms" 'BEGIN{if(a<=0) print "0.0"; else printf "%.1f", ((a-b)*100.0)/a}')"

report="$ARTIFACT_DIR/dump_txn_benchmark_report.md"
cat >"$report" <<EOF
# Dump Transaction Strategy Benchmark Report

- Timestamp (UTC): $(date -u +"%Y-%m-%dT%H:%M:%SZ")
- Target: ${SQLI_BENCH_HOST}:${SQLI_BENCH_PORT} / ${bench_db} (server=${SQLI_BENCH_SERVER})
- Locales: client=${client_locale}, db=${db_locale}
- Runs per scenario: ${runs}
- User tables measured: ${table_count}

## Scenarios

1. single_tx: one .dump over all tables (single global transaction wrapper).
2. per_table_tx: iterate all tables with .dump TABLE (transaction per table).

## Results

| Scenario | Avg elapsed ms | P95 elapsed ms | Avg dump size MiB |
|---|---:|---:|---:|
| single_tx | ${single_avg_ms} | ${single_p95_ms} | ${single_avg_mib} |
| per_table_tx | ${legacy_avg_ms} | ${legacy_p95_ms} | ${legacy_avg_mib} |

- Average latency improvement (single_tx vs per_table_tx): **${delta_pct}%**

## Raw artifacts

- doc/status/artifacts/dump_txn_tmp/single_tx_runs.log
- doc/status/artifacts/dump_txn_tmp/per_table_tx_runs.log
EOF

echo "Wrote $report"
