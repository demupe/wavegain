/* Separate-file output paths and atomic no-clobber publication. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "misc.h"
#include "output.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define SEP '\\'
#define is_separator(c) ((c) == '/' || (c) == '\\')
#define canonical(path) _fullpath(NULL, (path), 0)
#define mkdir_one(path) _mkdir(path)
#else
#include <unistd.h>
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
	return !stat(path, &st) && (st.st_mode & S_IFDIR);
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

int wg_output_inside(const char *directory, const char *path)
{
	char *a = canonical(directory), *b = canonical(path);
	int result = 0;
	if (a && b) {
		size_t n = strlen(a);
#ifdef _WIN32
		result = !_strnicmp(a, b, n) && (!b[n] || is_separator(b[n]) || is_separator(a[n - 1]));
#else
		result = !strncmp(a, b, n) && (!b[n] || is_separator(b[n]) || is_separator(a[n - 1]));
#endif
	}
	free(a);
	free(b);
	return result;
}

int wg_output_publish(const char *temporary, const char *destination)
{
#ifdef _WIN32
	return MoveFileExA(temporary, destination, MOVEFILE_WRITE_THROUGH) != 0;
#else
	/* Both names are in the output directory. link fails if destination exists,
	 * including dangling symlinks, without ever replacing its directory entry. */
	if (link(temporary, destination)) return 0;
	if (unlink(temporary)) fprintf(stderr, " Warning: cannot remove temporary file %s.\n", temporary);
	return 1;
#endif
}
