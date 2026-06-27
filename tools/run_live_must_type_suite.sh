#!/usr/bin/env bash
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly DEFAULT_BUILD_DIR="${ROOT_DIR}/build"
readonly ARTIFACT_DIR="${ROOT_DIR}/doc/status/artifacts"
mkdir -p "${ARTIFACT_DIR}"

host="${SQLI_LIVE_HOST:-${SQLI_BENCH_HOST:-}}"
port="${SQLI_LIVE_PORT:-${SQLI_BENCH_PORT:-}}"
db="${SQLI_LIVE_DB:-${SQLI_BENCH_DB:-}}"
server="${SQLI_LIVE_SERVER:-${SQLI_BENCH_SERVER:-}}"
user="${SQLI_LIVE_USER:-${SQLI_BENCH_USER:-}}"
pass="${SQLI_LIVE_PASS:-${SQLI_BENCH_PASS:-}}"
client_locale="${SQLI_CLIENT_LOCALE:-en_US.utf8}"
db_locale="${SQLI_DB_LOCALE:-de_DE.CP1252}"

if [[ -z "$host" || -z "$port" || -z "$db" || -z "$user" || -z "$pass" ]]; then
  cat <<EOF >&2
Missing live DB parameters.
Set either:
  SQLI_LIVE_HOST SQLI_LIVE_PORT SQLI_LIVE_DB SQLI_LIVE_SERVER SQLI_LIVE_USER SQLI_LIVE_PASS
or:
  SQLI_BENCH_HOST SQLI_BENCH_PORT SQLI_BENCH_DB SQLI_BENCH_SERVER SQLI_BENCH_USER SQLI_BENCH_PASS
EOF
  exit 2
fi

readonly BUILD_DIR="${SQLI_BUILD_DIR:-${DEFAULT_BUILD_DIR}}"
readonly SQLICONN_BIN="${BUILD_DIR}/sqliconn"
readonly PREPARED_PROBE_BIN="${BUILD_DIR}/sqli_prepared_probe"

if [[ ! -x "${SQLICONN_BIN}" ]]; then
  echo "sqliconn binary not found: ${SQLICONN_BIN}" >&2
  exit 2
fi
if [[ ! -x "${PREPARED_PROBE_BIN}" ]]; then
  echo "sqli_prepared_probe binary not found: ${PREPARED_PROBE_BIN}" >&2
  exit 2
fi

table_name="sqli_dt_must_$(date +%s)"
tmp_dir="${ARTIFACT_DIR}/dt_tmp"
rm -rf "${tmp_dir}"
mkdir -p "${tmp_dir}"

report="${ARTIFACT_DIR}/dt_must_type_report.md"
pass_count=0
fail_count=0
declare -a lines

record_failure() {
  local label="$1"
  local file_base="$2"

  echo "Command failed for ${label}" >&2
  if [[ -f "${file_base}.err" ]]; then
    echo "--- ${label} stderr ---" >&2
    cat "${file_base}.err" >&2
  fi
}

run_sql() {
  local sql="$1"
  local out_file="$2"
  SQLI_LOG_LEVEL=ERROR \
  SQLI_CLIENT_LOCALE="$client_locale" \
  SQLI_DB_LOCALE="$db_locale" \
  "${SQLICONN_BIN}" "${host}" "${port}" "${db}" "${user}" "${pass}" "${server}" "${sql}" >"${out_file}.out" 2>"${out_file}.err"
}

check_ok() {
  local id="$1"
  local desc="$2"
  local sql="$3"
  local key="${tmp_dir}/${id}"
  if run_sql "$sql" "$key" && grep -q "ok=1" "$key.out"; then
    lines+=("| $id | PASS | $desc |")
    pass_count=$((pass_count + 1))
  else
    lines+=("| $id | FAIL | $desc |")
    fail_count=$((fail_count + 1))
  fi
}

setup_ok=true
setup_key="${tmp_dir}/setup"
create_sql="
CREATE TABLE ${table_name} (
  id INTEGER,
  c_varchar VARCHAR(128),
  c_char CHAR(4),
  c_nvarchar NVARCHAR(128),
  c_nchar NCHAR(4),
  c_small SMALLINT,
  c_int INTEGER,
  c_big BIGINT,
  c_dec DECIMAL(12,4),
  c_num NUMERIC(10,3),
  c_money MONEY(12,2),
  c_smallfloat SMALLFLOAT,
  c_float FLOAT,
  c_date DATE,
  c_bool BOOLEAN
)"
if ! run_sql "$create_sql" "$setup_key"; then
  record_failure "create_table" "${setup_key}"
  setup_ok=false
fi

if $setup_ok; then
  if ! run_sql "INSERT INTO ${table_name} (id,c_varchar,c_char,c_nvarchar,c_nchar,c_small,c_int,c_big,c_dec,c_num,c_money,c_smallfloat,c_float,c_date,c_bool) VALUES (1,'hello','AB','hello_n','AB',32767,2147483647,9223372036854775807,12345.6789,-42.125,-98765.43,12.5,123.25,DATE('2026-06-20'),'t')" "${tmp_dir}/ins1"; then record_failure "insert_1" "${tmp_dir}/ins1"; setup_ok=false; fi
  if ! run_sql "INSERT INTO ${table_name} (id,c_varchar,c_char,c_nvarchar,c_nchar,c_small,c_int,c_big,c_dec,c_num,c_money,c_smallfloat,c_float,c_date,c_bool) VALUES (2,'ÄÖÜß','XY','ÄÖÜß_n','XY',-32767,-2147483647,-9223372036854775807,0.0001,0.125,1.00,-7.5,-3.75,DATE('2024-01-01'),'f')" "${tmp_dir}/ins2"; then record_failure "insert_2" "${tmp_dir}/ins2"; setup_ok=false; fi
  if ! run_sql "INSERT INTO ${table_name} (id,c_varchar,c_char,c_nvarchar,c_nchar,c_small,c_int,c_big,c_dec,c_num,c_money,c_smallfloat,c_float,c_date,c_bool) VALUES (3,'','CD',NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL)" "${tmp_dir}/ins3"; then record_failure "insert_3" "${tmp_dir}/ins3"; setup_ok=false; fi
  if ! run_sql "INSERT INTO ${table_name} (id,c_varchar,c_char,c_nvarchar,c_nchar,c_small,c_int,c_big,c_dec,c_num,c_money,c_smallfloat,c_float,c_date,c_bool) VALUES (4,'BBBB','EF','BBBB_n','EF',1,2,3,4.0000,5.000,6.00,7.0,8.0,DATE('2023-12-31'),'t')" "${tmp_dir}/ins4"; then record_failure "insert_4" "${tmp_dir}/ins4"; setup_ok=false; fi
  if ! run_sql "INSERT INTO ${table_name} (id,c_varchar,c_char,c_nvarchar,c_nchar,c_small,c_int,c_big,c_dec,c_num,c_money,c_smallfloat,c_float,c_date,c_bool) VALUES (5,'AAAA','GH','AAAA_n','GH',1,2,3,4.0000,5.000,6.00,7.0,8.0,DATE('2023-12-31'),'t')" "${tmp_dir}/ins5"; then record_failure "insert_5" "${tmp_dir}/ins5"; setup_ok=false; fi
  if ! run_sql "INSERT INTO ${table_name} (id,c_varchar,c_char,c_nvarchar,c_nchar,c_small,c_int,c_big,c_dec,c_num,c_money,c_smallfloat,c_float,c_date,c_bool) VALUES (6,'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234567','IJ','long_n','IJ',1,2,3,4.0000,5.000,6.00,7.0,8.0,DATE('2023-12-31'),'t')" "${tmp_dir}/ins6"; then record_failure "insert_6" "${tmp_dir}/ins6"; setup_ok=false; fi
fi

if $setup_ok; then
  check_ok "DT-001" "VARCHAR ASCII roundtrip" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_varchar='hello'"
  check_ok "DT-002" "VARCHAR UTF-8 roundtrip" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_varchar='ÄÖÜß'"
  check_ok "DT-003" "CHAR padding/trim behavior" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE RTRIM(c_char)='AB'"
  check_ok "DT-004" "NCHAR/NVARCHAR null and predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_nvarchar IS NULL AND c_nchar IS NULL"
  check_ok "DT-005" "Empty string vs NULL distinction" "SELECT CASE WHEN SUM(CASE WHEN c_varchar='' THEN 1 ELSE 0 END)=1 AND SUM(CASE WHEN c_varchar IS NULL THEN 1 ELSE 0 END)=0 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-006" "Long string near buffer boundary" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE LENGTH(c_varchar)>=60"
  check_ok "DT-007" "Sort/collation baseline" "SELECT CASE WHEN MIN(c_varchar)='AAAA' THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_varchar IN ('AAAA','BBBB')"

  check_ok "DT-101" "SMALLINT max/min range read" "SELECT CASE WHEN SUM(CASE WHEN c_small=32767 THEN 1 ELSE 0 END)=1 AND SUM(CASE WHEN c_small=-32767 THEN 1 ELSE 0 END)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-102" "INTEGER max/min range read" "SELECT CASE WHEN SUM(CASE WHEN c_int=2147483647 THEN 1 ELSE 0 END)=1 AND SUM(CASE WHEN c_int=-2147483647 THEN 1 ELSE 0 END)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-103" "BIGINT max/min range read" "SELECT CASE WHEN SUM(CASE WHEN c_big=9223372036854775807 THEN 1 ELSE 0 END)=1 AND SUM(CASE WHEN c_big=-9223372036854775807 THEN 1 ELSE 0 END)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-104" "Negative values and null handling" "SELECT CASE WHEN SUM(CASE WHEN c_small<0 THEN 1 ELSE 0 END)>=1 AND SUM(CASE WHEN c_small IS NULL THEN 1 ELSE 0 END)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-105" "Integer WHERE filters" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_int=2147483647"
  check_ok "DT-106" "Integer aggregate plausibility" "SELECT CASE WHEN SUM(c_small) IS NOT NULL THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_small IS NOT NULL"
  check_ok "DT-108" "Multi-row integer roundtrip presence" "SELECT CASE WHEN COUNT(*)>=2 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_int IS NOT NULL"

  check_ok "DT-201" "DECIMAL exact roundtrip predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_dec=12345.6789"
  check_ok "DT-202" "NUMERIC exact roundtrip predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_num=-42.125"
  check_ok "DT-203" "MONEY roundtrip predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_money=-98765.43"
  check_ok "DT-204" "Decimal scale handling" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_dec=0.0001"
  check_ok "DT-205" "Decimal precision path available" "SELECT CASE WHEN MAX(ABS(c_dec))>=12345 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-206" "Decimal null handling" "SELECT CASE WHEN SUM(CASE WHEN c_dec IS NULL THEN 1 ELSE 0 END)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-208" "Decimal comparison operators" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_dec>1"
  check_ok "DT-209" "Decimal ordering path" "SELECT CASE WHEN MIN(c_dec) IS NOT NULL THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_dec IS NOT NULL"
  check_ok "DT-210" "Numeric ordering path" "SELECT CASE WHEN MAX(c_num) IS NOT NULL THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_num IS NOT NULL"

  check_ok "DT-301" "SMALLFLOAT roundtrip predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_smallfloat=12.5"
  check_ok "DT-302" "FLOAT/DOUBLE roundtrip predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_float=123.25"
  check_ok "DT-303" "Float negative value path" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_float<0"
  check_ok "DT-304" "Float filter behavior" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_smallfloat>0"
  check_ok "DT-305" "Float aggregate behavior" "SELECT CASE WHEN AVG(c_float) IS NOT NULL THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_float IS NOT NULL"
  check_ok "DT-306" "Float null handling" "SELECT CASE WHEN SUM(CASE WHEN c_float IS NULL THEN 1 ELSE 0 END)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"

  check_ok "DT-401" "DATE roundtrip predicate" "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_date=DATE('2026-06-20')"
  check_ok "DT-402" "DATE edge-value path" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_date=DATE('2023-12-31')"
  check_ok "DT-403" "DATE null handling" "SELECT CASE WHEN SUM(CASE WHEN c_date IS NULL THEN 1 ELSE 0 END)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-404" "DATE predicate behavior" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_date>=DATE('2024-01-01')"

  check_ok "DT-501" "BOOLEAN true predicate" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_bool='t'"
  check_ok "DT-502" "BOOLEAN false predicate" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_bool='f'"
  check_ok "DT-503" "BOOLEAN null handling" "SELECT CASE WHEN SUM(CASE WHEN c_bool IS NULL THEN 1 ELSE 0 END)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name}"
  check_ok "DT-504" "BOOLEAN comparison behavior" "SELECT CASE WHEN COUNT(*)>=1 THEN 1 ELSE 0 END AS ok FROM ${table_name} WHERE c_bool IS NOT NULL"

  prep_key="${tmp_dir}/prepared"
  if SQLI_LOG_LEVEL=ERROR SQLI_CLIENT_LOCALE="$client_locale" SQLI_DB_LOCALE="$db_locale" \
      "${PREPARED_PROBE_BIN}" "${host}" "${port}" "${db}" "${user}" "${pass}" "${table_name}" "${server}" "${client_locale}" "${db_locale}" \
      >"${prep_key}.out" 2>"${prep_key}.err"; then
    :
  else
    record_failure "prepared_probe" "${prep_key}"
  fi
  if grep -q "DT-008=PASS" "${prep_key}.out"; then lines+=("| DT-008 | PASS | Prepared string binding coverage |"); pass_count=$((pass_count + 1)); else lines+=("| DT-008 | FAIL | Prepared string binding coverage |"); fail_count=$((fail_count + 1)); fi
  if grep -q "DT-107=PASS" "${prep_key}.out"; then lines+=("| DT-107 | PASS | Prepared Int32/Int64 binding coverage |"); pass_count=$((pass_count + 1)); else lines+=("| DT-107 | FAIL | Prepared Int32/Int64 binding coverage |"); fail_count=$((fail_count + 1)); fi
  if grep -q "DT-207=PASS" "${prep_key}.out"; then lines+=("| DT-207 | PASS | Prepared decimal binding coverage |"); pass_count=$((pass_count + 1)); else lines+=("| DT-207 | FAIL | Prepared decimal binding coverage |"); fail_count=$((fail_count + 1)); fi
  if grep -q "DT-307=PASS" "${prep_key}.out"; then lines+=("| DT-307 | PASS | Prepared float binding coverage |"); pass_count=$((pass_count + 1)); else lines+=("| DT-307 | FAIL | Prepared float binding coverage |"); fail_count=$((fail_count + 1)); fi
  if grep -q "DT-405=PASS" "${prep_key}.out"; then lines+=("| DT-405 | PASS | Prepared date binding coverage |"); pass_count=$((pass_count + 1)); else lines+=("| DT-405 | FAIL | Prepared date binding coverage |"); fail_count=$((fail_count + 1)); fi
else
  lines+=("| SETUP | FAIL | Could not create test table ${table_name}; all DT checks blocked |")
  fail_count=$((fail_count + 1))
fi

if ! run_sql "DROP TABLE ${table_name}" "${tmp_dir}/drop"; then
  record_failure "drop_table" "${tmp_dir}/drop"
fi

total=$((pass_count + fail_count))
pass_rate="0"
if [[ "$total" -gt 0 ]]; then
  pass_rate="$(awk -v p="$pass_count" -v t="$total" 'BEGIN { printf "%.1f", (p*100.0)/t }')"
fi

cat >"$report" <<EOF
# Live Must-Type Coverage Report

- Timestamp (UTC): $(date -u +"%Y-%m-%dT%H:%M:%SZ")
- Target: ${host}:${port} / ${db} (server=${server})
- Locales: client=${client_locale}, db=${db_locale}
- Temporary table: ${table_name}

## Summary

- Total checks: **${total}**
- Passed: **${pass_count}**
- Failed: **${fail_count}**
- Pass rate: **${pass_rate}%**

## Results

| Test ID | Status | Note |
|---|---|---|
$(printf '%s\n' "${lines[@]}")

## Notes

- This suite uses \`sqliconn\` direct SQL execution.
- Prepared-binding IDs (\`DT-008/107/207/307/405\`) are validated through \`sqli_prepared_probe\`.
EOF

rm -rf "${tmp_dir}"
echo "Wrote ${report}"
