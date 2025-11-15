#!/usr/bin/env bash
# Formats all C/C++ files under repo-root/src and continues on errors.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src_dir="$repo_root/src"

cmd=(clang-format-18 --verbose -i)
if ! command -v "${cmd[0]}" >/dev/null 2>&1; then
  echo "ERROR: clang-format-18 not found in PATH" >&2
  exit 2
fi

[ -d "$src_dir" ] || { echo "ERROR: src directory not found: $src_dir" >&2; exit 2; }

# collect files (follow symlinks if supported)
find_prefix=()
if find -L "$src_dir" -maxdepth 0 >/dev/null 2>&1; then
  find_prefix=(-L)
fi

mapfile -d '' -t files < <(
  find "${find_prefix[@]}" "$src_dir" -type f \
    \( -iname '*.c' -o -iname '*.cc' -o -iname '*.cpp' -o -iname '*.cxx' -o -iname '*.h' -o -iname '*.hh' -o -iname '*.hpp' \) \
    -print0
)

if [ "${#files[@]}" -eq 0 ]; then
  echo "No C/C++ files found under: $src_dir" >&2
  exit 0
fi

echo "Found ${#files[@]} files to format."
failures=0
formatted=0

for f in "${files[@]}"; do
  [ -r "$f" ] || { echo "SKIP (unreadable): $f" >&2; ((failures++)); continue; }
  # run clang-format but DO NOT let a failure abort the loop
  if "${cmd[@]}" "$f"; then
    ((formatted++))
  else
    echo "FAILED: clang-format on: $f" >&2
    # capture last exit for reporting but continue
    ((failures++))
  fi
done

echo "Result: formatted=${formatted}, failed=${failures}, total=${#files[@]}"
# exit non-zero if any failures to let CI / caller detect problems
if [ "$failures" -ne 0 ]; then
  exit 1
fi
exit 0


