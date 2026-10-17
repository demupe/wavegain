#ifndef WG_OUTPUT_H
#define WG_OUTPUT_H
char *wg_output_absolute(const char *path, int directory);
char *wg_output_join(const char *directory, const char *name);
int wg_output_mkdirs(const char *path);
/* 1 inside/equal, 0 outside, -1 if the physical location cannot be checked. */
int wg_output_inside(const char *directory, const char *path);
int wg_output_publish(const char *temporary, const char *destination);
#endif
