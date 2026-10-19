/* Separate-file output paths and no-clobber publication. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include "misc.h"
#include "output.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <wchar.h>
#define SEP '\\'
#define is_separator(c) ((c) == '/' || (c) == '\\')
#define canonical(path) _fullpath(NULL, (path), 0)
#define mkdir_one(path) _mkdir(path)
#else
#include <unistd.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#define SEP '/'
#define is_separator(c) ((c) == '/')
#define canonical(path) realpath((path), NULL)
#define mkdir_one(path) mkdir((path), 0777)
#endif

char *wg_output_join(const char *directory, const char *name)
{
	size_t a = strlen(directory), b = strlen(name);
	if (!a) return strdup(name);
	char *result;
	if (a > (size_t)-1 - b - 2) return NULL;
	result = malloc(a + b + 2);
	if (result) { memcpy(result, directory, a); result[a] = SEP; memcpy(result + a + 1, name, b + 1); }
	return result;
}

static int directory_exists(const char *path)
{
	struct stat st;
	return !stat(path, &st) && (st.st_mode & S_IFMT) == S_IFDIR;
}

int wg_output_mkdirs(const char *path)
{
	char *copy = strdup(path);
	size_t i;
	int ok = 1;
	if (!copy || !*copy) { free(copy); return 0; }
	for (i = 1; copy[i]; i++) if (is_separator(copy[i])) {
		char saved = copy[i];
		copy[i] = 0;
#ifdef _WIN32
		if (i == 2 && copy[1] == ':') { copy[i] = saved; continue; }
#endif
		if (!directory_exists(copy) && mkdir_one(copy)) { ok = 0; break; }
		copy[i] = saved;
	}
	if (ok && !directory_exists(path) && mkdir_one(path)) ok = 0;
	free(copy);
	return ok;
}

char *wg_output_absolute(const char *path, int directory)
{
	char *copy, *slash = NULL, *parent, *result;
	size_t i;
	if (!*path) return NULL;
	if (directory) return wg_output_mkdirs(path) ? canonical(path) : NULL;
	copy = strdup(path);
	if (!copy) return NULL;
	for (i = 0; copy[i]; i++) if (is_separator(copy[i])) slash = copy + i;
	if (slash && !slash[1]) { free(copy); return NULL; }
	if (slash) {
		const char *directory_name;
#ifdef _WIN32
		char drive[4] = {copy[0], ':', '\\', 0};
#endif
		*slash = 0;
		directory_name = *copy ? copy : "/";
#ifdef _WIN32
		if (strlen(copy) == 2 && copy[1] == ':') directory_name = drive;
#endif
		parent = wg_output_mkdirs(directory_name) ? canonical(directory_name) : NULL;
	} else parent = canonical(".");
	result = parent ? wg_output_join(parent, slash ? slash + 1 : copy) : NULL;
	free(parent);
	free(copy);
	return result;
}

#ifdef _WIN32
/* _fullpath normalizes spelling only. Resolve existing directory handles for
 * containment checks, including junctions, short names and drive aliases.
 * Keep NT paths private: output filenames still use the original spelling. */
static wchar_t *directory_identity(const char *path)
{
	HANDLE handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                            NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	wchar_t *result = NULL;
	DWORD capacity, length;
	if (handle == INVALID_HANDLE_VALUE) return NULL;
	capacity = GetFinalPathNameByHandleW(handle, NULL, 0, VOLUME_NAME_NT);
	if (capacity && (size_t)capacity <= SIZE_MAX / sizeof(*result)) {
		result = malloc((size_t)capacity * sizeof(*result));
		if (result) {
			length = GetFinalPathNameByHandleW(handle, result, capacity, VOLUME_NAME_NT);
			if (!length || length >= capacity) { free(result); result = NULL; }
		}
	}
	CloseHandle(handle);
	return result;
}
#endif

int wg_output_inside(const char *directory, const char *path)
{
#ifdef _WIN32
	wchar_t *a = directory_identity(directory), *b = directory_identity(path);
#else
	char *a = canonical(directory), *b = canonical(path);
#endif
	int result = -1;
	if (a && b) {
#ifdef _WIN32
		size_t n = wcslen(a), m = wcslen(b);
		if (n && n <= INT_MAX) {
			result = 0;
			if (m >= n) {
				int comparison = CompareStringOrdinal(a, (int)n, b, (int)n, TRUE);
				if (!comparison) result = -1;
				else result = comparison == CSTR_EQUAL && (!b[n] || b[n] == L'\\' || a[n - 1] == L'\\');
			}
		}
#else
		size_t n = strlen(a);
		result = !strncmp(a, b, n) && (!b[n] || is_separator(b[n]) || is_separator(a[n - 1]));
#endif
	}
	free(a);
	free(b);
	return result;
}

#ifndef _WIN32
static void remove_published_temporary(const char *temporary)
{
	if (unlink(temporary)) fprintf(stderr, " Warning: cannot remove temporary file %s.\n", temporary);
}

/* Last resort for filesystems lacking both exclusive rename and hard links.
 * O_EXCL still prevents replacement, but readers may see a partial copy.
 * On failure retain both names: unlinking destination could remove a file
 * another process put there after deleting our entry. */
static int publish_copy(const char *temporary, const char *destination)
{
	unsigned char buffer[32768];
	struct stat st;
	int input = -1, output = -1, created = 0, error;
	ssize_t count;
	input = open(temporary, O_RDONLY);
	if (input < 0) return 0;
	if (fstat(input, &st)) goto failed;
	output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (output < 0) goto failed;
	created = 1;
	for (;;) {
		size_t offset = 0;
		count = read(input, buffer, sizeof(buffer));
		if (count < 0) { if (errno == EINTR) continue; goto failed; }
		if (!count) break;
		while (offset < (size_t)count) {
			ssize_t written = write(output, buffer + offset, (size_t)count - offset);
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) { if (!written) errno = EIO; goto failed; }
			offset += (size_t)written;
		}
	}
	if (fchmod(output, st.st_mode & 0777)) goto failed;
	if (close(output)) { output = -1; goto failed; }
	output = -1;
	if (close(input)) { input = -1; goto failed; }
	remove_published_temporary(temporary);
	return 1;
failed:
	error = errno;
	if (output >= 0) close(output);
	if (input >= 0) close(input);
	if (created) fprintf(stderr, " Warning: incomplete output retained at %s.\n", destination);
	errno = error;
	return 0;
}
#endif

int wg_output_publish(const char *temporary, const char *destination)
{
#ifdef _WIN32
	return MoveFileExA(temporary, destination, MOVEFILE_WRITE_THROUGH) != 0;
#else
	/* Prefer exclusive rename: FAT/exFAT can rename but cannot create hard links.
	 * Never fall back to an ordinary rename, which could overwrite a racer. */
#if defined(__APPLE__)
	if (!renamex_np(temporary, destination, RENAME_EXCL)) return 1;
	if (errno != ENOTSUP && errno != ENOSYS && errno != EINVAL) return 0;
#elif defined(__linux__) && defined(SYS_renameat2)
	/* Direct syscall also works with libc versions predating renameat2().
	 * Linux UAPI defines RENAME_NOREPLACE as bit 0. */
	if (!syscall(SYS_renameat2, AT_FDCWD, temporary, AT_FDCWD, destination, 1U)) return 1;
	if (errno != EOPNOTSUPP && errno != ENOSYS && errno != EINVAL) return 0;
#endif
	/* Both names are in the output directory. link fails if destination exists,
	 * including dangling symlinks, without ever replacing its directory entry. */
	if (link(temporary, destination)) {
		if (errno == EPERM || errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENOSYS || errno == EMLINK)
			return publish_copy(temporary, destination);
		return 0;
	}
	remove_published_temporary(temporary);
	return 1;
#endif
}
