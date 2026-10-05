CC ?= clang
CFLAGS ?= -O2 -Wall
PREFIX ?= /data/data/com.termux/files/usr

SHIMDIR ?= $(HOME)/ccmusl/root/lib

all: pieload execshim.so

pieload: pieload.c
	$(CC) $(CFLAGS) -o $@ $<

execshim.so: execshim.c
	$(CC) -O2 -Wall -fPIC -shared -nostdlib -ffreestanding \
		-fno-stack-protector -fno-builtin -o $@ $<

seccomp-probe: seccomp-probe.c
	$(CC) $(CFLAGS) -o $@ $<

install: pieload execshim.so
	install -m 755 pieload $(PREFIX)/bin/pieload
	install -d $(SHIMDIR)
	install -m 644 execshim.so $(SHIMDIR)/execshim.so

clean:
	rm -f pieload execshim.so seccomp-probe

.PHONY: all install clean
