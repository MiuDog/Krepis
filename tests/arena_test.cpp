#include "krepis/arena.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <utility>
#include <vector>

using krepis::Arena;
using krepis_test::expect;

namespace {

[[nodiscard]] bool is_aligned(const void* pointer, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0;
}

// 只為推進游標或撐大容量而配置，刻意忽略回傳值。
void bump(Arena& arena, std::size_t bytes, std::size_t alignment) {
    static_cast<void>(arena.allocate(bytes, alignment));
}

void test_alignment_is_honoured() {
    Arena arena(1024);
    // 先配置 1 個位元組使游標落在奇數位置，再要求較大的對齊。
    bump(arena, 1, 1);
    for (std::size_t alignment : {2u, 4u, 8u, 16u, 32u, 64u}) {
        void* pointer = arena.allocate(1, alignment);
        expect(is_aligned(pointer, alignment), "配置結果符合要求的對齊");
        bump(arena, 1, 1);  // 再次打亂游標
    }
}

void test_allocations_do_not_overlap() {
    Arena arena(1024);
    constexpr std::size_t count = 64;
    std::vector<std::uint32_t*> pointers;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t* slot = arena.allocate_array<std::uint32_t>(1);
        *slot = static_cast<std::uint32_t>(i);
        pointers.push_back(slot);
    }
    bool all_intact = true;
    for (std::size_t i = 0; i < count; ++i) {
        if (*pointers[i] != static_cast<std::uint32_t>(i)) {
            all_intact = false;
        }
    }
    expect(all_intact, "連續配置彼此不重疊");
}

void test_grows_beyond_single_block() {
    Arena arena(256);
    expect(arena.block_count() == 1, "初始只有一個 block");
    // 配置量遠超過單一 block，強制成長。
    for (int i = 0; i < 64; ++i) {
        bump(arena, 64, 8);
    }
    expect(arena.block_count() > 1, "容量不足時自動新增 block");
    expect(arena.bytes_used() <= arena.bytes_reserved(), "已用量不超過保留量");
}

void test_oversized_request_gets_dedicated_block() {
    Arena arena(128);
    void* pointer = arena.allocate(8192, 64);
    expect(pointer != nullptr, "超過預設 block 大小的請求仍可滿足");
    expect(is_aligned(pointer, 64), "專屬 block 的配置仍符合對齊");
    expect(arena.bytes_reserved() >= 8192, "保留量涵蓋大型請求");
}

void test_reset_reuses_capacity() {
    Arena arena(256);
    for (int i = 0; i < 64; ++i) {
        bump(arena, 64, 8);
    }
    const std::size_t reserved_before = arena.bytes_reserved();
    const std::size_t blocks_before = arena.block_count();

    arena.reset();
    expect(arena.bytes_used() == 0, "reset 後已用量歸零");
    expect(arena.bytes_reserved() == reserved_before, "reset 不縮減已保留容量");
    expect(arena.block_count() == blocks_before, "reset 不釋放 block");

    // 重跑同樣的配置量，不應再向系統要記憶體。
    for (int i = 0; i < 64; ++i) {
        bump(arena, 64, 8);
    }
    expect(arena.bytes_reserved() == reserved_before, "重用既有 block，未新增保留容量");
    expect(arena.block_count() == blocks_before, "重用既有 block，未新增 block");
}

// 迴歸測試：reset() 後 current_ 回到 head_，若新 block 接在 current_ 之後
// 會覆寫 head_->next 並洩漏後續整條串列。新 block 必須接在串列尾。
void test_reset_then_overflow_keeps_chain() {
    Arena arena(256);
    for (int i = 0; i < 32; ++i) {
        bump(arena, 64, 8);
    }
    const std::size_t blocks_before = arena.block_count();
    expect(blocks_before > 1, "前置條件：已有多個 block");

    arena.reset();
    // 單一請求即超過 head block，迫使跨 block 配置。
    bump(arena, 200, 8);
    bump(arena, 200, 8);

    expect(arena.block_count() >= blocks_before, "reset 後溢位不得遺失既有 block");
}

void test_move_transfers_ownership() {
    Arena source(512);
    std::uint32_t* slot = source.allocate_array<std::uint32_t>(1);
    *slot = 7;
    const std::size_t reserved = source.bytes_reserved();

    Arena moved = std::move(source);
    expect(moved.bytes_reserved() == reserved, "移動後保留量隨之轉移");
    expect(*slot == 7, "移動後既有配置的內容仍然有效");
    expect(source.bytes_reserved() == 0, "被移出的 Arena 不再持有資源");
}

void test_zero_size_allocation_is_usable() {
    Arena arena(256);
    void* first = arena.allocate(0, 8);
    void* second = arena.allocate(0, 8);
    expect(first != nullptr, "零大小配置不回傳 nullptr");
    expect(first != second, "零大小配置彼此不同");
}

}  // namespace

int main() {
    test_alignment_is_honoured();
    test_allocations_do_not_overlap();
    test_grows_beyond_single_block();
    test_oversized_request_gets_dedicated_block();
    test_reset_reuses_capacity();
    test_reset_then_overflow_keeps_chain();
    test_move_transfers_ownership();
    test_zero_size_allocation_is_usable();
    return krepis_test::report("krepis.arena");
}
