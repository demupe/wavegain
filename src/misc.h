#ifndef MISC_H
#define MISC_H

#ifdef _WIN32
/* <stdint.h> rather than __int64: MinGW only defines __int64 once a system
 * header has been included, and dither.c includes this header first. */
#include <stdint.h>
typedef int64_t             Int64_t;
typedef uint64_t            Uint64_t;
#define chdir _chdir
#define strdup _strdup
#define getcwd _getcwd
#define getpid _getpid
#else
typedef signed   long long  Int64_t;
typedef unsigned long long  Uint64_t;
#endif

void file_error(const char* message, ...);
char* last_path(const char* path);
extern void write_log(const char *fmt, ...);

#endif /* MISC_H */
