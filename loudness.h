#ifndef WG_LOUDNESS_H
#define WG_LOUDNESS_H
#include <stddef.h>
typedef struct wg_meter WG_METER;
WG_METER *wg_meter_create(unsigned long rate, unsigned int channels, int loudness, int peak);
int wg_meter_add(WG_METER *meter, double **pcm, size_t frames);
int wg_meter_finish(WG_METER *meter, double *lufs, double *peak);
int wg_meter_album(WG_METER **meters, size_t count, double *lufs);
void wg_meter_free(WG_METER *meter);
#endif
