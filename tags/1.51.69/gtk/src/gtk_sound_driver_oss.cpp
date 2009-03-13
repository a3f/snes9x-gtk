#include "gtk_s9x.h"
#include "gtk_sound_driver_oss.h"

#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <fcntl.h>

static int 
base2log (int num)
{
    int power;

    if (num < 1)
        return 0;

    for (power = 0; num > 1; power++)
    {
        num >>= 1;
    }

    return power;
}

static int
powerof2 (int num)
{
    return (1 << num);
}

gpointer
oss_thread (gpointer data)
{
    ((S9xOSSSoundDriver *) data)->mixer_thread ();
    return NULL;
}

S9xOSSSoundDriver::S9xOSSSoundDriver(void)
{
    filedes = -1;
    sound_buffer = NULL;
    thread_die = 0;
    thread = NULL;

    return;
}

void
S9xOSSSoundDriver::init (void)
{
    return;
}

void
S9xOSSSoundDriver::terminate (void)
{
    if (filedes >= 0)
    {
        close (filedes);
    }

    free (sound_buffer);

    return;
}

void
S9xOSSSoundDriver::start (void)
{
    if (!thread)
    {
        thread_die = 0;
        thread = g_thread_create (oss_thread,
                                  (gpointer) this,
                                  TRUE,
                                  NULL);
    }

    return;
}

void
S9xOSSSoundDriver::stop (void)
{
    if (thread != NULL)
    {
        thread_die = 1;
        g_thread_join (thread);
        thread = NULL;
    }

    return;
}

bool8
S9xOSSSoundDriver::open_device (int mode, bool8 stereo, int buffer_size)
{
    int temp;

    printf ("OSS sound driver initializing...\n");

    printf ("    --> (Device: /dev/dsp)...");

    filedes = open ("/dev/dsp", O_WRONLY);

    if (filedes < 0)
        goto fail;

    printf ("OK\n");


    if (so.sixteen_bit)
    {
        printf ("    --> (Format: 16-bit)...");

        temp = AFMT_S16_NE;
        if (ioctl (filedes, SNDCTL_DSP_SETFMT, &temp) < 0)
            goto close_fail;
    }
    else
    {
        printf ("    --> (Format: 8-bit)...");

        temp = AFMT_U8;
        if (ioctl (filedes, SNDCTL_DSP_SETFMT, &temp) < 0)
            goto close_fail;
    }

    printf ("OK\n");

    if (Settings.Stereo)
    {
        temp = 2;
        printf ("    --> (Stereo)...");
    }
    else
    {
        temp = 1;
        printf ("    --> (Mono)...");
    }

    if (ioctl (filedes, SNDCTL_DSP_CHANNELS, &temp) < 0)
        goto close_fail;

    printf ("OK\n");

    printf ("    --> (Frequency: %d)...", so.playback_rate);
    if (ioctl (filedes, SNDCTL_DSP_SPEED, &so.playback_rate) < 0)
        goto close_fail;

    printf ("OK\n");

    /* OSS requires a power-of-two buffer size, first 16 bits are the number
     * of fragments to generate, second 16 are the respective power-of-two. */
    temp = (2 << 16) | base2log (so.buffer_size);
    so.buffer_size = powerof2 (temp & 0xffff);
    printf ("    --> (Buffer size: %d bytes, %dms latency)...",
            so.buffer_size,
            (((so.buffer_size * 1000) >> (so.stereo ? 1 : 0))
                                 >> (so.sixteen_bit ? 1 : 0))
                              / (so.playback_rate));

    if (ioctl (filedes, SNDCTL_DSP_SETFRAGMENT, &temp) < 0)
        goto close_fail;

    printf ("OK\n");

    sound_buffer = (uint8 *) malloc (so.buffer_size);

    return TRUE;

close_fail:

    close (filedes);

fail:
    printf ("failed\n");

    return FALSE;
}

void
S9xOSSSoundDriver::mix (void)
{
    return;
}

void
S9xOSSSoundDriver::mixer_thread (void)
{
    while (1)
    {
        if (thread_die)
        {
            return;
        }
        else
        {
            S9xMixSamples (sound_buffer, so.buffer_size >> (so.sixteen_bit ? 1 : 0));

            write (filedes, (char *) sound_buffer, so.buffer_size);
        }
    }

    return;
}
