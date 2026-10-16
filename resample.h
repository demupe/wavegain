#ifndef WG_RESAMPLE_H
#define WG_RESAMPLE_H
/* Streaming 2:1 analysis-only decimator; output audio is never resampled. */
typedef struct {
	double taps[127], history[2][127];
	unsigned long long position;
	int cursor, channels;
} WG_DECIMATOR;
void wg_decimator_init(WG_DECIMATOR *state, int channels);
int wg_decimator_process(WG_DECIMATOR *state, double **pcm, int frames);
#endif
