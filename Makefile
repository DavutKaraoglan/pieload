CC ?= clang
CFLAGS ?= -O2 -Wall
PREFIX ?= /data/data/com.termux/files/usr

all: pieload

pieload: pieload.c
	$(CC) $(CFLAGS) -o $@ $<

seccomp-probe: seccomp-probe.c
	$(CC) $(CFLAGS) -o $@ $<

install: pieload
	install -m 755 pieload $(PREFIX)/bin/pieload

clean:
	rm -f pieload seccomp-probe

.PHONY: all install clean
