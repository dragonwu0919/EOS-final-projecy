CC = gcc
CFLAGS = -Wall -Wextra -O2
DEBUG_FLAGS = -g -O0
LDFLAGS =

SRC = kitchenImp.c netlink_listener.c order_named_pipe.c read_named_pipe.c
OBJ = $(SRC:.c=.o)
TARGETS = netlink_listener order_named_pipe read_named_pipe kitchenImp

.PHONY: all clean debug

# Default build
all: $(TARGETS)

# Debug build
debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean $(TARGETS)

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