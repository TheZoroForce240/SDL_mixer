/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include <SDL3/SDL.h>

#include <SDL3_mixer/SDL_mixer.h>
#include "mixer.h"
#include "music.h"
#include "load_aiff.h"
#include "load_voc.h"
#include "load_sndfile.h"

#define MIX_INTERNAL_EFFECT__
#include "effects_internal.h"

/* Magic numbers for various audio file formats */
#define RIFF        0x46464952      /* "RIFF" */
#define WAVE        0x45564157      /* "WAVE" */
#define FORM        0x4d524f46      /* "FORM" */
#define CREA        0x61657243      /* "Crea" */

#if defined(SDL_BUILD_MAJOR_VERSION)
SDL_COMPILE_TIME_ASSERT(SDL_BUILD_MAJOR_VERSION,
                        SDL_MIXER_MAJOR_VERSION == SDL_BUILD_MAJOR_VERSION);
SDL_COMPILE_TIME_ASSERT(SDL_BUILD_MINOR_VERSION,
                        SDL_MIXER_MINOR_VERSION == SDL_BUILD_MINOR_VERSION);
SDL_COMPILE_TIME_ASSERT(SDL_BUILD_MICRO_VERSION,
                        SDL_MIXER_MICRO_VERSION == SDL_BUILD_MICRO_VERSION);
#endif

/* Limited by its encoding in SDL_VERSIONNUM */
SDL_COMPILE_TIME_ASSERT(SDL_MIXER_MAJOR_VERSION_min, SDL_MIXER_MAJOR_VERSION >= 0);
SDL_COMPILE_TIME_ASSERT(SDL_MIXER_MAJOR_VERSION_max, SDL_MIXER_MAJOR_VERSION <= 10);
SDL_COMPILE_TIME_ASSERT(SDL_MIXER_MINOR_VERSION_min, SDL_MIXER_MINOR_VERSION >= 0);
SDL_COMPILE_TIME_ASSERT(SDL_MIXER_MINOR_VERSION_max, SDL_MIXER_MINOR_VERSION <= 999);
SDL_COMPILE_TIME_ASSERT(SDL_MIXER_MICRO_VERSION_min, SDL_MIXER_MICRO_VERSION >= 0);
SDL_COMPILE_TIME_ASSERT(SDL_MIXER_MICRO_VERSION_max, SDL_MIXER_MICRO_VERSION <= 999);

static int audio_opened = 0;
static SDL_AudioSpec mixer;
static SDL_AudioDeviceID audio_device;
static SDL_AudioStream *audio_stream;
static Uint8 *audio_mixbuf;
static int audio_mixbuflen;


typedef struct _Mix_effectinfo
{
    Mix_EffectFunc_t callback;
    Mix_EffectDone_t done_callback;
    void *udata;
    struct _Mix_effectinfo *next;
} effect_info;

static struct _Mix_Channel {
    Mix_Chunk *chunk;
    int playing;
    Uint64 paused;
    Uint8 *samples;
    int volume;
    int looping;
    int tag;
    Uint64 expire;
    Uint64 start_time;
    Mix_Fading fading;
    int fade_volume;
    int fade_volume_reset;
    Uint64 fade_length;
    Uint64 ticks_fade;
    effect_info *effects;
} *mix_channel = NULL;

static effect_info *posteffects = NULL;

static int num_channels;
static int reserved_channels = 0;


/* Support for hooking into the mixer callback system */
static Mix_MixCallback mix_postmix = NULL;
static void *mix_postmix_data = NULL;

/* rcg07062001 callback to alert when channels are done playing. */
static Mix_ChannelFinishedCallback channel_done_callback = NULL;

/* Support for user defined music functions */
static Mix_MixCallback mix_music = music_mixer;
static void *music_data = NULL;

/* rcg06042009 report available decoders at runtime. */
static const char **chunk_decoders = NULL;
static int num_decoders = 0;

static SDL_AtomicInt master_volume = { MIX_MAX_VOLUME };

int Mix_GetNumChunkDecoders(void)
{
    return num_decoders;
}

const char *Mix_GetChunkDecoder(int index)
{
    if ((index < 0) || (index >= num_decoders)) {
        return NULL;
    }
    return chunk_decoders[index];
}

bool Mix_HasChunkDecoder(const char *name)
{
    int index;
    for (index = 0; index < num_decoders; ++index) {
        if (SDL_strcasecmp(name, chunk_decoders[index]) == 0) {
            return true;
        }
    }
    return false;
}

void add_chunk_decoder(const char *decoder)
{
    int i;
    void *ptr;

    /* Check to see if we already have this decoder */
    for (i = 0; i < num_decoders; ++i) {
        if (SDL_strcmp(chunk_decoders[i], decoder) == 0) {
            return;
        }
    }

    ptr = SDL_realloc((void *)chunk_decoders, (size_t)(num_decoders + 1) * sizeof(const char *));
    if (ptr == NULL) {
        return;  /* oh well, go on without it. */
    }
    chunk_decoders = (const char **) ptr;
    chunk_decoders[num_decoders++] = decoder;
}

/* rcg06192001 get linked library's version. */
int Mix_Version(void)
{
    return SDL_MIXER_VERSION;
}

/*
 * Returns a bitmask of already loaded modules (MIX_INIT_* flags).
 *
 * Note that functions other than Mix_Init() may cause a module to get loaded
 * (hence the looping over the interfaces instead of maintaining a set of flags
 * just in Mix_Init() and Mix_Quit()).
 */
static MIX_InitFlags get_loaded_mix_init_flags(void)
{
    int i;
    MIX_InitFlags loaded_init_flags = 0;

    for (i = 0; i < get_num_music_interfaces(); ++i) {
        Mix_MusicInterface *interface;

        interface = get_music_interface(i);
        if (interface->loaded) {
            switch (interface->type) {
            case MUS_FLAC:
                loaded_init_flags |= MIX_INIT_FLAC;
                break;
            case MUS_WAVPACK:
                loaded_init_flags |= MIX_INIT_WAVPACK;
                break;
            case MUS_MOD:
                loaded_init_flags |= MIX_INIT_MOD;
                break;
            case MUS_MP3:
                loaded_init_flags |= MIX_INIT_MP3;
                break;
            case MUS_OGG:
                loaded_init_flags |= MIX_INIT_OGG;
                break;
            case MUS_MID:
                loaded_init_flags |= MIX_INIT_MID;
                break;
            case MUS_OPUS:
                loaded_init_flags |= MIX_INIT_OPUS;
                break;
            default:
                break;
            }
        }
    }

    return loaded_init_flags;
}

MIX_InitFlags Mix_Init(MIX_InitFlags flags)
{
    MIX_InitFlags result = 0;
    MIX_InitFlags already_loaded = get_loaded_mix_init_flags();

    if (flags & MIX_INIT_FLAC) {
        if (load_music_type(MUS_FLAC)) {
            open_music_type(MUS_FLAC);
            result |= MIX_INIT_FLAC;
        } else {
            SDL_SetError("FLAC support not available");
        }
    }
    if (flags & MIX_INIT_WAVPACK) {
        if (load_music_type(MUS_WAVPACK)) {
            open_music_type(MUS_WAVPACK);
            result |= MIX_INIT_WAVPACK;
        } else {
            SDL_SetError("WavPack support not available");
        }
    }
    if (flags & MIX_INIT_MOD) {
        if (load_music_type(MUS_MOD)) {
            open_music_type(MUS_MOD);
            result |= MIX_INIT_MOD;
        } else {
            SDL_SetError("MOD support not available");
        }
    }
    if (flags & MIX_INIT_MP3) {
        if (load_music_type(MUS_MP3)) {
            open_music_type(MUS_MP3);
            result |= MIX_INIT_MP3;
        } else {
            SDL_SetError("MP3 support not available");
        }
    }
    if (flags & MIX_INIT_OGG) {
        if (load_music_type(MUS_OGG)) {
            open_music_type(MUS_OGG);
            result |= MIX_INIT_OGG;
        } else {
            SDL_SetError("OGG support not available");
        }
    }
    if (flags & MIX_INIT_OPUS) {
        if (load_music_type(MUS_OPUS)) {
            open_music_type(MUS_OPUS);
            result |= MIX_INIT_OPUS;
        } else {
            SDL_SetError("OPUS support not available");
        }
    }
    if (flags & MIX_INIT_MID) {
        if (load_music_type(MUS_MID)) {
            open_music_type(MUS_MID);
            result |= MIX_INIT_MID;
        } else {
            SDL_SetError("MIDI support not available");
        }
    }
    result |= already_loaded;

    return result;
}

void Mix_Quit(void)
{
    unload_music();
    SNDFILE_uninit();
}

static bool _Mix_remove_all_effects(int channel, effect_info **e);

/*
 * rcg06122001 Cleanup effect callbacks.
 *  MAKE SURE Mix_LockAudio() is called before this (or you're in the
 *   audio callback).
 */
static void _Mix_channel_done_playing(int channel)
{
    if (channel_done_callback) {
        channel_done_callback(channel);
    }

    /*
     * Call internal function directly, to avoid locking audio from
     *   inside audio callback.
     */
    _Mix_remove_all_effects(channel, &mix_channel[channel].effects);
}


static void *Mix_DoEffects(int chan, void *snd, int len)
{
    int posteffect = (chan == MIX_CHANNEL_POST);
    effect_info *e = ((posteffect) ? posteffects : mix_channel[chan].effects);
    void *buf = snd;

    if (e != NULL) {    /* are there any registered effects? */
        /* if this is the postmix, we can just overwrite the original. */
        if (!posteffect) {
            buf = SDL_malloc((size_t)len);
            if (buf == NULL) {
                return snd;
            }
            SDL_memcpy(buf, snd, (size_t)len);
        }

        for (; e != NULL; e = e->next) {
            if (e->callback != NULL) {
                e->callback(chan, buf, len, e->udata);
            }
        }
    }

    /* be sure to SDL_free() the return value if != snd ... */
    return buf;
}


/* Mixing function */
static void SDLCALL
mix_channels(void *udata, SDL_AudioStream *astream, int len, int total)
{
    Uint8 *stream;
    Uint8 *mix_input;
    int i, mixable, master_vol;
    Uint64 sdl_ticks;

    (void)udata;
    (void)total;

    if (audio_mixbuflen < len) {
        void *ptr = SDL_aligned_alloc(SDL_GetSIMDAlignment(), len);
        if (!ptr) {
            return;  // oh well.
        }
        SDL_aligned_free(audio_mixbuf);
        audio_mixbuf = (Uint8 *) ptr;
        audio_mixbuflen = len;
    }

    stream = audio_mixbuf;

    /* Need to initialize the stream in SDL 1.3+ */
    SDL_memset(stream, SDL_GetSilenceValueForFormat(mixer.format), (size_t)len);

    /* Mix the music (must be done before the channels are added) */
    mix_music(music_data, stream, len);

    master_vol = SDL_GetAtomicInt(&master_volume);

    /* Mix any playing channels... */
    sdl_ticks = SDL_GetTicks();
    for (i = 0; i < num_channels; ++i) {
        if (!mix_channel[i].paused) {
            if (mix_channel[i].expire > 0 && mix_channel[i].expire < sdl_ticks) {
                /* Expiration delay for that channel is reached */
                mix_channel[i].playing = 0;
                mix_channel[i].looping = 0;
                mix_channel[i].fading = MIX_NO_FADING;
                mix_channel[i].expire = 0;
                _Mix_channel_done_playing(i);
            } else if (mix_channel[i].fading != MIX_NO_FADING) {
                Uint64 ticks = sdl_ticks - mix_channel[i].ticks_fade;
                if (ticks >= mix_channel[i].fade_length) {
                    Mix_Volume(i, mix_channel[i].fade_volume_reset); /* Restore the volume */
                    if (mix_channel[i].fading == MIX_FADING_OUT) {
                        mix_channel[i].playing = 0;
                        mix_channel[i].looping = 0;
                        mix_channel[i].expire = 0;
                        _Mix_channel_done_playing(i);
                    }
                    mix_channel[i].fading = MIX_NO_FADING;
                } else {
                    if (mix_channel[i].fading == MIX_FADING_OUT) {
                        int volume = (int)((mix_channel[i].fade_volume * (mix_channel[i].fade_length - ticks)) / mix_channel[i].fade_length);
                        Mix_Volume(i, volume);
                    } else {
                        int volume = (int)((mix_channel[i].fade_volume * ticks) / mix_channel[i].fade_length);
                        Mix_Volume(i, volume);
                    }
                }
            }
            if (mix_channel[i].playing > 0) {
                int volume = (master_vol * (mix_channel[i].volume * mix_channel[i].chunk->volume)) / (MIX_MAX_VOLUME * MIX_MAX_VOLUME);
                float fvolume = (float)volume / (float)MIX_MAX_VOLUME;
                int index = 0;
                int remaining = len;
                while (mix_channel[i].playing > 0 && index < len) {
                    remaining = len - index;
                    mixable = mix_channel[i].playing;
                    if (mixable > remaining) {
                        mixable = remaining;
                    }

                    mix_input = Mix_DoEffects(i, mix_channel[i].samples, mixable);
                    SDL_MixAudio(stream+index, mix_input, mixer.format, mixable, fvolume);
                    if (mix_input != mix_channel[i].samples)
                        SDL_free(mix_input);

                    mix_channel[i].samples += mixable;
                    mix_channel[i].playing -= mixable;
                    index += mixable;

                    /* rcg06072001 Alert app if channel is done playing. */
                    if (!mix_channel[i].playing && !mix_channel[i].looping) {
                        mix_channel[i].fading = MIX_NO_FADING;
                        mix_channel[i].expire = 0;
                        _Mix_channel_done_playing(i);

                        /* Update the volume after the application callback */
                        volume = (master_vol * (mix_channel[i].volume * mix_channel[i].chunk->volume)) / (MIX_MAX_VOLUME * MIX_MAX_VOLUME);
                        fvolume = (float)volume / (float)MIX_MAX_VOLUME;
                    }
                }

                /* If looping the sample and we are at its end, make sure
                   we will still return a full buffer */
                while (mix_channel[i].looping && index < len) {
                    int alen = mix_channel[i].chunk->alen;
                    remaining = len - index;
                    if (remaining > alen) {
                        remaining = alen;
                    }

                    mix_input = Mix_DoEffects(i, mix_channel[i].chunk->abuf, remaining);
                    SDL_MixAudio(stream+index, mix_input, mixer.format, remaining, fvolume);
                    if (mix_input != mix_channel[i].chunk->abuf)
                        SDL_free(mix_input);

                    if (mix_channel[i].looping > 0) {
                        --mix_channel[i].looping;
                    }
                    mix_channel[i].samples = mix_channel[i].chunk->abuf + remaining;
                    mix_channel[i].playing = mix_channel[i].chunk->alen - remaining;
                    index += remaining;
                }
                if (! mix_channel[i].playing && mix_channel[i].looping) {
                    if (mix_channel[i].looping > 0) {
                        --mix_channel[i].looping;
                    }
                    mix_channel[i].samples = mix_channel[i].chunk->abuf;
                    mix_channel[i].playing = mix_channel[i].chunk->alen;
                }
            }
        }
    }

    /* rcg06122001 run posteffects... */
    Mix_DoEffects(MIX_CHANNEL_POST, stream, len);

    if (mix_postmix) {
        mix_postmix(mix_postmix_data, stream, len);
    }

    SDL_PutAudioStreamData(astream, audio_mixbuf, len);
}

#if 0
static void PrintFormat(char *title, SDL_AudioSpec *fmt)
{
    printf("%s: %d bit %s audio (%s) at %u Hz\n", title, (fmt->format&0xFF),
            (fmt->format&0x8000) ? "signed" : "unsigned",
            (fmt->channels > 2) ? "surround" :
            (fmt->channels > 1) ? "stereo" : "mono", fmt->freq);
}
#endif

/* Open the mixer with a certain desired audio format */
bool Mix_OpenAudio(SDL_AudioDeviceID devid, const SDL_AudioSpec *spec)
{
    int i;

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            return false;
        }
    }

    /* If the mixer is already opened, increment open count */
    if (audio_opened) {
        if (spec && (spec->format == mixer.format) && (spec->channels == mixer.channels)) {
            ++audio_opened;
            return true;
        }
        while (audio_opened) {
            Mix_CloseAudio();
        }
    }

    if (devid == 0) {
        devid = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }

    if ((audio_device = SDL_OpenAudioDevice(devid, spec)) == 0) {
        return false;
    }

    SDL_GetAudioDeviceFormat(audio_device, &mixer, NULL);
    audio_stream = SDL_CreateAudioStream(&mixer, &mixer);
    if (!audio_stream) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
        return false;
    }

#if 0
    PrintFormat("Audio device", &mixer);
#endif

    num_channels = MIX_CHANNELS;
    mix_channel = (struct _Mix_Channel *) SDL_malloc(num_channels * sizeof(struct _Mix_Channel));

    /* Clear out the audio channels */
    for (i = 0; i < num_channels; ++i) {
        mix_channel[i].chunk = NULL;
        mix_channel[i].playing = 0;
        mix_channel[i].looping = 0;
        mix_channel[i].volume = MIX_MAX_VOLUME;
        mix_channel[i].fade_volume = MIX_MAX_VOLUME;
        mix_channel[i].fade_volume_reset = MIX_MAX_VOLUME;
        mix_channel[i].fading = MIX_NO_FADING;
        mix_channel[i].tag = -1;
        mix_channel[i].expire = 0;
        mix_channel[i].effects = NULL;
        mix_channel[i].paused = 0;
    }
    Mix_VolumeMusic(MIX_MAX_VOLUME);

    _Mix_InitEffects();

    add_chunk_decoder("WAVE");
    add_chunk_decoder("AIFF");
    add_chunk_decoder("VOC");

    /* Initialize the music players */
    open_music(&mixer);

    SDL_BindAudioStream(audio_device, audio_stream);
    SDL_SetAudioStreamGetCallback(audio_stream, mix_channels, NULL);

    audio_opened = 1;
    return true;
}

/* Pause or resume the audio streaming */
void Mix_PauseAudio(int pause_on)
{
    if (pause_on) {
        SDL_PauseAudioDevice(audio_device);
    } else {
        SDL_ResumeAudioDevice(audio_device);
    }
    Mix_LockAudio();
    pause_async_music(pause_on);
    Mix_UnlockAudio();
}

/* Dynamically change the number of channels managed by the mixer.
   If decreasing the number of channels, the upper channels are
   stopped.
 */
int Mix_AllocateChannels(int numchans)
{
    struct _Mix_Channel *mix_channel_tmp;

    if (numchans<0 || numchans==num_channels)
        return num_channels;

    if (numchans < num_channels) {
        /* Stop the affected channels */
        int i;
        for (i = numchans; i < num_channels; i++) {
            Mix_UnregisterAllEffects(i);
            Mix_HaltChannel(i);
        }
    }
    Mix_LockAudio();
    /* Allocate channels into temporary pointer */
    if (numchans) {
        mix_channel_tmp = (struct _Mix_Channel *) SDL_realloc(mix_channel, numchans * sizeof(struct _Mix_Channel));
    } else {
        /* Handle 0 numchans */
        SDL_free(mix_channel);
        mix_channel_tmp = NULL;
    }
    /* Check the allocation */
    if (mix_channel_tmp || !numchans) {
        /* Apply the temporary pointer on success */
        mix_channel = mix_channel_tmp;
        if (numchans > num_channels) {
            /* Initialize the new channels */
            int i;
            for (i = num_channels; i < numchans; i++) {
                mix_channel[i].chunk = NULL;
                mix_channel[i].playing = 0;
                mix_channel[i].looping = 0;
                mix_channel[i].volume = MIX_MAX_VOLUME;
                mix_channel[i].fade_volume = MIX_MAX_VOLUME;
                mix_channel[i].fade_volume_reset = MIX_MAX_VOLUME;
                mix_channel[i].fading = MIX_NO_FADING;
                mix_channel[i].tag = -1;
                mix_channel[i].expire = 0;
                mix_channel[i].effects = NULL;
                mix_channel[i].paused = 0;
            }
        }
        num_channels = numchans;
    } else {
        /* On error mix_channel remains intact */
        SDL_SetError("Channel allocation failed");
    }
    Mix_UnlockAudio();
    return num_channels; /* If the return value equals numchans the allocation was successful */
}

/* Return the actual mixer parameters */
bool Mix_QuerySpec(int *frequency, SDL_AudioFormat *format, int *channels)
{
    if (audio_opened) {
        if (frequency) {
            *frequency = mixer.freq;
        }
        if (format) {
            *format = mixer.format;
        }
        if (channels) {
            *channels = mixer.channels;
        }
    }
    return (audio_opened > 0);
}

typedef struct _MusicFragment
{
    Uint8 *data;
    int size;
    struct _MusicFragment *next;
} MusicFragment;

static SDL_AudioSpec *Mix_LoadMusic_IO(SDL_IOStream *src, bool closeio, SDL_AudioSpec *spec, Uint8 **audio_buf, Uint32 *audio_len)
{
    int i;
    Mix_MusicType music_type;
    Mix_MusicInterface *interface = NULL;
    void *music = NULL;
    Sint64 start;
    bool playing;
    MusicFragment *first = NULL, *last = NULL, *fragment = NULL;
    int count = 0;
    int fragment_size;

    music_type = detect_music_type(src);
    if (!load_music_type(music_type) || !open_music_type(music_type)) {
        return NULL;
    }

    *spec = mixer;

    /* Use fragments sized on full audio frame boundaries - this'll do */
    fragment_size = 4096/*spec->samples*/ * (SDL_AUDIO_BITSIZE(spec->format) / 8) * spec->channels;

    start = SDL_TellIO(src);
    for (i = 0; i < get_num_music_interfaces(); ++i) {
        interface = get_music_interface(i);
        if (!interface->opened) {
            continue;
        }
        if (interface->type != music_type) {
            continue;
        }
        if (!interface->CreateFromIO || !interface->GetAudio) {
            continue;
        }

        /* These music interfaces are not safe to use while music is playing */
        if (interface->api == MIX_MUSIC_NATIVEMIDI) {
            continue;
        }

        music = interface->CreateFromIO(src, closeio);
        if (music) {
            /* The interface owns the data source now */
            closeio = false;
            break;
        }

        /* Reset the stream for the next decoder */
        SDL_SeekIO(src, start, SDL_IO_SEEK_SET);
    }

    if (!music) {
        if (closeio) {
            SDL_CloseIO(src);
        }
        SDL_SetError("Unrecognized audio format");
        return NULL;
    }

    Mix_LockAudio();

    if (interface->Play) {
        interface->Play(music, 1);
    }
    playing = true;

    while (playing) {
        int left;

        fragment = (MusicFragment *)SDL_malloc(sizeof(*fragment));
        if (!fragment) {
            /* Uh oh, out of memory, let's return what we have */
            break;
        }
        fragment->data = (Uint8 *)SDL_malloc(fragment_size);
        if (!fragment->data) {
            /* Uh oh, out of memory, let's return what we have */
            SDL_free(fragment);
            break;
        }
        fragment->next = NULL;

        left = interface->GetAudio(music, fragment->data, fragment_size);
        if (left > 0) {
            playing = false;
        } else if (interface->IsPlaying) {
            playing = interface->IsPlaying(music);
        }
        fragment->size = (fragment_size - left);

        if (!first) {
            first = fragment;
        }
        if (last) {
            last->next = fragment;
        }
        last = fragment;
        ++count;
    }

    if (interface->Stop) {
        interface->Stop(music);
    }

    if (music) {
        interface->Delete(music);
    }

    Mix_UnlockAudio();

    if (count > 0) {
        *audio_len = (count - 1) * fragment_size + last->size;
        *audio_buf = (Uint8 *)SDL_malloc(*audio_len);
        if (*audio_buf) {
            Uint8 *dst = *audio_buf;
            for (fragment = first; fragment; fragment = fragment->next) {
                SDL_memcpy(dst, fragment->data, fragment->size);
                dst += fragment->size;
            }
        } else {
            spec = NULL;
        }
    } else {
        SDL_SetError("No audio data");
        spec = NULL;
    }

    while (first) {
        fragment = first;
        first = first->next;
        SDL_free(fragment->data);
        SDL_free(fragment);
    }

    if (closeio) {
        SDL_CloseIO(src);
    }
    return spec;
}

/* Load a wave file */
Mix_Chunk *Mix_LoadWAV_IO(SDL_IOStream *src, bool closeio)
{
    Uint8 magic[4];
    Mix_Chunk *chunk;
    SDL_AudioSpec wavespec, *loaded;

    /* rcg06012001 Make sure src is valid */
    if (!src) {
        SDL_SetError("Mix_LoadWAV_IO with NULL src");
        return NULL;
    }

    /* Make sure audio has been opened */
    if (!audio_opened) {
        SDL_SetError("Audio device hasn't been opened");
        if (closeio) {
            SDL_CloseIO(src);
        }
        return NULL;
    }

    /* Allocate the chunk memory */
    chunk = (Mix_Chunk *)SDL_malloc(sizeof(Mix_Chunk));
    if (chunk == NULL) {
        if (closeio) {
            SDL_CloseIO(src);
        }
        return NULL;
    }

    /* Find out what kind of audio file this is */
    if (SDL_ReadIO(src, magic, 4) != 4) {
        SDL_free(chunk);
        if (closeio) {
            SDL_CloseIO(src);
        }
        SDL_SetError("Couldn't read first 4 bytes of audio data");
        return NULL;
    }
    /* Seek backwards for compatibility with older loaders */
    SDL_SeekIO(src, -4, SDL_IO_SEEK_CUR);

    /* First try loading via libsndfile */
    SDL_zero(wavespec);
    loaded = Mix_LoadSndFile_IO(src, closeio, &wavespec, (Uint8 **)&chunk->abuf, &chunk->alen);

    if (!loaded)  {
        if (SDL_memcmp(magic, "WAVE", 4) == 0 || SDL_memcmp(magic, "RIFF", 4) == 0) {
            loaded = SDL_LoadWAV_IO(src, closeio, &wavespec, (Uint8 **)&chunk->abuf, &chunk->alen) ? &wavespec : NULL;
        } else if (SDL_memcmp(magic, "FORM", 4) == 0) {
            loaded = Mix_LoadAIFF_IO(src, closeio, &wavespec, (Uint8 **)&chunk->abuf, &chunk->alen);
        } else if (SDL_memcmp(magic, "Crea", 4) == 0) {
            loaded = Mix_LoadVOC_IO(src, closeio, &wavespec, (Uint8 **)&chunk->abuf, &chunk->alen);
        } else {
            loaded = Mix_LoadMusic_IO(src, closeio, &wavespec, (Uint8 **)&chunk->abuf, &chunk->alen);
        }
    }
    if (!loaded) {
        /* The individual loaders have closed src if needed */
        SDL_free(chunk);
        return NULL;
    }

#if 0
    PrintFormat("Audio device", &mixer);
    PrintFormat("-- Wave file", &wavespec);
#endif

    chunk->allocated = 1;
    chunk->volume = MIX_MAX_VOLUME;

    /* Build the audio converter and create conversion buffers */
    if (wavespec.format != mixer.format ||
        wavespec.channels != mixer.channels ||
        wavespec.freq != mixer.freq) {

        Uint8 *dst_data = NULL;
        int dst_len = 0;

        if (!SDL_ConvertAudioSamples(&wavespec, chunk->abuf, chunk->alen, &mixer, &dst_data, &dst_len)) {
            SDL_free(chunk->abuf);
            SDL_free(chunk);
            return NULL;
        }

        SDL_free(chunk->abuf);
        chunk->abuf = dst_data;
        chunk->alen = dst_len;
    }

    return chunk;
}

Mix_Chunk *Mix_LoadWAV(const char *file)
{
    return Mix_LoadWAV_IO(SDL_IOFromFile(file, "rb"), 1);
}


/* Load a wave file of the mixer format from a memory buffer */
Mix_Chunk *Mix_QuickLoad_WAV(Uint8 *mem)
{
    Mix_Chunk *chunk;
    Uint8 magic[4];

    /* Make sure audio has been opened */
    if (! audio_opened) {
        SDL_SetError("Audio device hasn't been opened");
        return NULL;
    }

    /* Allocate the chunk memory */
    chunk = (Mix_Chunk *)SDL_calloc(1,sizeof(Mix_Chunk));
    if (chunk == NULL) {
        return NULL;
    }

    /* Essentially just skip to the audio data (no error checking - fast) */
    chunk->allocated = 0;
    mem += 12; /* WAV header */
    do {
        SDL_memcpy(magic, mem, 4);
        mem += 4;
        chunk->alen = ((mem[3]<<24)|(mem[2]<<16)|(mem[1]<<8)|(mem[0]));
        mem += 4;
        chunk->abuf = mem;
        mem += chunk->alen;
    } while (SDL_memcmp(magic, "data", 4) != 0);
    chunk->volume = MIX_MAX_VOLUME;

    return chunk;
}

/* Load raw audio data of the mixer format from a memory buffer */
Mix_Chunk *Mix_QuickLoad_RAW(Uint8 *mem, Uint32 len)
{
    Mix_Chunk *chunk;

    /* Make sure audio has been opened */
    if (! audio_opened) {
        SDL_SetError("Audio device hasn't been opened");
        return NULL;
    }

    /* Allocate the chunk memory */
    chunk = (Mix_Chunk *)SDL_malloc(sizeof(Mix_Chunk));
    if (chunk == NULL) {
        return NULL;
    }

    /* Essentially just point at the audio data (no error checking - fast) */
    chunk->allocated = 0;
    chunk->alen = len;
    chunk->abuf = mem;
    chunk->volume = MIX_MAX_VOLUME;

    return chunk;
}

/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
static void Mix_HaltChannel_locked(int which)
{
    if (Mix_Playing(which)) {
        mix_channel[which].playing = 0;
        mix_channel[which].looping = 0;
        _Mix_channel_done_playing(which);
    }
    mix_channel[which].expire = 0;
    if (mix_channel[which].fading != MIX_NO_FADING) /* Restore volume */
        mix_channel[which].volume = mix_channel[which].fade_volume_reset;
    mix_channel[which].fading = MIX_NO_FADING;
}

/* Free an audio chunk previously loaded */
void Mix_FreeChunk(Mix_Chunk *chunk)
{
    int i;

    /* Caution -- if the chunk is playing, the mixer will crash */
    if (chunk) {
        /* Guarantee that this chunk isn't playing */
        Mix_LockAudio();
        if (mix_channel) {
            for (i = 0; i < num_channels; ++i) {
                if (chunk == mix_channel[i].chunk) {
                    Mix_HaltChannel_locked(i);
                }
            }
        }
        Mix_UnlockAudio();
        /* Actually free the chunk */
        if (chunk->allocated) {
            SDL_free(chunk->abuf);
        }
        SDL_free(chunk);
    }
}

/* Set a function that is called after all mixing is performed.
   This can be used to provide real-time visual display of the audio stream
   or add a custom mixer filter for the stream data.
*/
void Mix_SetPostMix(Mix_MixCallback mix_func, void *arg)
{
    Mix_LockAudio();
    mix_postmix_data = arg;
    mix_postmix = mix_func;
    Mix_UnlockAudio();
}

/* Add your own music player or mixer function.
   If 'mix_func' is NULL, the default music player is re-enabled.
 */
void Mix_HookMusic(Mix_MixCallback mix_func, void *arg)
{
    Mix_LockAudio();
    if (mix_func != NULL) {
        music_data = arg;
        mix_music = mix_func;
    } else {
        music_data = NULL;
        mix_music = music_mixer;
    }
    Mix_UnlockAudio();
}

void *Mix_GetMusicHookData(void)
{
    return music_data;
}

void Mix_ChannelFinished(Mix_ChannelFinishedCallback channel_finished)
{
    Mix_LockAudio();
    channel_done_callback = channel_finished;
    Mix_UnlockAudio();
}


/* Reserve the first channels (0 -> n-1) for the application, i.e. don't allocate
   them dynamically to the next sample if requested with a -1 value below.
   Returns the number of reserved channels.
 */
int Mix_ReserveChannels(int num)
{
    if (num < 0)
        num = 0;
    if (num > num_channels)
        num = num_channels;
    reserved_channels = num;
    return num;
}

static int checkchunkintegral(Mix_Chunk *chunk)
{
    int frame_width = 1;

    if ((mixer.format & 0xFF) == 16) frame_width = 2;
    frame_width *= mixer.channels;
    while (chunk->alen % frame_width) chunk->alen--;
    return chunk->alen;
}

/* Play an audio chunk on a specific channel.
   If the specified channel is -1, play on the first free channel.
   'ticks' is the number of milliseconds at most to play the sample, or -1
   if there is no limit.
   Returns which channel was used to play the sound.
*/
int Mix_PlayChannelTimed(int which, Mix_Chunk *chunk, int loops, int ticks)
{
    int i;

    /* Don't play null pointers :-) */
    if (chunk == NULL) {
        SDL_SetError("Tried to play a NULL chunk");
        return -1;
    }
    if (!checkchunkintegral(chunk)) {
        SDL_SetError("Tried to play a chunk with a bad frame");
        return -1;
    }

    /* Lock the mixer while modifying the playing channels */
    Mix_LockAudio();
    {
        /* If which is -1, play on the first free channel */
        if (which == -1) {
            for (i = reserved_channels; i < num_channels; ++i) {
                if (!Mix_Playing(i))
                    break;
            }
            if (i == num_channels) {
                SDL_SetError("No free channels available");
                which = -1;
            } else {
                which = i;
            }
        } else {
            if (Mix_Playing(which))
                _Mix_channel_done_playing(which);
        }

        /* Queue up the audio data for this channel */
        if (which >= 0 && which < num_channels) {
            Uint64 sdl_ticks = SDL_GetTicks();
            mix_channel[which].samples = chunk->abuf;
            mix_channel[which].playing = (int)chunk->alen;
            mix_channel[which].looping = loops;
            mix_channel[which].chunk = chunk;
            mix_channel[which].paused = 0;
            mix_channel[which].fading = MIX_NO_FADING;
            mix_channel[which].start_time = sdl_ticks;
            mix_channel[which].expire = (ticks > 0) ? (sdl_ticks + ticks) : 0;
        }
    }
    Mix_UnlockAudio();

    /* Return the channel on which the sound is being played */
    return which;
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)
{
    return Mix_PlayChannelTimed(channel, chunk, loops, -1);
}

/* Change the expiration delay for a channel */
int Mix_ExpireChannel(int which, int ticks)
{
    int status = 0;

    if (which == -1) {
        int i;
        for (i = 0; i < num_channels; ++i) {
            status += Mix_ExpireChannel(i, ticks);
        }
    } else if (which < num_channels) {
        Mix_LockAudio();
        mix_channel[which].expire = (ticks>0) ? (SDL_GetTicks() + (Uint32)ticks) : 0;
        Mix_UnlockAudio();
        ++status;
    }
    return status;
}

/* Fade in a sound on a channel, over ms milliseconds */
int Mix_FadeInChannelTimed(int which, Mix_Chunk *chunk, int loops, int ms, int ticks)
{
    int i;

    /* Don't play null pointers :-) */
    if (chunk == NULL) {
        return -1;
    }
    if (!checkchunkintegral(chunk)) {
        SDL_SetError("Tried to play a chunk with a bad frame");
        return -1;
    }

    /* Lock the mixer while modifying the playing channels */
    Mix_LockAudio();
    {
        /* If which is -1, play on the first free channel */
        if (which == -1) {
            for (i = reserved_channels; i < num_channels; ++i) {
                if (!Mix_Playing(i))
                    break;
            }
            if (i == num_channels) {
                which = -1;
            } else {
                which = i;
            }
        } else {
            if (Mix_Playing(which))
                _Mix_channel_done_playing(which);
        }

        /* Queue up the audio data for this channel */
        if (which >= 0 && which < num_channels) {
            Uint64 sdl_ticks = SDL_GetTicks();
            mix_channel[which].samples = chunk->abuf;
            mix_channel[which].playing = (int)chunk->alen;
            mix_channel[which].looping = loops;
            mix_channel[which].chunk = chunk;
            mix_channel[which].paused = 0;
            if (mix_channel[which].fading == MIX_NO_FADING) {
                mix_channel[which].fade_volume_reset = mix_channel[which].volume;
            }
            mix_channel[which].fading = MIX_FADING_IN;
            mix_channel[which].fade_volume = mix_channel[which].volume;
            mix_channel[which].volume = 0;
            mix_channel[which].fade_length = (Uint64)ms;
            mix_channel[which].start_time = mix_channel[which].ticks_fade = sdl_ticks;
            mix_channel[which].expire = (ticks > 0) ? (sdl_ticks + ticks) : 0;
        }
    }
    Mix_UnlockAudio();

    /* Return the channel on which the sound is being played */
    return which;
}

int Mix_FadeInChannel(int channel, Mix_Chunk *chunk, int loops, int ms)
{
    return Mix_FadeInChannelTimed(channel, chunk, loops, ms, -1);
}


/* Set volume of a particular channel */
int Mix_Volume(int which, int volume)
{
    int i;
    int prev_volume = 0;

    if (which == -1) {
        for (i = 0; i < num_channels; ++i) {
            prev_volume += Mix_Volume(i, volume);
        }
        prev_volume /= num_channels;
    } else if (which < num_channels) {
        prev_volume = mix_channel[which].volume;
        if (volume >= 0) {
            if (volume > MIX_MAX_VOLUME) {
                volume = MIX_MAX_VOLUME;
            }
            mix_channel[which].volume = volume;
        }
    }
    return prev_volume;
}
/* Set volume of a particular chunk */
int Mix_VolumeChunk(Mix_Chunk *chunk, int volume)
{
    int prev_volume;

    if (chunk == NULL) {
        return -1;
    }
    prev_volume = chunk->volume;
    if (volume >= 0) {
        if (volume > MIX_MAX_VOLUME) {
            volume = MIX_MAX_VOLUME;
        }
        chunk->volume = volume;
    }
    return prev_volume;
}

/* Halt playing of a particular channel */
void Mix_HaltChannel(int which)
{
    int i;

    Mix_LockAudio();
    if (which == -1) {
        for (i = 0; i < num_channels; ++i) {
            Mix_HaltChannel_locked(i);
        }
    } else if (which < num_channels) {
        Mix_HaltChannel_locked(which);
    }
    Mix_UnlockAudio();
}

/* Halt playing of a particular group of channels */
void Mix_HaltGroup(int tag)
{
    int i;

    for (i = 0; i < num_channels; ++i) {
        if (mix_channel[i].tag == tag) {
            Mix_HaltChannel(i);
        }
    }
}

/* Fade out a channel and then stop it automatically */
int Mix_FadeOutChannel(int which, int ms)
{
    int status;

    status = 0;
    if (audio_opened) {
        if (which == -1) {
            int i;

            for (i = 0; i < num_channels; ++i) {
                status += Mix_FadeOutChannel(i, ms);
            }
        } else if (which < num_channels) {
            Mix_LockAudio();
            if (Mix_Playing(which) &&
                (mix_channel[which].volume > 0) &&
                (mix_channel[which].fading != MIX_FADING_OUT)) {
                mix_channel[which].fade_volume = mix_channel[which].volume;
                mix_channel[which].fade_length = (Uint64)ms;
                mix_channel[which].ticks_fade = SDL_GetTicks();

                /* only change fade_volume_reset if we're not fading. */
                if (mix_channel[which].fading == MIX_NO_FADING) {
                    mix_channel[which].fade_volume_reset = mix_channel[which].volume;
                }

                mix_channel[which].fading = MIX_FADING_OUT;

                ++status;
            }
            Mix_UnlockAudio();
        }
    }
    return status;
}

/* Halt playing of a particular group of channels */
int Mix_FadeOutGroup(int tag, int ms)
{
    int i;
    int status = 0;
    for (i = 0; i < num_channels; ++i) {
        if (mix_channel[i].tag == tag) {
            status += Mix_FadeOutChannel(i,ms);
        }
    }
    return status;
}

Mix_Fading Mix_FadingChannel(int which)
{
    if (which < 0 || which >= num_channels) {
        return MIX_NO_FADING;
    }
    return mix_channel[which].fading;
}

/* Check the status of a specific channel.
   If the specified mix_channel is -1, check all mix channels.
*/
int Mix_Playing(int which)
{
    int status;

    status = 0;
    if (which == -1) {
        int i;

        for (i = 0; i < num_channels; ++i) {
            if ((mix_channel[i].playing > 0) ||
                mix_channel[i].looping)
            {
                ++status;
            }
        }
    } else if (which < num_channels) {
        if ((mix_channel[which].playing > 0) ||
             mix_channel[which].looping)
        {
            ++status;
        }
    }
    return status;
}

/* rcg06072001 Get the chunk associated with a channel. */
Mix_Chunk *Mix_GetChunk(int channel)
{
    Mix_Chunk *retval = NULL;

    if ((channel >= 0) && (channel < num_channels)) {
        retval = mix_channel[channel].chunk;
    }

    return retval;
}

/* Close the mixer, halting all playing audio */
void Mix_CloseAudio(void)
{
    int i;

    if (audio_opened) {
        if (audio_opened == 1) {
            for (i = 0; i < num_channels; i++) {
                Mix_UnregisterAllEffects(i);
            }
            Mix_UnregisterAllEffects(MIX_CHANNEL_POST);
            close_music();
            Mix_HaltChannel(-1);
            _Mix_DeinitEffects();
            SDL_DestroyAudioStream(audio_stream);
            audio_stream = NULL;
            SDL_CloseAudioDevice(audio_device);
            audio_device = 0;
            SDL_free(mix_channel);
            mix_channel = NULL;
            SDL_aligned_free(audio_mixbuf);
            audio_mixbuf = NULL;
            audio_mixbuflen = 0;
            /* rcg06042009 report available decoders at runtime. */
            SDL_free((void *)chunk_decoders);
            chunk_decoders = NULL;
            num_decoders = 0;
        }
        --audio_opened;
    }
}

/* Pause a particular channel (or all) */
void Mix_Pause(int which)
{
    Uint64 sdl_ticks = SDL_GetTicks();
    if (which == -1) {
        int i;

        for (i = 0; i < num_channels; ++i) {
            if (Mix_Playing(i)) {
                mix_channel[i].paused = sdl_ticks;
            }
        }
    } else if (which < num_channels) {
        if (Mix_Playing(which)) {
            mix_channel[which].paused = sdl_ticks;
        }
    }
}

/* Pause playing of a particular group of channels */
void Mix_PauseGroup(int tag)
{
    int i;

    for (i=0; i<num_channels; ++i) {
        if (mix_channel[i].tag == tag) {
            Mix_Pause(i);
        }
    }
}

/* Resume a paused channel */
void Mix_Resume(int which)
{
    Uint64 sdl_ticks = SDL_GetTicks();

    Mix_LockAudio();
    if (which == -1) {
        int i;

        for (i = 0; i < num_channels; ++i) {
            if (Mix_Playing(i)) {
                if (mix_channel[i].expire > 0) {
                    mix_channel[i].expire += sdl_ticks - mix_channel[i].paused;
                }
                mix_channel[i].paused = 0;
            }
        }
    } else if (which < num_channels) {
        if (Mix_Playing(which)) {
            if (mix_channel[which].expire > 0) {
                mix_channel[which].expire += sdl_ticks - mix_channel[which].paused;
            }
            mix_channel[which].paused = 0;
        }
    }
    Mix_UnlockAudio();
}

/* Resume playing of a particular group of channels */
void Mix_ResumeGroup(int tag)
{
    int i;

    for (i=0; i<num_channels; ++i) {
        if (mix_channel[i].tag == tag) {
            Mix_Resume(i);
        }
    }
}

int Mix_Paused(int which)
{
    if (which < 0) {
        int status = 0;
        int i;
        for (i = 0; i < num_channels; ++i) {
            if (Mix_Playing(i) && mix_channel[i].paused) {
                ++status;
            }
        }
        return status;
    }
    if (which < num_channels) {
        return (Mix_Playing(which) && mix_channel[which].paused != 0);
    }
    return 0;
}

/* Change the group of a channel */
bool Mix_GroupChannel(int which, int tag)
{
    if (which < 0 || which > num_channels) {
        return false;
    }

    Mix_LockAudio();
    mix_channel[which].tag = tag;
    Mix_UnlockAudio();
    return true;
}

/* Assign several consecutive channels to a group */
bool Mix_GroupChannels(int from, int to, int tag)
{
    bool status = true;
    for (; from <= to; ++from) {
        status &= Mix_GroupChannel(from, tag);
    }
    return status;
}

/* Finds the first available channel in a group of channels */
int Mix_GroupAvailable(int tag)
{
    int i;
    for (i = 0; i < num_channels; i++) {
        if ((tag == -1 || tag == mix_channel[i].tag) && !Mix_Playing(i)) {
            return i;
        }
    }
    return -1;
}

int Mix_GroupCount(int tag)
{
    int count = 0;
    int i;

    if (tag == -1) {
        return num_channels;  /* minor optimization; no need to go through the loop. */
    }

    for (i = 0; i < num_channels; i++) {
        if (mix_channel[i].tag == tag) {
            ++count;
        }
    }
    return count;
}

/* Finds the "oldest" sample playing in a group of channels */
int Mix_GroupOldest(int tag)
{
    int chan = -1;
    Uint64 mintime = SDL_GetTicks();
    int i;
    for (i = 0; i < num_channels; i++) {
        if ((mix_channel[i].tag == tag || tag == -1) && Mix_Playing(i)
             && mix_channel[i].start_time <= mintime) {
            mintime = mix_channel[i].start_time;
            chan = i;
        }
    }
    return chan;
}

/* Finds the "most recent" (i.e. last) sample playing in a group of channels */
int Mix_GroupNewer(int tag)
{
    int chan = -1;
    Uint64 maxtime = 0;
    int i;
    for (i = 0; i < num_channels; i++) {
        if ((mix_channel[i].tag == tag || tag == -1) && Mix_Playing(i)
             && mix_channel[i].start_time >= maxtime) {
            maxtime = mix_channel[i].start_time;
            chan = i;
        }
    }
    return chan;
}



/*
 * rcg06122001 The special effects exportable API.
 *  Please see effect_*.c for internally-implemented effects, such
 *  as Mix_SetPanning().
 */

/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
static bool _Mix_register_effect(effect_info **e, Mix_EffectFunc_t f, Mix_EffectDone_t d, void *arg)
{
    effect_info *new_e;

    if (!e) {
        return SDL_SetError("Internal error");
    }

    if (f == NULL) {
        return SDL_SetError("NULL effect callback");
    }

    new_e = SDL_malloc(sizeof(effect_info));
    if (new_e == NULL) {
        return false;
    }

    new_e->callback = f;
    new_e->done_callback = d;
    new_e->udata = arg;
    new_e->next = NULL;

    /* add new effect to end of linked list... */
    if (*e == NULL) {
        *e = new_e;
    } else {
        effect_info *cur = *e;
        while (1) {
            if (cur->next == NULL) {
                cur->next = new_e;
                break;
            }
            cur = cur->next;
        }
    }

    return true;
}


/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
static bool _Mix_remove_effect(int channel, effect_info **e, Mix_EffectFunc_t f)
{
    effect_info *cur;
    effect_info *prev = NULL;
    effect_info *next = NULL;

    if (!e) {
        return SDL_SetError("Internal error");
    }

    for (cur = *e; cur != NULL; cur = cur->next) {
        if (cur->callback == f) {
            next = cur->next;
            if (cur->done_callback != NULL) {
                cur->done_callback(channel, cur->udata);
            }
            SDL_free(cur);

            if (prev == NULL) {   /* removing first item of list? */
                *e = next;
            } else {
                prev->next = next;
            }
            return true;
        }
        prev = cur;
    }

    return SDL_SetError("No such effect registered");
}


/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
static bool _Mix_remove_all_effects(int channel, effect_info **e)
{
    effect_info *cur;
    effect_info *next;

    if (!e) {
        return SDL_SetError("Internal error");
    }

    for (cur = *e; cur != NULL; cur = next) {
        next = cur->next;
        if (cur->done_callback != NULL) {
            cur->done_callback(channel, cur->udata);
        }
        SDL_free(cur);
    }
    *e = NULL;

    return true;
}


/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
bool _Mix_RegisterEffect_locked(int channel, Mix_EffectFunc_t f, Mix_EffectDone_t d, void *arg)
{
    effect_info **e = NULL;

    if (channel == MIX_CHANNEL_POST) {
        e = &posteffects;
    } else {
        if ((channel < 0) || (channel >= num_channels)) {
            return SDL_SetError("Invalid channel number");
        }
        e = &mix_channel[channel].effects;
    }

    return _Mix_register_effect(e, f, d, arg);
}

bool Mix_RegisterEffect(int channel, Mix_EffectFunc_t f,
            Mix_EffectDone_t d, void *arg)
{
    bool retval;
    Mix_LockAudio();
    retval = _Mix_RegisterEffect_locked(channel, f, d, arg);
    Mix_UnlockAudio();
    return retval;
}


/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
bool _Mix_UnregisterEffect_locked(int channel, Mix_EffectFunc_t f)
{
    effect_info **e = NULL;

    if (channel == MIX_CHANNEL_POST) {
        e = &posteffects;
    } else {
        if ((channel < 0) || (channel >= num_channels)) {
            return SDL_SetError("Invalid channel number");
        }
        e = &mix_channel[channel].effects;
    }

    return _Mix_remove_effect(channel, e, f);
}

bool Mix_UnregisterEffect(int channel, Mix_EffectFunc_t f)
{
    bool retval;
    Mix_LockAudio();
    retval = _Mix_UnregisterEffect_locked(channel, f);
    Mix_UnlockAudio();
    return retval;
}

/* MAKE SURE you hold the audio lock (Mix_LockAudio()) before calling this! */
bool _Mix_UnregisterAllEffects_locked(int channel)
{
    effect_info **e = NULL;

    if (channel == MIX_CHANNEL_POST) {
        e = &posteffects;
    } else {
        if ((channel < 0) || (channel >= num_channels)) {
            return SDL_SetError("Invalid channel number");
        }
        e = &mix_channel[channel].effects;
    }

    return _Mix_remove_all_effects(channel, e);
}

bool Mix_UnregisterAllEffects(int channel)
{
    bool retval;
    Mix_LockAudio();
    retval = _Mix_UnregisterAllEffects_locked(channel);
    Mix_UnlockAudio();
    return retval;
}

void Mix_LockAudio(void)
{
    SDL_LockAudioStream(audio_stream);
}

void Mix_UnlockAudio(void)
{
    SDL_UnlockAudioStream(audio_stream);
}

int Mix_MasterVolume(int volume)
{
    int prev_volume = SDL_GetAtomicInt(&master_volume);
    if (volume < 0) {
        return prev_volume;
    }
    if (volume > MIX_MAX_VOLUME) {
        volume = MIX_MAX_VOLUME;
    }
    SDL_SetAtomicInt(&master_volume, volume);
    return prev_volume;
}


int Mix_GetChannelPlayingTime(int channel)
{
    return mix_channel[channel].playing;
}
void Mix_SetChannelPlayingTime(int channel, int playing)
{
    mix_channel[channel].playing = playing;
}
