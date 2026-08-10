#!/usr/bin/env bash
# Fetches web-platform-tests into third_party/wpt, pinned and sparse.
#
# WPT is not vendored. It is 1.5 million files and it moves every day; a copy in
# this repository would be a fork nobody maintains, and its churn would drown
# every real diff. What *is* tracked is the revision (tools/wpt/REVISION) and
# which directories are in scope (tools/wpt/directories.txt), which is enough
# for two machines to run the same tests and get the same answer.
#
#   tools/wpt/fetch.sh              # clone or update to the pinned revision
#   tools/wpt/fetch.sh --latest     # move the pin to origin/master and update it
#
# A partial clone (--filter=blob:none) plus sparse checkout is what makes this
# tolerable: git downloads the blobs for the listed directories and no others.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
wpt_dir="${repo_root}/third_party/wpt"
revision_file="${repo_root}/tools/wpt/REVISION"
directories_file="${repo_root}/tools/wpt/directories.txt"
remote="https://github.com/web-platform-tests/wpt.git"

want_latest=0
for argument in "$@"; do
  case "${argument}" in
    --latest) want_latest=1 ;;
    -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: ${argument}" >&2; exit 2 ;;
  esac
done

# The sparse patterns, comments and blank lines stripped.
mapfile -t patterns < <(grep -v '^[[:space:]]*#' "${directories_file}" | grep -v '^[[:space:]]*$')
if [[ ${#patterns[@]} -eq 0 ]]; then
  echo "tools/wpt/directories.txt lists no directories" >&2
  exit 1
fi

if [[ ! -d "${wpt_dir}/.git" ]]; then
  echo "==> cloning ${remote} (blobless, sparse) into ${wpt_dir}"
  mkdir -p "$(dirname "${wpt_dir}")"
  git clone --filter=blob:none --no-checkout --sparse "${remote}" "${wpt_dir}"
fi

cd "${wpt_dir}"
git sparse-checkout init --no-cone
printf '%s\n' "${patterns[@]}" | git sparse-checkout set --stdin --no-cone

if [[ "${want_latest}" -eq 1 ]]; then
  echo "==> fetching origin/master"
  git fetch --filter=blob:none origin master
  revision="$(git rev-parse FETCH_HEAD)"
  echo "${revision}" > "${revision_file}"
  echo "==> pinned tools/wpt/REVISION to ${revision}"
else
  if [[ ! -f "${revision_file}" ]]; then
    echo "no ${revision_file}; run with --latest once to create the pin" >&2
    exit 1
  fi
  revision="$(tr -d '[:space:]' < "${revision_file}")"
  if ! git cat-file -e "${revision}^{commit}" 2>/dev/null; then
    echo "==> fetching pinned revision ${revision}"
    git fetch --filter=blob:none origin "${revision}"
  fi
fi

echo "==> checking out ${revision}"
git checkout --detach "${revision}" >/dev/null 2>&1

count="$(git ls-files | wc -l)"
echo "==> ${wpt_dir} at ${revision} (${count} files in scope)"
