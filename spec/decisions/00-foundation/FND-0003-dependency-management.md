# FND-0003：第三方依賴的取得方式

## 狀態

Accepted

## 日期

- 提出：2026-08-16
- 接受：2026-08-17

## 背景

`00-foundation/README.md` 列有「依賴管理方式」為待決項，且它擋住兩件事：
引入 HarfBuzz／ICU（`TXT`），以及引入測試框架（目前以退出碼＋CTest，無框架依賴）。

三個候選：vcpkg、CMake `FetchContent`、git submodule。

## 決定本題的三個專案事實

| 事實 | 影響 |
|---|---|
| **Krepis 是庫，不是應用** | 消費者（Notist 及未來的 `-ist`）的建置走 Flutter → Gradle／Xcode／CMake。要求他們先裝套件管理器是把成本轉嫁出去 |
| **目標平台含 Android NDK 與 iOS** | 交叉編譯是最痛的一段。依賴若以**專案自己的 toolchain file** 編譯，行為與主專案一致 |
| **依賴預期極少**（0 個，預計 2–3 個） | 自動依賴解析的價值低；管理器本身的成本相對變高 |

## 候選比較

| | vcpkg | `FetchContent` | git submodule |
|---|---|---|---|
| 需要額外工具 | **要** | 不用（CMake 內建） | 不用（git 內建） |
| 依賴用誰的編譯設定 | vcpkg triplet | **專案自己的 toolchain** | **專案自己的 toolchain** |
| 交叉編譯 NDK／iOS | triplet 存在但相對脆弱 | 一致 | 一致 |
| 對消費者的負擔 | **他也得裝 vcpkg** | 無感 | `clone --recursive`（易忘） |
| 離線建置 | 需 cache | 首次需網路（來源目錄可覆寫） | **完全離線** |
| 版本鎖定 | manifest ＋ baseline | `GIT_TAG` 可指 commit SHA | commit SHA，最嚴 |
| 依賴的依賴 | **自動解析** | 手動接 | 手動接 |
| 打補丁 | port overlay，最麻煩 | patch command，中等 | **直接改，最容易** |

## 決策

### 1. 以 `FetchContent` 為預設

理由是上表加上三個專案事實：不需消費者裝任何東西、依賴以專案自身 toolchain 編譯
（交叉編譯行為一致）、且依賴數量少到不需要自動解析。

### 2. 退到 submodule 的條件

**需要對依賴打補丁，且補丁不小時**改用 submodule。submodule 的補丁維護遠比
`FetchContent` 的 patch command 容易。

### 3. 配套規則（引入任何依賴時必須遵守）

- **pin 到 commit SHA，不得 pin tag。** tag 可被上游移動。
- **顯式設定依賴的 CMake option，不依賴其預設值。** 依賴的選項會成為你的 cache 變數。
- **以 `EXCLUDE_FROM_ALL` 引入**，避免依賴的 install 規則污染 Krepis 的安裝目標。
- **提供 `FETCHCONTENT_SOURCE_DIR_<NAME>` 覆寫路徑**，以支援離線建置與 CI 快取。
- **依賴的依賴必須明確處理**，不得假設會自動解決。

### 4. 重新評估 vcpkg 的觸發條件

**若確定需要 ICU**，本決策必須重新評估。ICU 龐大且交叉編譯至 NDK／iOS 困難，
是唯一足以翻轉本決策的依賴。

## 這份決策關閉哪一道閘門

**引入第三方庫的方式已定案**，任何需要 HarfBuzz、測試框架或其他依賴的工作不再被
「依賴管理方式未決」擋住。

## 被淘汰的方案

| 方案 | 處置 | 理由 |
|---|---|---|
| vcpkg | **淘汰**（除非需要 ICU） | ①Krepis 是庫，要求消費者裝 vcpkg 是把成本轉嫁到 Notist 的 Flutter／Gradle／Xcode 建置流程；②依賴只有 2–3 個時，自動解析的收益低於管理器本身的成本；③NDK／iOS triplet 相對脆弱，而交叉編譯正是本專案最痛的一段 |
| submodule 為預設 | **淘汰為預設，保留為退路** | 多一層 git 操作且 `--recursive` 易被遺忘；`FetchContent` 已能以來源目錄覆寫達成離線 |

## 後果

### 正面

- 消費者建置 Krepis 不需安裝任何額外工具。
- 依賴以專案自身 toolchain 編譯，交叉編譯到 NDK／iOS 的行為與主專案一致。

### 負面

- **每次乾淨建置都要重新編譯依賴**，CI 必須有 cache。
- 依賴的依賴要手動處理。
- 依賴的 CMake target 與選項會進入 Krepis 的建置圖，需要紀律隔離。

## 附帶觀察

這項早期觀察已被後續實測取代：`TXT-0001` 正式選定 HarfBuzz 10.1.0、libunibreak 6.1 與
SheenBidi 2.8 的單一跨平台 stack，因此 P1 即會觸發本決策。三者必須 pin commit SHA、以 Krepis
toolchain 編譯並隔離上游 install／ALL；不引入 ICU，所以不觸發重新評估 vcpkg 的條件。

## 相關決策

- **本決策依賴**：[`FND-0001`](FND-0001-scope-language-boundary-and-rejections.md)（Krepis 是庫，
  不得把成本轉嫁給消費者）。
- **依賴本決策**：`TXT-*`（HarfBuzz／ICU 的引入）、`00-foundation` 的測試框架選型。
