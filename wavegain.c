/* License provenance: see doc/license-notices.txt; original notices retained. */
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_RECURSIVE
#include "recurse.h"
#endif

/*Gcc uses LL as a suffix for long long int (64 bit) types - Marc Brooker 8/4/2004*/
#ifdef __GNUC__
#define ROUND64(x)   ( doubletmp = (x) + Dither.Add + (Int64_t)0x001FFFFD80000000LL, *(Int64_t*)(&doubletmp) - (Int64_t)0x433FFFFD80000000LL )
#else
#define ROUND64(x)   ( doubletmp = (x) + Dither.Add + (Int64_t)0x001FFFFD80000000L, *(Int64_t*)(&doubletmp) - (Int64_t)0x433FFFFD80000000L )
#endif

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

void report_gain(FILE_LIST *file, const double *dc, SETTINGS *settings)
{
    double peak = corrected_peak(file, dc, settings);
    double scale;
    int left = 0, right = 0;
    file->track_gain = selected_gain(file->raw_gain, peak, settings);
    scale = pow(10., file->track_gain * .05);
    file->track_peak = peak * scale; /* Predicted peak BEFORE the limiter. */
    if (!settings->no_offset && !settings->fast) {
        left = (int)(dc[0] * -32768.);
        right = file->channels == 2 ? (int)(dc[1] * -32768.) : 0;
    }
    fprintf(stderr, " %+6.2lf dB | %6.0lf | %5.2lf | %8.0lf | %4d  |  %4d  | %s\n",
            file->track_gain, peak, scale, file->track_peak, left, right, file->filename);
    if (write_to_log)
        write_log(" %+6.2lf dB | %6.0lf | %5.2lf | %8.0lf | %4d  |  %4d  | %s\n",
                  file->track_gain, peak, scale, file->track_peak, left, right, file->filename);
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
        fprintf(stderr, " Skipping File %s, it has already been processed.\n", filename); goto done;
    }
    if ((settings->first_file || !settings->audiophile ? InitGainAnalysis(opt.rate) :
         ResetSampleFrequency(opt.rate)) != INIT_GAIN_ANALYSIS_OK) {
        fprintf(stderr, " Error Initializing Gain Analysis (non-standard samplerate?)\n"); goto done;
    }
    if (settings->first_file) {
        total_samples = 0.;
        fprintf(stderr, "\n Analyzing...\n\n    Gain   |  Peak  | Scale | New Peak |Left DC|Right DC| Track\n"
                        "           |        |       |          |Offset | Offset |\n"
                        " --------------------------------------------------------------\n");
        if (write_to_log) write_log("\n Analyzing...\n    Gain   | Peak | Scale | New Peak |Left DC|Right DC| Track\n");
    }
    sampled = settings->fast && !opt.std_in &&
              opt.total_samples_per_channel * (opt.samplesize / 8) * opt.channels > 8192000;
    step = (Int64_t)(opt.total_samples_per_channel * (opt.samplesize / 8) * opt.channels / 1200);
    info->channels = opt.channels;
    for (ch = 0; ch < opt.channels; ch++) {
        buffer[ch] = malloc(BUFFER_LEN * sizeof(double));
        if (!buffer[ch]) goto done;
        info->minimum[ch] = HUGE_VAL; info->maximum[ch] = -HUGE_VAL;
        offset[ch] = dc_offset[ch] = 0.;
    }
    while (!sampled || iteration < 200) {
        Int64_t position = sampled ? step * (100 + iteration * 5) : 0;
        if (sampled && !opt.is_wave64) position += ((wavfile *)opt.readdata)->data_start;
        count = opt.read_samples(opt.readdata, buffer, BUFFER_LEN, sampled, position);
        if (count < 0) { fprintf(stderr, " Error reading %s.\n", filename); goto done; }
        if (!count) break;
        frames += count;
        for (ch = 0; ch < opt.channels; ch++) {
            for (j = 0; j < count; j++) {
                double value = buffer[ch][j];
                if (!isfinite(value)) { fprintf(stderr, " Non-finite PCM in %s.\n", filename); goto done; }
                info->minimum[ch] = fmin(info->minimum[ch], value);
                info->maximum[ch] = fmax(info->maximum[ch], value);
                if (!settings->fast) offset[ch] += value;
                buffer[ch][j] = value * 32767.;
            }
        }
        if (AnalyzeSamples(buffer[0], buffer[1], count, opt.channels) != GAIN_ANALYSIS_OK) goto done;
        iteration++;
    }
    if (!frames) { fprintf(stderr, " No audio samples in %s.\n", filename); goto done; }
    info->raw_gain = GetTitleGain();
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
	wavegain_opt *wg_opts = malloc(sizeof(wavegain_opt));
	FILE         *infile;
	audio_file   *aufile;
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
	int          tempSize = strlen(filename) + strlen(template) + 1;
	char*        tempName = NULL;
	struct stat  fst;

	memset(wg_opts, 0, sizeof(wavegain_opt));

	wg_opts->force = settings->force;
	wg_opts->undo = settings->undo;

	infile = fopen(filename, "rb");

	if (infile == NULL) {
		fprintf (stderr, " Not able to open input file %s.\n", filename) ;
		goto exit;
	}
	wg_opts->apply_gain = 1;
	wg_opts->write_chunk = settings->write_chunk;

	/*
	 * Now, we need to select an input audio format
	 */

	format = open_audio_file(infile, wg_opts);
	if (!format) {
		if (wg_opts->readdata) wav_close(wg_opts->readdata);
		if (wg_opts)
			free(wg_opts);
		fclose(infile);
		/* error reported by reader */
		fprintf (stderr, " Unrecognized file format for %s.\n", filename) ;
	}
	else if (wg_opts->undo && !wg_opts->gain_chunk) {
		format->close_func(wg_opts->readdata);
		if (wg_opts)
			free(wg_opts);
		fclose(infile);
		fprintf(stderr, " Skipping file: %s - 'gain' chunk not found.\n", filename);
		if (write_to_log) {
			write_log(" Skipping file: %s - 'gain' chunk not found.\n", filename);
		}
		result = 1;
	}
	else if (wg_opts->gain_scale == 1.0 && !wg_opts->force) {
		format->close_func(wg_opts->readdata);
		if (wg_opts)
			free(wg_opts);
		fclose(infile);
		fprintf(stderr, " Skipping file: %s - Gain already undone.\n", filename);
		if (write_to_log) {
			write_log(" Skipping file: %s - Gain already undone.\n", filename);
		}
		result = 1;
	}
	else {
		double **pcm = malloc(sizeof(double *) * wg_opts->channels);

		for (i = 0; i < wg_opts->channels; i++)
			pcm[i] = malloc(BUFFER_LEN * sizeof(double));

		switch(settings->format) {
			case WAV_NO_FMT:
				if (wg_opts->format == WAV_FMT_AIFF || wg_opts->format == WAV_FMT_AIFC8
								   || wg_opts->format == WAV_FMT_AIFC16) {
					wg_opts->format = WAV_FMT_AIFF;
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
		_snprintf(tempName, tempSize, "%s%s", filename, template);


		Init_Dither (wg_opts->samplesize, settings->shapingtype);

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

		aufile = open_output_audio_file(tempName, wg_opts);

		if (aufile == NULL) {
			fprintf (stderr, " Not able to open output file %s.\n", tempName);
			fclose(infile);
			if (!settings->std_out) remove(tempName);
			goto exit;
		}

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
				for(k = 0; k < wg_opts->channels; k++) {
					for(j = 0; j < bout; j++, i++) {
						Int64_t val;
						double Sum;

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
							val = dither_output(settings->dithering, settings->shapingtype, i,
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
				/* write to file */
                if (!write_audio_file(aufile, sample_buffer, bout * wg_opts->channels)) {
                    output_failed = 1;
                    break;
                }
			}
		}
conversion_done:
		free(sample_buffer);
		for (i = 0; i < wg_opts->channels; i++)
			if (pcm[i]) free(pcm[i]);
		if (pcm) free(pcm);
		format->close_func(wg_opts->readdata);
		if (!close_audio_file(infile, aufile, wg_opts) || output_failed) {
			fclose(infile);
			if (!settings->std_out) remove(tempName);
			fprintf(stderr, " Error finalizing output; original file preserved.\n");
			goto exit;
		}
        fclose(infile);
        fprintf(stderr, "This file 100%% done\tAll files %3.0f%% done\r",
                settings->progress_total > 0. ? fmin(100., 100. * settings->progress_done / settings->progress_total) : 100.);

        if (!settings->std_out) {
#ifdef _WIN32
			/* WIN32's rename(temp, original) does not allow original to exist,
			 * so we must remove() it first. Ideally, there should be a way
			 * to remove+rename in a single, atomic function, so if either one
			 * fails, the original file is preserved.
			 */
			if (remove(filename) != 0) {
				fprintf(stderr, " Error deleting old file '%s'\n", filename);
				goto exit;
			}
#endif
#ifndef _WIN32
			/* copy owner, group, permissions from original to output */
			/* TODO: add -p|--preserve=[=ATTR_LIST] (see `man cp`) and only copy
			 * attributes if enabled. Silently ignore errors on EPERM
			 */
			stat(filename, &fst);
			if (
				/* FIXME: Could/Should both calls be perfomed in a single step? */
				chown(tempName, fst.st_uid, -1) != 0 ||
				chown(tempName, -1 ,fst.st_gid) != 0
			)
				fprintf(stderr, "Warning: could not change file owner: %s (%s)\n", tempName, filename);
			chmod(tempName, fst.st_mode);
#endif
			if (rename(tempName, filename) != 0) {
				fprintf(stderr, " Error renaming '%s' to '%s' (uh-oh)\n", tempName, filename);
				goto exit;
			}
		}
		result = 1;
	}
exit:
	if(tempName) free(tempName);
	return result;
}


