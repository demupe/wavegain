/* Audit-only instrumentation: stop before the Windows -e allocation overrun.
 * No external command or input audio is supplied. Compile with root sources
 * except main.c, which is included below. This probe is not part of the gate. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

static char *command_allocation;
static void *audit_malloc(size_t size)
{
	void *p = malloc(size);
	if (size == 8192) command_allocation = p;
	return p;
}
static int audit_sprintf(char *dest, const char *format, ...)
{
	va_list ap, copy;
	int needed, result;
	uintptr_t start = (uintptr_t)command_allocation, address = (uintptr_t)dest;
	va_start(ap, format);
	va_copy(copy, ap);
	needed = vsnprintf(NULL, 0, format, copy);
	va_end(copy);
	if (start && address >= start && address - start < 8192 &&
	    needed >= (int)(8192 - (address - start))) {
		fprintf(stderr, "AUDIT: sprintf would write %d bytes plus NUL at offset %llu into 8192-byte command allocation\n",
		        needed, (unsigned long long)(address - start));
		exit(99);
	}
	result = vsprintf(dest, format, ap);
	va_end(ap);
	return result;
}
#define malloc audit_malloc
#define sprintf audit_sprintf
#define main wavegain_main
#include "../src/main.c"
#undef main
#undef sprintf
#undef malloc
int main(void)
{
	char argument[9001];
	char *argv[] = {"audit", "-e", "nonexistent-audit-command", argument, NULL};
	memset(argument, 'x', sizeof(argument) - 1);
	argument[sizeof(argument) - 1] = 0;
	return wavegain_main(4, argv);
}
