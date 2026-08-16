#include "krepis/id_generator.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
// 平台實作藏在此處；標頭不洩漏任何 Windows 型別。
#include <windows.h>
// bcrypt.h 必須在 windows.h 之後。
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace krepis {
namespace {

// 依 FND-0002 D5：無法取得亂數時，身分無法生成，繼續執行只會產出損壞的文件。
[[noreturn]] void abort_on_random_failure(const char* what) {
    std::fprintf(stderr, "krepis: 無法取得亂數（%s），身分無法生成\n", what);
    std::abort();
}

#if defined(_WIN32)

class WindowsRandomSource final : public RandomSource {
public:
    void fill(std::span<std::byte> out) override {
        if (out.empty()) {
            return;
        }
        const NTSTATUS status = BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(out.data()),
            static_cast<ULONG>(out.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status != 0) {
            abort_on_random_failure("BCryptGenRandom");
        }
    }
};

#else

// 其他平台的實作於該平台工作展開時補上（見 spec/architecture.md 的平台策略）。
class UnimplementedRandomSource final : public RandomSource {
public:
    void fill(std::span<std::byte>) override {
        abort_on_random_failure("本平台尚未提供 RandomSource 實作");
    }
};

#endif

}  // namespace

RandomSource& platform_random_source() {
#if defined(_WIN32)
    static WindowsRandomSource source;
#else
    static UnimplementedRandomSource source;
#endif
    return source;
}

ObjectId RandomIdGenerator::next() {
    // nil 的機率是 2^-128，實務上永不發生。
    // 這個迴圈的用途是讓「nil 代表沒有物件」成為可證明的 invariant 而非慣例。
    for (;;) {
        std::byte bytes[ObjectId::encoded_size];
        source_->fill(std::span<std::byte>(bytes, ObjectId::encoded_size));
        const ObjectId id =
            decode_object_id(std::span<const std::byte, ObjectId::encoded_size>(bytes, ObjectId::encoded_size));
        if (!id.is_nil()) {
            return id;
        }
    }
}

ObjectId SequentialIdGenerator::next() {
    const ObjectId id{high_, next_low_};
    if (next_low_ == UINT64_MAX) {
        next_low_ = 1;  // 跳過 0，維持永不產生 nil 的不變條件
        ++high_;
    } else {
        ++next_low_;
    }
    return id;
}

}  // namespace krepis
