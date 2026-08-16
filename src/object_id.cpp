#include "krepis/object_id.hpp"

namespace krepis {
namespace {

void write_be64(std::uint64_t value, std::byte* out) noexcept {
    for (int i = 0; i < 8; ++i) {
        const int shift = (7 - i) * 8;
        out[i] = static_cast<std::byte>((value >> shift) & 0xFF);
    }
}

[[nodiscard]] std::uint64_t read_be64(const std::byte* in) noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(std::to_integer<unsigned char>(in[i]));
    }
    return value;
}

constexpr char lowercase_hex_digits[] = "0123456789abcdef";

// 只接受小寫十六進位。大寫在此回傳 -1，由呼叫端拒絕（DOC-0002 D4）。
[[nodiscard]] constexpr int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace

void encode_object_id(const ObjectId& id, std::span<std::byte, ObjectId::encoded_size> out) noexcept {
    write_be64(id.high, out.data());
    write_be64(id.low, out.data() + 8);
}

ObjectId decode_object_id(std::span<const std::byte, ObjectId::encoded_size> bytes) noexcept {
    ObjectId id{};
    id.high = read_be64(bytes.data());
    id.low = read_be64(bytes.data() + 8);
    return id;
}

std::string object_id_to_text(const ObjectId& id) {
    std::string text;
    text.resize(ObjectId::text_size);

    const std::uint64_t halves[2] = {id.high, id.low};
    std::size_t position = 0;
    for (std::uint64_t half : halves) {
        for (int nibble = 15; nibble >= 0; --nibble) {
            const auto digit = static_cast<std::size_t>((half >> (nibble * 4)) & 0xF);
            text[position++] = lowercase_hex_digits[digit];
        }
    }
    return text;
}

Result<ObjectId> parse_object_id(std::string_view text) {
    if (text.size() != ObjectId::text_size) {
        return Error{ErrorCode::invalid_argument, "ObjectId 文字必須恰好 32 個字元"};
    }

    std::uint64_t halves[2] = {0, 0};
    for (std::size_t i = 0; i < ObjectId::text_size; ++i) {
        const int digit = hex_value(text[i]);
        if (digit < 0) {
            // 涵蓋大寫、分隔符、前綴與任何非十六進位字元。
            return Error{ErrorCode::invalid_argument, "ObjectId 文字只接受小寫十六進位字元"};
        }
        halves[i / 16] = (halves[i / 16] << 4) | static_cast<std::uint64_t>(digit);
    }

    return ObjectId{halves[0], halves[1]};
}

}  // namespace krepis
