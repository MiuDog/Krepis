#include "krepis/id_generator.hpp"

#include "test_support.hpp"

#include <set>

using krepis::ObjectId;
using krepis::RandomIdGenerator;
using krepis::SequentialIdGenerator;
using krepis_test::expect;

namespace {

// DOC-0001 D6：測試可使用固定序列。
void test_sequential_is_deterministic() {
    SequentialIdGenerator a{1};
    SequentialIdGenerator b{1};
    bool identical = true;
    for (int i = 0; i < 100; ++i) {
        if (a.next() != b.next()) {
            identical = false;
        }
    }
    expect(identical, "相同 seed 產生相同序列");
}

void test_sequential_never_returns_nil() {
    SequentialIdGenerator generator{0};  // seed 0 應被提升為 1
    bool any_nil = false;
    for (int i = 0; i < 1000; ++i) {
        if (generator.next().is_nil()) {
            any_nil = true;
        }
    }
    expect(!any_nil, "SequentialIdGenerator 永不產生 nil");
}

void test_sequential_has_no_duplicates() {
    SequentialIdGenerator generator{1};
    std::set<ObjectId> seen;
    for (int i = 0; i < 5000; ++i) {
        seen.insert(generator.next());
    }
    expect(seen.size() == 5000, "確定性序列不重複");
}

// 迴繞邊界：low 到達 UINT64_MAX 時進位到 high，且跳過 0。
void test_sequential_wraps_without_nil() {
    SequentialIdGenerator generator{UINT64_MAX};
    const ObjectId first = generator.next();
    const ObjectId second = generator.next();

    expect(first == ObjectId{0, UINT64_MAX}, "迴繞前的最後一個值");
    expect(second == ObjectId{1, 1}, "迴繞後進位到 high 且 low 跳過 0");
    expect(!second.is_nil(), "迴繞後仍不是 nil");
}

// DOC-0002 D3：正式生成器永不產生 nil，且分佈足以避免碰撞。
void test_random_generator_properties() {
    RandomIdGenerator generator{krepis::platform_random_source()};
    std::set<ObjectId> seen;
    bool any_nil = false;
    constexpr int sample_count = 10000;

    for (int i = 0; i < sample_count; ++i) {
        const ObjectId id = generator.next();
        if (id.is_nil()) {
            any_nil = true;
        }
        seen.insert(id);
    }

    expect(!any_nil, "RandomIdGenerator 永不產生 nil");
    expect(seen.size() == sample_count, "一萬次生成無重複");
}

// 兩個獨立的正式生成器不應產生相同序列。
void test_random_generators_are_independent() {
    RandomIdGenerator a{krepis::platform_random_source()};
    RandomIdGenerator b{krepis::platform_random_source()};
    bool any_collision = false;
    for (int i = 0; i < 1000; ++i) {
        if (a.next() == b.next()) {
            any_collision = true;
        }
    }
    expect(!any_collision, "獨立生成器的序列不相同");
}

}  // namespace

int main() {
    test_sequential_is_deterministic();
    test_sequential_never_returns_nil();
    test_sequential_has_no_duplicates();
    test_sequential_wraps_without_nil();
    test_random_generator_properties();
    test_random_generators_are_independent();
    return krepis_test::report("krepis.id_generator");
}
