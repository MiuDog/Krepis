#include "krepis/error.hpp"

#include "test_support.hpp"

#include <string>
#include <utility>

using krepis::Error;
using krepis::ErrorCode;
using krepis::Result;
using krepis_test::expect;

namespace {

// 模擬一個以 Result 回報可恢復失敗的介面。
Result<int> parse_positive(int raw) {
    if (raw <= 0) {
        return Error{ErrorCode::invalid_argument, "raw 必須為正"};
    }
    return raw;
}

Result<void> require_even(int value) {
    if (value % 2 != 0) {
        return Error{ErrorCode::out_of_range, "value 必須為偶數"};
    }
    return {};
}

void test_success_path() {
    Result<int> result = parse_positive(42);
    expect(result.is_ok(), "成功時 is_ok 為真");
    expect(result.value() == 42, "成功時可取回原值");
}

void test_failure_path() {
    Result<int> result = parse_positive(-1);
    expect(!result.is_ok(), "失敗時 is_ok 為偽");
    expect(result.error().code() == ErrorCode::invalid_argument, "錯誤分類被完整保留");
    expect(result.error().detail() != nullptr, "detail 供除錯使用時可取得");
}

void test_void_specialisation() {
    Result<void> ok = require_even(4);
    expect(ok.is_ok(), "void 特化的成功情形");

    Result<void> failed = require_even(3);
    expect(!failed.is_ok(), "void 特化的失敗情形");
    expect(failed.error().code() == ErrorCode::out_of_range, "void 特化保留錯誤分類");
}

// 確認成功值可為僅可移動的型別，且 take() 不複製。
void test_move_only_value() {
    Result<std::string> result = std::string("krepis");
    expect(result.is_ok(), "字串成功值");
    std::string taken = std::move(result).take();
    expect(taken == "krepis", "take 取回完整內容");
}

// FND-0002 要求呼叫者一律以 code 判斷，不得分析訊息文字。
// 本測試固定住 code 的實際數值 —— 這些值會經 C ABI 傳出，變動即為破壞相容性。
void test_error_codes_are_stable() {
    expect(static_cast<int>(ErrorCode::invalid_argument) == 1, "invalid_argument 為 1");
    expect(static_cast<int>(ErrorCode::out_of_range) == 2, "out_of_range 為 2");
    expect(static_cast<int>(ErrorCode::not_found) == 3, "not_found 為 3");
    expect(static_cast<int>(ErrorCode::invalid_state) == 4, "invalid_state 為 4");
    expect(static_cast<int>(ErrorCode::unsupported) == 5, "unsupported 為 5");
    expect(static_cast<int>(ErrorCode::version_mismatch) == 6, "version_mismatch 為 6");
}

}  // namespace

int main() {
    test_success_path();
    test_failure_path();
    test_void_specialisation();
    test_move_only_value();
    test_error_codes_are_stable();
    return krepis_test::report("krepis.error");
}
