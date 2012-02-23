/* Copyright 2007-2009 Ben Hutchings.
 * See the file "COPYING" for licence details.
 */
/* Source that reads audio from an ALSA device and combines with black video */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <getopt.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>

#include <asoundlib.h>

#include "config.h"
#include "dif.h"
#include "pcm.h"
#include "protocol.h"
#include "socket.h"

static struct option options[] = {
    {"host",   1, NULL, 'h'},
    {"port",   1, NULL, 'p'},
    {"system", 1, NULL, 's'},
    {"rate",   1, NULL, 'r'},
    {"delay",  1, NULL, 'd'},
    {"help",   0, NULL, 'H'},
    {NULL,     0, NULL, 0}
};

static char * mixer_host = NULL;
static char * mixer_port = NULL;

static void handle_config(const char * name, const char * value)
{
    if (strcmp(name, "MIXER_HOST") == 0)
    {
	free(mixer_host);
	mixer_host = strdup(value);
    }
    else if (strcmp(name, "MIXER_PORT") == 0)
    {
	free(mixer_port);
	mixer_port = strdup(value);
    }
}

static void usage(const char * progname)
{
    fprintf(stderr,
	    "\
Usage: %s [-h HOST] [-p PORT] [-s ntsc|pal] \\\n\
           [-r 48000|32000|44100] [-d DELAY] [DEVICE]\n",
	    progname);
}

struct transfer_params {
    snd_pcm_t *              pcm;
    snd_pcm_uframes_t        hw_frame_count;
    const struct dv_system * system;
    enum dv_sample_rate      sample_rate_code;
    snd_pcm_uframes_t        delay_size;
    int                      sock;
};


static void transfer_frames(struct transfer_params * params)
{
    static uint8_t buf[DIF_MAX_FRAME_SIZE];
    unsigned avail_count = 0;
    unsigned serial_num = 0;

    const snd_pcm_uframes_t buffer_size =
	(params->delay_size >= 2000 ? params->delay_size : 2000)
	+ params->hw_frame_count - 1;
    pcm_sample * samples =
	malloc(sizeof(pcm_sample) * PCM_CHANNELS * buffer_size);

    dv_buffer_fill_dummy(buf, params->system);

    for (;;)
    {
	unsigned frame_count =
	    params->system->audio_frame_counts[params->sample_rate_code].std_cycle[
		serial_num % params->system->audio_frame_counts[params->sample_rate_code].std_cycle_len];

	while (avail_count < params->delay_size || avail_count < frame_count)
	{
	    snd_pcm_sframes_t rc = snd_pcm_readi(params->pcm,
						 samples + PCM_CHANNELS * avail_count,
						 params->hw_frame_count);
	    if (rc < 0)
	    {
		// Recover from buffer underrun
		if (rc == -EPIPE && snd_pcm_prepare(params->pcm) == 0)
		{
		    fprintf(stderr, "WARN: Failing to keep up with audio source\n");
		    continue;
		}
		else
		{
		    fprintf(stderr, "ERROR: snd_pcm_readi: %s\n", snd_strerror(rc));
		    exit(1);
		}
	    }
	    avail_count += rc;
	}

	dv_buffer_set_audio(buf, params->sample_rate_code, frame_count, samples);

	if (write(params->sock, buf, params->system->size)
	    != (ssize_t)params->system->size)
	{
	    perror("ERROR: write");
	    exit(1);
	}

	memmove(samples, samples + PCM_CHANNELS * frame_count,
		sizeof(pcm_sample) * PCM_CHANNELS *
		(avail_count - frame_count));
	avail_count -= frame_count;
	++serial_num;
    }
}

int main(int argc, char ** argv)
{
    /* Initialise settings from configuration files. */
    dvswitch_read_config(handle_config);

    struct transfer_params params;
    char * system_name = NULL;
    long sample_rate = 48000;
    double delay = 0.2;

    /* Parse arguments. */

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:s:r:d:", options, NULL)) != -1)
    {
	switch (opt)
	{
	case 'h':
	    free(mixer_host);
	    mixer_host = strdup(optarg);
	    break;
	case 'p':
	    free(mixer_port);
	    mixer_port = strdup(optarg);
	    break;
	case 's':
	    free(system_name);
	    system_name = strdup(optarg);
	    break;
	case 'r':
	    sample_rate = strtol(optarg, NULL, 10);
	    break;
	case 'd':
	    delay = strtod(optarg, NULL);
	    break;
	case 'H': /* --help */
	    usage(argv[0]);
	    return 0;
	default:
	    usage(argv[0]);
	    return 2;
	}
    }

    if (!mixer_host || !mixer_port)
    {
	fprintf(stderr, "%s: mixer hostname and port not defined\n",
		argv[0]);
	return 2;
    }

    if (!system_name || !strcasecmp(system_name, "pal"))
    {
	params.system = &dv_system_625_50;
    }
    else if (!strcasecmp(system_name, "ntsc"))
    {
	params.system = &dv_system_525_60;
    }
    else
    {
	fprintf(stderr, "%s: invalid system name \"%s\"\n", argv[0], system_name);
	return 2;
    }

    if (sample_rate == 32000)
    {
	params.sample_rate_code = dv_sample_rate_32k;
    }
    else if (sample_rate == 48000)
    {
	params.sample_rate_code = dv_sample_rate_48k;
    }
    else
    {
	fprintf(stderr, "%s: invalid sample rate %ld\n", argv[0], sample_rate);
	return 2;
    }

    if (delay >= 0.0)
    {
	params.delay_size = delay * sample_rate;
    }
    else
    {
	fprintf(stderr, "%s: delays do not work that way!\n", argv[0]);
	return 2;
    }

    if (argc > optind + 1)
    {
	fprintf(stderr, "%s: excess argument \"%s\"\n",
		argv[0], argv[optind + 1]);
	usage(argv[0]);
	return 2;
    }


    const char * device = (argc == optind) ? "default" : argv[optind];
    int rc;

    /* Prepare to capture and connect a socket to the mixer. */

    printf("INFO: Capturing from %s\n", device);
    rc = snd_pcm_open(&params.pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0)
    {
	fprintf(stderr, "ERROR: snd_pcm_open: %s\n", snd_strerror(rc));
	return 1;
    }

    snd_pcm_hw_params_t * hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    rc = snd_pcm_hw_params_any(params.pcm, hw_params);
    if (rc < 0)
    {
	fprintf(stderr, "ERROR: snd_pcm_hw_params_any: %s\n", snd_strerror(rc));
	return 1;
    }
    rc = snd_pcm_hw_params_set_access(params.pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc >= 0)
	rc = snd_pcm_hw_params_set_format(params.pcm, hw_params, SND_PCM_FORMAT_S16);
    if (rc >= 0)
	snd_pcm_hw_params_set_channels(params.pcm, hw_params, 2);
    if (rc >= 0)
	snd_pcm_hw_params_set_rate_resample(params.pcm, hw_params, 1);
    if (rc >= 0)
	snd_pcm_hw_params_set_rate(params.pcm, hw_params, sample_rate, 0);
    if (rc >= 0)
    {
	params.hw_frame_count =
	    params.system->audio_frame_counts[params.sample_rate_code].std_cycle[0];
	rc = snd_pcm_hw_params_set_period_size_near(params.pcm, hw_params,
						    &params.hw_frame_count, 0);
    }
    if (rc >= 0)
    {
	unsigned buffer_time = 250000;
	rc = snd_pcm_hw_params_set_buffer_time_near(params.pcm, hw_params,
						    &buffer_time, 0);
    }
    if (rc >= 0)
	rc = snd_pcm_hw_params(params.pcm, hw_params);
    if (rc < 0)
    {
	fprintf(stderr, "ERROR: snd_pcm_hw_params: %s\n", snd_strerror(rc));
	return 1;
    }

    printf("INFO: Connecting to %s:%s\n", mixer_host, mixer_port);
    params.sock = create_connected_socket(mixer_host, mixer_port);
    assert(params.sock >= 0); /* create_connected_socket() should handle errors */
    if (write(params.sock, GREETING_SOURCE, GREETING_SIZE) != GREETING_SIZE)
    {
	perror("ERROR: write");
	exit(1);
    }
    printf("INFO: Connected.\n");

    transfer_frames(&params);

    close(params.sock);
    snd_pcm_close(params.pcm);

    return 0;
}
