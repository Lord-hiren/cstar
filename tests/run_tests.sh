#!/usr/bin/env bash
# tests/run_tests.sh — cstar test harness.
#
# For each tests/cases/*.pen:
#   * runs `build/cstar run <case>` capturing stdout/stderr/exit code;
#     stdin is the optional <case>.in file, or /dev/null (end of input)
#     when absent, so input() can never hang the harness;
#   * <name>.out  (required) — exact byte match of stdout;
#   * <name>.err  (optional) — every line must appear as a substring of
#     stderr; when absent, stderr must be empty;
#   * <name>.exit (optional) — expected exit code, default 0.
# Plus eleven CLI-level extra checks: `check` on a valid case, `check` on a
# compile-error case, `build` + direct execution of a valid case, `.cst`
# acceptance (run, bare form, default build name), the legacy `.pen`/`.pent`
# extensions, a `sha256sum`-less PATH (caching disabled, never SIGPIPE),
# strict and valid `check` on imports, the legacy `pentest` alias, the
# `index.cst` default entry, and CSTAR_PATH / legacy PENTEST_PATH.

set -u

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root" || exit 1

cstar="build/cstar"
if [ ! -x "$cstar" ] || [ ! -x build/pentest ]; then
  echo "$cstar not found; running make..."
  if ! make -s; then
    echo "error: failed to build $cstar" >&2
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

  "$cstar" run "$pen" <"$stdin_file" >"$stdout_file" 2>"$stderr_file"
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
"$cstar" check "$valid_case" >"$work/cv.out" 2>"$work/cv.err"
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
if "$cstar" check "$error_case" >"$work/ce.out" 2>"$work/ce.err"; then
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
if "$cstar" build "$valid_case" -o "$work/x" \
    >"$work/br.out" 2>"$work/br.err"; then
  expected_build_exit=0
  if [ -f "${valid_case%.pen}.exit" ]; then
    expected_build_exit="$(tr -d '[:space:]' <"${valid_case%.pen}.exit")"
  fi
  "$work/x" >"$work/br.run.out" 2>"$work/br.run.err"
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

expected_out="${valid_case%.pen}.out"

# check_source_ext DIR FILE LABEL — `cstar run FILE` and the bare `cstar FILE`
# form both print the valid case's output, and `cstar build FILE` (run in DIR)
# writes the default output DIR/script (the basename without its extension).
check_source_ext() {
  local dir="$1" file="$2" label="$3" out_name="$4"
  mkdir -p "$dir"
  cp "$valid_case" "$dir/$file"
  "$cstar" run "$dir/$file" >"$dir/r1.out" 2>"$dir/r1.err"
  local e1=$?
  if [ "$e1" -ne 0 ] || ! cmp -s "$expected_out" "$dir/r1.out"; then
    detail+="  [$label] 'cstar run $file' exited $e1 or printed the wrong output"$'\n'
    detail+="$(head -n 10 "$dir/r1.err" | sed 's/^/    /')"$'\n'
  fi
  "$cstar" "$dir/$file" >"$dir/r2.out" 2>"$dir/r2.err"
  local e2=$?
  if [ "$e2" -ne 0 ] || ! cmp -s "$expected_out" "$dir/r2.out"; then
    detail+="  [$label] bare 'cstar $file' exited $e2 or printed the wrong output"$'\n'
  fi
  (
    cd "$dir" || exit 1
    "$repo_root/$cstar" build "$file"
  ) >"$dir/b.out" 2>"$dir/b.err"
  if [ ! -x "$dir/$out_name" ]; then
    detail+="  [$label] 'cstar build $file' did not produce ./$out_name"$'\n'
    detail+="$(head -n 10 "$dir/b.err" | sed 's/^/    /')"$'\n'
  elif ! "$dir/$out_name" | cmp -s "$expected_out" -; then
    detail+="  [$label] built ./$out_name printed the wrong output"$'\n'
  fi
}

# Extra check (d): `.cst` source extension — run, bare form, and `build`
# maps `script.cst` to the default output `./script` (no extension).
n=$((n + 1))
detail=""
check_source_ext "$work/ext_cst" "script.cst" ".cst" "script"
if [ -n "$detail" ]; then
  report_fail "$n" "cst_extension" "$detail"
else
  report_pass "$n" "cst_extension"
fi

# Extra check (d2): the legacy `.pen` / `.pent` extensions still work (until
# v0.4) and build to the same extension-free default name.
n=$((n + 1))
detail=""
check_source_ext "$work/ext_pen" "script.pen" ".pen" "script"
check_source_ext "$work/ext_pent" "script.pent" ".pent" "script"
if [ -n "$detail" ]; then
  report_fail "$n" "legacy_extensions" "$detail"
else
  report_pass "$n" "legacy_extensions"
fi

# Extra check (e): a PATH without `sha256sum` must disable caching and
# still build+run (SPEC §10: "If `sha256sum` is unavailable, caching is
# disabled and every `run` compiles"). Regression: the driver died with
# SIGPIPE (exit 141) mid-compile and left an empty `cstar-hash-*`
# (formerly `pentest-hash-*`) digest file behind.
n=$((n + 1))
detail=""
mkdir -p "$work/onlybin" "$work/notmp" "$work/nocache"
ln -sf "$(command -v clang)" "$work/onlybin/clang"
ln -sf "$(command -v ld)" "$work/onlybin/ld"
PATH="$work/onlybin" TMPDIR="$work/notmp" XDG_CACHE_HOME="$work/nocache" \
  "$cstar" run "$valid_case" >"$work/nh.out" 2>"$work/nh.err"
nh_exit=$?
if [ "$nh_exit" -ne 0 ]; then
  detail+="  run without sha256sum exited $nh_exit (141 = SIGPIPE death)"$'\n'
  detail+="$(head -n 10 "$work/nh.err" | sed 's/^/    /')"$'\n'
fi
if ! cmp -s "${valid_case%.pen}.out" "$work/nh.out"; then
  detail+="  run without sha256sum stdout mismatch:"$'\n'
  detail+="$(diff -u "${valid_case%.pen}.out" "$work/nh.out" | head -n 20 | sed 's/^/    /')"$'\n'
fi
leftover_hash=( "$work/notmp"/cstar-hash-* "$work/notmp"/pentest-hash-* )
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
"$cstar" check tests/cases/imp_check.pen >"$work/ci.out" 2>"$work/ci.err"
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
if "$cstar" check tests/cases/imp_unused.pen >"$work/cu.out" 2>"$work/cu.err"; then
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

# Extra check (h): the legacy `pentest` command (an alias of the same driver,
# kept until v0.4) reports the same version and runs programs.
n=$((n + 1))
detail=""
if [ "$(build/pentest --version 2>&1)" != "$("$cstar" --version 2>&1)" ]; then
  detail+="  'pentest --version' differs from 'cstar --version'"$'\n'
fi
case "$("$cstar" --version 2>&1)" in
  "cstar "*) ;;
  *) detail+="  'cstar --version' does not start with 'cstar '"$'\n' ;;
esac
build/pentest run "$valid_case" >"$work/al.out" 2>"$work/al.err"
if [ "$?" -ne 0 ] || ! cmp -s "$expected_out" "$work/al.out"; then
  detail+="  'pentest run' (alias) failed or printed the wrong output"$'\n'
  detail+="$(head -n 10 "$work/al.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "cli_alias" "$detail"
else
  report_pass "$n" "cli_alias"
fi

# Extra check (i): `index.cst` is the default entry — `run`, `check` and
# `build` without a file use ./index.cst (build writes ./index); without
# ./index.cst it is a usage error (exit 2).
n=$((n + 1))
detail=""
mkdir -p "$work/idx" "$work/noidx"
cp "$valid_case" "$work/idx/index.cst"
(cd "$work/idx" && "$repo_root/$cstar" run) >"$work/idx/r.out" 2>"$work/idx/r.err"
if [ "$?" -ne 0 ] || ! cmp -s "$expected_out" "$work/idx/r.out"; then
  detail+="  'cstar run' (no file) did not run ./index.cst"$'\n'
  detail+="$(head -n 10 "$work/idx/r.err" | sed 's/^/    /')"$'\n'
fi
(cd "$work/idx" && "$repo_root/$cstar" check) >"$work/idx/c.out" 2>"$work/idx/c.err"
if [ "$?" -ne 0 ] || [ -s "$work/idx/c.out" ] || [ -s "$work/idx/c.err" ]; then
  detail+="  'cstar check' (no file) was not a silent success"$'\n'
fi
(cd "$work/idx" && "$repo_root/$cstar" build) >"$work/idx/b.out" 2>"$work/idx/b.err"
if [ ! -x "$work/idx/index" ] || ! "$work/idx/index" | cmp -s "$expected_out" -; then
  detail+="  'cstar build' (no file) did not produce a working ./index"$'\n'
  detail+="$(head -n 10 "$work/idx/b.err" | sed 's/^/    /')"$'\n'
fi
(cd "$work/noidx" && "$repo_root/$cstar" run) >"$work/noidx/r.out" 2>"$work/noidx/r.err"
noidx_exit=$?
if [ "$noidx_exit" -ne 2 ] || ! grep -F -q "index.cst does not exist" "$work/noidx/r.err"; then
  detail+="  'cstar run' without ./index.cst: expected exit 2 + message, got $noidx_exit"$'\n'
  detail+="$(head -n 3 "$work/noidx/r.err" | sed 's/^/    /')"$'\n'
fi
if [ -n "$detail" ]; then
  report_fail "$n" "index_entry" "$detail"
else
  report_pass "$n" "index_entry"
fi

# Extra check (j): module search roots — $CSTAR_PATH, then the legacy
# $PENTEST_PATH (until v0.4); CSTAR_PATH wins when both are set.
n=$((n + 1))
detail=""
mkdir -p "$work/mp/via_cstar" "$work/mp/via_pentest"
printf 'tag():\n    return "cstar"\n' >"$work/mp/via_cstar/mpmod.cst"
printf 'tag():\n    return "pentest"\n' >"$work/mp/via_pentest/mpmod.cst"
printf 'import mpmod\nprint(mpmod.tag())\n' >"$work/mp/main.cst"
mp1="$(CSTAR_PATH="$work/mp/via_cstar" "$cstar" run "$work/mp/main.cst" 2>&1)"
mp2="$(PENTEST_PATH="$work/mp/via_pentest" "$cstar" run "$work/mp/main.cst" 2>&1)"
mp3="$(CSTAR_PATH="$work/mp/via_cstar" PENTEST_PATH="$work/mp/via_pentest" \
  "$cstar" run "$work/mp/main.cst" 2>&1)"
[ "$mp1" = "cstar" ] || detail+="  CSTAR_PATH import: expected 'cstar', got '$mp1'"$'\n'
[ "$mp2" = "pentest" ] || detail+="  PENTEST_PATH import: expected 'pentest', got '$mp2'"$'\n'
[ "$mp3" = "cstar" ] || detail+="  CSTAR_PATH must be searched first: got '$mp3'"$'\n'
if [ -n "$detail" ]; then
  report_fail "$n" "module_path" "$detail"
else
  report_pass "$n" "module_path"
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
