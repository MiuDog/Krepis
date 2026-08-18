#!/usr/bin/env bash
#
# LAY-0002 D17 閘門 5：AddressSanitizer 與 race detector（第二工具鏈）
#
# 為什麼需要這支腳本：
#   - 閘門 5 明文禁止以「本機沒有報錯」代替競態證據，必須有 sanitizer 實跑結果。
#   - **TSan 不支援 Windows**，因此無法在主要開發環境（MSVC）執行。
#   - GitHub Actions 已配置但受帳務阻塞（2026-08-18）。
#   → 在 WSL／Linux 以 GCC 執行是不依賴外部服務的關閉路徑。
#
# 為什麼 GCC 可以：GCC 的 -fsanitize=address / =thread 就是 LLVM 那一套 sanitizer，
# 且 GCC 相對 MSVC 本身即為閘門 5 所要求的「第二工具鏈」。clang 亦可，換 CXX 即可。
#
# 用法：
#   bash scripts/run-sanitizers.sh              # 兩者都跑
#   bash scripts/run-sanitizers.sh asan         # 只跑 ASan + UBSan
#   bash scripts/run-sanitizers.sh tsan         # 只跑 TSan
#   CXX=clang++ bash scripts/run-sanitizers.sh  # 改用 clang
#
# 從 Windows 呼叫：
#   wsl -d Ubuntu-24.04 -- bash -lc "cd /mnt/c/Projects/Krepis && bash scripts/run-sanitizers.sh"

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 刻意建置在 Linux 檔案系統而非 /mnt/c：跨檔案系統的編譯會慢上數倍。
BUILD_ROOT="${BUILD_ROOT:-/tmp/krepis-sanitizers}"
CXX_BIN="${CXX:-g++}"

overall_status=0

print_header() {
    echo
    echo "=================================================================="
    echo "  $1"
    echo "=================================================================="
}

run_suite() {
    local name="$1"
    local cxx_flags="$2"
    local link_flags="$3"
    local build_dir="${BUILD_ROOT}/${name}"

    print_header "${name}：設定與建置"
    echo "編譯器：$(${CXX_BIN} --version | head -1)"
    echo "旗標：${cxx_flags}"
    echo "建置目錄：${build_dir}"

    if ! cmake -S "${REPO_ROOT}" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
        -DCMAKE_CXX_FLAGS="${cxx_flags}" \
        -DCMAKE_EXE_LINKER_FLAGS="${link_flags}" \
        > "${build_dir}.configure.log" 2>&1; then
        echo "設定失敗，見 ${build_dir}.configure.log"
        tail -20 "${build_dir}.configure.log"
        return 1
    fi

    if ! cmake --build "${build_dir}" -j"$(nproc)" > "${build_dir}.build.log" 2>&1; then
        echo "建置失敗，見 ${build_dir}.build.log"
        tail -30 "${build_dir}.build.log"
        return 1
    fi
    echo "建置成功。"

    print_header "${name}：執行測試"
    # sanitizer 的診斷走 stderr，--output-on-failure 不足以看到通過案例中的警告，
    # 因此一律保留完整輸出再自行檢查。
    local test_log="${build_dir}.ctest.log"

    # TSan 的 shadow memory 需要特定位址佈局，與現代 Linux 核心較高的 ASLR 熵值
    # （vm.mmap_rnd_bits）衝突，會在啟動時就以
    #   FATAL: ThreadSanitizer: unexpected memory mapping
    # 失敗——**連空測試都跑不起來**，看起來像 11 個測試全部競態，實則一個都沒執行。
    #
    # `setarch -R` 只關閉該 process 的 ASLR，不需要 sudo，也不改動系統設定。
    # （另一個做法 `sysctl vm.mmap_rnd_bits=28` 需要 root 且影響全機。）
    local runner=()
    if [ "${name}" = "tsan" ] && command -v setarch > /dev/null 2>&1; then
        runner=(setarch -R)
        echo "以 setarch -R 執行（規避 TSan 與 ASLR 熵值的衝突）"
    fi

    "${runner[@]}" ctest --test-dir "${build_dir}" --output-on-failure > "${test_log}" 2>&1
    local ctest_status=$?

    tail -20 "${test_log}"

    # ctest 回報成功不代表沒有 sanitizer 診斷：某些情境下 runtime 只印警告而不改變退出碼。
    local diagnostics
    diagnostics=$(grep -cE "(ERROR: (Address|Thread|Leak)Sanitizer|runtime error:|WARNING: ThreadSanitizer)" \
        "${test_log}" || true)

    if [ "${ctest_status}" -ne 0 ]; then
        echo
        echo ">>> ${name}：測試失敗（ctest 退出碼 ${ctest_status}）"
        grep -nE "(ERROR: (Address|Thread|Leak)Sanitizer|runtime error:|WARNING: ThreadSanitizer)" \
            "${test_log}" | head -20
        return 1
    fi

    if [ "${diagnostics}" -gt 0 ]; then
        echo
        echo ">>> ${name}：ctest 通過，但發現 ${diagnostics} 筆 sanitizer 診斷 —— **不算通過**"
        grep -nE "(ERROR: (Address|Thread|Leak)Sanitizer|runtime error:|WARNING: ThreadSanitizer)" \
            "${test_log}" | head -20
        return 1
    fi

    echo
    echo ">>> ${name}：通過，且無 sanitizer 診斷。"
    return 0
}

mkdir -p "${BUILD_ROOT}"

target="${1:-all}"

if [ "${target}" = "all" ] || [ "${target}" = "asan" ]; then
    # UBSan 一併開啟：未定義行為與記憶體錯誤成因不同，分開跑會漏。
    # halt_on_error 使第一筆診斷即失敗，避免後續輸出淹沒真正的第一因。
    if ! ASAN_OPTIONS="detect_leaks=1:halt_on_error=1" \
         UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
         run_suite "asan-ubsan" \
            "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g -O1" \
            "-fsanitize=address,undefined"; then
        overall_status=1
    fi
fi

if [ "${target}" = "all" ] || [ "${target}" = "tsan" ]; then
    # TSan 與 ASan 互斥，必須分開建置。
    if ! TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" \
         run_suite "tsan" \
            "-fsanitize=thread -fno-omit-frame-pointer -g -O1" \
            "-fsanitize=thread"; then
        overall_status=1
    fi
fi

print_header "閘門 5 總結"
if [ "${overall_status}" -eq 0 ]; then
    echo "全部 sanitizer 建置通過且無診斷。"
    echo "此結果可作為 LAY-0002 D17 閘門 5 的證據。"
else
    echo "有 sanitizer 回報問題 —— 閘門 5 **不通過**。"
    echo "記錄檔在 ${BUILD_ROOT}/ 之下。"
fi

exit "${overall_status}"
