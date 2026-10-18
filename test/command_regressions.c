/* Exercise Windows command construction without executing a command. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static int api_fault, allocation, fail_allocation;
static void *command_malloc(size_t size)
{
	if (++allocation == fail_allocation) return NULL;
	return malloc(size);
}
static DWORD WINAPI command_full(LPCSTR name, DWORD capacity, LPSTR out, LPSTR *part)
{
	if (api_fault == 1) return 0;
	if (api_fault == 2) return 100;
	if (api_fault == 3 && capacity) return 0;
	return GetFullPathNameA(name, capacity, out, part);
}
static DWORD WINAPI command_short(LPCSTR name, LPSTR out, DWORD capacity)
{
	(void)name;
	if (api_fault == 4) return 5; /* Size changes between query and fill. */
	if (api_fault == 5) {
		if (capacity >= 5) { memcpy(out, "tool", 5); return 4; }
		return 5;
	}
	if (api_fault == 6) {
		if (capacity >= 10) { memcpy(out, "tool path", 10); return 9; }
		return 10;
	}
	return 0; /* Builtin or nonexistent executable: preserve the supplied name. */
}
#define malloc command_malloc
#define GetFullPathNameA command_full
#define GetShortPathNameA command_short
#endif
#define main wavegain_main
#include "../main.c"
#undef main
#ifdef _WIN32
#undef malloc
#undef GetFullPathNameA
#undef GetShortPathNameA
#endif

int main(void)
{
#ifdef _WIN32
	char large[9001], *argument[] = {large}, *args[] = {"a b", ""};
	char *command;
	memset(large, 'x', sizeof(large)); large[9000] = 0;
	command = build_command("echo", 2, args);
	assert(command && !strcmp(command, "echo \"a b\" \"\"")); free(command);
	assert(!build_command("echo", 1, argument));
	large[8184] = 0; /* 4 + space + quotes + 8184 = 8191. */
	command = build_command("echo", 1, argument);
	assert(command && strlen(command) == 8191); free(command);
	large[8184] = 'x'; large[8185] = 0;
	assert(!build_command("echo", 1, argument));
	for (api_fault = 1; api_fault <= 5; api_fault++) {
		command = build_command("echo", 0, NULL);
		if (api_fault == 1) assert(command && !strcmp(command, "echo"));
		else if (api_fault == 5) assert(command && !strcmp(command, "tool"));
		else assert(!command);
		free(command);
	}
	api_fault = 5;
	for (fail_allocation = 1; fail_allocation <= 3; fail_allocation++) {
		allocation = 0;
		assert(!build_command("echo", 0, NULL));
	}
	fail_allocation = api_fault = 0;
	command = build_command("echo hello", 0, NULL);
	assert(command && !strcmp(command, "echo hello")); free(command);
	api_fault = 6;
	command = build_command("echo", 0, NULL);
	assert(command && !strcmp(command, "\"\"tool path\"\"")); free(command);
	puts("Windows command limits, quoting, Win32 length faults and allocation failures passed");
#else
	puts("Windows command construction: skipped on POSIX");
#endif
	return 0;
}
