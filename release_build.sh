#!/usr/bin/env bash
#
# Run the release verification flow:
# 1. verify clean git worktree
# 2. configure/build/test the project
# 3. increment the project version in CMakeLists.txt
# 4. build the Debian package via tools/build_deb.sh
# 5. create a git tag for the new version
#
# Usage:
#   ./release_build.sh [deb_output_dir]
#
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT_DIR="${SCRIPT_DIR}"
readonly CMAKE_FILE="${ROOT_DIR}/CMakeLists.txt"
readonly TEST_BUILD_DIR="${ROOT_DIR}/build-release-check"
readonly BUILD_DEB_SCRIPT="${ROOT_DIR}/tools/build_deb.sh"

RESTORE_CMAKE_FILE=0
ORIGINAL_CMAKE_CONTENT=""

die() {
  echo "release_build.sh: $*" >&2
  exit 1
}

restore_cmake_file() {
  if [[ ${RESTORE_CMAKE_FILE} -eq 1 ]]; then
    printf '%s' "${ORIGINAL_CMAKE_CONTENT}" >"${CMAKE_FILE}"
  fi
}

on_exit() {
  local exit_code=$?

  if [[ ${exit_code} -ne 0 ]]; then
    restore_cmake_file
  fi

  exit "${exit_code}"
}

trap on_exit EXIT

require_clean_worktree() {
  local status_output

  status_output="$(git status --short)"
  if [[ -n "${status_output}" ]]; then
    echo "${status_output}" >&2
    die "git worktree is not clean"
  fi
}

extract_version() {
  local version_line
  local version

  version_line="$(grep -E '^project\([^)]* VERSION [0-9]+\.[0-9]+\.[0-9]+' "${CMAKE_FILE}" || true)"
  if [[ -z "${version_line}" ]]; then
    die "could not parse version from ${CMAKE_FILE}"
  fi

  version="$(sed -E 's/^project\([^)]* VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/' <<<"${version_line}")"
  if [[ ! ${version} =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    die "parsed invalid version from ${CMAKE_FILE}: ${version}"
  fi

  printf '%s' "${version}"
}

increment_patch_version() {
  local version="$1"
  local major minor patch

  IFS='.' read -r major minor patch <<<"${version}"
  patch=$((patch + 1))

  printf '%s.%s.%s' "${major}" "${minor}" "${patch}"
}

update_cmake_version() {
  local old_version="$1"
  local new_version="$2"
  local updated_content

  ORIGINAL_CMAKE_CONTENT="$(<"${CMAKE_FILE}")"
  updated_content="${ORIGINAL_CMAKE_CONTENT/VERSION ${old_version}/VERSION ${new_version}}"

  if [[ "${updated_content}" == "${ORIGINAL_CMAKE_CONTENT}" ]]; then
    die "failed to update version in ${CMAKE_FILE}"
  fi

  printf '%s' "${updated_content}" >"${CMAKE_FILE}"
  RESTORE_CMAKE_FILE=1
}

run_tests() {
  cmake -S "${ROOT_DIR}" -B "${TEST_BUILD_DIR}"
  cmake --build "${TEST_BUILD_DIR}" -j"$(nproc)"
  ctest --test-dir "${TEST_BUILD_DIR}" --output-on-failure
}

build_deb_package() {
  local output_dir="$1"

  if [[ -n "${output_dir}" ]]; then
    "${BUILD_DEB_SCRIPT}" "${output_dir}"
  else
    "${BUILD_DEB_SCRIPT}"
  fi
}

create_version_tag() {
  local version="$1"
  local tag_name="v${version}"

  if git rev-parse -q --verify "refs/tags/${tag_name}" >/dev/null; then
    die "git tag ${tag_name} already exists"
  fi

  git tag -a "${tag_name}" -m "Release ${tag_name}"
}

main() {
  local deb_output_dir="${1:-}"
  local current_version
  local next_version

  require_clean_worktree

  echo "==> Running release test suite"
  run_tests

  current_version="$(extract_version)"
  next_version="$(increment_patch_version "${current_version}")"

  echo "==> Incrementing version: ${current_version} -> ${next_version}"
  update_cmake_version "${current_version}" "${next_version}"

  echo "==> Building Debian package for ${next_version}"
  build_deb_package "${deb_output_dir}"

  echo "==> Tagging repository with v${next_version}"
  create_version_tag "${next_version}"

  RESTORE_CMAKE_FILE=0
  echo "==> Release preparation finished for v${next_version}"
}

main "$@"
