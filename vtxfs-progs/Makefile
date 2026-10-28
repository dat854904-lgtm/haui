# SPDX-License-Identifier: GPL-2.0-only
.PHONY: all tools test clean install
all tools:
	$(MAKE) -C tools all
test:
	TMPDIR=/dev/shm bash tests/run.sh
clean:
	$(MAKE) -C tools clean
install:
	$(MAKE) -C tools install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
