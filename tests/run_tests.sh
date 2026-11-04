#!/usr/bin/env bash
# tests/run_tests.sh — Pentest M1 test harness.
#
# For each tests/cases/*.pen:
#   * runs `build/pentest run <case>` capturing stdout/stderr/exit code;
#     stdin is the optional <case>.in file, or /dev/null (end of input)
#     when absent, so input() can never hang the harness;
#   * <name>.out  (required) — exact byte match of stdout;
#   * <name>.err  (optional) — every line must appear as a substring of
#     stderr; when absent, stderr must be empty;
#   * <name>.exit (optional) — expected exit code, default 0.
# Plus five CLI-level extra checks: `check` on a valid case, `check` on a
# compile-error case, `build` + direct execution of a valid case,
# `.pent` source-extension acceptance (run both CLI forms + default naming),
# and a `sha256sum`-less PATH (caching disabled, never SIGPIPE).

set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root" || exit 1

if [ ! -x build/pentest ]; then
  echo "build/pentest not found; running make..."
  if ! make -s; then
    echo "error: failed to build build/pentest" >&2
    exit 1
  fi
fi

# Isolation: private cache dir and private scratch dir per harness run.
export XDG_CACHE_HOME
XDG_CACHE_HOME="$(mktemp -d)"
work="$(mktemp -d)"
trap 'rm -rf "$XDG_CACHE_HOME" "$work"' EXIT

pass=0
fail=0
failure_names=()

report_pass() {
  pass=$((pass + 1))
  echo "PASS $1 $2"
}

report_fail() {
  fail=$((fail + 1))
  failure_names+=("$2")
  echo "FAIL $1 $2"
  if [ -n "${3:-}" ]; then
    printf '%s\n' "$3" | head -n 40
  fi
}

shopt -s nullglob
cases=(tests/cases/*.pen)
if [ "${#cases[@]}" -eq 0 ]; then
  echo "error: no test cases found in tests/cases" >&2
  exit 1
fi

n=0
for pen in "${cases[@]}"; do
  n=$((n + 1))
  base="${pen%.pen}"
  name="$(basename "$base")"

  stdout_file="$work/$name.stdout"
  stderr_file="$work/$name.stderr"

  stdin_file="/dev/null"
  if [ -f "$base.in" ]; then stdin_file="$base.in"; fi

  build/pentest run "$pen" <"$stdin_file" >"$stdout_file" 2>"$stderr_file"
  actual_exit=$?

  detail=""

  expected_exit=0
  if [ -f "$base.exit" ]; then
    expected_exit="$(tr -d '[:space:]' <"$base.exit")"
  fi
  if [ "$actual_exit" -ne "$expected_exit" ]; then
    detail+="  exit code: expected $expected_exit, got $actual_exit"$'\n'
  fi

  if [ ! -f "$base.out" ]; then
    detail+="  missing required sidecar: $base.out"$'\n'
  elif ! cmp -s "$base.out" "$stdout_file"; then
    detail+="  stdout mismatch (expected vs actual):"$'\n'
    detail+="$(diff -u "$base.out" "$stdout_file" | head -n 20 | sed 's/^/    /')"$'\n'
  fi

  if [ -f "$base.err" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
      [ -z "$line" ] && continue
      if ! grep -F -q -- "$line" "$stderr_file"; then
        detail+="  stderr missing expected substring: $line"$'\n'
        detail+="  actual stderr:"$'\n'
        detail+="$(head -n 10 "$stderr_file" | sed 's/^/    /')"$'\n'
      fi
    done <"$base.err"
  elif [ -s "$stderr_file" ]; then
    detail+="  unexpected stderr output:"$'\n'
    detail+="$(head -n 10 "$stderr_file" | sed 's/^/    /')"$'\n'
  fi

  if [ -n "$detail" ]; then
    report_fail "$n" "$name" "$detail"
  else
    report_pass "$n" "$name"
  fi
done

# Extra check (a): `check` on a valid case — exit 0, completely silent.
n=$((n + 1))
valid_case="tests/cases/arithmetic.pen"
detail=""
build/pentest check "$valid_case" >"$work/cv.out" 2>"$work/cv.err"
cv_exit=$?
if [ "$cv_exit" -ne 0 ]; then
  detail+="  check exited $cv_exit, expected 0"$'\n'
fi
if [ -s "$work/cv.out" ]; then
  detail+="  check wrote to stdout:"$'\n'
  detail+="$(head -n 10 "$work/cv.out" | sed 's/^/    /')"$'\n'
fi
if [ -s "$work/cv.err" ]; then
  detail+="  check wrote to stderr:"$'\n'
  detail+="$(head -n 10 "$work/cv.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "check_valid" "$detail"
else
  report_pass "$n" "check_valid"
fi

# Extra check (b): `check` on a compile-error case — nonzero exit.
n=$((n + 1))
error_case="tests/cases/err_undefined_function.pen"
detail=""
if build/pentest check "$error_case" >"$work/ce.out" 2>"$work/ce.err"; then
  detail="  check exited 0, expected nonzero"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "check_error" "$detail"
else
  report_pass "$n" "check_error"
fi

# Extra check (c): `build` a valid case, run the produced binary directly.
n=$((n + 1))
detail=""
if build/pentest build "$valid_case" -o "$work/x.pentest" \
    >"$work/br.out" 2>"$work/br.err"; then
  expected_build_exit=0
  if [ -f "${valid_case%.pen}.exit" ]; then
    expected_build_exit="$(tr -d '[:space:]' <"${valid_case%.pen}.exit")"
  fi
  "$work/x.pentest" >"$work/br.run.out" 2>"$work/br.run.err"
  br_exit=$?
  if [ "$br_exit" -ne "$expected_build_exit" ]; then
    detail+="  built binary exit: expected $expected_build_exit, got $br_exit"$'\n'
  fi
  if ! cmp -s "${valid_case%.pen}.out" "$work/br.run.out"; then
    detail+="  built binary stdout mismatch (expected vs actual):"$'\n'
    detail+="$(diff -u "${valid_case%.pen}.out" "$work/br.run.out" | head -n 20 | sed 's/^/    /')"$'\n'
  fi
else
  detail+="  build failed:"$'\n'
  detail+="$(head -n 10 "$work/br.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "build_roundtrip" "$detail"
else
  report_pass "$n" "build_roundtrip"
fi

# Extra check (d): `.pent` source extension — `pentest run` and the bare
# `pentest` form both execute a `.pent` file, and `build` maps
# `script.pent` to the default output `script.pentest`.
n=$((n + 1))
detail=""
cp "$valid_case" "$work/script.pent"
expected_out="${valid_case%.pen}.out"
build/pentest run "$work/script.pent" >"$work/pent1.out" 2>"$work/pent1.err"
p1_exit=$?
if [ "$p1_exit" -ne 0 ]; then
  detail+="  'pentest run script.pent' exited $p1_exit, expected 0"$'\n'
  detail+="$(head -n 10 "$work/pent1.err" | sed 's/^/    /')"$'\n'
fi
if ! cmp -s "$expected_out" "$work/pent1.out"; then
  detail+="  'pentest run script.pent' stdout mismatch:"$'\n'
  detail+="$(diff -u "$expected_out" "$work/pent1.out" | head -n 20 | sed 's/^/    /')"$'\n'
fi
build/pentest "$work/script.pent" >"$work/pent2.out" 2>"$work/pent2.err"
p2_exit=$?
if [ "$p2_exit" -ne 0 ]; then
  detail+="  bare 'pentest script.pent' exited $p2_exit, expected 0"$'\n'
  detail+="$(head -n 10 "$work/pent2.err" | sed 's/^/    /')"$'\n'
fi
if ! cmp -s "$expected_out" "$work/pent2.out"; then
  detail+="  bare 'pentest script.pent' stdout mismatch:"$'\n'
  detail+="$(diff -u "$expected_out" "$work/pent2.out" | head -n 20 | sed 's/^/    /')"$'\n'
fi
(
  cd "$work" || exit 1
  "$repo_root/build/pentest" build script.pent
) >"$work/pent3.out" 2>"$work/pent3.err"
if [ ! -x "$work/script.pentest" ]; then
  detail+="  'build script.pent' did not produce script.pentest"$'\n'
  detail+="$(head -n 10 "$work/pent3.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "pent_extension" "$detail"
else
  report_pass "$n" "pent_extension"
fi

# Extra check (e): a PATH without `sha256sum` must disable caching and
# still build+run (SPEC §10: "If `sha256sum` is unavailable, caching is
# disabled and every `run` compiles"). Regression: the driver died with
# SIGPIPE (exit 141) mid-compile and left an empty `pentest-hash-*`
# digest file behind.
n=$((n + 1))
detail=""
mkdir -p "$work/onlybin" "$work/notmp" "$work/nocache"
ln -sf "$(command -v clang)" "$work/onlybin/clang"
ln -sf "$(command -v ld)" "$work/onlybin/ld"
PATH="$work/onlybin" TMPDIR="$work/notmp" XDG_CACHE_HOME="$work/nocache" \
  build/pentest run "$valid_case" >"$work/nh.out" 2>"$work/nh.err"
nh_exit=$?
if [ "$nh_exit" -ne 0 ]; then
  detail+="  run without sha256sum exited $nh_exit (141 = SIGPIPE death)"$'\n'
  detail+="$(head -n 10 "$work/nh.err" | sed 's/^/    /')"$'\n'
fi
if ! cmp -s "${valid_case%.pen}.out" "$work/nh.out"; then
  detail+="  run without sha256sum stdout mismatch:"$'\n'
  detail+="$(diff -u "${valid_case%.pen}.out" "$work/nh.out" | head -n 20 | sed 's/^/    /')"$'\n'
fi
leftover_hash=( "$work/notmp"/pentest-hash-* )
if [ "${#leftover_hash[@]}" -gt 0 ]; then
  detail+="  hash scratch left behind: ${leftover_hash[*]}"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "no_sha256sum" "$detail"
else
  report_pass "$n" "no_sha256sum"
fi

# Extra check (f): `check` on a valid multi-file program — silent success.
n=$((n + 1))
detail=""
build/pentest check tests/cases/imp_check.pen >"$work/ci.out" 2>"$work/ci.err"
ci_exit=$?
if [ "$ci_exit" -ne 0 ]; then
  detail+="  check exited $ci_exit, expected 0"$'\n'
fi
if [ -s "$work/ci.out" ] || [ -s "$work/ci.err" ]; then
  detail+="  check was not silent:"$'\n'
  detail+="$(head -n 10 "$work/ci.out" "$work/ci.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "check_imports_valid" "$detail"
else
  report_pass "$n" "check_imports_valid"
fi

# Extra check (g): `check` validates unused imported bodies. Run/build prune
# them (imp_unused passes), but strict mode must report the error.
n=$((n + 1))
detail=""
if build/pentest check tests/cases/imp_unused.pen >"$work/cu.out" 2>"$work/cu.err"; then
  detail="  check exited 0, expected failure for the unused broken body"$'\n'
fi
if ! grep -F -q "lib/broken.pen" "$work/cu.err"; then
  detail+="  diagnostic did not name the imported file:"$'\n'
  detail+="$(head -n 10 "$work/cu.err" | sed 's/^/    /')"$'\n'
fi
if ! grep -F -q "missing_name" "$work/cu.err"; then
  detail+="  diagnostic did not name the unused body's error:"$'\n'
  detail+="$(head -n 10 "$work/cu.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "check_imports_strict" "$detail"
else
  report_pass "$n" "check_imports_strict"
fi

echo "passed: $pass failed: $fail"
if [ "$fail" -gt 0 ]; then
  echo "failed cases:"
  i=0
  while [ "$i" -lt "${#failure_names[@]}" ]; do
    echo "  ${failure_names[$i]}"
    i=$((i + 1))
  done
  exit 1
fi
exit 0
