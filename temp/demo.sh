#!/bin/bash

echo "🔄 卸載舊模組..."
sudo rmmod order_send 2>/dev/null
sudo rmmod kitchen 2>/dev/null || sudo modprobe -r kitchen
sudo rmmod order_receive 2>/dev/null

echo "🧹 清除殘留裝置節點..."
sudo rm -f /dev/chef_tasks /dev/kitchen

echo "📦 載入核心模組（依序）..."
sudo insmod order_receive.ko || { echo "❌ 載入 order_receive 失敗"; exit 1; }
echo "✅ order_receive 已載入"

sudo insmod kitchen.ko || { echo "❌ 載入 kitchen 失敗"; exit 1; }
echo "✅ kitchen 已載入"

sudo insmod order_send.ko || { echo "❌ 載入 order_send 失敗"; exit 1; }
echo "✅ order_send 已載入"

echo "🛠️ 編譯 GUI 程式..."
gcc -o gui gui.c -lncurses || { echo "❌ GUI 編譯失敗"; exit 1; }

echo "🚀 啟動 GUI"
sudo ./gui


