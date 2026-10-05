#!/bin/bash
# ============================================================
#  跑跑看：自己寫的記憶體配置器（malloc）在你的 Mac 上編譯、測試、比速度
#
#  這個檔案在 Finder 裡雙擊就會打開「終端機」來執行。它會做四件事：
#    1. 編譯配置器（靜態函式庫、可以取代系統 malloc 的動態函式庫、除錯版）。
#    2. 跑全部的正確性測試：單元測試、隨機操作測試、多執行緒壓力測試、
#       除錯模式抓錯測試，以及「讓一般程式改用我們的 malloc」的測試。
#    3. 跑一個幾秒鐘的小比賽：同一支程式裡，系統 malloc 和 mallockit
#       各做一樣的事，比每次操作花幾奈秒（ns）。
#    4. 用瀏覽器打開完整的量測結果網頁（作者在 Apple M5 上量的）。
#
#  需要先裝好：Xcode Command Line Tools（提供 make、clang、python3）。
#  沒裝的話，在終端機執行：xcode-select --install
# ============================================================

# 先切換到這個檔案所在的資料夾。資料夾名稱有空白和中文，
# 所以 "$(dirname "$0")" 一定要用雙引號包起來。
cd "$(dirname "$0")" || exit 1

pause_and_exit() {
  echo
  read -r -p "按 Enter 關閉視窗..."
  exit "$1"
}

# 檢查需要的工具有沒有裝
for tool in make cc python3; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "找不到 $tool。請先在終端機執行：xcode-select --install"
    pause_and_exit 1
  fi
done

echo "== 1/4 編譯（make build，最多同時用 4 個核心）..."
if ! make -s -j4 build >/dev/null; then
  echo "編譯失敗，上面的訊息會說明原因。"
  pause_and_exit 1
fi
echo "   完成：build/ 資料夾裡有 libmallockit.a、libmallockit.dylib 和測試程式。"

echo
echo "== 2/4 正確性測試（約 30 秒）"
echo "   ok 代表通過，FAIL 代表失敗（會印出哪一行檢查沒過、用的亂數種子）。"
if ! ./build/test_unit | tail -n 6; then
  echo "單元／壓力測試有失敗！請把上面的 FAIL 那幾行拿去問人。"
  pause_and_exit 1
fi
echo "   --- 除錯模式：故意做錯事（寫超過、重複 free...），看它抓不抓得到"
if ! ./build/test_guard | tail -n 3; then
  echo "除錯模式測試有失敗！"
  pause_and_exit 1
fi
echo "   --- 讓普通程式（C 和 C++）改用 mallockit 執行"
if ! make -s test-override 2>&1 | grep -E "smoke"; then
  echo "取代系統 malloc 的測試失敗！"
  pause_and_exit 1
fi

echo
echo "== 3/4 小比賽（每項跑 3 次取中位數；數字是每次操作的奈秒數，越小越快）"
echo "   這台電腦如果同時在做別的事，數字會跳動，這是正常的。"
./build/ubench --quick | tee build/ubench-quick.txt

echo
echo "== 4/4 摘要"
python3 - <<'PY'
rows = [l.split() for l in open("build/ubench-quick.txt") if l[0].isalpha() and not l.startswith("benchmark")]
for r in rows:
    name = " ".join(r[:-2]); s, m = float(r[-2]), float(r[-1])
    print(f"   {name:28s} mallockit 是系統的 {s / m:4.1f} 倍快" if m < s else f"   {name:28s} mallockit 比系統慢（{m / s:.1f} 倍時間）")
PY

echo
echo "完成！瀏覽器會打開完整的量測結果：docs/results.html"
echo "（那份是作者在 Apple M5 上，和系統 malloc、mimalloc、jemalloc 比的結果。）"
open docs/results.html
pause_and_exit 0
