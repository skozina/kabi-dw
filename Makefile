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
SRCS=generate.c ksymtab.c check.c utils.c main.c stack.c objects.c

CC=gcc
CFLAGS=-Wall -O0 -g --std=c99 -c
LDFLAGS=-ldw -lelf -lfl

YACC=bison
YACCFLAGS=-d -t

FLEX=flex
FLEXFLAGS=

OBJS=$(SRCS:.c=.o)
OBJS+=parser.yy.o parser.tab.o

.PHONY: clean all depend

all: $(PROG)

debug: CFLAGS += -DDEBUG
debug: FLEXFLAGS += -d
debug: $(PROG)


$(PROG): $(OBJS)
	$(CC) -o $(PROG) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $< -o $@

parser.tab.c: parser.y
	$(YACC) $(YACCFLAGS) parser.y

parser.yy.c: parser.tab.c parser.l parser.h
#	$(YACC) $(YACCFLAGS) parser.y
	$(FLEX) $(FLEXFLAGS) -o parser.yy.c parser.l
#	$(CC) $(CFLAGS) parser.tab.c parser.yy.c

depend: .depend

.depend: $(SRCS)
	$(CC) $(CFLAGS) -MM $^ > ./.depend

-include .depend

clean:
	rm -f $(PROG) $(OBJS) .depend parser *.tab.c *.tab.h *.yy.c
