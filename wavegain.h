#ifndef WAVEGAIN_H
#define WAVEGAIN_H

#include "main.h"

#define NO_PEAK -1.f
#define NO_GAIN -10000.f

extern int get_gain(const char *filename, double *track_peak, double *track_gain, double *dc_offset, double *offset,
	SETTINGS *settings, FILE_LIST *info);
extern int write_gains(const char *filename, double radio_gain, double audiophile_gain, double TitlePeak,
	double *dc_offset, double *album_dc_offset, SETTINGS *settings);

#endif /* WAVEGAIN_H */


extern double corrected_peak(const FILE_LIST *file, const double *dc, const SETTINGS *settings);
extern double selected_gain(double raw_gain, double peak, const SETTINGS *settings);
extern void report_gain(FILE_LIST *file, const double *dc, SETTINGS *settings);
extern double audio_duration(const char *filename, const SETTINGS *settings);
