#include "krepis/id_generator.hpp"

#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
// 平台實作藏在此處；標頭不洩漏任何 Windows 型別。
#include <windows.h>
// bcrypt.h 必須在 windows.h 之後。
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <cstdlib>  // arc4random_buf
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>  // getrandom（glibc 2.25+）
#include <unistd.h>
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

#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

// arc4random_buf 依契約永不失敗、永遠填滿，因此不需要迴圈或錯誤處理。
class BsdRandomSource final : public RandomSource {
public:
    void fill(std::span<std::byte> out) override {
        if (out.empty()) {
            return;
        }
        ::arc4random_buf(out.data(), out.size());
    }
};

#else

// POSIX（Linux 等）。
//
// **必須完整填滿或中止**：部分填充會靜默產生熵不足的 ObjectId，
// 那比直接失敗危險得多——文件看起來正常，但身分的唯一性保證已經破了（FND-0002 D5）。
class PosixRandomSource final : public RandomSource {
public:
    void fill(std::span<std::byte> out) override {
        if (out.empty()) {
            return;
        }

        std::size_t filled = 0;
        while (filled < out.size()) {
            // getrandom 可能被信號中斷，也可能只填一部分——兩者都必須迴圈處理。
            const ssize_t written =
                ::getrandom(out.data() + filled, out.size() - filled, 0);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                // 核心太舊或 syscall 不可用時退回 /dev/urandom。
                fill_from_urandom(out.subspan(filled));
                return;
            }
            filled += static_cast<std::size_t>(written);
        }
    }

private:
    static void fill_from_urandom(std::span<std::byte> out) {
        const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            abort_on_random_failure("getrandom 與 /dev/urandom 皆不可用");
        }

        std::size_t filled = 0;
        while (filled < out.size()) {
            const ssize_t written = ::read(fd, out.data() + filled, out.size() - filled);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                abort_on_random_failure("讀取 /dev/urandom 失敗");
            }
            if (written == 0) {
                ::close(fd);
                abort_on_random_failure("/dev/urandom 提前 EOF");
            }
            filled += static_cast<std::size_t>(written);
        }
        ::close(fd);
    }
};

#endif

}  // namespace

RandomSource& platform_random_source() {
#if defined(_WIN32)
    static WindowsRandomSource source;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    static BsdRandomSource source;
#else
    static PosixRandomSource source;
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
