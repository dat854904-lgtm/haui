// SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
/*
 * mount.vtxfs — helper cho `mount -t vtxfs`.
 * Được util-linux gọi: mount.vtxfs <dev> <dir> [-o opts] [-f] [-v]
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>

#define VTXFS_MAX_SUBVOL_OPT 4096

static int parse_opts(char *opts, unsigned long *flags, char *data, size_t dn)
{
	char *save = NULL, *tok;

	*flags = 0;
	data[0] = '\0';
	if (!opts)
		return 0;
	for (tok = strtok_r(opts, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		if (strcmp(tok, "ro") == 0)
			*flags |= MS_RDONLY;
		else if (strcmp(tok, "rw") == 0)
			*flags &= ~MS_RDONLY;
		else if (strcmp(tok, "nosuid") == 0)
			*flags |= MS_NOSUID;
		else if (strcmp(tok, "suid") == 0)
			*flags &= ~MS_NOSUID;
		else if (strcmp(tok, "nodev") == 0)
			*flags |= MS_NODEV;
		else if (strcmp(tok, "dev") == 0)
			*flags &= ~MS_NODEV;
		else if (strcmp(tok, "noexec") == 0)
			*flags |= MS_NOEXEC;
		else if (strcmp(tok, "exec") == 0)
			*flags &= ~MS_NOEXEC;
		else if (!strcmp(tok, "user") || !strcmp(tok, "users"))
			*flags |= MS_NOSUID | MS_NODEV | MS_NOEXEC;
		else if (!strcmp(tok, "owner") || !strcmp(tok, "group"))
			*flags |= MS_NOSUID | MS_NODEV;
		else if (!strcmp(tok, "remount"))
			*flags |= MS_REMOUNT;
		else if (!strcmp(tok, "bind"))
			*flags |= MS_BIND;
		else if (!strcmp(tok, "noatime"))
			*flags |= MS_NOATIME;
		else if (!strcmp(tok, "nodiratime"))
			*flags |= MS_NODIRATIME;
		else if (!strcmp(tok, "sync"))
			*flags |= MS_SYNCHRONOUS;
		else if (!strcmp(tok, "dirsync"))
			*flags |= MS_DIRSYNC;
		else if (!strcmp(tok, "async"))
			*flags &= ~MS_SYNCHRONOUS;
		else if (!strcmp(tok, "relatime"))
			*flags |= MS_RELATIME;
		else if (!strcmp(tok, "strictatime"))
			*flags |= MS_STRICTATIME;
		else if (!strcmp(tok, "defaults") || !strcmp(tok, "auto") ||
			 !strcmp(tok, "noauto") || !strcmp(tok, "nofail") ||
			 !strcmp(tok, "_netdev") || !strcmp(tok, "nouser"))
			; /* userspace-only mount(8) options */
		else if (strncmp(tok, "subvol=", 7) == 0) {
			if (strlen(data) + strlen(tok) + 2 > dn)
				return -ENAMETOOLONG;
			if (data[0])
				strncat(data, ",", dn - strlen(data) - 1);
			strncat(data, tok, dn - strlen(data) - 1);
		} else {
			fprintf(stderr,
				"mount.vtxfs: unsupported option '%s'\n", tok);
			return -EINVAL; /* Unknown flags are never silently ignored. */
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *dir = NULL, *opts = NULL;
	unsigned long flags = 0;
	char data[VTXFS_MAX_SUBVOL_OPT] = {0};
	char *opts_dup = NULL;
	int i, fake = 0, rc;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			opts = argv[++i];
		else if (strcmp(argv[i], "-f") == 0)
			fake = 1;
		else if (strcmp(argv[i], "-v") == 0)
			; /* verbose, bỏ qua */
		else if (!dev)
			dev = argv[i];
		else if (!dir)
			dir = argv[i];
	}
	if (!dev || !dir) {
		fprintf(stderr, "usage: mount.vtxfs <dev> <dir> [-o opts]\n");
		return 1;
	}
	opts_dup = opts ? strdup(opts) : NULL;
	if (parse_opts(opts_dup, &flags,
		       data, sizeof(data)) != 0) {
		fprintf(stderr, "mount.vtxfs: tuỳ chọn không hợp lệ\n");
		free(opts_dup);
		return 1;
	}
	free(opts_dup);
	if (fake) {
		printf("mount.vtxfs: (fake) %s -> %s flags=0x%lx data=%s\n",
		       dev, dir, flags, data);
		return 0;
	}
	rc = mount(dev, dir, "vtxfs", flags, data[0] ? data : NULL);
	if (rc != 0) {
		fprintf(stderr, "mount.vtxfs: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}
