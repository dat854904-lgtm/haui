# SPDX-License-Identifier: GPL-2.0-only
CC      ?= gcc
CFLAGS  += -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
           -I../include -I../uapi
CORE := ../core/vtxfs_csum.c ../core/vtxfs_leaf.c \
        ../core/vtxfs_leaf_balance.c ../core/vtxfs_internal.c \
        ../core/vtxfs_internal_balance.c ../core/vtxfs_btree_ro.c \
        ../core/vtxfs_btree_rw.c ../core/vtxfs_vag.c \
        ../core/vtxfs_engine.c ../core/vtxfs_posix.c \
        ../core/vtxfs_fsck.c ../core/vtxfs_stream.c \
        ../core/vtxfs_compress.c
LDLIBS  ?= -lzstd -llz4 -lz

BINS := mkfs.vtxfs fsck.vtxfs vtxfs-inspect mount.vtxfs \
        vtxfs-balance vtxfs-defrag vtxfs-send vtxfs-receive replay-log

all: $(BINS) subdirs

mkfs.vtxfs: mkfs_vtxfs.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
fsck.vtxfs: fsck_vtxfs.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
vtxfs-inspect: vtxfs_inspect.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
mount.vtxfs: mount.vtxfs.c
	$(CC) $(CFLAGS) $< -o $@
vtxfs-balance: vtxfs-balance.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
vtxfs-defrag: vtxfs-defrag.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
vtxfs-send: vtxfs-send.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
vtxfs-receive: vtxfs-receive.c $(CORE)
	$(CC) $(CFLAGS) $< $(CORE) $(LDLIBS) -o $@
replay-log: replay-log.c
	$(CC) $(CFLAGS) $< -o $@

subdirs:
	$(MAKE) -C vtxfs
	$(MAKE) -C vtxfs-image

clean:
	rm -f $(BINS)
	$(MAKE) -C vtxfs clean
	$(MAKE) -C vtxfs-image clean

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
SBINDIR ?= $(PREFIX)/sbin

install: all
	install -d -m 0755 $(DESTDIR)$(SBINDIR)
	install -d -m 0755 $(DESTDIR)$(BINDIR)
	install -m 0755 mkfs.vtxfs $(DESTDIR)$(SBINDIR)/mkfs.vtxfs
	install -m 0755 fsck.vtxfs $(DESTDIR)$(SBINDIR)/fsck.vtxfs
	install -m 0755 mount.vtxfs $(DESTDIR)$(SBINDIR)/mount.vtxfs
	install -m 0755 vtxfs-inspect $(DESTDIR)$(BINDIR)/vtxfs-inspect
	install -m 0755 vtxfs-balance $(DESTDIR)$(BINDIR)/vtxfs-balance
	install -m 0755 vtxfs-defrag $(DESTDIR)$(BINDIR)/vtxfs-defrag
	install -m 0755 vtxfs-send $(DESTDIR)$(BINDIR)/vtxfs-send
	install -m 0755 vtxfs-receive $(DESTDIR)$(BINDIR)/vtxfs-receive
	install -m 0755 replay-log $(DESTDIR)$(BINDIR)/replay-log
	$(MAKE) -C vtxfs install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	$(MAKE) -C vtxfs-image install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)

.PHONY: all subdirs clean install
