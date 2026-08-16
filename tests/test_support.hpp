#pragma once

// 第一版不引入測試框架依賴（見 spec/decisions/00-foundation/README.md 的待決問題）。
// 以退出碼作為 pass／fail，交由 CTest 判定。

#include <cstdio>

namespace krepis_test {

inline int failure_count = 0;

inline void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failure_count;
    }
}

// 於 main 末端回傳本函式的結果。
inline int report(const char* suite) {
    if (failure_count == 0) {
        std::printf("%s: all checks passed\n", suite);
        return 0;
    }
    std::fprintf(stderr, "%s: %d check(s) failed\n", suite, failure_count);
    return 1;
}

}  // namespace krepis_test
