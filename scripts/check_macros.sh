#!/usr/bin/env bash
#  Compile-check every macro under `macros/` using ROOT's ACLiC.
#
#  ACLiC (`.L file.cpp+`) shells out to the system compiler and links
#  against the framework library — so this catches header / API
#  regressions that the interpreted `.x` path can mask via lazy
#  evaluation.
#
#  Why this script and not the build:
#    Macros aren't first-class CMake targets — they're standalone .cpp
#    files driven by ROOT.  This is the only QA gate that exercises
#    `#include "../lib_loader.h"` end-to-end.
#
#  Usage:    scripts/check_macros.sh [path...]
#  Default:  all .cpp files under macros/examples + macros/utilities
#
#  Exit code: 0 if every macro compiles, 1 if any failed.

set -u
set -o pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${repo_root}/build"

if [[ -f "${build_dir}/libbeam_test_analysis.dylib" ]]; then
    framework_lib="${build_dir}/libbeam_test_analysis.dylib"
elif [[ -f "${build_dir}/libbeam_test_analysis.so" ]]; then
    framework_lib="${build_dir}/libbeam_test_analysis.so"
else
    echo "ERROR: framework library not found under ${build_dir}." >&2
    echo "       Run 'cmake --build build' first." >&2
    exit 2
fi

#  ROOT_INCLUDE_PATH is the ACLiC-visible counterpart to the
#  R__ADD_INCLUDE_PATH directives in macros/lib_loader.h
#  (those are cling-only and ignored by ACLiC).
export ROOT_INCLUDE_PATH="${repo_root}/include:${repo_root}/build/_deps/mist-src/include:${repo_root}/build/_deps/tomlplusplus-src/include${ROOT_INCLUDE_PATH:+:${ROOT_INCLUDE_PATH}}"

#  Build the macro list.
if [[ $# -gt 0 ]]; then
    macros=("$@")
else
    macros=()
    while IFS= read -r -d '' f; do
        macros+=("$f")
    done < <(find "${repo_root}/macros/examples" "${repo_root}/macros/utilities" \
                  -maxdepth 1 -name '*.cpp' -print0 2>/dev/null)
fi

if [[ ${#macros[@]} -eq 0 ]]; then
    echo "No macros to check."
    exit 0
fi

pass=0
fail=0
failed_list=()
tmp_log="$(mktemp)"

#  ACLiC leaves a swarm of intermediate files next to each .cpp
#  (`*_ACLiC_dict.*`, `*_cpp.d`, `*_cpp.so`, `*_cpp_ACLiC_*`).  Wipe
#  them on exit so the source tree stays clean — the script is meant
#  to be a QA gate, not a build cache.
cleanup() {
    rm -f "${tmp_log}"
    find "${repo_root}/macros" -maxdepth 2 \
        \( -name '*_ACLiC_*' -o -name '*_cpp.d' -o -name '*_cpp.so' \) \
        -delete 2>/dev/null || true
}
trap cleanup EXIT

for m in "${macros[@]}"; do
    name="$(basename "${m}")"
    printf '  %-50s ' "${name}"
    #  `.L file.cpp+` triggers ACLiC compile.  Trailing `.q` exits root
    #  cleanly so a successful compile doesn't drop us into the prompt.
    #  AddLinkedLibs prepends our framework dylib to ACLiC's link command
    #  so symbols like `lightdata_writer(...)` resolve.  R__LOAD_LIBRARY
    #  in lib_loader.h only handles the runtime dlopen, not the link step.
    #  AddLinkedLibs       — feeds ACLiC's link command (resolves symbols).
    #  gSystem->Load       — pre-loads the framework so the macro's .so
    #                        finds `@rpath/libbeam_test_analysis.dylib`
    #                        already in the process image at dlopen.
    if root -l -b -q \
            -e "gSystem->Load(\"${framework_lib}\");" \
            -e "gSystem->AddLinkedLibs(\"${framework_lib}\");" \
            -e ".L ${m}+" >"${tmp_log}" 2>&1; then
        echo "OK"
        pass=$((pass + 1))
    else
        echo "FAIL"
        fail=$((fail + 1))
        failed_list+=("${name}")
        sed 's/^/      | /' "${tmp_log}"
    fi
done

echo
echo "─────────────────────────────────────────────────────────"
echo "  passed: ${pass}    failed: ${fail}    total: ${#macros[@]}"
if [[ ${fail} -gt 0 ]]; then
    echo "  failures:"
    for name in "${failed_list[@]}"; do
        echo "    - ${name}"
    done
    exit 1
fi
exit 0
