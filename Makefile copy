# 設定交叉編譯器和相關選項
CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -Werror
LDFLAGS =

# 設定交叉編譯器與架構
CROSS_COMPILE = aarch64-linux-gnu-

# 指定 Linux kernel source tree
KDIR := /home/ericwang/linux
PWD  := $(shell pwd)

# 這裡列出你要 build 的模組檔（.c）
obj-m := kitchen.o order_receive.o order_send.o

# 預設目標：編譯所有模組
all:
	$(MAKE) -C $(KDIR) $(CFLAGS) ARCH=arm64 M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) modules
	gcc -o netlink_listener netlink_listener.c
# 清理編譯產物
clean:
	$(MAKE) -C $(KDIR) ARCH=arm64 M=$(PWD) clean
	rm -f netlink_listener

sync:
	scp /home/ericwang/project/gui.c ericwang@192.168.222.222:/home/ericwang/project
	scp /home/ericwang/project/kitchen.ko ericwang@192.168.222.222:/home/ericwang/project
	scp /home/ericwang/project/order_send.ko ericwang@192.168.222.222:/home/ericwang/project
	scp /home/ericwang/project/order_receive.ko ericwang@192.168.222.222:/home/ericwang/project
	scp /home/ericwang/project/demo.sh ericwang@192.168.222.222:/home/ericwang/project
	scp /home/ericwang/project/order_comm.h ericwang@192.168.222.222:/home/ericwang/project
	


