CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS =

SRC = kitchenImp.c netlink_listener.c order_named_pipe.c read_named_pipe.c
OBJ = $(SRC:.c=.o)
TARGETS = netlink_listener order_named_pipe read_named_pipe

.PHONY: all clean

all: $(TARGETS)

kitchenImp: kitchenImp.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

netlink_listener: netlink_listener.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

order_named_pipe: order_named_pipe.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

read_named_pipe: read_named_pipe.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGETS)