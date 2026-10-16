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
#include "config.h"
#endif

#include <ctype.h>
#include "getopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "config.h"
#include "gain_analysis.h"
#include "i18n.h"
#include "audio.h"
#include "misc.h"
#include "wavegain.h"
#include "main.h"
#include "dither.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_RECURSIVE
#include "recurse.h"
#endif

/*This is an ugly way of doing this - but it works for the moment*/
#ifndef MAX_PATH
#define MAX_PATH 512
#endif

int	      write_to_log = 0;
char          log_file_name[MAX_PATH];
extern double total_samples;

/**
 * \brief Allocate a FILE_LIST node.
 *
 * Allocate a FILE_LIST node. filename is set to file while track_peak and
 * track_gain are set to NO_PEAK and NO_GAIN respectively. The rest of the
 * fields are set to zero.
 *
 * \param file  name of file to get a node for.
 * \return  the allocated node or NULL (in which case a message has been
 *          printed).
 */
static FILE_LIST* alloc_node(const char* file)
{
	FILE_LIST* node;

	node = calloc(1, sizeof(*node));

	if (node != NULL) {
		node->filename = strdup(file);

		if (node->filename != NULL) {
			node->track_peak = NO_PEAK;
			node->track_gain = NO_GAIN;
			node->dc_offset[0] = 0.;
			node->dc_offset[1] = 0.;
			node->offset[0] = 0.;
			node->offset[1] = 0.;
			return node;
		}
		free(node);
	}

	fprintf(stderr, _("Out of memory\n"));
	return NULL;
}


/**
 * \brief Allocate a new FILE_LIST node and add it to the end of a list.
 *
 * Allocate a new FILE_LIST node and add it to the end of a list.
 *
 * \param list  pointer to the list (first node) pointer. If the list
 *              pointer contains NULL, it is set to the new node. Otherwise 
 *              the new node is added the last node in the list.
 * \param file  name of the file to get a node for.
 * \return  0 if successful and -1 if the node couldn't be allocated (in
 *          which case a message has been printed).
 */
int add_to_list(FILE_LIST** list, const char* file)
{
	if (*list == NULL) {
		*list = alloc_node(file);

		if (*list == NULL)
			return -1;
	}
	else {
		FILE_LIST* node = *list;

		while (node->next_file != NULL)
			node = node->next_file;

		node->next_file = alloc_node(file);

		if (node->next_file == NULL)
			return -1;
	}

	return 0;
}


/**
 * Free all nodes in a FILE_LIST list.
 *
 * \param list  pointer to first node in list. Can be NULL, in which case 
 *              nothing is done.
 */
void free_list(FILE_LIST* list)
{
	FILE_LIST* next;

	while (list != NULL) {
		next = list->next_file;
		free((void *) list->filename);
		free(list);
		list = next;
	}
}

static char s_floatToAscii[256];
const char* ftos(double f, const char* format)
{
	sprintf(s_floatToAscii, format, f);
	return s_floatToAscii;
}

/**
 * \brief Processs the file in file_list.
 *
 * Process the files in file_list. If settings->album is set, the files are
 * considered to belong to one album.
 *
 * \param file_list  list of files to process.
 * \param settings   settings and global variables.
 * \return  0 if successful and -1 if an error occured (in which case a
 *          message has been printed).
 */
int process_files(FILE_LIST* file_list, SETTINGS* settings, const char* dir __attribute__((unused)))
{
	FILE_LIST* file;
	int failed = 0;
	double     audiophile_gain = 0.,
	           scale = 1.,
	           album_dc_offset[2] = {0., 0.};

	settings->album_peak = NO_PEAK;

	if (file_list == NULL)
		return 0;

	settings->first_file = 1;
	total_samples = 0.;

	/* Undo previously applied gain */
    settings->progress_total = settings->progress_done = 0.;
    if (settings->undo) {
        for (file = file_list; file; file = file->next_file)
            if (file->filename) settings->progress_total += audio_duration(file->filename, settings);
        for (file = file_list; file; file = file->next_file) {
			if (file->filename == NULL)
				continue;
			if (!write_gains(file->filename, 0, 0, 0, 0, 0, settings)) {
				fprintf(stderr, " Error processing GAIN for file - %s\n", file->filename);
				failed = 1;
				continue;
			}
		}
	}
	else {
		/* Analyze the files */
		for (file = file_list; file; file = file->next_file) {
			if (file->filename == NULL)
				continue;

			{
				int status = get_gain(file->filename, &file->track_peak, &file->track_gain,
				                      file->dc_offset, file->offset, settings, file);
				if (status != 1) {
					if (!status) failed = 1; /* A deliberate skip returns 2. */
					free((void *)file->filename);
					file->filename = NULL;
					continue;
				}
			}

			album_dc_offset[0] += file->offset[0];
			album_dc_offset[1] += file->offset[1];
		}

        if (settings->first_file) return failed ? -1 : 0;
        if (total_samples > 0.) {
            album_dc_offset[0] /= total_samples;
            album_dc_offset[1] /= total_samples;
        }
        settings->album_peak = 0.;
        for (file = file_list; file; file = file->next_file) {
            const double *dc;
            if (!file->filename) continue;
            dc = settings->adc ? album_dc_offset : file->dc_offset;
            report_gain(file, dc, settings);
            settings->album_peak = fmax(settings->album_peak, corrected_peak(file, dc, settings));
        }

		if (!settings->no_offset && settings->adc) {
			int dc_l = (int)(album_dc_offset[0] * 32768 * -1);
			int dc_r = (int)(album_dc_offset[1] * 32768 * -1);
			fprintf(stderr, " ********************* Album DC Offset | %4d  |  %4d  | ***************\n",
				dc_l, dc_r);
			if(write_to_log) {
				write_log(" ********************* Album DC Offset | %4d  |  %4d  | ***************\n",
					dc_l, dc_r);
			}
		}

		fprintf(stderr, "\n");
		if(write_to_log)
			write_log("\n");

        if (settings->audiophile) {
            audiophile_gain = selected_gain(GetAlbumGain(), settings->album_peak, settings);
            scale = pow(10., audiophile_gain * .05);
            if (settings->scale) fprintf(stdout, "%8.6lf", scale);
            fprintf(stderr, " Recommended Album Gain: %+6.2f dB\tScale: %6.4f\n\n", audiophile_gain, scale);
            if (write_to_log) write_log(" Recommended Album Gain: %+6.2f dB\tScale: %6.4f\n\n", audiophile_gain, scale);
        }

        if (settings->apply_gain) {
            settings->progress_total = settings->progress_done = 0.;
            for (file = file_list; file; file = file->next_file) {
                const double *dc;
                double gain;
                int correction;
                if (!file->filename) continue;
                dc = settings->adc ? album_dc_offset : file->dc_offset;
                gain = settings->audiophile ? audiophile_gain : file->track_gain;
                correction = !settings->no_offset &&
                    (fabs(dc[0]) > 1. / 32768. || fabs(dc[1]) > 1. / 32768.);
                file->apply = settings->need_to_process || settings->force || settings->limiter ||
                              fabs(gain) >= .1 || correction;
                if (file->apply) settings->progress_total += file->duration;
            }
            for (file = file_list; file; file = file->next_file) {
                if (!file->filename) continue;
                if (!file->apply) {
                    fprintf(stderr, " No Gain adjustment or DC correction required for file: %s, skipping.\n", file->filename);
                    continue;
                }
                if (!write_gains(file->filename, file->track_gain, audiophile_gain, file->track_peak,
                                 file->dc_offset, album_dc_offset, settings)) {
                    fprintf(stderr, " Error processing GAIN for file - %s\n", file->filename);
                    failed = 1;
                }
            }
        }
	}

	fprintf(stderr, "\n WaveGain Processing completed %s\n", failed ? "with errors" : "normally");
	if (write_to_log)
		write_log("\n WaveGain Processing completed %s\n\n", failed ? "with errors" : "normally");

#ifdef _WIN32
	if (settings->cmd) { 	/* execute user command */
		FILE_LIST* file;
		char buff[MAX_PATH];
		char *b, *p, *q;
		double track_scale = 1.0;

		SetEnvironmentVariable("ALBUM_GAIN", ftos(audiophile_gain, "%.2lf"));
		SetEnvironmentVariable("ALBUM_SCALE", ftos(scale, "%.5lf"));
		SetEnvironmentVariable("ALBUM_NEW_PEAK", ftos(settings->album_peak * scale, "%.0lf"));
		SetEnvironmentVariable("ALBUM_PEAK", ftos(settings->album_peak, "%.0lf"));

		for (file = file_list; file; file = file->next_file) {
			if (file->filename == NULL)
				continue;

			if (dir[0] == '.' && dir[1] == '\\') dir += 2;

			strcpy(buff, file->filename);
			b = (buff[0] == '.' && buff[1] == '\\') ? buff+2 : buff;
			SetEnvironmentVariable("INPUT_FILE", b);

			p = strrchr(b, '\\');
			if (p) {
				p[0] = '\0'; ++p;
				SetEnvironmentVariable("INPUT_FDIR", b);
				SetEnvironmentVariable("INPUT_RDIR", b); // assume dir = "."
			}
			else {
				p = b;
				SetEnvironmentVariable("INPUT_FDIR", ".");
				SetEnvironmentVariable("INPUT_RDIR", dir);
			}

			q = strrchr(p, '.');
			if (q) q[0] = '\0';
			SetEnvironmentVariable("INPUT_NAME", p);

			track_scale = (pow(10., file->track_gain * 0.05));
			SetEnvironmentVariable("TRACK_SCALE", ftos(track_scale, "%.5lf"));
			SetEnvironmentVariable("TRACK_GAIN", ftos(file->track_gain, "%.2lf"));
			SetEnvironmentVariable("TRACK_NEW_PEAK", ftos(file->track_peak, "%.0lf"));
			SetEnvironmentVariable("TRACK_PEAK", ftos(file->track_peak / track_scale, "%.0lf"));

			SetEnvironmentVariable("DC_OFFSET_L", ftos((int)(file->dc_offset[0]*(-32768)), "%.0lf"));
			SetEnvironmentVariable("DC_OFFSET_R", ftos((int)(file->dc_offset[1]*(-32768)), "%.0lf"));

			fprintf(stderr, "\n Executing command on \"%s\":\n\n", file->filename);
			system(settings->cmd);
		}
	}
#endif

	return failed ? -1 : 0;
}


/**
 * Print out a list of options and the command line syntax.
 */
static void usage(void)
{
    fprintf(stdout, _("WaveGain v" WAVEGAIN_VERSION " Compiled " __DATE__ ".\n"));
#include "help_generated.h"
}


static const struct option long_options[] = {
	{"help",	0, NULL, 'h'},
	{"album",	0, NULL, 'a'},
	{"radio",	0, NULL, 'r'},
	{"adc",		0, NULL, 'q'},
	{"no_offset",	0, NULL, 'p'},
	{"calculate",	0, NULL, 'c'},
	{"scale",	0, NULL, 'x'},
	{"apply",	0, NULL, 'y'},
	{"write",	0, NULL, 'w'},
	{"force",	0, NULL,  0 },
	{"undo-gain",	0, NULL,  0 },
	{"rf64",	0, NULL,  0 },
	{"fast",	0, NULL, 's'},
	{"stdout",	0, NULL, 'o'},
#ifdef ENABLE_RECURSIVE
	{"recursive",   0, NULL, 'z'},
#endif
	{"log",		0, NULL, 'l'},
	{"logfile",	1, NULL, 'f'},
	{"noclip",	0, NULL, 'n'},
	{"dither",	1, NULL, 'd'},
	{"limiter",	0, NULL, 't'},
	{"gain",	1, NULL, 'g'},
	{"bits",	1, NULL, 'b'},
#ifdef _WIN32
	{"exec",	1, NULL, 'e'},
#endif
	{NULL,		0, NULL , 0}
};


#ifdef ENABLE_RECURSIVE
#define ARG_STRING "harqpcxywsozlf:nd:tg:b:e:"
#else
#define ARG_STRING "harqpcxywsolf:nd:tg:b:e:"
#endif


int main(int argc, char** argv)
{
	SETTINGS settings;
	int      option_index = 1,
	         ret,
	         i,
	         bits;

#ifdef _WIN32
 	char     CmdDir[MAX_PATH];
 	char     *p;
#endif

	memset(&settings, 0, sizeof(settings));
	settings.first_file = 1;
	settings.radio = 1;
	settings.clip_prev = 1;
	settings.outbitwidth = 16;
	settings.format = WAV_NO_FMT;

#ifdef _WIN32
	/* Is this good enough? Or do we need to consider multi-byte codepages as 
	* well? 
	*/
	SetConsoleOutputCP(GetACP());

	GetModuleFileName(NULL, CmdDir, MAX_PATH);
	p = strrchr(CmdDir, '\\') + 1;
	p[0] = '\0';
#endif

	while ((ret = getopt_long(argc, argv, ARG_STRING, long_options, &option_index)) != -1) {
		switch(ret) {
			case 0:
				if (!strcmp(long_options[option_index].name, "force")) {
					settings.force = 1;
				}
				else if (!strcmp(long_options[option_index].name, "undo-gain")) {
					settings.undo = 1;
				}
				else if (!strcmp(long_options[option_index].name, "rf64")) {
					settings.rf64 = 1;
				}
				else {
					fprintf(stderr, "Internal error parsing command line options\n");
					exit(1);
				}
				break;
			case 'h':
				usage();
				exit(0);
				break;
			case 'a':
				settings.audiophile = 1;
				break;
			case 'r':
				settings.radio = 1;
				break;
			case 'q':
				settings.adc = 1;
				break;
			case 'p':
				settings.no_offset = 1;
				break;
			case 'c':
				settings.apply_gain = 0;
				break;
			case 'x':
				settings.scale = 1;
				break;
			case 'y':
				settings.apply_gain = 1;
				settings.scale = 0;
				break;
			case 'w':
				settings.write_chunk = 1;
				break;
			case 's':
				settings.fast = 1;
				break;
			case 'o':
				settings.std_out = 1;
				break;
#ifdef ENABLE_RECURSIVE
			case 'z':
				settings.recursive = 1;
				break;
#endif
			case 'l':
				write_to_log = 1;
#ifdef _WIN32
				if (strlen(CmdDir) + strlen(LOG_NAME) >= sizeof(log_file_name)) {
					fprintf(stderr, "Log filename is too long.\n");
					return EXIT_FAILURE;
				}
				strcpy(log_file_name, CmdDir);
				strcat(log_file_name, LOG_NAME);
#else
				strcpy(log_file_name, LOG_NAME);
#endif
				break;
			case 'f':
				write_to_log = 1;
				if (strlen(optarg) >= sizeof(log_file_name)) {
					fprintf(stderr, "Log filename is too long.\n");
					return EXIT_FAILURE;
				}
				strcpy(log_file_name, optarg);
				break;
			case 'n':
				settings.clip_prev = 0;
				break;
			case 'd':
				settings.need_to_process = 1;
				settings.dithering = 1;
   				if(sscanf(optarg, "%d", &settings.shapingtype) != 1) {
	    				fprintf(stderr, "Warning: dither type %s not recognised, using default\n", optarg);
		    			break;
				}
				if (settings.shapingtype == 0)
					settings.dithering = 0;
				else if (settings.shapingtype == 1)
					settings.shapingtype = 0;
				else if (settings.shapingtype == 2)
					settings.shapingtype = 1;
				else if (settings.shapingtype == 3)
					settings.shapingtype = 2;
				else if (settings.shapingtype == 4)
					settings.shapingtype = 3;
				break;
			case 't':
				settings.limiter = 1;
				break;
			case '?':
				return EXIT_FAILURE;
			case 'g':
				if(sscanf(optarg, "%lf", &settings.man_gain) != 1 || !isfinite(settings.man_gain)) {
	    				fprintf(stderr, "Warning: manual gain %s not recognised, ignoring\n", optarg);
					settings.man_gain = 0.;
					break;
				}
				if(settings.man_gain < -20.0) {
	    				fprintf(stderr, "Warning: manual gain %s is out of range, "
					                 "applying gain of -20.0dB\n", optarg);
					settings.man_gain = -20.0;
				}
				else if(settings.man_gain > 12.0) {
	    				fprintf(stderr, "Warning: manual gain %s is out of range, "
					                 "applying gain of +12.0dB\n", optarg);
					settings.man_gain = 12.0;
				}
				break;
			case 'b':
				settings.need_to_process = 1;
   				if(sscanf(optarg, "%d", &bits) != 1) {
	    				fprintf(stderr, "Warning: output format %s not recognised, using default\n", optarg);
		    			break;
				}
				if (bits == 1) {
					settings.outbitwidth = 8;
					settings.format = WAV_FMT_8BIT;
		    			break;
				}
				else if (bits == 2) {
					settings.outbitwidth = 16;
					settings.format = WAV_FMT_16BIT;
		    			break;
				}
				else if (bits == 3) {
					settings.outbitwidth = 24;
					settings.format = WAV_FMT_24BIT;
		    			break;
				}
				else if (bits == 4) {
					settings.outbitwidth = 32;
					settings.format = WAV_FMT_32BIT;
		    			break;
				}
				else if (bits == 5) {
					settings.outbitwidth = 32;
					settings.format = WAV_FMT_FLOAT;
		    			break;
				}
				else if (bits == 6) {
					settings.outbitwidth = 16;
					settings.format = WAV_FMT_AIFF;
		    			break;
				}
				else {
	    				fprintf(stderr, "Warning: output format %s not recognised, using default\n", optarg);
					break;
				}
				break;
#ifdef _WIN32
			case 'e': {
				char *p;
				int k;
				char * lpPart[MAX_PATH]={NULL};

				settings.cmd = (char *) malloc(1024*8); /* 8Kb is XP's limit */
				if (settings.cmd == NULL) {
					fprintf(stderr, "Failed to allocate memory for cmd...\n");
					return 1;
				}

				p = settings.cmd;

				k = GetFullPathName(argv[optind - 1], 1024*8, p, lpPart);
				k = GetShortPathName(p, p, MAX_PATH);
				if (k == 0) {
					p += sprintf (p, "%s", argv[optind - 1]);
				} else {
					p += k;
				}

				for (k = optind; k < argc; ++k) {
					p += sprintf(p, " \"%s\"", argv[k]);
				}

				argc = optind;
				break;
			}
#endif
		}
	}
	if (settings.fast) {
		settings.apply_gain = 0;
		settings.no_offset = 1;
	}
	if (settings.undo == 1) {
		settings.write_chunk = 0;
		settings.no_offset = 1;
	}

	if (optind >= argc) {
		fprintf(stderr, _("No files specified.\n"));
		usage();
		return EXIT_SUCCESS;
	}

	if (write_to_log) {
		write_log("Command line:\n\t");
		for(i = 0; i < argc; i++)
			write_log("%s ",  argv[i]);
		write_log("\n");
	}

	if (!strcmp(argv[optind], "-")) {
		FILE_LIST info = {0};
		int status;
		info.filename = "-";
		status = get_gain("-", &info.track_peak, &info.track_gain,
		                  info.dc_offset, info.offset, &settings, &info);
		if (status != 1) return status == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
		report_gain(&info, info.dc_offset, &settings);
		return EXIT_SUCCESS;
	}
	else {
		for (i = optind; i < argc; ++i) {
#ifdef ENABLE_RECURSIVE
			if (process_argument(argv[i], &settings) < 0) {
				free_list(settings.file_list);
				return EXIT_FAILURE;
			}
#else
			if (add_to_list(&settings.file_list, argv[i]) < 0)
				return EXIT_FAILURE;
#endif
		}

		/* Process files (perhaps remaining) in list */
		ret = process_files(settings.file_list, &settings, ".");
		free_list(settings.file_list);
		settings.file_list = NULL;
		if (settings.cmd) free(settings.cmd);
		settings.cmd = NULL;
	}

	return (ret < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
