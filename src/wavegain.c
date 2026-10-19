/*
 * function: ReplayGain support for Wave files (http://www.replaygain.org)
 *
 * Reads in a Vorbis file, figures out the peak and ReplayGain levels and
 * applies the Gain tags to the Wave file
 *
 * This program is distributed under the GNU General Public License, version 
 * 2.1. A copy of this license is included with this source.
 *
 * Copyright (C) 2002-2004 John Edwards
 * Additional code by Magnus Holmgren and Gian-Carlo Pascutto
 * Linux patch by Marc Brooker
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#ifndef _WIN32
/* For handling file attributes (owner, group, permissions) */
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
# ifndef __APPLE__
#  include <sys/io.h>
# endif
#endif

#include <fcntl.h>

#ifndef __APPLE__
#include <malloc.h>
#endif

#include "gain_analysis.h"
#include "i18n.h"
#include "getopt.h"
#include "misc.h"
#include "audio.h"
#include "dither.h"
#include "main.h"
#include "wavegain.h"
#include "resample.h"
#include "output.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_RECURSIVE
#include "recurse.h"
#endif

/* Preserve the legacy quantizer's bit arithmetic without aliasing a double. */
static Int64_t round64(double value)
{
	Int64_t bits;
	/* Values this far beyond PCM range must saturate before the bit trick. */
	if (value > 0x1p40) return INT64_C(2147483648);
	if (value < -0x1p40) return -INT64_C(2147483648);
	double rounded = value + Dither.Add + (Int64_t)0x001FFFFD80000000LL;
	memcpy(&bits, &rounded, sizeof(bits));
	return bits - (Int64_t)0x433FFFFD80000000LL;
}
#define ROUND64(x) round64(x)

#ifndef _WIN32
#define _snprintf snprintf
#endif

extern int          write_to_log;
dither_t            Dither;
double              doubletmp;
double              total_samples;

/* Replaced with a double based function for consistency 2005-11-17
static float FABS(float x)
{
	unsigned int *ix=(unsigned int *)&x;
	*ix&=0x7fffffffUL;
	return(x);
}
*/
/* Dither output */
Int64_t dither_output(int dithering, int shapingtype, int i, double Sum, int k, int format)
{
	double Sum2;
	Int64_t val;
	/* Avoid overflowing the quantizer/history on extreme float or undo gain. */
	Sum = fmax(-2147483648., fmin(2147483648., Sum));
	if(dithering) {
		if(!shapingtype) {
			double  tmp = Random_Equi ( Dither.Dither );
			Sum2 = tmp - Dither.LastRandomNumber [k];
			Dither.LastRandomNumber [k] = (int)tmp;
			Sum2 = Sum += Sum2;
			val = ROUND64 (Sum2)  &  Dither.Mask;
		}
		else {
			Sum2  = Random_Triangular ( Dither.Dither ) - scalar16 ( Dither.DitherHistory[k], Dither.FilterCoeff + i );
			Sum  += Dither.DitherHistory [k] [(-1-i)&15] = (float)Sum2;
			Sum2  = Sum + scalar16 ( Dither.ErrorHistory [k], Dither.FilterCoeff + i );
			val = ROUND64 (Sum2)  &  Dither.Mask;
			Dither.ErrorHistory [k] [(-1-i)&15] = (float)(Sum - val);
		}
	}
	else
		val = (Int64_t)(ROUND64 (Sum));

	if (format == WAV_FMT_8BIT)
		val = val >> 24;
	else if (format == WAV_FMT_16BIT || format == WAV_FMT_AIFF)
		val = val >> 16;
	else if (format == WAV_FMT_24BIT)
		val = val >> 8;

	return (val);
}

/* The common integer path has no dither history or limiter dependencies.
 * Validate input first so the independent quantization loop can vectorize,
 * while retaining the legacy scaling, clamp and quantizer arithmetic. */
static int apply_plain_gain(double **pcm, int channels, int frames,
                            const double *dc, double scale, int bits)
{
	int ch, i, shift = 32 - bits;
	Int64_t maximum = (INT64_C(1) << (bits - 1)) - 1;
	/* Preserve the legacy symmetric 32-bit saturation at +/-2147483647. */
	Int64_t minimum = bits == 32 ? -maximum : -(INT64_C(1) << (bits - 1));
	for (ch = 0; ch < channels; ch++)
		for (i = 0; i < frames; i++)
			if (!isfinite(pcm[ch][i])) return 0;
	for (ch = 0; ch < channels; ch++) {
		for (i = 0; i < frames; i++) {
			double value = pcm[ch][i], sum;
			Int64_t quantized;
			if (dc) value -= dc[ch];
			value *= scale;
			sum = value * 2147483647.f;
			sum = fmax(-2147483648., fmin(2147483648., sum));
			quantized = ROUND64(sum) >> shift;
			if (quantized > maximum) quantized = maximum;
			else if (quantized < minimum) quantized = minimum;
			pcm[ch][i] = (double)quantized;
		}
	}
	return 1;
}

/* Get the gain and peak value for a file. Runs in audiophile mode if 
 * audiophile is true.
 *
 * If an error occured, 0 is returned (a message has been printed).
 */

/* Peaks are in ReplayGain's 32767 scale; extrema are normalized samples. */
double corrected_peak(const FILE_LIST *file, const double *dc, const SETTINGS *settings)
{
    double peak = 0.;
    int ch;
    for (ch = 0; ch < file->channels; ch++) {
        double correction = settings->no_offset || settings->fast ? 0. : dc[ch];
        double value = fmax(fabs(file->minimum[ch] - correction),
                            fabs(file->maximum[ch] - correction));
        peak = fmax(peak, value * 32767.);
    }
    return peak;
}

double selected_gain(double raw_gain, double peak, const SETTINGS *settings)
{
    double scale = pow(10., (raw_gain + settings->man_gain) * .05);
    /* One ReplayGain PCM unit of headroom also protects coarse quantization. */
    if (settings->clip_prev) scale = fmin(scale, 32767. / (peak + 1.));
    return scale > 0. ? 20. * log10(scale) : NO_GAIN;
}

double true_peak_gain(const FILE_LIST *file, const SETTINGS *settings)
{
	int format = settings->format ? (int)settings->format : file->format;
	int bits = settings->format ? (format == WAV_FMT_AIFF ? 16 : format == WAV_FMT_FLOAT ? 32 : format * 8) : file->samplesize;
	double margin = format == WAV_FMT_FLOAT ? 1e-7 : 4. / ldexp(1., bits - 1);
	double ceiling = pow(10., (settings->peak_ceiling - .1) / 20.) - margin;
	if (ceiling <= 0.) return NAN;
	return file->true_peak > 0. ? 20. * log10(ceiling / file->true_peak) : HUGE_VAL;
}

int measure_file(FILE_LIST *file, const double *dc, const SETTINGS *settings)
{
	wavegain_opt opt = {0};
	FILE *in = NULL;
	double *pcm[2] = {NULL, NULL};
	unsigned long long frames = 0;
	int ch, j, ok = 0;
	long count;
	if (file->meter) {
		opt.rate = file->rate;
		frames = file->frames;
		if (!wg_meter_finish(file->meter, &file->loudness, &file->true_peak)) goto done;
		goto measured;
	}
	in = fopen(file->filename, "rb");
	if (!in || !open_audio_file(in, &opt) || opt.channels != file->channels || opt.rate != file->rate) goto done;
	file->meter = wg_meter_create((unsigned long)opt.rate, opt.channels, settings->lufs, settings->true_peak);
	if (!file->meter) goto done;
	for (ch = 0; ch < opt.channels; ch++) if (!(pcm[ch] = malloc(BUFFER_LEN * sizeof(double)))) goto done;
	while ((count = opt.read_samples(opt.readdata, pcm, BUFFER_LEN, 0, 0)) > 0) {
		frames += count;
		for (ch = 0; ch < opt.channels; ch++) for (j = 0; j < count; j++)
			if (!settings->no_offset) pcm[ch][j] -= dc[ch];
		if (!wg_meter_add(file->meter, pcm, (size_t)count)) goto done;
	}
	if (count < 0 || frames != file->frames ||
	    !wg_meter_finish(file->meter, &file->loudness, &file->true_peak)) goto done;
measured:
	if (settings->lufs) {
		if (frames < (unsigned long long)((opt.rate + 5) / 10) * 4) {
			fprintf(stderr, " LUFS measurement requires at least 400 ms: %s.\n", file->filename); goto done;
		}
		file->raw_gain = isfinite(file->loudness) ? settings->target_lufs - file->loudness : 0.;
		fprintf(stderr, " Integrated loudness: %.2f LUFS | %s\n", file->loudness, file->filename);
	}
	if (settings->true_peak) {
		if (isnan(true_peak_gain(file, settings))) {
			fprintf(stderr, " Peak ceiling leaves insufficient room for output quantization.\n"); goto done;
		}
		fprintf(stderr, " Input true peak after DC correction: %.3f dBTP | %s\n",
		        file->true_peak > 0. ? 20. * log10(file->true_peak) : -HUGE_VAL, file->filename);
	}
	ok = 1;
done:
	free(pcm[0]); free(pcm[1]);
	if (opt.readdata) wav_close(opt.readdata);
	if (in) fclose(in);
	if (!ok) fprintf(stderr, " Error measuring loudness/true peak: %s.\n", file->filename);
	return ok;
}

void report_gain(FILE_LIST *file, const double *dc, SETTINGS *settings)
{
    double peak = corrected_peak(file, dc, settings);
    double scale;
    double left = 0., right = 0.;
    file->track_gain = selected_gain(file->raw_gain, peak, settings);
    if (settings->true_peak) file->track_gain = fmin(file->track_gain, true_peak_gain(file, settings));
    scale = pow(10., file->track_gain * .05);
    file->track_peak = peak * scale; /* Predicted peak BEFORE the limiter. */
    if (!settings->no_offset && !settings->fast) {
        left = trunc(dc[0] * -32768.) + 0.;
        right = file->channels == 2 ? trunc(dc[1] * -32768.) + 0. : 0.;
    }
    fprintf(stderr, " %+6.2lf dB | %6.0lf | %5.2lf | %8.0lf | %4.0f  |  %4.0f  | %s\n",
            file->track_gain, peak, scale, file->track_peak, left, right, file->filename);
    if (write_to_log)
        write_log(" %+6.2lf dB | %6.0lf | %5.2lf | %8.0lf | %4.0f  |  %4.0f  | %s\n",
                  file->track_gain, peak, scale, file->track_peak, left, right, file->filename);
    if ((settings->lufs || settings->true_peak) && file->track_gain + .005 < file->raw_gain + settings->man_gain)
        fprintf(stderr, " Peak prevention reduced the requested gain by %.2f dB.\n", file->raw_gain + settings->man_gain - file->track_gain);
    if (settings->scale && !settings->audiophile) fprintf(stdout, "%8.6lf", scale);
}

int get_gain(const char *filename, double *track_peak, double *track_gain,
             double *dc_offset, double *offset, SETTINGS *settings, FILE_LIST *info)
{
    wavegain_opt opt = {0};
    FILE *in = NULL;
    input_format *format = NULL;
    double *buffer[2] = {NULL, NULL};
    unsigned long long frames = 0;
    int ch, j, iteration = 0, sampled, ok = 0;
    Int64_t step;
    long count;
    int high_rate = 0;
    WG_DECIMATOR decimator;
    opt.force = settings->force;
    opt.std_in = !strcmp(filename, "-");
    in = opt.std_in ? stdin : fopen(filename, "rb");
    if (!in) { fprintf(stderr, " Not able to open input file %s.\n", filename); goto done; }
#ifdef _WIN32
    if (opt.std_in) _setmode(_fileno(in), _O_BINARY);
#endif
    format = open_audio_file(in, &opt);
    if (!format || opt.channels < 1 || opt.channels > 2) {
        fprintf(stderr, " Unsupported input file %s.\n", filename); goto done;
    }
    if (opt.gain_chunk && !settings->force) {
        fprintf(stderr, " Skipping File %s, it has already been processed.\n", filename); ok = 2; goto done;
    }
    high_rate = opt.rate == 176400 || opt.rate == 192000;
    if (high_rate) wg_decimator_init(&decimator, opt.channels);
    if (!settings->lufs && (settings->first_file || !settings->audiophile ? InitGainAnalysis(high_rate ? opt.rate / 2 : opt.rate) :
         ResetSampleFrequency(high_rate ? opt.rate / 2 : opt.rate)) != INIT_GAIN_ANALYSIS_OK) {
        fprintf(stderr, " Error Initializing Gain Analysis (non-standard samplerate?)\n"); goto done;
    }
    if (settings->first_file) {
        /* Same table header on the console and in the log. */
        static const char header[] =
            "\n Analyzing...\n\n    Gain   |  Peak  | Scale | New Peak |Left DC|Right DC| Track\n"
            "           |        |       |          |Offset | Offset |\n"
            " --------------------------------------------------------------\n";
        total_samples = 0.;
        fputs(header, stderr);
        if (write_to_log) write_log("%s", header);
    }
    sampled = settings->fast && !opt.std_in &&
              opt.total_samples_per_channel * (opt.samplesize / 8) * opt.channels > 8192000;
    step = (Int64_t)(opt.total_samples_per_channel * (opt.samplesize / 8) * opt.channels / 1200);
    info->channels = opt.channels;
    info->rate = opt.rate;
    info->samplesize = opt.samplesize;
    info->format = opt.format;
    if (settings->lufs && !(opt.rate == 8000 || opt.rate == 11025 || opt.rate == 12000 ||
        opt.rate == 16000 || opt.rate == 22050 || opt.rate == 24000 || opt.rate == 32000 ||
        opt.rate == 44100 || opt.rate == 48000 || opt.rate == 64000 || opt.rate == 88200 ||
        opt.rate == 96000 || high_rate)) {
        fprintf(stderr, " Unsupported sample rate for LUFS: %ld Hz.\n", opt.rate);
        goto done;
    }
    for (ch = 0; ch < opt.channels; ch++) {
        buffer[ch] = malloc(BUFFER_LEN * sizeof(double));
        if (!buffer[ch]) goto done;
        info->minimum[ch] = HUGE_VAL; info->maximum[ch] = -HUGE_VAL;
        offset[ch] = dc_offset[ch] = 0.;
    }
    if (settings->no_offset && (settings->lufs || settings->true_peak)) {
        info->meter = wg_meter_create((unsigned long)opt.rate, opt.channels, settings->lufs, settings->true_peak);
        if (!info->meter) goto done;
    }
    while (!sampled || iteration < 200) {
        Int64_t position = sampled ? step * (100 + iteration * 5) : 0;
        count = opt.read_samples(opt.readdata, buffer, BUFFER_LEN, sampled, position);
        if (count < 0) { fprintf(stderr, " Error reading %s.\n", filename); goto done; }
        if (!count) break;
        if (info->meter && !wg_meter_add(info->meter, buffer, (size_t)count)) goto done;
        frames += count;
		for (ch = 0; ch < opt.channels; ch++) {
			double minimum = info->minimum[ch], maximum = info->maximum[ch];
			/* Retain per-addition binary64 rounding on extended-precision targets. */
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
			double sum = offset[ch];
#else
			volatile double sum = offset[ch];
#endif
			double *samples = buffer[ch];
			for (j = 0; j < count; j++) {
				double value = samples[j];
				if (!isfinite(value)) { fprintf(stderr, " Non-finite PCM in %s.\n", filename); goto done; }
				minimum = fmin(minimum, value);
				maximum = fmax(maximum, value);
				if (!settings->fast) sum += value;
				if (!settings->lufs && !high_rate) samples[j] = value * 32767.;
			}
			info->minimum[ch] = minimum; info->maximum[ch] = maximum;
			offset[ch] = sum;
		}
        if (!settings->lufs) {
            int analysis_count = (int)count;
            if (high_rate) {
                analysis_count = wg_decimator_process(&decimator, buffer, (int)count);
                for (ch = 0; ch < opt.channels; ch++) for (j = 0; j < analysis_count; j++) buffer[ch][j] *= 32767.;
            }
            if (AnalyzeSamples(buffer[0], buffer[1], (size_t)analysis_count, opt.channels) != GAIN_ANALYSIS_OK) goto done;
        }
        iteration++;
    }
    if (!frames) { fprintf(stderr, " No audio samples in %s.\n", filename); goto done; }
    if (high_rate && !settings->lufs) {
        double tail[2][63] = {{0}}, *p[2] = {tail[0], tail[1]};
        int n = wg_decimator_process(&decimator, p, 63);
        for (ch = 0; ch < opt.channels; ch++) for (j = 0; j < n; j++) p[ch][j] *= 32767.;
        if (AnalyzeSamples(p[0], p[1], (size_t)n, opt.channels) != GAIN_ANALYSIS_OK) goto done;
    }
    info->raw_gain = settings->lufs ? 0. : GetTitleGain();
    if (info->raw_gain == GAIN_NOT_ENOUGH_SAMPLES) {
        fprintf(stderr, " Insufficient audio for ReplayGain in %s.\n", filename); goto done;
    }
    info->frames = sampled ? opt.total_samples_per_channel : frames;
    info->duration = (double)info->frames / opt.rate;
    for (ch = 0; ch < opt.channels; ch++)
        dc_offset[ch] = settings->fast ? 0. : offset[ch] / frames;
    *track_gain = selected_gain(info->raw_gain, corrected_peak(info, dc_offset, settings), settings);
    *track_peak = corrected_peak(info, dc_offset, settings) * pow(10., *track_gain * .05);
    total_samples += (double)info->frames;
    settings->first_file = 0;
    ok = 1;
done:
    free(buffer[0]); free(buffer[1]);
    if (opt.readdata) wav_close(opt.readdata);
    free(opt.header);
    if (in && in != stdin) fclose(in);
    return ok;
}

double audio_duration(const char *filename, const SETTINGS *settings)
{
    wavegain_opt opt = {0};
    FILE *in = fopen(filename, "rb");
    double duration = 0.;
    if (!in) return 0.;
    if (open_audio_file(in, &opt) && opt.rate > 0 && opt.gain_chunk &&
        (opt.gain_scale != 1. || settings->force))
        duration = (double)opt.total_samples_per_channel / opt.rate;
    if (opt.readdata) wav_close(opt.readdata);
    fclose(in);
    return duration;
}

/* Use the ReplayGain calculations to adjust the gain on the wave file.
 * If audiophile_gain is selected, that value is used, otherwise the
 * radio_gain value is used.
 */
int write_gains(const char *filename, double radio_gain, double audiophile_gain,
                double TitlePeak __attribute__((unused)),
                double *dc_offset, double *album_dc_offset, SETTINGS *settings)
{
	wavegain_opt *wg_opts = calloc(1, sizeof(*wg_opts));
	FILE         *infile = NULL;
	audio_file   *aufile = NULL;
	double       *pcm[2] = {NULL, NULL};
	int          temp_created = 0, keep_temp = 0;
	WG_METER     *output_meter = NULL;
	const char   *destination = settings->current_output ? settings->current_output : filename;
	int          readcount,
	             result = 0,
	             output_failed = 0,
	             i;
	double       Gain;
	double       scale;
	Uint64_t     processed = 0;
	double       last_update = -1.;
	double       wrap_prev_pos = 0;
	double       wrap_prev_neg = 0;
	void         *sample_buffer = NULL;
	input_format *format;

	char         template[] = ".tmp_XXXXXX";
	size_t       tempSize = strlen(destination) + sizeof(template);
	char*        tempName = NULL;
#ifndef _WIN32
	struct stat  fst;	/* owner and permissions are copied on POSIX only */
#endif

	if (!wg_opts) goto exit;

	wg_opts->force = settings->force;
	wg_opts->undo = settings->undo;

	infile = fopen(filename, "rb");

	if (infile == NULL) {
		fprintf (stderr, " Not able to open input file %s.\n", filename) ;
		goto exit;
	}
	wg_opts->apply_gain = 1;
	wg_opts->write_chunk = settings->write_chunk;
	wg_opts->rf64 = settings->rf64;

	/*
	 * Now, we need to select an input audio format
	 */

	format = open_audio_file(infile, wg_opts);
	if (!format) {
		fprintf(stderr, " Unrecognized file format for %s.\n", filename);
		goto exit;
	}
	if (wg_opts->channels < 1 || wg_opts->channels > 2) goto exit;
	if (wg_opts->undo && !wg_opts->gain_chunk) {
		fprintf(stderr, " Skipping file: %s - 'gain' chunk not found.\n", filename);
		if (write_to_log) write_log(" Skipping file: %s - 'gain' chunk not found.\n", filename);
		result = 1;
		goto exit;
	}
	if (wg_opts->gain_chunk && wg_opts->gain_scale == 1.0 && !wg_opts->force) {
		fprintf(stderr, " Skipping file: %s - Gain already undone.\n", filename);
		if (write_to_log) write_log(" Skipping file: %s - Gain already undone.\n", filename);
		result = 1;
		goto exit;
	}
	{
		for (i = 0; i < wg_opts->channels; i++) {
			pcm[i] = malloc(BUFFER_LEN * sizeof(double));
			if (!pcm[i]) goto exit;
		}

		switch(settings->format) {
			case WAV_NO_FMT:
				if (wg_opts->format == WAV_FMT_AIFF || wg_opts->format == WAV_FMT_AIFC8
								   || wg_opts->format == WAV_FMT_AIFC16) {
					wg_opts->format = WAV_FMT_AIFF;
					wg_opts->samplesize = 16;
					wg_opts->endianness = BIG;
					wrap_prev_pos = 0x7fff;
					wrap_prev_neg = -0x8000;
				}
				else if (wg_opts->format == WAV_FMT_8BIT) {
					wrap_prev_pos = 0x7f;
					wrap_prev_neg = -0x80;
				}
				else if (wg_opts->format == WAV_FMT_16BIT) {
					wrap_prev_pos = 0x7fff;
					wrap_prev_neg = -0x8000;
				}
				else if (wg_opts->format == WAV_FMT_24BIT) {
					wrap_prev_pos = 0x7fffff;
					wrap_prev_neg = -0x800000;
				}
				else if (wg_opts->format == WAV_FMT_32BIT) {
					wrap_prev_pos = 0x7fffffff;
					wrap_prev_neg = -0x7fffffff;
				}
				else if (wg_opts->format == WAV_FMT_FLOAT) {
					wrap_prev_pos = 1 - (1 / 0x80000000);
					wrap_prev_neg = -1.;
				}
				break;
			case WAV_FMT_8BIT:
				wg_opts->format = WAV_FMT_8BIT;
				wg_opts->samplesize = 8;
				wg_opts->endianness = LITTLE;
				wrap_prev_pos = 0x7f;
				wrap_prev_neg = -0x80;
				break;
			case WAV_FMT_AIFF:
				wg_opts->format = WAV_FMT_AIFF;
				wg_opts->samplesize = 16;
				wg_opts->endianness = BIG;
				wrap_prev_pos = 0x7fff;
				wrap_prev_neg = -0x8000;
				break;
			case WAV_FMT_16BIT:
				wg_opts->format = WAV_FMT_16BIT;
				wg_opts->samplesize = 16;
				wg_opts->endianness = LITTLE;
				wrap_prev_pos = 0x7fff;
				wrap_prev_neg = -0x8000;
				break;
			case WAV_FMT_24BIT:
				wg_opts->format = WAV_FMT_24BIT;
				wg_opts->samplesize = 24;
				wg_opts->endianness = LITTLE;
				wrap_prev_pos = 0x7fffff;
				wrap_prev_neg = -0x800000;
				break;
			case WAV_FMT_32BIT:
				wg_opts->format = WAV_FMT_32BIT;
				wg_opts->samplesize = 32;
				wg_opts->endianness = LITTLE;
				wrap_prev_pos = 0x7fffffff;
				wrap_prev_neg = -0x7fffffff;
				break;
			case WAV_FMT_FLOAT:
				wg_opts->format = WAV_FMT_FLOAT;
				wg_opts->samplesize = 32;
				wg_opts->endianness = LITTLE;
				wrap_prev_pos = 1 - (1 / 0x80000000);
				wrap_prev_neg = -1.;
				break;
		}

		wg_opts->std_out = settings->std_out;

		/* Create temp file name */
		if ((tempName = malloc(tempSize * sizeof(*tempName))) == NULL) {
			fprintf(stderr, " Error allocating memory for output file name\n");
			goto exit;
		}
		_snprintf(tempName, tempSize, "%s%s", destination, template);


		Init_Dither (wg_opts->samplesize, settings->shapingtype);
		if (settings->dither_profile) {
			if (wg_opts->format == WAV_FMT_FLOAT) {
				fprintf(stderr, " Rate dither requires integer output.\n"); goto exit;
			}
			Init_Rate_Dither(wg_opts->rate, settings->shapingtype);
		}
		if (settings->true_peak) {
			output_meter = wg_meter_create((unsigned long)wg_opts->rate, wg_opts->channels, 0, 1);
			if (!output_meter) goto exit;
		}

		if (wg_opts->undo) {
			scale = 1.0 / wg_opts->gain_scale;
		        Gain = 20. * log10(scale);
			wg_opts->gain_scale = 1.0;
		}
		else {
			if (settings->audiophile)
				Gain = audiophile_gain;
			else
				Gain = radio_gain;

			scale = pow(10., Gain * 0.05);
			wg_opts->gain_scale = scale;
		}

		if (!valid_gain_scale(scale)) {
			fprintf(stderr, " Error: invalid or non-invertible gain scale.\n");
			goto exit;
		}
		aufile = open_output_audio_file(tempName, wg_opts);

		if (aufile == NULL) {
			fprintf (stderr, " Not able to open output file %s.\n", tempName);
			goto exit;
		}
		temp_created = !settings->std_out;

		fprintf(stderr, "                                             \r");
		fprintf(stderr, " Applying Gain of %+5.2lf dB to file: %s\n", Gain, filename);
		if (write_to_log) {
			write_log(" Applying Gain of %+5.2lf dB to file: %s\n", Gain, filename);
		}

        {
            size_t element_size;
            switch (wg_opts->format) {
            case WAV_FMT_8BIT: element_size = sizeof(char); break;
            case WAV_FMT_16BIT:
            case WAV_FMT_AIFF: element_size = sizeof(short); break;
            case WAV_FMT_FLOAT: element_size = sizeof(float); break;
            default: element_size = sizeof(int32_t); break;
            }
            sample_buffer = malloc(element_size * wg_opts->channels * BUFFER_LEN);
            if (!sample_buffer) {
                fprintf(stderr, " Error allocating PCM output buffer.\n");
                output_failed = 1;
                goto conversion_done;
            }
        }

		while (1) {

			readcount = wg_opts->read_samples(wg_opts->readdata, pcm, BUFFER_LEN, 0, 0);

            if (readcount > 0) {
                double elapsed;
                processed += readcount;
                settings->progress_done += (double)readcount / wg_opts->rate;
                elapsed = (double)processed / wg_opts->rate;
                if (elapsed - last_update >= 1.) {
                    double percent = wg_opts->total_samples_per_channel ?
                        fmin(100., 100. * (double)processed / wg_opts->total_samples_per_channel) : 0.;
                    double all = settings->progress_total > 0. ?
                        fmin(100., 100. * settings->progress_done / settings->progress_total) : percent;
                    fprintf(stderr, "This file %3.0f%% done\tAll files %3.0f%% done\r", percent, all);
                    last_update = elapsed;
                }
            }

			if (readcount == 0) {
				output_failed = ferror(infile) != 0;
				break;
			} 
            else if (readcount < 0) {
                output_failed = 1;
                break;
            }
			else {
				int   convsize = BUFFER_LEN;
				int   j,
				      i = 0,
				      k;
				int   bout = (readcount < convsize ? readcount : convsize);

				/* scale doubles to 8, 16, 24 or 32 bit signed ints 
				 * (host order) (unless float output)
				 * and apply ReplayGain scaling, etc. 
				 */
				if (!settings->dithering && !settings->dither_profile &&
				    !settings->limiter && wg_opts->format != WAV_FMT_FLOAT) {
					const double *dc = settings->no_offset ? NULL :
						settings->adc ? album_dc_offset : dc_offset;
					if (!apply_plain_gain(pcm, wg_opts->channels, bout, dc, scale,
					                      wg_opts->samplesize)) {
						output_failed = 1;
						goto conversion_done;
					}
				} else for(k = 0; k < wg_opts->channels; k++) {
					for(j = 0; j < bout; j++, i++) {
						Int64_t val;
						double Sum;
						if (!isfinite(pcm[k][j])) {
							output_failed = 1;
							goto conversion_done;
						}

						if (!settings->no_offset) {
							if (settings->adc)
								pcm[k][j] -= album_dc_offset[k];
							else
								pcm[k][j] -= dc_offset[k];
						}

						pcm[k][j] *= scale;
						if (settings->limiter) {	/* smooth -6dB knee */
							if (pcm[k][j] < -0.5)
								pcm[k][j] = tanh((pcm[k][j] + 0.5) / (1-0.5)) * (1-0.5) - 0.5;
							else if (pcm[k][j] > 0.5)
								pcm[k][j] = tanh((pcm[k][j] - 0.5) / (1-0.5)) * (1-0.5) + 0.5;
						}
						if (wg_opts->format != WAV_FMT_FLOAT) {
							Sum = pcm[k][j]*2147483647.f;
							if (i > 31)
								i = 0;
							val = settings->dither_profile ? Rate_Dither(Sum, k, wg_opts->samplesize) :
								dither_output(settings->dithering, settings->shapingtype, i,
								              Sum, k, wg_opts->format);
							if (val > (Int64_t)wrap_prev_pos)
								val = (Int64_t)wrap_prev_pos;
							else if (val < (Int64_t)wrap_prev_neg)
								val = (Int64_t)wrap_prev_neg;
							pcm[k][j] = (double)val;
						}
						else {
							if (pcm[k][j] > wrap_prev_pos)
								pcm[k][j] = wrap_prev_pos;
							else if (pcm[k][j] < wrap_prev_neg)
								pcm[k][j] = wrap_prev_neg;
						}
					}
				}
				sample_buffer = output_to_PCM(pcm, sample_buffer, wg_opts->channels,
						bout, wg_opts->format);
				if (output_meter) {
					for (k = 0; k < wg_opts->channels; k++) for (j = 0; j < bout; j++)
						pcm[k][j] = wg_opts->format == WAV_FMT_FLOAT ? (float)pcm[k][j] : pcm[k][j] / ldexp(1., wg_opts->samplesize - 1);
					if (!wg_meter_add(output_meter, pcm, (size_t)bout)) { output_failed = 1; break; }
				}
				/* write to file */
                if (!write_audio_file(aufile, sample_buffer, bout * wg_opts->channels)) {
                    output_failed = 1;
                    break;
                }
			}
		}
conversion_done:
		if (output_meter && !output_failed) {
			double peak;
			if (!wg_meter_finish(output_meter, NULL, &peak) || peak > pow(10., settings->peak_ceiling / 20.) * (1. + 1e-6)) {
				fprintf(stderr, " Output true peak exceeds the requested ceiling.\n"); output_failed = 1;
			} else fprintf(stderr, " Output true peak: %.3f dBTP\n", peak > 0. ? 20. * log10(peak) : -HUGE_VAL);
		}
		free(sample_buffer);
		sample_buffer = NULL;
		wav_close(wg_opts->readdata);
		wg_opts->readdata = NULL;
		{
			int finalized = close_audio_file(infile, aufile, wg_opts);
			wg_opts = NULL;
			aufile = NULL;
			if (!finalized || output_failed) {
				fprintf(stderr, " Error finalizing output; original file preserved.\n");
				goto exit;
			}
		}
		if (fclose(infile)) { infile = NULL; goto exit; }
		infile = NULL;
        fprintf(stderr, "This file 100%% done\tAll files %3.0f%% done\r",
                settings->progress_total > 0. ? fmin(100., 100. * settings->progress_done / settings->progress_total) : 100.);

        if (!settings->std_out) {
			if (settings->current_output) {
#ifndef _WIN32
				/* Retain the input's access permissions, not mkstemp's private mode.
				 * New files keep the creating user/group and no special mode bits. */
				if (stat(filename, &fst) || chmod(tempName, fst.st_mode & 0777)) {
					keep_temp = 1;
					fprintf(stderr, " Cannot set output permissions; completed file kept at %s.\n", tempName);
					goto exit;
				}
#endif
				if (!wg_output_publish(tempName, destination)) {
					keep_temp = 1;
					fprintf(stderr, " Cannot publish output %s; completed file kept at %s.\n", destination, tempName);
					goto exit;
				}
				temp_created = 0;
			} else {
#ifndef _WIN32
			/* copy owner, group, permissions from original to output */
			/* TODO: add -p|--preserve=[=ATTR_LIST] (see `man cp`) and only copy
			 * attributes if enabled. Silently ignore errors on EPERM
			 */
			if (stat(filename, &fst)) goto exit;
			if (
				/* FIXME: Could/Should both calls be perfomed in a single step? */
				chown(tempName, fst.st_uid, -1) != 0 ||
				chown(tempName, -1 ,fst.st_gid) != 0
			)
				fprintf(stderr, "Warning: could not change file owner: %s (%s)\n", tempName, filename);
			chmod(tempName, fst.st_mode);
#endif
#ifdef _WIN32
			if (!MoveFileExA(tempName, filename, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
			if (rename(tempName, filename) != 0) {
#endif
				keep_temp = 1;
				fprintf(stderr, " Error replacing '%s'; completed output kept at '%s'.\n", filename, tempName);
				goto exit;
			}
			temp_created = 0;
			}
		}
		result = 1;
	}
exit:
	wg_meter_free(output_meter);
	free(sample_buffer);
	for (i = 0; i < 2; i++) free(pcm[i]);
	if (wg_opts) {
		if (wg_opts->readdata) wav_close(wg_opts->readdata);
		free(wg_opts->header);
		free(wg_opts);
	}
	if (infile) fclose(infile);
	if (temp_created && !keep_temp) remove(tempName);
	free(tempName);
	return result;
}


