#pragma once

// 依 FND-0002 D4：內部可恢復的失敗以自訂最小 Result<T, Error> 表達。
// 不使用 std::expected —— 本專案最終要在 Android NDK 與 Xcode 上建置，
// 降低工具鏈版本需求的價值高於少寫一個型別。
//
// 依 FND-0002 D5：記憶體不足與程式錯誤不是可恢復的失敗，不得以 Error 表達。

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

namespace krepis {

// 機器可判斷的錯誤分類。
//
// 依 FND-0002：呼叫者一律以 code 判斷，**不得分析錯誤訊息文字**。
// 新增分類時只可追加於末端，既有數值不得變動 —— 這些值會經 C ABI 傳出。
enum class ErrorCode : std::uint32_t {
    invalid_argument = 1,  // 呼叫端傳入的值不符合契約
    out_of_range = 2,      // 索引或範圍超出目標
    not_found = 3,         // 指定的識別不存在
    invalid_state = 4,     // 目標物件當前狀態不允許此操作
    unsupported = 5,       // 此建置或此平台不支援
    version_mismatch = 6,  // 核心與呼叫端的契約版本不符
    revision_conflict = 7, // Transaction 的 base content revision 已過期
    missing_glyph = 8,     // 所有候選字型都缺少指定 grapheme
};

// 一次可恢復失敗的完整描述。
//
// 責任：攜帶機器可判斷的失敗分類，以及僅供除錯的靜態說明。
// 不負責：攜帶可變長度或需配置的訊息 —— 那會使錯誤路徑本身可能失敗。
// 維持的不變條件：`code` 永遠是有效的 ErrorCode；`detail` 若非 nullptr 則指向靜態儲存期字串；
// `has_context()` 為 true 時 context 是該 code 定義的機器可判斷數值。
// 擁有哪些資源：無。
// 生命週期：值型別，可自由複製與保存。
// 錯誤語意：本身即錯誤描述。
// 執行緒安全程度：不可變，可跨執行緒自由共享。
// 可否複製／移動：兩者皆可，且為平凡操作。
class Error {
public:
    // detail 必須是**靜態儲存期**的字串（字面值或全域常數）；
    // 傳入區域緩衝區會產生懸空指標。
    constexpr explicit Error(ErrorCode code, const char* detail = nullptr) noexcept
        : code_(code), detail_(detail), context_(0), has_context_(false) {}

    // context 是與錯誤類型相關的機器可判斷數值；例如 missing_glyph 的 UTF-8 byte offset。
    constexpr Error(ErrorCode code, const char* detail, std::uint64_t context) noexcept
        : code_(code), detail_(detail), context_(context), has_context_(true) {}

    [[nodiscard]] constexpr ErrorCode code() const noexcept { return code_; }

    // 僅供人閱讀與紀錄。**不得用於控制流程。**
    [[nodiscard]] constexpr const char* detail() const noexcept { return detail_; }
    [[nodiscard]] constexpr bool has_context() const noexcept { return has_context_; }
    [[nodiscard]] constexpr std::uint64_t context() const noexcept {
        assert(has_context_ && "錯誤沒有數值 context");
        return context_;
    }

private:
    ErrorCode code_;
    const char* detail_;
    std::uint64_t context_;
    bool has_context_;
};

// 成功值為 T、失敗為 Error 的結果型別。
//
// 責任：以型別強制呼叫者在取值前先檢查成功與否。
// 不負責：表達不可恢復的狀況（OOM、程式錯誤）—— 那些依 FND-0002 D5 直接終止。
// 維持的不變條件：恰好持有 T 或 Error 其中之一。
// 擁有哪些資源：持有 T 時擁有該值。
// 生命週期：值型別。
// 錯誤語意：在 `!is_ok()` 時呼叫 value()，或在 `is_ok()` 時呼叫 error()，
//           **是程式錯誤而非可恢復的失敗**，以 assert 攔截。
// 執行緒安全程度：與一般值型別相同，無內部同步。
// 可否複製／移動：取決於 T。
template <typename T>
class Result {
    static_assert(!std::is_same_v<std::decay_t<T>, Error>, "Result<Error> 會使成功與失敗無法區分");
    static_assert(!std::is_reference_v<T>, "Result 不持有參照");

public:
    // 以成功值建構。刻意非 explicit，使 `return value;` 可直接成立。
    constexpr Result(T value) : storage_(std::move(value)) {}

    // 以失敗建構。
    constexpr Result(Error error) : storage_(error) {}

    [[nodiscard]] constexpr bool is_ok() const noexcept { return storage_.index() == 0; }

    // 前置條件：is_ok()。
    [[nodiscard]] constexpr const T& value() const& noexcept {
        assert(is_ok() && "在失敗的 Result 上取值");
        return *std::get_if<0>(&storage_);
    }

    // 前置條件：is_ok()。取走成功值，呼叫後本物件不應再被使用。
    [[nodiscard]] constexpr T take() && {
        assert(is_ok() && "在失敗的 Result 上取值");
        return std::move(*std::get_if<0>(&storage_));
    }

    // 前置條件：!is_ok()。
    [[nodiscard]] constexpr Error error() const noexcept {
        assert(!is_ok() && "在成功的 Result 上取錯誤");
        return *std::get_if<1>(&storage_);
    }

private:
    std::variant<T, Error> storage_;
};

// 無成功值的特化：只表達「成功」或「某個失敗」。
template <>
class Result<void> {
public:
    // 成功。
    constexpr Result() noexcept : error_(ErrorCode::invalid_state), has_error_(false) {}

    // 失敗。刻意非 explicit，使 `return Error{...};` 可直接成立。
    constexpr Result(Error error) noexcept : error_(error), has_error_(true) {}

    [[nodiscard]] constexpr bool is_ok() const noexcept { return !has_error_; }

    // 前置條件：!is_ok()。
    [[nodiscard]] constexpr Error error() const noexcept {
        assert(!is_ok() && "在成功的 Result 上取錯誤");
        return error_;
    }

private:
    Error error_;
    bool has_error_;
};

}  // namespace krepis
