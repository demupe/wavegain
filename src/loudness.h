#ifndef WG_LOUDNESS_H
#define WG_LOUDNESS_H
#include <stddef.h>
typedef struct wg_meter WG_METER;
typedef enum { WG_PEAK_PRECISE = 0, WG_PEAK_FAST = 1 } WG_PEAK_MODE;
WG_METER *wg_meter_create(unsigned long rate, unsigned int channels, int loudness, int peak);
WG_METER *wg_meter_create_mode(unsigned long rate, unsigned int channels, int loudness, int peak,
	WG_PEAK_MODE peak_mode);
int wg_meter_add(WG_METER *meter, double **pcm, size_t frames);
int wg_meter_finish(WG_METER *meter, double *lufs, double *peak);
int wg_meter_album(WG_METER **meters, size_t count, double *lufs);
void wg_meter_free(WG_METER *meter);
#endif
