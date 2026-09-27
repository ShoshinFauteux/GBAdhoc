#!/usr/bin/env bash
# Dependency-light source checks for the scripts that support the PSP build.
# C/C++ diagnostics remain the responsibility of the host and PSP build gates.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

printf '%s\n' '== shell syntax =='
while IFS= read -r -d '' script; do
  bash -n "$script"
  printf '  ok %s\n' "${script#"$ROOT"/}"
done < <(find tools -type f -name '*.sh' -print0 | sort -z)

printf '%s\n' '== Python syntax =='
python3 - <<'PY'
import ast
from pathlib import Path

for path in sorted(Path("tools").rglob("*.py")):
    ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    print(f"  ok {path}")
PY

printf '%s\n' '== whitespace/errors in tracked and new files =='
if git rev-parse --git-dir >/dev/null 2>&1; then
  git -c core.autocrlf=false -c core.safecrlf=false diff --check
  while IFS= read -r -d '' path; do
    new_file_check="$(git -c core.autocrlf=false -c core.safecrlf=false \
      diff --no-index --check -- /dev/null "$path" 2>&1 || true)"
    if [[ -n "$new_file_check" ]]; then
      printf '%s\n' "$new_file_check" >&2
      exit 1
    fi
  done < <(git ls-files --others --exclude-standard -z)
elif [[ -f .git ]]; then
  # A Windows linked worktree stores a Windows path in its .git pointer. When
  # this script runs under WSL, translate that pointer for Git's Linux build.
  gitdir_pointer="$(sed -n 's/^gitdir: //p' .git)"
  if [[ "$gitdir_pointer" =~ ^([A-Za-z]):/(.*)$ ]]; then
    drive="${BASH_REMATCH[1],,}"
    gitdir="/mnt/$drive/${BASH_REMATCH[2]}"
    GIT_DIR="$gitdir" GIT_WORK_TREE="$ROOT" \
      git -c core.autocrlf=false -c core.safecrlf=false diff --check
    while IFS= read -r -d '' path; do
      new_file_check="$(GIT_DIR="$gitdir" GIT_WORK_TREE="$ROOT" \
        git -c core.autocrlf=false -c core.safecrlf=false \
        diff --no-index --check -- /dev/null "$path" 2>&1 || true)"
      if [[ -n "$new_file_check" ]]; then
        printf '%s\n' "$new_file_check" >&2
        exit 1
      fi
    done < <(GIT_DIR="$gitdir" GIT_WORK_TREE="$ROOT" \
      git ls-files --others --exclude-standard -z)
  else
    echo "cannot resolve linked-worktree Git pointer: $gitdir_pointer" >&2
    exit 2
  fi
else
  echo 'not inside a Git worktree' >&2
  exit 2
fi

if command -v shellcheck >/dev/null 2>&1; then
  printf '%s\n' '== ShellCheck =='
  find tools -type f -name '*.sh' -print0 | xargs -0 shellcheck
else
  printf '%s\n' '  ShellCheck unavailable; bash -n was used for shell syntax.'
fi

printf '%s\n' 'Source checks passed. C/C++ warnings are checked by the host/PSP builds.'
