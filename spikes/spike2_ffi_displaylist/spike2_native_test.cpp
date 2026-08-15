#include "engine_abi.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <cassert>

int main() {
    std::cout << "========================================\n";
    std::cout << "[Spike 2] C ABI 介面原生測試\n";
    std::cout << "========================================\n";

    KrepisEngineHandle engine = nullptr;
    KrepisStatus status = krepis_engine_create(&engine);
    assert(status == KREPIS_OK && engine != nullptr);

    krepis_engine_set_viewport(engine, 800.0f, 600.0f, 0.0f);

    // 插入 100 個段落
    for (int i = 0; i < 100; ++i) {
        std::string text = "段落 #" + std::to_string(i) + "：Krepis 結構化筆記基座庫增量版面測試。";
        krepis_engine_insert_paragraph(engine, i, text.c_str());
    }

    uint32_t count = 0;
    krepis_engine_get_paragraph_count(engine, &count);
    assert(count == 100);

    float total_height = 0.0f;
    krepis_engine_layout(engine, &total_height);
    std::cout << "總段落數: " << count << ", 總版面高度: " << total_height << "\n";
    assert(total_height > 0.0f);

    // 取得 Display List
    const uint8_t* dl_ptr = nullptr;
    uint32_t dl_size = 0;
    KrepisDisplayListHandle dl_handle = nullptr;
    status = krepis_engine_acquire_display_list(engine, &dl_ptr, &dl_size, &dl_handle);
    assert(status == KREPIS_OK && dl_ptr != nullptr && dl_size > 0);

    std::cout << "Display List 二進位緩衝區大小: " << dl_size << " bytes\n";

    status = krepis_display_list_release(engine, dl_handle);
    assert(status == KREPIS_OK);

    krepis_engine_destroy(engine);
    std::cout << "--> [通過] C ABI 介面原生建立、段落插入、版面重排、Display List 取得與釋放成功！\n\n";
    return 0;
}
