/* Exercise native no-replace publication with hard links disabled, and the
 * safe fallback when the native operation is unavailable. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#ifdef _WIN32
#include <assert.h>
#include <string.h>
#include <windows.h>
#include "../src/output.c"
static void write_file(const char *path, const char *text)
{
	FILE *out = fopen(path, "wb");
	assert(out && fwrite(text, 1, strlen(text), out) == strlen(text) && !fclose(out));
}
static void expect_file(const char *path, const char *text)
{
	char bytes[32] = {0};
	FILE *in = fopen(path, "rb");
	assert(in && fread(bytes, 1, sizeof(bytes), in) == strlen(text) && !fclose(in));
	assert(!strcmp(bytes, text));
}
int main(void)
{
	char base[MAX_PATH], folder[MAX_PATH], from[MAX_PATH + 32], to[MAX_PATH + 32];
	DWORD length = GetTempPathA(sizeof(base), base);
	assert(length && length < sizeof(base));
	assert(GetTempFileNameA(base, "wgp", 0, folder) && DeleteFileA(folder) && CreateDirectoryA(folder, NULL));
	assert(snprintf(from, sizeof(from), "%s\\temporary", folder) > 0);
	assert(snprintf(to, sizeof(to), "%s\\destination", folder) > 0);
	write_file(from, "new audio");
	assert(wg_output_publish(from, to));
	expect_file(to, "new audio");
	assert(GetFileAttributesA(from) == INVALID_FILE_ATTRIBUTES);
	/* A destination appearing after preflight must still never be replaced. */
	write_file(from, "another audio");
	assert(!wg_output_publish(from, to));
	expect_file(to, "new audio"); expect_file(from, "another audio");
	assert(DeleteFileA(from) && DeleteFileA(to) && RemoveDirectoryA(folder));
	puts("Windows publication: successful move and post-preflight collision preserve destination and temporary bytes");
	return 0;
}
#else
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#endif

static int link_disabled, link_calls, native_error, copy_write_error, write_interrupted;
static int test_link(const char *, const char *);
static ssize_t test_write(int, const void *, size_t);
#define link test_link
#define write test_write
#if defined(__APPLE__)
static int test_rename(const char *, const char *, unsigned int);
#define renamex_np test_rename
#elif defined(__linux__) && defined(SYS_renameat2)
static long test_syscall(long, ...);
#define syscall test_syscall
#endif
#include "../src/output.c"
#undef link
#undef write
#undef renamex_np
#undef syscall

static int test_link(const char *src, const char *dst)
{
	link_calls++;
	if (link_disabled) { errno = EPERM; return -1; }
	return link(src, dst);
}
static ssize_t test_write(int fd, const void *buffer, size_t count)
{
	if (copy_write_error) { errno = ENOSPC; return -1; }
	if (write_interrupted) { write_interrupted = 0; errno = EINTR; return -1; }
	if (count > 3) count = 3; /* Force short writes in the copy fallback. */
	return write(fd, buffer, count);
}
#if defined(__APPLE__)
static int test_rename(const char *src, const char *dst, unsigned int flags)
{
	assert(flags == RENAME_EXCL);
	if (native_error) { errno = native_error; return -1; }
	return renamex_np(src, dst, flags);
}
#elif defined(__linux__) && defined(SYS_renameat2)
static long test_syscall(long number, ...)
{
	va_list args;
	int srcfd, dstfd;
	const char *src, *dst;
	unsigned int flags;
	assert(number == SYS_renameat2);
	va_start(args, number);
	srcfd = va_arg(args, int); src = va_arg(args, const char *);
	dstfd = va_arg(args, int); dst = va_arg(args, const char *);
	flags = va_arg(args, unsigned int);
	va_end(args);
	assert(srcfd == AT_FDCWD && dstfd == AT_FDCWD && flags == 1U);
	if (native_error) { errno = native_error; return -1; }
	return syscall(number, srcfd, src, dstfd, dst, flags);
}
#endif

static void write_file(const char *name, const char *value)
{
	FILE *f = fopen(name, "wb");
	assert(f && fwrite(value, 1, strlen(value), f) == strlen(value));
	assert(!fclose(f));
}

static void collision_tests(void)
{
	char data[16] = {0};
	struct stat st;
	FILE *f;
	write_file("temporary", "new audio");
	write_file("destination", "original");
	assert(!wg_output_publish("temporary", "destination"));
	assert(!stat("temporary", &st));
	f = fopen("destination", "rb");
	assert(f && fread(data, 1, sizeof(data), f) == strlen("original"));
	assert(!fclose(f) && !strcmp(data, "original"));
	assert(!unlink("destination"));
	assert(!symlink("missing-target", "destination"));
	assert(!wg_output_publish("temporary", "destination"));
	assert(!lstat("destination", &st) && S_ISLNK(st.st_mode));
	assert(!stat("temporary", &st));
	assert(!unlink("destination"));
	assert(!mkdir("destination", 0700));
	assert(!wg_output_publish("temporary", "destination"));
	assert(!stat("destination", &st) && S_ISDIR(st.st_mode));
	assert(!rmdir("destination"));
	assert(!unlink("temporary"));
}

int main(void)
{
	char folder[] = "/tmp/wavegain-publish-XXXXXX";
	struct stat st;
	assert(mkdtemp(folder) && !chdir(folder));
#if defined(__APPLE__) || (defined(__linux__) && defined(SYS_renameat2))
	link_disabled = 1;
	write_file("temporary", "new audio");
	assert(wg_output_publish("temporary", "destination"));
	assert(!link_calls && stat("temporary", &st) && errno == ENOENT);
	assert(!stat("destination", &st) && st.st_size == 9);
	assert(!unlink("destination"));
	collision_tests();
	assert(!link_calls);
	/* Permission failures must not be retried as a different operation. */
	native_error = EACCES;
	write_file("temporary", "new audio");
	assert(!wg_output_publish("temporary", "destination") && !link_calls);
	assert(!unlink("temporary"));
	native_error = ENOSYS;
#endif
	link_disabled = 0;
	write_file("temporary", "new audio");
	assert(wg_output_publish("temporary", "destination") && link_calls);
	assert(stat("temporary", &st) && errno == ENOENT);
	assert(!unlink("destination"));
	collision_tests();
	link_disabled = 1;
	write_interrupted = 1;
	write_file("temporary", "new audio");
	assert(!chmod("temporary", 0640));
	assert(wg_output_publish("temporary", "destination"));
	assert(!stat("destination", &st) && st.st_size == 9 && (st.st_mode & 0777) == 0640);
	assert(stat("temporary", &st) && errno == ENOENT);
	assert(!unlink("destination"));
	collision_tests();
	copy_write_error = 1;
	write_file("temporary", "new audio");
	assert(!wg_output_publish("temporary", "destination"));
	assert(!stat("temporary", &st));
	assert(!stat("destination", &st) && st.st_size == 0);
	assert(!unlink("destination"));
	assert(!unlink("temporary"));
	assert(!chdir("/") && !rmdir(folder));
	puts("Output publication: native no-link, collisions/symlinks, link/copy fallbacks, permissions and failed-copy retention passed");
	return 0;
}
#endif
