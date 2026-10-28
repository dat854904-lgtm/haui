# SPDX-License-Identifier: GPL-2.0-only
CC      ?= gcc
CFLAGS  += -std=c17 -O2 -Wall -Wextra -Werror -pedantic \
           -I../../include -I../../uapi
CORE := ../../core/vtxfs_csum.c ../../core/vtxfs_leaf.c \
        ../../core/vtxfs_leaf_balance.c ../../core/vtxfs_internal.c \
        ../../core/vtxfs_internal_balance.c ../../core/vtxfs_btree_ro.c \
        ../../core/vtxfs_btree_rw.c ../../core/vtxfs_vag.c \
        ../../core/vtxfs_engine.c ../../core/vtxfs_posix.c \
        ../../core/vtxfs_fsck.c ../../core/vtxfs_stream.c \
        ../../core/vtxfs_compress.c
LDLIBS  ?= -lzstd -llz4 -lz
CMDS := main.c common.c cmds-filesystem.c cmds-subvolume.c cmds-inspect.c \
        cmds-rescue.c cmds-property.c cmds-reflink.c cmds-balance.c \
        cmds-defrag.c cmds-reaper.c cmds-rescue-fix-data-checksum.c cmds-restore.c \
        cmds-device.c cmds-scrub.c cmds-quota.c cmds-send.c cmds-receive.c cmds-frag.c \
        cmds-admin.c cmds-bench.c cmds-ctl.c cmds-file.c cmds-check.c cmds-format.c cmds-image.c cmds-cp.c

all: vtxfs
vtxfs: $(CMDS) $(CORE) common.h commands.h
	$(CC) $(CFLAGS) $(CMDS) $(CORE) $(LDLIBS) -o $@
clean:
	rm -f vtxfs

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SYMLINKS := vtxfsctl vtxfs-stat vtxfs-map vtxfs-block vtxfs-bitmap vtxfs-bench vtxfs-admin vtxfs-verify vtxfs-frag vtxfs-scrub vtxfs-gui vtxfs-sys vtxfs-file vtxfs-check vtxfs-format vtxfs-label vtxfs-cp

install: vtxfs
	install -d -m 0755 $(DESTDIR)$(BINDIR)
	install -m 0755 vtxfs $(DESTDIR)$(BINDIR)/vtxfs
	@for s in $(SYMLINKS); do \
		ln -sf vtxfs $(DESTDIR)$(BINDIR)/$$s; \
	done

.PHONY: all clean install
