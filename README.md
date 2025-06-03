# EOS-final-projecy

小組專案

# 注意事項
- gui.c 會開啟的 FIFO 檔是用 macro 設定的，在使用時須需要調整 DEBUG 的設置來調
  整上述 macro 指向的地址。DEBUG = true 時會啟動偵錯設定，此時的 fifo 地址指向的是
  由 dev_simulator 創建的裝置；DEBUG = false 則會把 fifo 指向 kernel driver 提供的裝置。