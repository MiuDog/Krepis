#include "krepis/leaf_key.hpp"

#include <cassert>
#include <cstddef>

namespace krepis {
namespace {

// 以兩個 uint64_t 表示的 128-bit 無號值。MSVC 無可依賴的原生 128-bit 整數（LAY-0002 D13）。
struct U128 {
    std::uint64_t high;
    std::uint64_t low;
};

[[nodiscard]] U128 sub(const U128& a, const U128& b) noexcept {
    const std::uint64_t low = a.low - b.low;
    const std::uint64_t borrow = (a.low < b.low) ? 1u : 0u;
    return U128{a.high - b.high - borrow, low};
}

[[nodiscard]] U128 add(const U128& a, const U128& b) noexcept {
    const std::uint64_t low = a.low + b.low;
    const std::uint64_t carry = (low < a.low) ? 1u : 0u;
    return U128{a.high + b.high + carry, low};
}

[[nodiscard]] U128 shift_right_one(const U128& a) noexcept {
    return U128{a.high >> 1, (a.low >> 1) | (a.high << 63)};
}

[[nodiscard]] bool is_zero(const U128& a) noexcept {
    return a.high == 0 && a.low == 0;
}

[[nodiscard]] bool is_less_than_two(const U128& a) noexcept {
    return a.high == 0 && a.low < 2;
}

// 128-bit 除以 64-bit，回傳商。divisor 不得為零。
//
// 逐位長除法。餘數恆小於 divisor（< 2^64），因此可用單一 uint64_t 保存；
// 但 `remainder << 1` 可能溢位到第 65 位，故以 carry 明確處理：
// carry 為 1 時真值為 `2^64 + remainder`，必然 >= divisor，
// 而 uint64_t 的 `remainder - divisor` 迴繞後正好等於 `2^64 + remainder - divisor`。
//
// 複雜度：固定 128 次迭代。只在 relabel 時呼叫，不在熱路徑上。
[[nodiscard]] U128 divide(const U128& value, std::uint64_t divisor) noexcept {
    assert(divisor != 0 && "divisor 不得為零");

    U128 quotient{0, 0};
    std::uint64_t remainder = 0;

    for (int bit = 127; bit >= 0; --bit) {
        const std::uint64_t next_bit =
            (bit >= 64) ? ((value.high >> (bit - 64)) & 1u) : ((value.low >> bit) & 1u);

        const std::uint64_t carry = remainder >> 63;
        remainder = (remainder << 1) | next_bit;

        std::uint64_t quotient_bit = 0;
        if (carry != 0 || remainder >= divisor) {
            remainder -= divisor;
            quotient_bit = 1;
        }

        if (bit >= 64) {
            quotient.high |= quotient_bit << (bit - 64);
        } else {
            quotient.low |= quotient_bit << bit;
        }
    }
    return quotient;
}

[[nodiscard]] U128 to_u128(const LeafKey& key) noexcept {
    return U128{key.high, key.low};
}

[[nodiscard]] LeafKey to_key(const U128& value) noexcept {
    return LeafKey{value.high, value.low};
}

}  // namespace

std::optional<LeafKey> leaf_key_midpoint(const LeafKey& left, const LeafKey& right) noexcept {
    assert(left < right && "leaf_key_midpoint 要求 left < right");

    const U128 gap = sub(to_u128(right), to_u128(left));
    const U128 half = shift_right_one(gap);
    if (is_zero(half)) {
        // left 與 right 相鄰，兩者之間沒有可用整數。
        // 呼叫端必須依 LAY-0002 D13 對附近 window 重新編號。
        return std::nullopt;
    }
    return to_key(add(to_u128(left), half));
}

bool leaf_key_distribute(const LeafKey& left, const LeafKey& right, std::size_t count,
                         LeafKey* out) noexcept {
    assert(left < right && "leaf_key_distribute 要求 left < right");
    assert(count >= 1 && "count 至少為 1");
    assert(out != nullptr && "out 不得為空");

    const U128 gap = sub(to_u128(right), to_u128(left));

    // 分成 count + 1 段，使首尾兩側也保有間隙，避免緊貼既有標籤。
    const std::uint64_t segments = static_cast<std::uint64_t>(count) + 1;
    if (segments < count) {
        return false;  // count 溢位，區間不可能容納
    }

    const U128 step = divide(gap, segments);
    if (is_less_than_two(step)) {
        // step=1 雖能產生相異標籤，卻會讓相鄰標籤沒有下一次 split 的中點。
        // D22 要求 relabel 後立刻重試 split，因此必須擴張 window。
        return false;
    }

    U128 cursor = to_u128(left);
    for (std::size_t i = 0; i < count; ++i) {
        cursor = add(cursor, step);
        out[i] = to_key(cursor);
    }
    return true;
}

}  // namespace krepis
