/* Fault-inject Windows diagnostic formatting without changing system state. */
#include <stdio.h>
char log_file_name[512];
#ifndef _WIN32
int main(void) { puts("Windows diagnostic faults: skipped on POSIX"); return 0; }
#else
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <windows.h>

static int format_failure;
static DWORD expected_code;
static int diagnostic_vfprintf(FILE *out, const char *format, va_list args)
{
	/* Printing can overwrite the caller's thread-local Windows error. */
	SetLastError(ERROR_INVALID_HANDLE);
	return vfprintf(out, format, args);
}
static DWORD WINAPI diagnostic_format(DWORD flags, LPCVOID source, DWORD code,
                                      DWORD language, LPSTR output, DWORD size, va_list *args)
{
	const char message[] = "literal %s %n %%";
	char *buffer;
	(void)source; (void)language; (void)size; (void)args;
	assert(flags & FORMAT_MESSAGE_ALLOCATE_BUFFER);
	assert(code == expected_code);
	if (format_failure) return 0;
	buffer = LocalAlloc(LMEM_FIXED, sizeof(message));
	assert(buffer);
	memcpy(buffer, message, sizeof(message));
	*(char **)output = buffer;
	return sizeof(message) - 1;
}
#define vfprintf diagnostic_vfprintf
#define FormatMessageA diagnostic_format
#include "../recurse.c"
#undef vfprintf
#undef FormatMessageA

/* Recursion's unrelated application callbacks are not exercised here. */
int add_to_list(FILE_LIST **list, const char *file) { (void)list; (void)file; return 0; }
int process_files(FILE_LIST *list, SETTINGS *settings, const char *dir)
{ (void)list; (void)settings; (void)dir; return 0; }
void free_list(FILE_LIST *list) { (void)list; }

int main(void)
{
	int failure;
	DIRECTORY entry = {0};
	entry.find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
	entry.find_data.dwReserved0 = IO_REPARSE_TAG_SYMLINK;
	assert(directory_is_link(&entry));
	entry.find_data.dwReserved0 = IO_REPARSE_TAG_MOUNT_POINT;
	assert(directory_is_link(&entry));
	/* Cloud placeholders must not disappear from ordinary directory scans. */
	entry.find_data.dwReserved0 = 0x9000001A; /* IO_REPARSE_TAG_CLOUD */
	assert(!directory_is_link(&entry));
	entry.find_data.dwReserved0 = IO_REPARSE_TAG_SYMLINK;
	entry.find_data.dwFileAttributes = FILE_ATTRIBUTE_REPARSE_POINT;
	assert(!directory_is_link(&entry)); /* file links retain existing behavior */
	entry.find_data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
	assert(!directory_is_link(&entry)); /* reserved tag is invalid without the flag */
	assert(wg_output_inside(".", "") == -1);
	for (failure = 0; failure <= 1; failure++) {
		FILE *capture = tmpfile();
		int saved;
		char text[256] = {0};
		assert(capture);
		fflush(stderr);
		saved = _dup(_fileno(stderr));
		assert(saved >= 0 && !_dup2(_fileno(capture), _fileno(stderr)));
		format_failure = failure;
		expected_code = 12345;
		SetLastError(expected_code);
		file_error_win("Scan: %s: ", "path");
		fflush(stderr);
		assert(!_dup2(saved, _fileno(stderr)));
		_close(saved);
		rewind(capture);
		assert(fread(text, 1, sizeof(text) - 1, capture) > 0);
		assert(strstr(text, failure ? "Scan: path: Windows error 12345." : "Scan: path: literal %s %n %%"));
		fclose(capture);
	}
	puts("Windows diagnostics, directory-link/cloud classification and failed containment checks passed");
	return 0;
}
#endif
