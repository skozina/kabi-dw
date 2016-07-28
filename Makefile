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
SRCS=generate.c ksymtab.c check.c utils.c main.c stack.c

CC=gcc
CFLAGS=-Wall -O0 -g --std=c99 -c
LDFLAGS=-ldw -lelf

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
