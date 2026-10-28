# SPDX-License-Identifier: GPL-2.0-only
# Root dispatcher: Linux kernel Kbuild lives at fs/vtxfs/Makefile.
KDIR ?= /lib/modules/$(shell uname -r)/build
PREFIX ?= /usr/local
INCLUDEDIR ?= $(PREFIX)/include

.PHONY: all kernel progs test clean install install-progs modules_install install-headers
all: kernel progs
kernel:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/fs/vtxfs CONFIG_VTXFS_FREE_CACHE_TEST=m modules
progs:
	$(MAKE) -C vtxfs-progs all
test:
	$(MAKE) -C vtxfs-progs test
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/fs/vtxfs clean
	$(MAKE) -C vtxfs-progs clean
modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/fs/vtxfs modules_install
install-headers:
	install -d -m 0755 $(DESTDIR)$(INCLUDEDIR)/vtxfs
	install -m 0644 fs/vtxfs/uapi/vtxfs_uapi.h $(DESTDIR)$(INCLUDEDIR)/vtxfs/
	install -m 0644 fs/vtxfs/include/vtxfs_disk.h $(DESTDIR)$(INCLUDEDIR)/vtxfs/
install-progs:
	$(MAKE) -C vtxfs-progs install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
install: install-progs install-headers
