#include "krepis/arena.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>

namespace krepis {
namespace {

// 依 FND-0002 D5：記憶體不足不視為可恢復的失敗。
// 為它撐出一條錯誤路徑會污染每一個介面，而該路徑實務上永遠不會被正確測試。
[[noreturn]] void abort_on_out_of_memory(std::size_t requested) {
    std::fprintf(stderr, "krepis: arena 配置 %zu 位元組失敗，記憶體不足\n", requested);
    std::abort();
}

[[nodiscard]] constexpr bool is_power_of_two(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// 回傳在 base + used 之後、對齊到 alignment 的位址所需的填補位元組數。
[[nodiscard]] std::size_t padding_for(const std::uint8_t* base, std::size_t used,
                                      std::size_t alignment) noexcept {
    const std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(base) + used;
    const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment) - 1;
    const std::uintptr_t aligned = (cursor + mask) & ~mask;
    return static_cast<std::size_t>(aligned - cursor);
}

}  // namespace

Arena::Arena(std::size_t block_bytes)
    : block_bytes_(block_bytes == 0 ? default_block_bytes : block_bytes) {
    push_block(block_bytes_);
    current_ = head_;
}

Arena::~Arena() {
    release_all();
}

Arena::Arena(Arena&& other) noexcept
    : head_(other.head_),
      tail_(other.tail_),
      current_(other.current_),
      block_bytes_(other.block_bytes_),
      bytes_used_(other.bytes_used_),
      bytes_reserved_(other.bytes_reserved_) {
    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.current_ = nullptr;
    other.bytes_used_ = 0;
    other.bytes_reserved_ = 0;
}

Arena& Arena::operator=(Arena&& other) noexcept {
    if (this != &other) {
        release_all();
        head_ = other.head_;
        tail_ = other.tail_;
        current_ = other.current_;
        block_bytes_ = other.block_bytes_;
        bytes_used_ = other.bytes_used_;
        bytes_reserved_ = other.bytes_reserved_;
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.current_ = nullptr;
        other.bytes_used_ = 0;
        other.bytes_reserved_ = 0;
    }
    return *this;
}

// 新 block 一律接在串列尾。
// 不可接在 current_ 之後 —— reset() 後 current_ 會回到 head_，
// 若此時接在 current_ 之後會覆寫 head_->next 並洩漏後續整條串列。
void Arena::push_block(std::size_t capacity) {
    const std::size_t total = sizeof(Block) + capacity;
    void* raw = std::malloc(total);
    if (raw == nullptr) {
        abort_on_out_of_memory(total);
    }

    Block* block = static_cast<Block*>(raw);
    block->next = nullptr;
    block->capacity = capacity;
    block->used = 0;

    if (tail_ != nullptr) {
        tail_->next = block;
    } else {
        head_ = block;
    }
    tail_ = block;
    bytes_reserved_ += capacity;
}

void Arena::release_all() noexcept {
    Block* block = head_;
    while (block != nullptr) {
        Block* next = block->next;
        std::free(block);
        block = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    current_ = nullptr;
    bytes_used_ = 0;
    bytes_reserved_ = 0;
}

void* Arena::allocate(std::size_t bytes, std::size_t alignment) {
    assert(is_power_of_two(alignment) && "alignment 必須是 2 的冪");
    assert(current_ != nullptr && "已被移出的 Arena 不可再配置");

    if (bytes == 0) {
        // 回傳一個對齊且不與其他配置重疊的位址，避免呼叫端誤判為失敗。
        bytes = 1;
    }

    // 先在目前 block 內嘗試。
    {
        const std::size_t padding = padding_for(current_->data(), current_->used, alignment);
        if (current_->used + padding + bytes <= current_->capacity) {
            std::uint8_t* result = current_->data() + current_->used + padding;
            current_->used += padding + bytes;
            bytes_used_ += padding + bytes;
            return result;
        }
    }

    // 再沿用串列中既有的後續 block（reset() 之後這些都是空的）。
    for (Block* block = current_->next; block != nullptr; block = block->next) {
        const std::size_t padding = padding_for(block->data(), block->used, alignment);
        if (block->used + padding + bytes <= block->capacity) {
            current_ = block;
            std::uint8_t* result = block->data() + block->used + padding;
            block->used += padding + bytes;
            bytes_used_ += padding + bytes;
            return result;
        }
    }

    // 既有 block 都容不下才新增。
    // 請求本身超過預設大小時給它專屬 block，容量含最壞情況的對齊填補。
    const std::size_t needed = bytes + alignment;
    push_block(needed > block_bytes_ ? needed : block_bytes_);
    current_ = tail_;

    const std::size_t padding = padding_for(current_->data(), 0, alignment);
    assert(padding + bytes <= current_->capacity && "新 block 仍容不下請求");
    std::uint8_t* result = current_->data() + padding;
    current_->used = padding + bytes;
    bytes_used_ += padding + bytes;
    return result;
}

void Arena::reset() noexcept {
    // 保留已配置的 block 供重用：每幀重用的場景刻意維持高水位，避免反覆向系統要記憶體。
    for (Block* block = head_; block != nullptr; block = block->next) {
        block->used = 0;
    }
    current_ = head_;
    bytes_used_ = 0;
}

std::size_t Arena::block_count() const noexcept {
    std::size_t count = 0;
    for (const Block* block = head_; block != nullptr; block = block->next) {
        ++count;
    }
    return count;
}

}  // namespace krepis
