# mallockit

**從零用 C 寫的多執行緒記憶體配置器（`malloc`），和 macOS 系統配置器、mimalloc、jemalloc 實測比較，
並用隨機操作測試、ThreadSanitizer、除錯（guard）模式和突變測試驗證正確性。**

[English](README.md) · [設計說明](docs/DESIGN.md) · [報告](docs/report.md) ·
[結果網頁](docs/results.html) · [導讀（初學者）](docs/導讀.zh-TW.md)

mallockit 實作了整組 C 記憶體配置 API（`malloc`、`free`、`calloc`、`realloc`、`posix_memalign`、
`aligned_alloc`、`memalign`、`valloc`、`pvalloc`、`malloc_usable_size`／`malloc_size`），支援 64 位元
macOS 和 Linux，而且可以替換「沒改過的程式」的系統配置器：Linux 用 `LD_PRELOAD`，macOS 用預設
malloc zone 加上 dyld interpose。起點是 MIT 6.172《軟體效能工程》的動態記憶體配置器專案（用空間
利用率和速度評分），後來擴充到能跑 DuckDB、SQLite、CPython 測試和 Lua 測試。設計大量參考
mimalloc；這是學習用的重新實作，不是新的配置器設計（見[相關專案](#相關專案)）。

## 量測結果

Apple M5（4 個效能核心＋6 個節能核心）、macOS 27，**在同時有其他工作的機器上**量測；
每項交錯執行 3～5 次取中位數；所有配置器跑同一支沒改過的執行檔（`DYLD_INSERT_LIBRARIES`）。
完整表格、圖和範圍：[docs/results.html](docs/results.html)。

| 工作負載（mimalloc-bench） | 執行緒 | mallockit | macOS 系統 | mimalloc 2.2.4 | jemalloc 5.3.0 |
|---|---:|---:|---:|---:|---:|
| glibc-simple（時間） | 1 | **0.92 s** | 1.36 s | 0.94 s | 2.25 s |
| cfrac（時間） | 1 | 1.71 s | 2.01 s | **1.70 s** | 2.68 s |
| espresso（時間） | 1 | **2.15 s** | 2.38 s | 2.18 s | 2.60 s |
| larson（次/秒） | 4 | **2.21 億** | 0.15 億 | 2.17 億 | 1.60 億 |
| larson（次/秒） | 10 | **3.80 億** | 0.36 億 | **3.80 億** | 2.55 億 |
| glibc-thread（次/秒） | 10 | **11.5 億** | 3.8 億 | 10.2 億 | 2.9 億 |
| xmalloc-test（free/秒） | 4 | 3.56 億 | 2.64 億 | **5.90 億** | 2.05 億 |
| mstress（時間） | 10 | 2.06 s | 1.89 s | **1.43 s** | 2.51 s |
| malloc-large（時間） | 1 | 0.56 s | **0.26 s** | 0.26 s | 1.49 s |
| malloc-large（峰值 MiB） | 1 | 444 | 672 | 622 | **414** |

34 個（工作負載, 執行緒數）組合的速度比幾何平均：mallockit 是**系統配置器的 2.08 倍**、
**jemalloc 的 1.76 倍**、**mimalloc 的 0.93 倍**。明顯輸的有：`malloc-large`（刻意用速度換記憶體）、
`xmalloc-test`（輸 mimalloc）、4 個執行緒以上的 `mstress`；[報告](docs/report.md)逐項分析。
jemalloc 在 macOS 上以 malloc zone 方式運作，每次呼叫都要多經過 libmalloc 的轉接。

**6.172 風格分數**（7 個自己產生的 trace；利用率 = 峰值有效資料量 / 峰值記憶體增量，
速度相對於系統配置器，分數 = U^0.5 · T^0.5 的幾何平均）：

| | mallockit | 系統 | mimalloc | jemalloc |
|---|---:|---:|---:|---:|
| 分數 | **1.34** | 0.75 | 1.22 | 0.76 |
| 利用率（幾何平均） | 0.72 | 0.56 | **0.76** | 0.72 |
| 速度（相對系統） | **2.49×** | 1.00× | 1.95× | 0.81× |


## 運作方式

```
 malloc(n ≤ 1 KiB)                                   free(p)
   heap = 這個執行緒的 heap（TSD 欄位）                 seg  = p & ~(4 MiB - 1)
   page = heap->direct[(n+15)/16]                      page = seg->pages[(p - seg) >> shift]
   從 page->free 拿一格 -> 完成（不用鎖）               seg->thread_id == 自己 ? 放進 page->local_free
   否則：合併別人還的和自己還的，或拿新的一頁                               : CAS 放進 page->xthread_free

 4 MiB segment（對齊 4 MiB）
 +----------+---------+---------+-----+---------+
 | 標頭 +   | page 1  | page 2  | ... | page 63 |   small：64 KiB 一頁，格子 16 B - 8 KiB
 | page 0   |         |         |     |         |   medium：512 KiB 一頁，格子 8 - 64 KiB
 +----------+---------+---------+-----+---------+   large：一個物件一個 segment（有快取，best fit）
```

* **44 種格子大小**：128 位元組以上每翻倍分 4 級，內部浪費低於 20%。
* **Free-list sharding**：每一頁有三條清單：配置用、擁有者自己還的、別的執行緒還的（無鎖）。
  最常見的 malloc 和 free 不用鎖，也不用原子指令。
* **滿頁與延遲釋放**：沒有空格的頁離開佇列；別的執行緒第一次還東西到這種頁時，透過清單指標上的
  標記改送到擁有者的 heap，讓擁有者找得回來。
* **型別穩定的 heap**：heap 結構永遠不釋放，所以執行緒結束的同時別人還東西也安全。新執行緒整套接手
  已結束執行緒的 heap；其他執行緒在跟作業系統要記憶體前，會先回收無主 heap 裡空掉的頁。
* **還給作業系統**：空頁和空 segment 閒置 10 毫秒後歸還（`MADV_DONTNEED`；macOS 用
  `MADV_FREE_REUSABLE`），快取裡最多留 64 MiB 還沒歸還的記憶體。
* **對齊**不需要標頭：格子大小對 64 KiB 以內任何 2 的次方取整後仍是合法大小，所以對齊的配置直接
  從一般的格子出。
* **除錯模式**（`libmallockit-debug`）：用 canary、每格一個配置位元、釋放後填滿特定值，抓出寫超過、
  重複 free（跨執行緒也行）、free 錯指標、free 後還寫入、free list 被破壞。

每個部分和被否決的替代方案寫在 [docs/DESIGN.md](docs/DESIGN.md)。

## 正確性驗證

<!-- VERIFICATION -->

## 使用方式

```sh
make build                       # build/libmallockit.{a,so|dylib}、build/libmallockit-debug.*
make test                        # 單元＋隨機＋壓力測試、除錯模式、替換系統 malloc 的測試

# Linux：任何動態連結的程式
LD_PRELOAD=$PWD/build/libmallockit.so python3 my_script.py

# macOS：沒有 SIP 保護、沒有 hardened runtime 的程式（Homebrew 裝的、自己編的）
DYLD_INSERT_LIBRARIES=$PWD/build/libmallockit.dylib /opt/homebrew/bin/python3.12 my_script.py

# 結束時印統計、調整歸還延遲、用除錯版
MALLOCKIT_STATS=1 MALLOCKIT_PURGE_DELAY=10 DYLD_INSERT_LIBRARIES=$PWD/build/libmallockit-debug.dylib ./prog
```

當函式庫用：連結 `build/libmallockit.a`，呼叫 [`include/mallockit.h`](include/mallockit.h) 裡的
`mk_malloc`／`mk_free`／…（另有 `mk_collect`、`mk_stats_get`，除錯版有 `mk_set_error_handler`）。
在 Mac 上雙擊 `跑跑看.command` 會一步步編譯、測試、跑一個短的速度比較。

其他指令：`make tsan`／`ubsan`／`asan`（sanitizer；ASan 只在 Linux，這版 macOS 上會卡住）、
`make mutants`、`make bench`（把 mimalloc、jemalloc、mimalloc-bench 固定版本抓到 `build/ext`）、
`make score`、`make ablation`、`make realprogs`、`make page`。

## 限制

* macOS 對 SIP 保護和 hardened runtime 的程式會忽略 `DYLD_INSERT_LIBRARIES`；在 macOS 上靜態連結
  mallockit 也不會替換其他函式庫用的配置器（two-level namespace）。`leaks(1)`／`heap(1)` 看不到
  mallockit 的區塊。
* 實測輸的項目：`malloc-large`（反覆配置 5～25 MiB 的緩衝區）和 `xmalloc-test`（一個執行緒配置、
  另一個釋放），數字和分析在報告裡。
* 閒置執行緒的空頁要等它下次走慢速路徑才會歸還；沒有背景歸還執行緒。
* 64 KiB 到 4 MiB 的物件每個佔一個（可快取重用的）4 MiB segment 的位址空間。
* 只支援 64 位元 macOS（arm64、x86-64）和 Linux（x86-64、arm64，glibc）；不支援 Windows 和 32 位元。

## 相關專案

* **MIT 6.172 專案 3**（動態記憶體配置器）：起點；學生在模擬的 `sbrk` heap 上實作
  `malloc`／`free`／`realloc`，用 trace 的利用率和速度評分。mallockit 的 `bench/score.py` 沿用這個
  想法，但用自己產生的 trace 和真的作業系統。
* **mimalloc**（Leijen、Zorn、de Moura，2019）：free-list sharding、本地／跨執行緒清單、延遲釋放、
  segment 分頁、macOS zone + interpose。mallockit 照這個設計。差別：mallockit 讓新執行緒整套接手
  已結束執行緒的 heap（mimalloc 是一個個 segment 放棄再回收）、64 KiB 以上的物件用整個快取的
  segment 而不是 arena 裡的 span，而且沒有 mimalloc 的安全功能、heap API 和多平台支援。
* **jemalloc**（Evans，2006；FreeBSD、Facebook 使用）：arena、每執行緒快取、extent、依時間衰減的
  歸還。**tcmalloc**（Google）：每執行緒或每 CPU 快取，下面接中央 free list。
* **snmalloc**（Liétar 等，2019）：跨執行緒釋放用批次訊息傳遞。
* **Hoard**（Berger 等，2000）：每處理器 heap 加 superblock，限制記憶體膨脹；false sharing 測試
  （`cache-scratch`、`cache-thrash`）出自這裡。
* **mimalloc-bench**：這裡用的測試程式集。

## 目錄

| 路徑 | 內容 |
|---|---|
| `src/` | `os.c`、`segment.c`、`page.c`、`heap.c`、`alloc.c`、`debug.c`、`override.c`；`mallockit.c` 把它們合成一個編譯單元 |
| `tests/` | 各層單元測試、隨機 trace、壓力測試、除錯模式測試、替換配置器的測試程式 |
| `bench/` | 量測腳本（`run.py`）、6.172 風格分數（`score.py`、`trace_replay.c`）、下載腳本、工作負載 |
| `tools/` | 突變測試、消融實驗、真實程式、驗證摘要 |
| `analysis/page.py` | 由 `results/` 產生 `docs/results.html` |
| `results/` | 文件中每個數字背後的原始量測資料（JSON lines） |

## 授權

MIT，見 [LICENSE](LICENSE)。
