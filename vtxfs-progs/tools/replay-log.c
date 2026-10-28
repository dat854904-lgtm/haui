// SPDX-License-Identifier: GPL-2.0-only
/*
 * replay-log.c — Fast, standalone dm-log-writes replay tool
 *
 * Implements the Linux dm-log-writes on-disk replay protocol:
 * Replays block I/O sequences logged by dm-log-writes onto a target image,
 * supporting --end-mark, --start-mark, and --check fua with --fsck verification.
 */

#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WRITE_LOG_MAGIC   0x6a736677736872ULL
#define WRITE_LOG_VERSION 1ULL

#define LOG_FLUSH_FLAG    (1ULL << 0)
#define LOG_FUA_FLAG      (1ULL << 1)
#define LOG_DISCARD_FLAG  (1ULL << 2)
#define LOG_MARK_FLAG     (1ULL << 3)
#define LOG_METADATA_FLAG (1ULL << 4)

struct log_write_super {
	uint64_t magic;
	uint64_t version;
	uint64_t nr_entries;
	uint32_t sectorsize;
} __attribute__((packed));

struct log_write_entry {
	uint64_t sector;
	uint64_t nr_sectors;
	uint64_t flags;
	uint64_t data_len;
} __attribute__((packed));

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s --log <log_dev> --replay <target_dev> [options]\n"
		"Options:\n"
		"  --log <file>        Device or image containing dm-log-writes stream\n"
		"  --replay <file>     Device or image to replay writes onto\n"
		"  --start-mark <str>  Start checking after this mark\n"
		"  --end-mark <str>    Stop replay when this mark is encountered\n"
		"  --check <fua>       Run check at each FUA event (e.g. 'fua')\n"
		"  --fsck <cmd>        Command to run at check points\n"
		"  --verbose           Verbose output\n"
		"  --help              Show this help\n",
		prog);
}

static ssize_t read_all(int fd, void *buf, size_t count)
{
	size_t done = 0;
	while (done < count) {
		ssize_t n = read(fd, (char *)buf + done, count - done);
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)n;
	}
	return (ssize_t)done;
}

static ssize_t pwrite_all(int fd, const void *buf, size_t count, off_t offset)
{
	size_t done = 0;
	while (done < count) {
		ssize_t n = pwrite(fd, (const char *)buf + done, count - done,
				   offset + (off_t)done);
		if (n <= 0) {
			if (n < 0 && errno == EINTR)
				continue;
			return -1;
		}
		done += (size_t)n;
	}
	return (ssize_t)done;
}

int main(int argc, char **argv)
{
	const char *log_path = NULL;
	const char *replay_path = NULL;
	const char *start_mark = NULL;
	const char *end_mark = NULL;
	const char *check_type = NULL;
	const char *fsck_cmd = NULL;
	int verbose = 0;

	static struct option long_opts[] = {
		{"log", required_argument, NULL, 'l'},
		{"replay", required_argument, NULL, 'r'},
		{"start-mark", required_argument, NULL, 's'},
		{"end-mark", required_argument, NULL, 'e'},
		{"check", required_argument, NULL, 'c'},
		{"fsck", required_argument, NULL, 'k'},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "l:r:s:e:c:k:vh", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'l':
			log_path = optarg;
			break;
		case 'r':
			replay_path = optarg;
			break;
		case 's':
			start_mark = optarg;
			break;
		case 'e':
			end_mark = optarg;
			break;
		case 'c':
			check_type = optarg;
			break;
		case 'k':
			fsck_cmd = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!log_path || !replay_path) {
		usage(argv[0]);
		return 1;
	}

	int log_fd = open(log_path, O_RDONLY);
	if (log_fd < 0) {
		fprintf(stderr, "replay-log: failed to open log '%s': %s\n",
			log_path, strerror(errno));
		return 1;
	}

	int replay_fd = open(replay_path, O_RDWR);
	if (replay_fd < 0) {
		fprintf(stderr, "replay-log: failed to open replay target '%s': %s\n",
			replay_path, strerror(errno));
		close(log_fd);
		return 1;
	}

	/* Read super block (sector 0, 512 bytes) */
	uint8_t super_buf[512];
	if (read_all(log_fd, super_buf, 512) != 512) {
		fprintf(stderr, "replay-log: failed to read superblock from '%s'\n", log_path);
		close(log_fd);
		close(replay_fd);
		return 1;
	}

	struct log_write_super *sb = (struct log_write_super *)super_buf;
	uint64_t magic = le64toh(sb->magic);
	uint64_t version = le64toh(sb->version);
	uint64_t nr_entries = le64toh(sb->nr_entries);
	uint32_t sectorsize = le32toh(sb->sectorsize);

	if (sectorsize == 0)
		sectorsize = 512;

	if (magic != WRITE_LOG_MAGIC) {
		fprintf(stderr, "replay-log: invalid magic 0x%" PRIx64 " (expected 0x%" PRIx64 ")\n",
			magic, (uint64_t)WRITE_LOG_MAGIC);
		close(log_fd);
		close(replay_fd);
		return 1;
	}

	if (version != WRITE_LOG_VERSION) {
		fprintf(stderr, "replay-log: unsupported version %" PRIu64 "\n", version);
		close(log_fd);
		close(replay_fd);
		return 1;
	}

	if (verbose) {
		printf("replay-log: %" PRIu64 " entries, sectorsize %u\n",
		       nr_entries, sectorsize);
	}

	int active = (start_mark == NULL);
	uint64_t entry_idx = 0;
	int check_fua = (check_type && strcmp(check_type, "fua") == 0);

	/* Buffer for log_write_entry (takes full sector = 512 bytes) */
	uint8_t entry_buf[512];
	void *data_buf = NULL;
	size_t data_cap = 0;

	while (nr_entries == 0 || entry_idx < nr_entries) {
		ssize_t r = read_all(log_fd, entry_buf, 512);
		if (r == 0)
			break; /* EOF */
		if (r != 512) {
			fprintf(stderr, "replay-log: short read on entry %" PRIu64 "\n", entry_idx);
			break;
		}

		struct log_write_entry *entry = (struct log_write_entry *)entry_buf;
		uint64_t sector = le64toh(entry->sector);
		uint64_t nr_sectors = le64toh(entry->nr_sectors);
		uint64_t flags = le64toh(entry->flags);
		uint64_t data_len = le64toh(entry->data_len);
		entry_idx++;

		if (flags & LOG_MARK_FLAG) {
			/* Mark name follows */
			size_t mark_read = data_len ? data_len : 512;
			/* Round up to sector boundary */
			size_t aligned_len = ((mark_read + 511) / 512) * 512;
			char *mark_str = malloc(aligned_len + 1);
			if (!mark_str) {
				fprintf(stderr, "replay-log: OOM\n");
				break;
			}
			if (read_all(log_fd, mark_str, aligned_len) != (ssize_t)aligned_len) {
				free(mark_str);
				fprintf(stderr, "replay-log: failed reading mark string\n");
				break;
			}
			mark_str[aligned_len] = '\0';

			if (verbose) {
				printf("replay-log: MARK '%s'\n", mark_str);
			}

			if (start_mark && strcmp(mark_str, start_mark) == 0)
				active = 1;

			if (end_mark && strcmp(mark_str, end_mark) == 0) {
				if (verbose)
					printf("replay-log: reached end mark '%s'\n", end_mark);
				free(mark_str);
				break;
			}
			free(mark_str);
			continue;
		}

		if (nr_sectors > 0) {
			size_t bytes = (size_t)nr_sectors * 512;
			if (bytes > data_cap) {
				void *newp = realloc(data_buf, bytes);
				if (!newp) {
					fprintf(stderr, "replay-log: OOM allocating %zu bytes\n", bytes);
					break;
				}
				data_buf = newp;
				data_cap = bytes;
			}

			if (!(flags & LOG_DISCARD_FLAG)) {
				if (read_all(log_fd, data_buf, bytes) != (ssize_t)bytes) {
					fprintf(stderr, "replay-log: failed reading data for sector %" PRIu64 "\n", sector);
					break;
				}
				if (active) {
					off_t off = (off_t)sector * 512;
					if (pwrite_all(replay_fd, data_buf, bytes, off) != (ssize_t)bytes) {
						fprintf(stderr, "replay-log: failed writing %zu bytes at offset %lld: %s\n",
							bytes, (long long)off, strerror(errno));
						break;
					}
				}
			} else {
				/* DISCARD */
				if (active) {
					off_t off = (off_t)sector * 512;
					memset(data_buf, 0, bytes);
					pwrite_all(replay_fd, data_buf, bytes, off);
				}
			}
		}

		if (flags & LOG_FLUSH_FLAG) {
			if (active)
				fdatasync(replay_fd);
		}

		if (flags & LOG_FUA_FLAG) {
			if (active) {
				fdatasync(replay_fd);
				if (check_fua && fsck_cmd) {
					if (verbose)
						printf("replay-log: FUA reached at entry %" PRIu64 ", running fsck...\n", entry_idx);
					int rc = system(fsck_cmd);
					if (WEXITSTATUS(rc) != 0) {
						fprintf(stderr, "replay-log: fsck failed at FUA entry %" PRIu64 " with exit code %d\n",
							entry_idx, WEXITSTATUS(rc));
						free(data_buf);
						close(log_fd);
						close(replay_fd);
						return 1;
					}
				}
			}
		}
	}

	free(data_buf);
	fdatasync(replay_fd);
	close(log_fd);
	close(replay_fd);

	if (verbose)
		printf("replay-log: replayed %" PRIu64 " entries successfully.\n", entry_idx);

	return 0;
}
