# 第一版不引入測試框架依賴：以退出碼作為 pass／fail，交由 CTest 判定。
# 引入框架（Catch2／GoogleTest）需在依賴管理方式定案後另行決定。

add_executable(krepis_version_test version_test.cpp)
target_link_libraries(krepis_version_test PRIVATE krepis::krepis)

add_test(NAME krepis.version COMMAND krepis_version_test)
