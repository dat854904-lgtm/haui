# SPDX-License-Identifier: GPL-2.0-only
CC      ?= gcc
CFLAGS  += -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
           -I../../include -I../../uapi
CORE := ../../core/vtxfs_csum.c ../../core/vtxfs_leaf.c \
        ../../core/vtxfs_leaf_balance.c ../../core/vtxfs_internal.c \
        ../../core/vtxfs_internal_balance.c ../../core/vtxfs_btree_ro.c \
        ../../core/vtxfs_btree_rw.c ../../core/vtxfs_vag.c \
        ../../core/vtxfs_engine.c ../../core/vtxfs_posix.c \
        ../../core/vtxfs_fsck.c ../../core/vtxfs_compress.c
LDLIBS  ?= -lzstd -llz4 -lz

all: vtxfs-image
vtxfs-image: main.c common.c common.h sanitize.c sanitize.h \
             image-create.c image-restore.c metadump.h
	$(CC) $(CFLAGS) main.c common.c sanitize.c image-create.c \
	      image-restore.c $(CORE) $(LDLIBS) -o $@
clean:
	rm -f vtxfs-image

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

install: vtxfs-image
	install -d -m 0755 $(DESTDIR)$(BINDIR)
	install -m 0755 vtxfs-image $(DESTDIR)$(BINDIR)/vtxfs-image

.PHONY: all clean install
