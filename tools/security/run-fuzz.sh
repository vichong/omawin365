#!/usr/bin/env bash
# Fresh builds and synthetic fixtures only; deliberately no cleanup or installs.
set -euo pipefail
umask 077
base=${1:-/tmp/omawin365-phase2.anFsS5}
seconds=${2:-30}
seed=20261002
[[ $seconds =~ ^[0-9]+$ ]] && ((seconds >= 1 && seconds <= 300)) || {
    echo 'Budget must be an integer from 1 to 300 seconds per target.' >&2; exit 2;
}
[[ -d $base && ! -L $base && $(stat -c %u "$base") == "$(id -u)" && $(stat -c %a "$base") == 700 ]] || {
    echo 'Provide an existing, owned, non-symlink 0700 evidence directory.' >&2; exit 2;
}
base=$(cd "$base" && pwd -P)
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)
run=$(mktemp -d "$base/fuzz.XXXXXXXX")
printf 'Evidence: %s\n' "$run"
exec > >(tee "$run/driver.log") 2>&1
printf 'Invocation: bash %q %q %q\n' "$repo/tools/security/run-fuzz.sh" "$base" "$seconds"
printf 'Seed: %s; UTC start: %s\n' "$seed" "$(date -u +%FT%TZ)"
cxx=${CXX:-clang++}
"$cxx" --version > "$run/compiler-version.txt"
pkg-config --modversion Qt6Core Qt6Network > "$run/qt-versions.txt"
git -C "$repo" rev-parse HEAD > "$run/revision.txt"
sha256sum "$repo"/src/{promptparser,profilestore,rdpprofile}.{cpp,h} "$repo"/tools/security/* "$repo/tests/synthetic_profile.h" > "$run/source-sha256.txt"
mkdir "$run/build" "$run/crashes-promptparser" "$run/crashes-profiles" "$run/fixture"
export TMPDIR="$run/fixture"
export HOME="$run/fixture"
export XDG_DATA_HOME="$run/fixture/data"
export XDG_CONFIG_HOME="$run/fixture/config"
export XDG_CACHE_HOME="$run/fixture/cache"
export XDG_RUNTIME_DIR="$run/fixture"
export OMAWIN365_FUZZ_FIXTURE="$run/fixture"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:symbolize=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export LC_ALL=C
printf 'ASAN_OPTIONS=%s\nUBSAN_OPTIONS=%s\n' "$ASAN_OPTIONS" "$UBSAN_OPTIONS"
python3 -I "$repo/tools/security/seed_corpus.py" "$run"
read -r -a qt_cflags <<< "$(pkg-config --cflags Qt6Core Qt6Network)"
read -r -a qt_libs <<< "$(pkg-config --libs Qt6Core Qt6Network)"
moc="$(pkg-config --variable=libexecdir Qt6Core)/moc"
common=(-std=c++20 -O1 -g -fPIC -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined
        -fno-sanitize-recover=all -Wall -Wextra -I"$repo/src" "${qt_cflags[@]}")
# Sequential compiler invocations (never more than -j4). Build commands preserved.
(
    set -x
    "$cxx" "${common[@]}" "$repo/tools/security/fuzz_promptparser.cpp" \
        "$repo/src/promptparser.cpp" "$repo/src/oauthcontract.cpp" "${qt_libs[@]}" -o "$run/build/fuzz_promptparser"
    "$moc" "$repo/src/profilestore.h" -o "$run/build/moc_profilestore.cpp"
    "$cxx" "${common[@]}" "$repo/tools/security/fuzz_profiles.cpp" \
        "$repo/src/profilestore.cpp" "$repo/src/rdpprofile.cpp" "$run/build/moc_profilestore.cpp" \
        "${qt_libs[@]}" -o "$run/build/fuzz_profiles"
) > "$run/build.log" 2>&1
printf 'target\texit_status\n' > "$run/status.tsv"
failed=0
for target in promptparser profiles; do
    max_len=65540
    [[ $target != profiles ]] || max_len=1048578
    command=("$run/build/fuzz_$target" "$run/corpus-$target" "-dict=$run/$target.dict"
             "-artifact_prefix=$run/crashes-$target/" "-seed=$seed" "-max_total_time=$seconds"
             "-max_len=$max_len" -timeout=5 -rss_limit_mb=1024 -malloc_limit_mb=512
             -print_final_stats=1)
    printf '%q ' "${command[@]}" >> "$run/commands.sh"
    printf '\n' >> "$run/commands.sh"
    printf 'Running %s (%s seconds)\n' "$target" "$seconds"
    status=0
    "${command[@]}" > "$run/$target.log" 2>&1 || status=$?
    printf '%s\t%s\n' "$target" "$status" >> "$run/status.tsv"
    ((status == 0)) || failed=1
    tail -n 12 "$run/$target.log"
done
python3 -I - "$run" "$seconds" "$seed" <<'PY'
import pathlib
import re
import sys
root = pathlib.Path(sys.argv[1])
rows = ["# Bounded synthetic fuzz run", "", f"Evidence: `{root}`", "",
        f"Seed: {sys.argv[3]}; budget: {sys.argv[2]} seconds per target (libFuzzer wall-clock stop).",
        "ASan + UBSan + libFuzzer; leak detection enabled; no sanitizer suppression/exclusion flags.", "",
        "| Target | Exit | Executions | Final cov / ft | Corpus files / bytes | Artifacts |",
        "|---|---:|---:|---|---|---:|"]
for line in (root / "status.tsv").read_text().splitlines()[1:]:
    target, status = line.split("\t")
    log = (root / f"{target}.log").read_text(errors="replace")
    executions = re.findall(r"stat::number_of_executed_units:\s*(\d+)", log)
    coverage = re.findall(r"cov: (\d+) ft: (\d+)", log)
    files = list((root / f"corpus-{target}").iterdir())
    artifacts = list((root / f"crashes-{target}").iterdir())
    rows.append(f"| {target} | {status} | {executions[-1] if executions else 'not reported'} | "
                f"{' / '.join(coverage[-1]) if coverage else 'not reported'} | "
                f"{len(files)} / {sum(p.stat().st_size for p in files)} | {len(artifacts)} |")
rows += ["", "## Provenance and limits", "",
         "Exact build commands: `build.log`; exact fuzz commands: `commands.sh`; sanitizer environment: `driver.log`.",
         "Versions: `compiler-version.txt`, `qt-versions.txt`; revision: `revision.txt`; input source hashes: `source-sha256.txt`.",
         "Full output: `promptparser.log`, `profiles.log`; statuses: `status.tsv`. All retained artifacts are synthetic.",
         "Qt/system shared libraries were not rebuilt or coverage-instrumented. Counters are libFuzzer edges/features, not source coverage percentages.",
         "Bounded fuzzing cannot prove absence of bugs or validate stock FreeRDP semantics, credential policy, filesystem races, browser behavior or live sessions.",
         "Fixed PRNG seed and deterministic starting corpus/schedules enable replay; wall-clock budgets do not guarantee identical execution counts/final corpus.",
         "Any nonzero status/artifact requires investigation; a timeout/resource failure or harness assertion is not by itself a confirmed production vulnerability."]
(root / "RESULTS.md").write_text("\n".join(rows) + "\n")
print((root / "RESULTS.md").read_text())
PY
printf 'UTC end: %s\n' "$(date -u +%FT%TZ)"
exit "$failed"
