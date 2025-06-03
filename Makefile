# 設定交叉編譯器和相關選項
CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -Werror
LDFLAGS =

# 設定交叉編譯器與架構
CROSS_COMPILE = aarch64-linux-gnu-

# 指定 Linux kernel source tree
KDIR := /home/dragonwu0919/Projects/EOS_25sp
PWD  := $(shell pwd)

# 這裡列出你要 build 的模組檔（.c）
obj-m := kitchen.o order_receive.o order_send.o

# 預設目標：編譯所有模組
all:
	$(MAKE) -C $(KDIR) $(CFLAGS) ARCH=arm64 M=$(PWD) CROSS_COMPILE=$(CROSS_COMPILE) modules

# 清理編譯產物
clean:
	$(MAKE) -C $(KDIR) ARCH=arm64 M=$(PWD) clean


sync:
	scp /home/dragonwu0919/Projects/EOS-final-projecy/* dragonwu0919@192.168.222.222:/home/dragonwu0919/Project


gui:
	gcc -Wall gui.c -o gui -lncurses -g
	objdump -d gui > $@.a

dev_simulator:
	gcc -Wall dev_simulator.c -o dev_simulator -lncurses -g
	objdump -d dev_simulator > $@.a

net:
	gcc netlink_listener.c -o netListener