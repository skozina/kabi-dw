# Copyright(C) 2016, Red Hat, Inc., Stanislav Kozina
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

PROG=kabi-dw
SRCS=generate.c ksymtab.c utils.c main.c stack.c objects.c hash.c
SRCS += compare.c show.c

CC?=gcc
CFLAGS+=-Wall -O2 --std=gnu99 -D_GNU_SOURCE -c
LDFLAGS+=-ldw -lelf

YACC=bison
YACCFLAGS=-d -t

FLEX=flex
FLEXFLAGS=

OBJS=$(SRCS:.c=.o)
OBJS+=parser.yy.o parser.tab.o

.PHONY: clean all depend debug asan

ifeq (,$(findstring -c,$(CFLAGS)))
override CFLAGS+=-c
endif

ifeq (,$(findstring -D_GNU_SOURCE,$(CFLAGS)))
override CFLAGS+=-D_GNU_SOURCE
endif

ifeq (,$(findstring -ldw,$(LDFLAGS)))
override LDFLAGS+=-ldw
endif

ifeq (,$(findstring -lelf,$(LDFLAGS)))
override LDFLAGS+=-lelf
endif

all: $(PROG)

debug: CFLAGS+=-g -DDEBUG
debug: LDFLAGS:=$(LDFLAGS)
debug: FLEXFLAGS+=-d
debug: $(PROG)

asan-debug: CFLAGS+=-g -DDEBUG -fsanitize=address
asan-debug: LDFLAGS:=-lasan $(LDFLAGS)
asan-debug: FLEXFLAGS+=-d
asan-debug: $(PROG)

asan: CFLAGS+=-fsanitize=address
asan: LDFLAGS:=-lasan $(LDFLAGS)
asan: $(PROG)

$(PROG): $(OBJS)
	$(CC) -o $(PROG) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

parser.tab.c: parser.y
	$(YACC) $(YACCFLAGS) parser.y

parser.yy.c: parser.tab.c parser.l parser.h
	$(FLEX) $(FLEXFLAGS) -o parser.yy.c parser.l

depend: .depend

.depend: $(SRCS)
	$(CC) $(CFLAGS) -MM $^ > ./.depend

-include .depend

clean:
	rm -f $(PROG) $(OBJS) .depend parser *.tab.c *.tab.h *.yy.c
