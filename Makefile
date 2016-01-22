PROG=kabi-dw
SRCS=kabi-dw.c main.c

CC=gcc
CFLAGS=-Wall -O0 --std=c99 -c
LDFLAGS=-ldw

OBJS=$(SRCS:.c=.o)

.PHONY: clean all depend

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) -o $(PROG) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

depend: .depend

.depend: $(SRCS)
	$(CC) $(CFLAGS) -MM $^ > ./.depend

-include .depend

clean:
	rm -f $(PROG) $(OBJS) .depend
