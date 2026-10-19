PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
# Runs on every Apple Silicon Mac (macOS 11+), and Intel too.
ARCH = -arch arm64 -arch x86_64 -mmacosx-version-min=11.0

ntfs: ntfs.c
	$(CC) $(CFLAGS) $(ARCH) -o $@ $< -framework CoreFoundation -framework DiskArbitration
	strip $@
	codesign -s - -f $@

install: ntfs
	install -d $(PREFIX)/bin
	install -m 755 ntfs $(PREFIX)/bin/ntfs

uninstall:
	rm -f $(PREFIX)/bin/ntfs

clean:
	rm -f ntfs
.PHONY: install uninstall clean
