#include <stdint.h>
#include <stdio.h>

typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef long snd_pcm_sframes_t;

extern int snd_pcm_open(snd_pcm_t **, const char *, int, int);
extern int snd_pcm_close(snd_pcm_t *);
extern int snd_pcm_prepare(snd_pcm_t *);
extern snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *, snd_pcm_uframes_t);
extern int snd_pcm_hw_params_malloc(snd_pcm_hw_params_t **);
extern void snd_pcm_hw_params_free(snd_pcm_hw_params_t *);
extern int snd_pcm_hw_params_any(snd_pcm_t *, snd_pcm_hw_params_t *);
extern int snd_pcm_hw_params_set_access(snd_pcm_t *, snd_pcm_hw_params_t *, int);
extern int snd_pcm_hw_params_set_format(snd_pcm_t *, snd_pcm_hw_params_t *, int);
extern int snd_pcm_hw_params_set_channels(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
extern int snd_pcm_hw_params_set_rate_near(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int *, int *);
extern int snd_pcm_hw_params(snd_pcm_t *, snd_pcm_hw_params_t *);

enum {
    SND_PCM_STREAM_PLAYBACK = 0,
    SND_PCM_ACCESS_RW_INTERLEAVED = 3,
    SND_PCM_FORMAT_S24_LE = 6,
    TEST_FRAMES = 256
};

static int configure(snd_pcm_t *pcm)
{
    snd_pcm_hw_params_t *params = 0;
    unsigned int rate = 44100;
    int direction = 0;
    int rc = snd_pcm_hw_params_malloc(&params);

    if (rc < 0)
        return rc;
    if ((rc = snd_pcm_hw_params_any(pcm, params)) < 0)
        goto out;
    if ((rc = snd_pcm_hw_params_set_access(pcm, params,
                                            SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        goto out;
    if ((rc = snd_pcm_hw_params_set_format(pcm, params,
                                            SND_PCM_FORMAT_S24_LE)) < 0)
        goto out;
    if ((rc = snd_pcm_hw_params_set_channels(pcm, params, 2)) < 0)
        goto out;
    if ((rc = snd_pcm_hw_params_set_rate_near(pcm, params,
                                               &rate, &direction)) < 0)
        goto out;
    if (rate != 44100) {
        rc = -1;
        goto out;
    }
    rc = snd_pcm_hw_params(pcm, params);
out:
    snd_pcm_hw_params_free(params);
    return rc;
}

int main(void)
{
    snd_pcm_t *master = 0;
    snd_pcm_t *headphone = 0;
    int32_t master_silence[TEST_FRAMES * 2] = {0};
    int32_t headphone_silence[TEST_FRAMES * 2] = {0};
    snd_pcm_sframes_t written;
    int rc;

    rc = snd_pcm_open(&master, "rx3-master", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0 || !master) {
        printf("FAIL open-master rc=%d\n", rc);
        return 1;
    }
    rc = snd_pcm_open(&headphone, "rx3-headphone", SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0 || !headphone) {
        printf("FAIL open-headphone rc=%d\n", rc);
        snd_pcm_close(master);
        return 1;
    }
    if ((rc = configure(master)) < 0 || (rc = configure(headphone)) < 0) {
        printf("FAIL configure rc=%d\n", rc);
        snd_pcm_close(headphone);
        snd_pcm_close(master);
        return 1;
    }
    if ((rc = snd_pcm_prepare(master)) < 0 ||
        (rc = snd_pcm_prepare(headphone)) < 0) {
        printf("FAIL prepare rc=%d\n", rc);
        snd_pcm_close(headphone);
        snd_pcm_close(master);
        return 1;
    }
    written = snd_pcm_writei(headphone, headphone_silence, TEST_FRAMES);
    if (written != TEST_FRAMES) {
        printf("FAIL write-headphone written=%ld\n", (long)written);
        snd_pcm_close(headphone);
        snd_pcm_close(master);
        return 1;
    }
    written = snd_pcm_writei(master, master_silence, TEST_FRAMES);
    if (written != TEST_FRAMES) {
        printf("FAIL write-master written=%ld\n", (long)written);
        snd_pcm_close(headphone);
        snd_pcm_close(master);
        return 1;
    }
    snd_pcm_close(headphone);
    snd_pcm_close(master);
    puts("PASS audioshim-ddj400-4ch-s16-silence");
    return 0;
}
