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
#include <errno.h>
#include <sys/stat.h>
#include "output.h"
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
		free(list->output);
		wg_meter_free(list->meter);
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
static int prepare_outputs(FILE_LIST *list, SETTINGS *settings)
{
	FILE_LIST *file, *other;
	if (!settings->output_file && !settings->output_dir) return 1;
	if (settings->output_file && list && list->next_file) {
		fprintf(stderr, " --output requires exactly one input file.\n"); return 0;
	}
	for (file = list; file; file = file->next_file) {
		struct stat st;
		if (settings->output_file) file->output = strdup(settings->output_file);
		else {
			char *folder = settings->output_subdir && *settings->output_subdir ?
				wg_output_join(settings->output_dir, settings->output_subdir) : strdup(settings->output_dir);
			if (!folder || !wg_output_mkdirs(folder)) { free(folder); return 0; }
			file->output = wg_output_join(folder, last_path(file->filename));
			free(folder);
		}
		if (!file->output) return 0;
		if (!stat(file->output, &st)) {
			fprintf(stderr, " Refusing to overwrite output %s.\n", file->output); return 0;
		}
		if (errno != ENOENT) { file_error(" Cannot inspect output %s: ", file->output); return 0; }
		for (other = list; other != file; other = other->next_file) {
#ifdef _WIN32
			if (!_stricmp(other->output, file->output))
#else
			if (!strcmp(other->output, file->output))
#endif
			{ fprintf(stderr, " Duplicate output %s.\n", file->output); return 0; }
		}
	}
	return 1;
}

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

	if (!prepare_outputs(file_list, settings)) return -1;
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
			settings->current_output = file->output;
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
            if ((settings->lufs || settings->true_peak) && !measure_file(file, dc, settings)) {
                /* Do not publish an album whose measurements are incomplete. */
                return -1;
            }
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
            double raw_album = GetAlbumGain();
            if (settings->lufs) {
                size_t count = 0, i = 0;
                double loudness;
                WG_METER **meters;
                for (file = file_list; file; file = file->next_file) if (file->filename) count++;
                if (!count || count > (size_t)-1 / sizeof(*meters)) return -1;
                meters = malloc(count * sizeof(*meters));
                if (!meters) return -1;
                for (file = file_list; file; file = file->next_file) if (file->filename) meters[i++] = file->meter;
                if (!wg_meter_album(meters, count, &loudness)) { free(meters); return -1; }
                free(meters);
                raw_album = isfinite(loudness) ? settings->target_lufs - loudness : 0.;
                fprintf(stderr, " Album integrated loudness: %.2f LUFS\n", loudness);
            }
            audiophile_gain = selected_gain(raw_album, settings->album_peak, settings);
            if (settings->true_peak) for (file = file_list; file; file = file->next_file)
                if (file->filename) audiophile_gain = fmin(audiophile_gain, true_peak_gain(file, settings));
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
                file->apply = file->output || settings->true_peak || settings->lufs || settings->need_to_process || settings->force || settings->limiter ||
                              fabs(gain) >= .1 || correction;
                if (file->apply) settings->progress_total += file->duration;
            }
            for (file = file_list; file; file = file->next_file) {
                if (!file->filename) continue;
                if (!file->apply) {
                    fprintf(stderr, " No Gain adjustment or DC correction required for file: %s, skipping.\n", file->filename);
                    continue;
                }
                settings->current_output = file->output;
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


enum { OPT_OUTPUT = 256, OPT_OUTPUT_DIR, OPT_TRUE_PEAK, OPT_PEAK_CEILING,
       OPT_LUFS, OPT_TARGET_LUFS, OPT_DITHER_PROFILE };

static int parse_level(const char *text, double *value, double minimum, double maximum)
{
	char *end;
	errno = 0;
	*value = strtod(text, &end);
	return !errno && end != text && !*end && isfinite(*value) && *value >= minimum && *value <= maximum;
}

static const struct option long_options[] = {
	{"output", 1, NULL, OPT_OUTPUT},
	{"output-dir", 1, NULL, OPT_OUTPUT_DIR},
	{"true-peak", 0, NULL, OPT_TRUE_PEAK},
	{"peak-ceiling", 1, NULL, OPT_PEAK_CEILING},
	{"lufs", 0, NULL, OPT_LUFS},
	{"target-lufs", 1, NULL, OPT_TARGET_LUFS},
	{"dither-profile", 1, NULL, OPT_DITHER_PROFILE},
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
	settings.peak_ceiling = -1.;
	settings.target_lufs = -23.;

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
			case OPT_OUTPUT: settings.output_file = optarg; break;
			case OPT_OUTPUT_DIR: settings.output_dir = optarg; break;
			case OPT_TRUE_PEAK: settings.true_peak = 1; break;
			case OPT_LUFS: settings.lufs = 1; break;
			case OPT_PEAK_CEILING:
				if (!parse_level(optarg, &settings.peak_ceiling, -60., 0.)) {
					fprintf(stderr, " --peak-ceiling requires a finite value from -60 to 0 dBTP.\n"); return EXIT_FAILURE;
				}
				settings.true_peak = 1; break;
			case OPT_TARGET_LUFS:
				if (!parse_level(optarg, &settings.target_lufs, -70., -5.)) {
					fprintf(stderr, " --target-lufs requires a finite value from -70 to -5 LUFS.\n"); return EXIT_FAILURE;
				}
				settings.lufs = 1; break;
			case OPT_DITHER_PROFILE:
				if (!strcmp(optarg, "rate")) settings.dither_profile = 1;
				else if (!strcmp(optarg, "legacy")) settings.dither_profile = 0;
				else { fprintf(stderr, " Unknown dither profile: %s.\n", optarg); return EXIT_FAILURE; }
				break;
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
	if ((settings.fast || settings.undo) && (settings.lufs || settings.true_peak)) {
		fprintf(stderr, " LUFS/true-peak measurement cannot be combined with fast/undo mode.\n"); return EXIT_FAILURE;
	}
	if (settings.dither_profile && (!settings.dithering || settings.format == WAV_FMT_FLOAT)) {
		fprintf(stderr, " Rate dither requires -d 1..4 and integer output.\n"); return EXIT_FAILURE;
	}
	if (settings.output_file && settings.output_dir) {
		fprintf(stderr, " Choose --output or --output-dir.\n"); return EXIT_FAILURE;
	}
	if (settings.output_file || settings.output_dir) {
		char *absolute;
		if (settings.std_out || settings.fast || (!settings.apply_gain && !settings.undo) ||
		    optind >= argc || !strcmp(argv[optind], "-")) {
			fprintf(stderr, " Output paths require file input and -y or --undo-gain, without --stdout/--fast.\n"); return EXIT_FAILURE;
		}
		if (settings.output_file) {
			struct stat st;
			if (argc - optind != 1 || strpbrk(argv[optind], "*?") ||
			    stat(argv[optind], &st) || (st.st_mode & S_IFMT) == S_IFDIR) {
				fprintf(stderr, " --output requires exactly one explicit input file.\n"); return EXIT_FAILURE;
			}
		}
		absolute = wg_output_absolute(settings.output_file ? settings.output_file : settings.output_dir, !settings.output_file);
		if (!absolute) { fprintf(stderr, " Cannot prepare output path.\n"); return EXIT_FAILURE; }
		if (settings.output_file) settings.output_file = absolute;
		else {
			settings.output_dir = absolute;
			for (i = optind; i < argc; i++) {
				struct stat st;
				if (!stat(argv[i], &st) && (st.st_mode & S_IFMT) == S_IFDIR) {
					int inside = wg_output_inside(argv[i], absolute);
					if (inside) {
						fprintf(stderr, inside < 0 ? " Cannot verify output directory location.\n" :
						        " Output directory must be outside input directories.\n");
						free(absolute); return EXIT_FAILURE;
					}
				}
			}
		}
	}
	if (optind < argc && !strcmp(argv[optind], "-") && (settings.lufs || settings.true_peak) && !settings.no_offset) {
		fprintf(stderr, " LUFS/true-peak on stdin requires -p; DC-corrected analysis needs seekable input.\n"); return EXIT_FAILURE;
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
		if (status != 1) { wg_meter_free(info.meter); return status == 2 ? EXIT_SUCCESS : EXIT_FAILURE; }
		if ((settings.lufs || settings.true_peak) && !measure_file(&info, info.dc_offset, &settings)) {
			wg_meter_free(info.meter); return EXIT_FAILURE;
		}
		report_gain(&info, info.dc_offset, &settings);
		wg_meter_free(info.meter);
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

	free(settings.output_file);
	free(settings.output_dir);
	return (ret < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
