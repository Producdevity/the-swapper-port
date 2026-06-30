#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef int32_t FMOD_RESULT;
typedef int64_t sf_count_t;

#define FMOD_OK 0
#define SOUND_MAGIC 0x53575053u
#define CHANNEL_MAGIC 0x53575043u
#define MAX_STREAM_CHANNELS 16
#define STREAM_DECODE_FRAMES 1024
#define STREAM_FILE_THRESHOLD_BYTES (1024 * 1024)
#define SFM_READ 0x10

typedef struct FmodSound
{
    uint32_t magic;
    void *chunk;
    int streamed;
    int loop_count;
    float volume;
    char path[1024];
} FmodSound;

typedef struct FmodChannel
{
    uint32_t magic;
    int mixer_channel;
    int stream_slot;
    FmodSound *sound;
} FmodChannel;

typedef struct SF_INFO
{
    sf_count_t frames;
    int samplerate;
    int channels;
    int format;
    int sections;
    int seekable;
} SF_INFO;

typedef struct StreamSlot
{
    int active;
    int paused;
    int loops_remaining;
    float volume;
    void *file;
    SF_INFO info;
} StreamSlot;

typedef void *(*SDL_RWFromFileFn)(const char *, const char *);
typedef void (*SDL_LockAudioFn)(void);
typedef void (*SDL_UnlockAudioFn)(void);
typedef int (*Mix_OpenAudioFn)(int, uint16_t, int, int);
typedef int (*Mix_AllocateChannelsFn)(int);
typedef void *(*Mix_LoadWAV_RWFn)(void *, int);
typedef int (*Mix_PlayChannelTimedFn)(int, void *, int, int);
typedef void (*Mix_SetPostMixFn)(void (*)(void *, uint8_t *, int), void *);
typedef int (*Mix_VolumeFn)(int, int);
typedef int (*Mix_PauseFn)(int);
typedef int (*Mix_ResumeFn)(int);
typedef int (*Mix_HaltChannelFn)(int);
typedef int (*Mix_PlayingFn)(int);
typedef void (*Mix_FreeChunkFn)(void *);
typedef void *(*SfOpenFn)(const char *, int, SF_INFO *);
typedef sf_count_t (*SfReadfShortFn)(void *, short *, sf_count_t);
typedef sf_count_t (*SfSeekFn)(void *, sf_count_t, int);
typedef int (*SfCloseFn)(void *);

static uintptr_t next_handle = 0x10000;
static int audio_loaded;
static int audio_ready;
static int stream_loaded;
static int stream_ready;
static int postmix_installed;

static SDL_RWFromFileFn p_SDL_RWFromFile;
static SDL_LockAudioFn p_SDL_LockAudio;
static SDL_UnlockAudioFn p_SDL_UnlockAudio;
static Mix_OpenAudioFn p_Mix_OpenAudio;
static Mix_AllocateChannelsFn p_Mix_AllocateChannels;
static Mix_LoadWAV_RWFn p_Mix_LoadWAV_RW;
static Mix_PlayChannelTimedFn p_Mix_PlayChannelTimed;
static Mix_SetPostMixFn p_Mix_SetPostMix;
static Mix_VolumeFn p_Mix_Volume;
static Mix_PauseFn p_Mix_Pause;
static Mix_ResumeFn p_Mix_Resume;
static Mix_HaltChannelFn p_Mix_HaltChannel;
static Mix_PlayingFn p_Mix_Playing;
static Mix_FreeChunkFn p_Mix_FreeChunk;
static SfOpenFn p_sf_open;
static SfReadfShortFn p_sf_readf_short;
static SfSeekFn p_sf_seek;
static SfCloseFn p_sf_close;
static StreamSlot stream_slots[MAX_STREAM_CHANNELS];

static void *new_handle(void)
{
    next_handle += 0x10;
    return (void *)next_handle;
}

static void trace_call(const char *name)
{
    const char *enabled = getenv("SWAPPER_TRACE_NATIVE");
    if (enabled == NULL || strcmp(enabled, "1") != 0)
        return;

    FILE *file = fopen("fmodex-trace.log", "a");
    if (file != NULL)
    {
        fprintf(file, "%s\n", name);
        fclose(file);
    }
}

static void trace_value(const char *name, const char *value)
{
    const char *enabled = getenv("SWAPPER_TRACE_NATIVE");
    if (enabled == NULL || strcmp(enabled, "1") != 0)
        return;

    FILE *file = fopen("fmodex-trace.log", "a");
    if (file != NULL)
    {
        fprintf(file, "%s\t%s\n", name, value != NULL ? value : "");
        fclose(file);
    }
}

static void *load_symbol(void *library, const char *name)
{
    return library != NULL ? dlsym(library, name) : NULL;
}

static void lock_audio(void)
{
    if (p_SDL_LockAudio != NULL)
        p_SDL_LockAudio();
}

static void unlock_audio(void)
{
    if (p_SDL_UnlockAudio != NULL)
        p_SDL_UnlockAudio();
}

static int clamp_sample(int value)
{
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return -32768;
    return value;
}

static void close_stream_slot(StreamSlot *slot)
{
    if (slot->file != NULL && p_sf_close != NULL)
        p_sf_close(slot->file);
    memset(slot, 0, sizeof(*slot));
}

static int init_streaming(void)
{
    if (stream_loaded)
        return stream_ready;

    stream_loaded = 1;

    void *sndfile = dlopen("libsndfile.so.1", RTLD_LAZY | RTLD_GLOBAL);
    p_sf_open = (SfOpenFn)load_symbol(sndfile, "sf_open");
    p_sf_readf_short = (SfReadfShortFn)load_symbol(sndfile, "sf_readf_short");
    p_sf_seek = (SfSeekFn)load_symbol(sndfile, "sf_seek");
    p_sf_close = (SfCloseFn)load_symbol(sndfile, "sf_close");

    stream_ready = p_Mix_SetPostMix != NULL &&
        p_sf_open != NULL &&
        p_sf_readf_short != NULL &&
        p_sf_seek != NULL &&
        p_sf_close != NULL;
    return stream_ready;
}

static void stream_postmix(void *userdata, uint8_t *stream, int len)
{
    (void)userdata;

    int16_t *output = (int16_t *)stream;
    int total_frames = len / (int)(sizeof(int16_t) * 2);
    short decode[STREAM_DECODE_FRAMES * 2];

    for (int slot_index = 0; slot_index < MAX_STREAM_CHANNELS; slot_index++)
    {
        StreamSlot *slot = &stream_slots[slot_index];
        if (!slot->active || slot->paused || slot->file == NULL)
            continue;

        int frame_offset = 0;
        while (frame_offset < total_frames && slot->active)
        {
            int wanted = total_frames - frame_offset;
            if (wanted > STREAM_DECODE_FRAMES)
                wanted = STREAM_DECODE_FRAMES;

            sf_count_t read_frames = p_sf_readf_short(slot->file, decode, wanted);
            if (read_frames <= 0)
            {
                if (slot->loops_remaining == -1 || slot->loops_remaining > 0)
                {
                    if (slot->loops_remaining > 0)
                        slot->loops_remaining--;
                    p_sf_seek(slot->file, 0, SEEK_SET);
                    continue;
                }

                close_stream_slot(slot);
                break;
            }

            for (int frame = 0; frame < read_frames; frame++)
            {
                int source = frame * slot->info.channels;
                int target = (frame_offset + frame) * 2;
                int left = decode[source];
                int right = slot->info.channels > 1 ? decode[source + 1] : left;

                left = (int)(left * slot->volume);
                right = (int)(right * slot->volume);

                output[target] = (int16_t)clamp_sample(output[target] + left);
                output[target + 1] = (int16_t)clamp_sample(output[target + 1] + right);
            }

            frame_offset += (int)read_frames;
        }
    }
}

static int init_audio(void)
{
    if (audio_loaded)
        return audio_ready;

    audio_loaded = 1;

    void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    void *mixer = dlopen("libSDL2_mixer-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);

    p_SDL_RWFromFile = (SDL_RWFromFileFn)load_symbol(sdl, "SDL_RWFromFile");
    p_SDL_LockAudio = (SDL_LockAudioFn)load_symbol(sdl, "SDL_LockAudio");
    p_SDL_UnlockAudio = (SDL_UnlockAudioFn)load_symbol(sdl, "SDL_UnlockAudio");
    p_Mix_OpenAudio = (Mix_OpenAudioFn)load_symbol(mixer, "Mix_OpenAudio");
    p_Mix_AllocateChannels = (Mix_AllocateChannelsFn)load_symbol(mixer, "Mix_AllocateChannels");
    p_Mix_LoadWAV_RW = (Mix_LoadWAV_RWFn)load_symbol(mixer, "Mix_LoadWAV_RW");
    p_Mix_PlayChannelTimed = (Mix_PlayChannelTimedFn)load_symbol(mixer, "Mix_PlayChannelTimed");
    p_Mix_SetPostMix = (Mix_SetPostMixFn)load_symbol(mixer, "Mix_SetPostMix");
    p_Mix_Volume = (Mix_VolumeFn)load_symbol(mixer, "Mix_Volume");
    p_Mix_Pause = (Mix_PauseFn)load_symbol(mixer, "Mix_Pause");
    p_Mix_Resume = (Mix_ResumeFn)load_symbol(mixer, "Mix_Resume");
    p_Mix_HaltChannel = (Mix_HaltChannelFn)load_symbol(mixer, "Mix_HaltChannel");
    p_Mix_Playing = (Mix_PlayingFn)load_symbol(mixer, "Mix_Playing");
    p_Mix_FreeChunk = (Mix_FreeChunkFn)load_symbol(mixer, "Mix_FreeChunk");

    if (p_SDL_RWFromFile == NULL || p_Mix_OpenAudio == NULL || p_Mix_LoadWAV_RW == NULL || p_Mix_PlayChannelTimed == NULL)
        return 0;

    if (p_Mix_OpenAudio(44100, 0x8010, 2, 1024) != 0)
        return 0;

    if (p_Mix_AllocateChannels != NULL)
        p_Mix_AllocateChannels(32);

    if (init_streaming() && !postmix_installed)
    {
        p_Mix_SetPostMix(stream_postmix, NULL);
        postmix_installed = 1;
    }

    audio_ready = 1;
    return 1;
}

static FmodSound *as_sound(void *handle)
{
    FmodSound *sound = (FmodSound *)handle;
    return sound != NULL && sound->magic == SOUND_MAGIC ? sound : NULL;
}

static FmodChannel *as_channel(void *handle)
{
    FmodChannel *channel = (FmodChannel *)handle;
    return channel != NULL && channel->magic == CHANNEL_MAGIC ? channel : NULL;
}

static int valid_stream_slot(int slot)
{
    return slot >= 0 && slot < MAX_STREAM_CHANNELS;
}

static int has_audio_extension(const char *path)
{
    return path != NULL && (
        strstr(path, ".ogg") != NULL ||
        strstr(path, ".wav") != NULL ||
        strstr(path, ".OGG") != NULL ||
        strstr(path, ".WAV") != NULL);
}

static int64_t get_file_size(const char *path)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0)
        return 0;
    return st.st_size;
}

static int should_stream_sound(const char *path, int requested_stream)
{
    if (!has_audio_extension(path))
        return 0;
    if (requested_stream)
        return 1;

    return get_file_size(path) >= STREAM_FILE_THRESHOLD_BYTES;
}

static void clear_guid(void *guid)
{
    if (guid != NULL)
        memset(guid, 0, 16);
}

static void clear_string(char *buffer, int32_t length)
{
    if (buffer != NULL && length > 0)
        buffer[0] = '\0';
}

static void clear_wide_string(char *buffer, int32_t length)
{
    if (buffer != NULL && length > 0)
        ((uint16_t *)buffer)[0] = 0;
}

static FmodSound *create_sound(const char *path, int requested_stream)
{
    FmodSound *sound = calloc(1, sizeof(*sound));
    if (sound == NULL)
        return NULL;

    sound->magic = SOUND_MAGIC;
    sound->volume = 1.0f;
    sound->loop_count = 0;

    if (has_audio_extension(path))
    {
        strncpy(sound->path, path, sizeof(sound->path) - 1);
        sound->streamed = should_stream_sound(sound->path, requested_stream);
        trace_value("FMOD_LoadPath", sound->path);
    }

    return sound;
}

static int load_sound(FmodSound *sound)
{
    if (sound == NULL)
        return 0;
    if (sound->chunk != NULL)
        return 1;
    if (sound->path[0] == '\0' || !init_audio())
        return 0;

    void *rw = p_SDL_RWFromFile(sound->path, "rb");
    if (rw == NULL)
        return 0;

    sound->chunk = p_Mix_LoadWAV_RW(rw, 1);
    return sound->chunk != NULL;
}

static FmodChannel *play_chunk_sound(FmodSound *sound, int paused)
{
    FmodChannel *channel = calloc(1, sizeof(*channel));
    if (channel == NULL)
        return NULL;

    channel->magic = CHANNEL_MAGIC;
    channel->mixer_channel = -1;
    channel->stream_slot = -1;
    channel->sound = sound;

    if (load_sound(sound))
    {
        channel->mixer_channel = p_Mix_PlayChannelTimed(-1, sound->chunk, sound->loop_count, -1);
        if (channel->mixer_channel >= 0 && p_Mix_Volume != NULL)
        {
            int volume = (int)(sound->volume * 128.0f);
            if (volume < 0)
                volume = 0;
            if (volume > 128)
                volume = 128;
            p_Mix_Volume(channel->mixer_channel, volume);
        }
        if (paused && channel->mixer_channel >= 0 && p_Mix_Pause != NULL)
            p_Mix_Pause(channel->mixer_channel);
    }

    return channel;
}

static FmodChannel *play_stream_sound(FmodSound *sound, int paused)
{
    if (sound == NULL || sound->path[0] == '\0' || !init_audio() || !init_streaming())
        return NULL;

    SF_INFO info;
    memset(&info, 0, sizeof(info));
    void *file = p_sf_open(sound->path, SFM_READ, &info);
    if (file == NULL)
        return NULL;

    if (info.samplerate != 44100 || info.channels < 1 || info.channels > 2)
    {
        p_sf_close(file);
        return NULL;
    }

    FmodChannel *channel = calloc(1, sizeof(*channel));
    if (channel == NULL)
    {
        p_sf_close(file);
        return NULL;
    }

    channel->magic = CHANNEL_MAGIC;
    channel->mixer_channel = -1;
    channel->stream_slot = -1;
    channel->sound = sound;

    lock_audio();
    for (int i = 0; i < MAX_STREAM_CHANNELS; i++)
    {
        if (!stream_slots[i].active)
        {
            stream_slots[i].active = 1;
            stream_slots[i].paused = paused ? 1 : 0;
            stream_slots[i].loops_remaining = sound->loop_count;
            stream_slots[i].volume = sound->volume;
            stream_slots[i].file = file;
            stream_slots[i].info = info;
            channel->stream_slot = i;
            break;
        }
    }
    unlock_audio();

    if (channel->stream_slot < 0)
    {
        p_sf_close(file);
        free(channel);
        return NULL;
    }

    return channel;
}

static FmodChannel *play_sound(FmodSound *sound, int paused)
{
    if (sound != NULL && sound->streamed)
    {
        FmodChannel *stream = play_stream_sound(sound, paused);
        if (stream != NULL)
            return stream;
    }

    return play_chunk_sound(sound, paused);
}

FMOD_RESULT FMOD_System_Create(void **system)
{
    trace_call("FMOD_System_Create");
    if (system != NULL)
        *system = new_handle();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Init(void *system, int32_t max_channels, int32_t flags, void *extra_driver_data)
{
    (void)system;
    (void)max_channels;
    (void)flags;
    (void)extra_driver_data;
    trace_call("FMOD_System_Init");
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateSound(void *system, const char *path, int32_t mode, void *info, void **sound)
{
    (void)system;
    (void)mode;
    (void)info;
    trace_call("FMOD_System_CreateSound");
    FmodSound *created = create_sound(path, 0);
    if (sound != NULL)
        *sound = created != NULL ? created : new_handle();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateStream(void *system, const char *path, int32_t mode, void *info, void **sound)
{
    (void)system;
    (void)mode;
    (void)info;
    trace_call("FMOD_System_CreateStream");
    FmodSound *created = create_sound(path, 1);
    if (sound != NULL)
        *sound = created != NULL ? created : new_handle();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_PlaySound(void *system, int32_t channel_id, void *sound_handle, int32_t paused, void **channel)
{
    (void)system;
    (void)channel_id;
    trace_call("FMOD_System_PlaySound");
    FmodChannel *created = play_sound(as_sound(sound_handle), paused);
    if (channel != NULL)
        *channel = created != NULL ? created : new_handle();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateChannelGroup(void *system, const char *name, void **group)
{
    (void)system;
    (void)name;
    trace_call("FMOD_System_CreateChannelGroup");
    if (group != NULL)
        *group = new_handle();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateDSP(void *system, void *description, void **dsp)
{
    (void)system;
    (void)description;
    trace_call("FMOD_System_CreateDSP");
    if (dsp != NULL)
        *dsp = NULL;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateDSPByPlugin(void *system, uint32_t plugin_handle, void **dsp)
{
    (void)system;
    (void)plugin_handle;
    trace_call("FMOD_System_CreateDSPByPlugin");
    if (dsp != NULL)
        *dsp = NULL;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_CreateDSPByType(void *system, int32_t dsp_type, void **dsp)
{
    (void)system;
    (void)dsp_type;
    trace_call("FMOD_System_CreateDSPByType");
    if (dsp != NULL)
        *dsp = NULL;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_SetDefaults(void *sound_handle, float frequency, float volume, float pan, int32_t priority)
{
    (void)frequency;
    (void)pan;
    (void)priority;
    trace_call("FMOD_Sound_SetDefaults");
    FmodSound *sound = as_sound(sound_handle);
    if (sound != NULL)
        sound->volume = volume;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_SetLoopCount(void *sound_handle, int32_t count)
{
    trace_call("FMOD_Sound_SetLoopCount");
    FmodSound *sound = as_sound(sound_handle);
    if (sound != NULL)
        sound->loop_count = count;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Sound_Release(void *sound_handle)
{
    trace_call("FMOD_Sound_Release");
    FmodSound *sound = as_sound(sound_handle);
    if (sound == NULL)
        return FMOD_OK;

    if (sound->chunk != NULL && p_Mix_FreeChunk != NULL)
        p_Mix_FreeChunk(sound->chunk);
    sound->magic = 0;
    free(sound);
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetVolume(void *channel_handle, float volume)
{
    trace_call("FMOD_Channel_SetVolume");
    FmodChannel *channel = as_channel(channel_handle);
    if (channel != NULL && valid_stream_slot(channel->stream_slot))
    {
        lock_audio();
        if (stream_slots[channel->stream_slot].active)
            stream_slots[channel->stream_slot].volume = volume;
        unlock_audio();
        return FMOD_OK;
    }

    if (channel != NULL && channel->mixer_channel >= 0 && p_Mix_Volume != NULL)
    {
        int mixer_volume = (int)(volume * 128.0f);
        if (mixer_volume < 0)
            mixer_volume = 0;
        if (mixer_volume > 128)
            mixer_volume = 128;
        p_Mix_Volume(channel->mixer_channel, mixer_volume);
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_SetPaused(void *channel_handle, int32_t paused)
{
    trace_call("FMOD_Channel_SetPaused");
    FmodChannel *channel = as_channel(channel_handle);
    if (channel != NULL && valid_stream_slot(channel->stream_slot))
    {
        lock_audio();
        if (stream_slots[channel->stream_slot].active)
            stream_slots[channel->stream_slot].paused = paused ? 1 : 0;
        unlock_audio();
        return FMOD_OK;
    }

    if (channel != NULL && channel->mixer_channel >= 0)
    {
        if (paused && p_Mix_Pause != NULL)
            p_Mix_Pause(channel->mixer_channel);
        else if (!paused && p_Mix_Resume != NULL)
            p_Mix_Resume(channel->mixer_channel);
    }
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_IsPlaying(void *channel_handle, int32_t *playing)
{
    trace_call("FMOD_Channel_IsPlaying");
    FmodChannel *channel = as_channel(channel_handle);
    if (channel != NULL && valid_stream_slot(channel->stream_slot))
    {
        if (playing != NULL)
            *playing = stream_slots[channel->stream_slot].active ? 1 : 0;
        return FMOD_OK;
    }

    if (playing != NULL)
        *playing = channel != NULL && channel->mixer_channel >= 0 && p_Mix_Playing != NULL
            ? p_Mix_Playing(channel->mixer_channel)
            : 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_Channel_Stop(void *channel_handle)
{
    trace_call("FMOD_Channel_Stop");
    FmodChannel *channel = as_channel(channel_handle);
    if (channel != NULL && valid_stream_slot(channel->stream_slot))
    {
        lock_audio();
        if (stream_slots[channel->stream_slot].active)
            close_stream_slot(&stream_slots[channel->stream_slot]);
        unlock_audio();
        channel->stream_slot = -1;
        return FMOD_OK;
    }

    if (channel != NULL && channel->mixer_channel >= 0 && p_Mix_HaltChannel != NULL)
        p_Mix_HaltChannel(channel->mixer_channel);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetVersion(void *system, uint32_t *version)
{
    (void)system;
    trace_call("FMOD_System_GetVersion");
    if (version != NULL)
        *version = 0x00044429u;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDSPBufferSize(void *system, uint32_t *buffer_length, int32_t *num_buffers)
{
    (void)system;
    trace_call("FMOD_System_GetDSPBufferSize");
    if (buffer_length != NULL)
        *buffer_length = 0;
    if (num_buffers != NULL)
        *num_buffers = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDSPClock(void *system, uint32_t *hi, uint32_t *lo)
{
    (void)system;
    trace_call("FMOD_System_GetDSPClock");
    if (hi != NULL)
        *hi = 0;
    if (lo != NULL)
        *lo = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDSPHead(void *system, void **dsp)
{
    (void)system;
    trace_call("FMOD_System_GetDSPHead");
    if (dsp != NULL)
        *dsp = new_handle();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDriver(void *system, int32_t *driver)
{
    (void)system;
    trace_call("FMOD_System_GetDriver");
    if (driver != NULL)
        *driver = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDriverCaps(
    void *system, int32_t id, int32_t *caps, int32_t *min_frequency, int32_t *max_frequency)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetDriverCaps");
    if (caps != NULL)
        *caps = 0;
    if (min_frequency != NULL)
        *min_frequency = 0;
    if (max_frequency != NULL)
        *max_frequency = 2;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDriverInfo(
    void *system, int32_t id, char *name, int32_t name_length, void *guid)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetDriverInfo");
    clear_string(name, name_length);
    clear_guid(guid);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetDriverInfoW(
    void *system, int32_t id, char *name, int32_t name_length, void *guid)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetDriverInfoW");
    clear_wide_string(name, name_length);
    clear_guid(guid);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetFileBufferSize(void *system, int32_t *file_buffer_size)
{
    (void)system;
    trace_call("FMOD_System_GetFileBufferSize");
    if (file_buffer_size != NULL)
        *file_buffer_size = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetGeometrySettings(void *system, float *max_world_size)
{
    (void)system;
    trace_call("FMOD_System_GetGeometrySettings");
    if (max_world_size != NULL)
        *max_world_size = 0.0f;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetNumDrivers(void *system, int32_t *num_drivers)
{
    (void)system;
    trace_call("FMOD_System_GetNumDrivers");
    if (num_drivers != NULL)
        *num_drivers = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetRecordDriverCaps(
    void *system, int32_t id, int32_t *caps, int32_t *min_frequency, int32_t *max_frequency)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetRecordDriverCaps");
    if (caps != NULL)
        *caps = 0;
    if (min_frequency != NULL)
        *min_frequency = 0;
    if (max_frequency != NULL)
        *max_frequency = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetRecordDriverInfo(
    void *system, int32_t id, char *name, int32_t name_length, void *guid)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetRecordDriverInfo");
    clear_string(name, name_length);
    clear_guid(guid);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetRecordDriverInfoW(
    void *system, int32_t id, char *name, int32_t name_length, void *guid)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetRecordDriverInfoW");
    clear_wide_string(name, name_length);
    clear_guid(guid);
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetRecordNumDrivers(void *system, int32_t *num_drivers)
{
    (void)system;
    trace_call("FMOD_System_GetRecordNumDrivers");
    if (num_drivers != NULL)
        *num_drivers = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetRecordPosition(void *system, int32_t id, uint32_t *position)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_GetRecordPosition");
    if (position != NULL)
        *position = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetSoundRAM(
    void *system, int32_t *current_alloced, int32_t *max_alloced, int32_t *total)
{
    (void)system;
    trace_call("FMOD_System_GetSoundRAM");
    if (current_alloced != NULL)
        *current_alloced = 0;
    if (max_alloced != NULL)
        *max_alloced = 0;
    if (total != NULL)
        *total = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetSoftwareFormat(
    void *system,
    int32_t *sample_rate,
    int32_t *format,
    int32_t *output_channels,
    int32_t *max_input_channels,
    int32_t *resample_method,
    int32_t *bits)
{
    (void)system;
    trace_call("FMOD_System_GetSoftwareFormat");
    if (sample_rate != NULL)
        *sample_rate = 44100;
    if (format != NULL)
        *format = 2;
    if (output_channels != NULL)
        *output_channels = 2;
    if (max_input_channels != NULL)
        *max_input_channels = 2;
    if (resample_method != NULL)
        *resample_method = 0;
    if (bits != NULL)
        *bits = 16;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_GetSpeakerMode(void *system, int32_t *speaker_mode)
{
    (void)system;
    trace_call("FMOD_System_GetSpeakerMode");
    if (speaker_mode != NULL)
        *speaker_mode = 2;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_IsRecording(void *system, int32_t id, int32_t *recording)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_IsRecording");
    if (recording != NULL)
        *recording = 0;
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_RecordStart(void *system, int32_t id, void *sound, int32_t loop)
{
    (void)system;
    (void)id;
    (void)sound;
    (void)loop;
    trace_call("FMOD_System_RecordStart");
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_RecordStop(void *system, int32_t id)
{
    (void)system;
    (void)id;
    trace_call("FMOD_System_RecordStop");
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Release(void *system)
{
    (void)system;
    trace_call("FMOD_System_Release");
    lock_audio();
    for (int i = 0; i < MAX_STREAM_CHANNELS; i++)
    {
        if (stream_slots[i].active)
            close_stream_slot(&stream_slots[i]);
    }
    unlock_audio();
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_Set3DRolloffCallback(void *system, void *callback)
{
    (void)system;
    (void)callback;
    trace_call("FMOD_System_Set3DRolloffCallback");
    return FMOD_OK;
}

FMOD_RESULT FMOD_System_SetFileBufferSize(void *system, int32_t file_buffer_size)
{
    (void)system;
    (void)file_buffer_size;
    trace_call("FMOD_System_SetFileBufferSize");
    return FMOD_OK;
}

#define FMOD_STUB(name) FMOD_RESULT name() { trace_call(#name); return FMOD_OK; }

FMOD_STUB(FMOD_ChannelGroup_AddDSP)
FMOD_STUB(FMOD_ChannelGroup_AddGroup)
FMOD_STUB(FMOD_ChannelGroup_Get3DOcclusion)
FMOD_STUB(FMOD_ChannelGroup_GetChannel)
FMOD_STUB(FMOD_ChannelGroup_GetDSPHead)
FMOD_STUB(FMOD_ChannelGroup_GetGroup)
FMOD_STUB(FMOD_ChannelGroup_GetMemoryInfo)
FMOD_STUB(FMOD_ChannelGroup_GetMute)
FMOD_STUB(FMOD_ChannelGroup_GetName)
FMOD_STUB(FMOD_ChannelGroup_GetNumChannels)
FMOD_STUB(FMOD_ChannelGroup_GetNumGroups)
FMOD_STUB(FMOD_ChannelGroup_GetParentGroup)
FMOD_STUB(FMOD_ChannelGroup_GetPaused)
FMOD_STUB(FMOD_ChannelGroup_GetPitch)
FMOD_STUB(FMOD_ChannelGroup_GetSpectrum)
FMOD_STUB(FMOD_ChannelGroup_GetSystemObject)
FMOD_STUB(FMOD_ChannelGroup_GetUserData)
FMOD_STUB(FMOD_ChannelGroup_GetVolume)
FMOD_STUB(FMOD_ChannelGroup_GetWaveData)
FMOD_STUB(FMOD_ChannelGroup_Override3DAttributes)
FMOD_STUB(FMOD_ChannelGroup_OverrideFrequency)
FMOD_STUB(FMOD_ChannelGroup_OverrideMute)
FMOD_STUB(FMOD_ChannelGroup_OverridePan)
FMOD_STUB(FMOD_ChannelGroup_OverridePaused)
FMOD_STUB(FMOD_ChannelGroup_OverrideReverbProperties)
FMOD_STUB(FMOD_ChannelGroup_OverrideSpeakerMix)
FMOD_STUB(FMOD_ChannelGroup_OverrideVolume)
FMOD_STUB(FMOD_ChannelGroup_Release)
FMOD_STUB(FMOD_ChannelGroup_Set3DOcclusion)
FMOD_STUB(FMOD_ChannelGroup_SetMute)
FMOD_STUB(FMOD_ChannelGroup_SetPaused)
FMOD_STUB(FMOD_ChannelGroup_SetPitch)
FMOD_STUB(FMOD_ChannelGroup_SetUserData)
FMOD_STUB(FMOD_ChannelGroup_SetVolume)
FMOD_STUB(FMOD_ChannelGroup_Stop)
FMOD_STUB(FMOD_Channel_AddDSP)
FMOD_STUB(FMOD_Channel_Get3DAttributes)
FMOD_STUB(FMOD_Channel_Get3DConeOrientation)
FMOD_STUB(FMOD_Channel_Get3DConeSettings)
FMOD_STUB(FMOD_Channel_Get3DCustomRolloff)
FMOD_STUB(FMOD_Channel_Get3DDopplerLevel)
FMOD_STUB(FMOD_Channel_Get3DMinMaxDistance)
FMOD_STUB(FMOD_Channel_Get3DOcclusion)
FMOD_STUB(FMOD_Channel_Get3DPanLevel)
FMOD_STUB(FMOD_Channel_Get3DSpread)
FMOD_STUB(FMOD_Channel_GetAudibility)
FMOD_STUB(FMOD_Channel_GetChannelGroup)
FMOD_STUB(FMOD_Channel_GetCurrentSound)
FMOD_STUB(FMOD_Channel_GetDSPHead)
FMOD_STUB(FMOD_Channel_GetDelay)
FMOD_STUB(FMOD_Channel_GetFrequency)
FMOD_STUB(FMOD_Channel_GetIndex)
FMOD_STUB(FMOD_Channel_GetInputChannelMix)
FMOD_STUB(FMOD_Channel_GetLoopCount)
FMOD_STUB(FMOD_Channel_GetLoopPoints)
FMOD_STUB(FMOD_Channel_GetLowPassGain)
FMOD_STUB(FMOD_Channel_GetMemoryInfo)
FMOD_STUB(FMOD_Channel_GetMode)
FMOD_STUB(FMOD_Channel_GetMute)
FMOD_STUB(FMOD_Channel_GetPan)
FMOD_STUB(FMOD_Channel_GetPaused)
FMOD_STUB(FMOD_Channel_GetPosition)
FMOD_STUB(FMOD_Channel_GetPriority)
FMOD_STUB(FMOD_Channel_GetReverbProperties)
FMOD_STUB(FMOD_Channel_GetSpeakerLevels)
FMOD_STUB(FMOD_Channel_GetSpeakerMix)
FMOD_STUB(FMOD_Channel_GetSpectrum)
FMOD_STUB(FMOD_Channel_GetSystemObject)
FMOD_STUB(FMOD_Channel_GetUserData)
FMOD_STUB(FMOD_Channel_GetVolume)
FMOD_STUB(FMOD_Channel_GetWaveData)
FMOD_STUB(FMOD_Channel_IsVirtual)
FMOD_STUB(FMOD_Channel_Set3DAttributes)
FMOD_STUB(FMOD_Channel_Set3DConeOrientation)
FMOD_STUB(FMOD_Channel_Set3DConeSettings)
FMOD_STUB(FMOD_Channel_Set3DCustomRolloff)
FMOD_STUB(FMOD_Channel_Set3DDopplerLevel)
FMOD_STUB(FMOD_Channel_Set3DMinMaxDistance)
FMOD_STUB(FMOD_Channel_Set3DOcclusion)
FMOD_STUB(FMOD_Channel_Set3DPanLevel)
FMOD_STUB(FMOD_Channel_Set3DSpread)
FMOD_STUB(FMOD_Channel_SetCallback)
FMOD_STUB(FMOD_Channel_SetChannelGroup)
FMOD_STUB(FMOD_Channel_SetDelay)
FMOD_STUB(FMOD_Channel_SetFrequency)
FMOD_STUB(FMOD_Channel_SetInputChannelMix)
FMOD_STUB(FMOD_Channel_SetLoopCount)
FMOD_STUB(FMOD_Channel_SetLoopPoints)
FMOD_STUB(FMOD_Channel_SetLowPassGain)
FMOD_STUB(FMOD_Channel_SetMode)
FMOD_STUB(FMOD_Channel_SetMute)
FMOD_STUB(FMOD_Channel_SetPan)
FMOD_STUB(FMOD_Channel_SetPosition)
FMOD_STUB(FMOD_Channel_SetPriority)
FMOD_STUB(FMOD_Channel_SetReverbProperties)
FMOD_STUB(FMOD_Channel_SetSpeakerLevels)
FMOD_STUB(FMOD_Channel_SetSpeakerMix)
FMOD_STUB(FMOD_Channel_SetUserData)
FMOD_STUB(FMOD_DSPConnection_GetInput)
FMOD_STUB(FMOD_DSPConnection_GetLevels)
FMOD_STUB(FMOD_DSPConnection_GetMemoryInfo)
FMOD_STUB(FMOD_DSPConnection_GetMix)
FMOD_STUB(FMOD_DSPConnection_GetOutput)
FMOD_STUB(FMOD_DSPConnection_GetUserData)
FMOD_STUB(FMOD_DSPConnection_SetLevels)
FMOD_STUB(FMOD_DSPConnection_SetMix)
FMOD_STUB(FMOD_DSPConnection_SetUserData)
FMOD_STUB(FMOD_DSP_AddInput)
FMOD_STUB(FMOD_DSP_DisconnectAll)
FMOD_STUB(FMOD_DSP_DisconnectFrom)
FMOD_STUB(FMOD_DSP_GetActive)
FMOD_STUB(FMOD_DSP_GetBypass)
FMOD_STUB(FMOD_DSP_GetDefaults)
FMOD_STUB(FMOD_DSP_GetInfo)
FMOD_STUB(FMOD_DSP_GetInput)
FMOD_STUB(FMOD_DSP_GetMemoryInfo)
FMOD_STUB(FMOD_DSP_GetNumInputs)
FMOD_STUB(FMOD_DSP_GetNumOutputs)
FMOD_STUB(FMOD_DSP_GetNumParameters)
FMOD_STUB(FMOD_DSP_GetOutput)
FMOD_STUB(FMOD_DSP_GetParameter)
FMOD_STUB(FMOD_DSP_GetParameterInfo)
FMOD_STUB(FMOD_DSP_GetSpeakerActive)
FMOD_STUB(FMOD_DSP_GetSystemObject)
FMOD_STUB(FMOD_DSP_GetType)
FMOD_STUB(FMOD_DSP_GetUserData)
FMOD_STUB(FMOD_DSP_Release)
FMOD_STUB(FMOD_DSP_Remove)
FMOD_STUB(FMOD_DSP_Reset)
FMOD_STUB(FMOD_DSP_SetActive)
FMOD_STUB(FMOD_DSP_SetBypass)
FMOD_STUB(FMOD_DSP_SetDefaults)
FMOD_STUB(FMOD_DSP_SetParameter)
FMOD_STUB(FMOD_DSP_SetSpeakerActive)
FMOD_STUB(FMOD_DSP_SetUserData)
FMOD_STUB(FMOD_DSP_ShowConfigDialog)
FMOD_STUB(FMOD_Debug_SetLevel)
FMOD_STUB(FMOD_Geometry_AddPolygon)
FMOD_STUB(FMOD_Geometry_Flush)
FMOD_STUB(FMOD_Geometry_GetActive)
FMOD_STUB(FMOD_Geometry_GetMaxPolygons)
FMOD_STUB(FMOD_Geometry_GetMemoryInfo)
FMOD_STUB(FMOD_Geometry_GetNumPolygons)
FMOD_STUB(FMOD_Geometry_GetPolygonAttributes)
FMOD_STUB(FMOD_Geometry_GetPolygonNumVertices)
FMOD_STUB(FMOD_Geometry_GetPolygonVertex)
FMOD_STUB(FMOD_Geometry_GetPosition)
FMOD_STUB(FMOD_Geometry_GetRotation)
FMOD_STUB(FMOD_Geometry_GetScale)
FMOD_STUB(FMOD_Geometry_GetUserData)
FMOD_STUB(FMOD_Geometry_Release)
FMOD_STUB(FMOD_Geometry_Save)
FMOD_STUB(FMOD_Geometry_SetActive)
FMOD_STUB(FMOD_Geometry_SetPolygonAttributes)
FMOD_STUB(FMOD_Geometry_SetPolygonVertex)
FMOD_STUB(FMOD_Geometry_SetPosition)
FMOD_STUB(FMOD_Geometry_SetRotation)
FMOD_STUB(FMOD_Geometry_SetScale)
FMOD_STUB(FMOD_Geometry_SetUserData)
FMOD_STUB(FMOD_Memory_GetStats)
FMOD_STUB(FMOD_Reverb_Get3DAttributes)
FMOD_STUB(FMOD_Reverb_GetActive)
FMOD_STUB(FMOD_Reverb_GetMemoryInfo)
FMOD_STUB(FMOD_Reverb_GetProperties)
FMOD_STUB(FMOD_Reverb_GetUserData)
FMOD_STUB(FMOD_Reverb_Release)
FMOD_STUB(FMOD_Reverb_Set3DAttributes)
FMOD_STUB(FMOD_Reverb_SetActive)
FMOD_STUB(FMOD_Reverb_SetProperties)
FMOD_STUB(FMOD_Reverb_SetUserData)
FMOD_STUB(FMOD_SoundGroup_GetMaxAudible)
FMOD_STUB(FMOD_SoundGroup_GetMaxAudibleBehavior)
FMOD_STUB(FMOD_SoundGroup_GetMemoryInfo)
FMOD_STUB(FMOD_SoundGroup_GetMuteFadeSpeed)
FMOD_STUB(FMOD_SoundGroup_GetName)
FMOD_STUB(FMOD_SoundGroup_GetNumPlaying)
FMOD_STUB(FMOD_SoundGroup_GetNumSounds)
FMOD_STUB(FMOD_SoundGroup_GetSound)
FMOD_STUB(FMOD_SoundGroup_GetSystemObject)
FMOD_STUB(FMOD_SoundGroup_GetUserData)
FMOD_STUB(FMOD_SoundGroup_GetVolume)
FMOD_STUB(FMOD_SoundGroup_Release)
FMOD_STUB(FMOD_SoundGroup_SetMaxAudible)
FMOD_STUB(FMOD_SoundGroup_SetMaxAudibleBehavior)
FMOD_STUB(FMOD_SoundGroup_SetMuteFadeSpeed)
FMOD_STUB(FMOD_SoundGroup_SetUserData)
FMOD_STUB(FMOD_SoundGroup_SetVolume)
FMOD_STUB(FMOD_SoundGroup_Stop)
FMOD_STUB(FMOD_Sound_AddSyncPoint)
FMOD_STUB(FMOD_Sound_DeleteSyncPoint)
FMOD_STUB(FMOD_Sound_Get3DConeSettings)
FMOD_STUB(FMOD_Sound_Get3DCustomRolloff)
FMOD_STUB(FMOD_Sound_Get3DMinMaxDistance)
FMOD_STUB(FMOD_Sound_GetDefaults)
FMOD_STUB(FMOD_Sound_GetFormat)
FMOD_STUB(FMOD_Sound_GetLength)
FMOD_STUB(FMOD_Sound_GetLoopCount)
FMOD_STUB(FMOD_Sound_GetLoopPoints)
FMOD_STUB(FMOD_Sound_GetMemoryInfo)
FMOD_STUB(FMOD_Sound_GetMode)
FMOD_STUB(FMOD_Sound_GetMusicChannelVolume)
FMOD_STUB(FMOD_Sound_GetMusicNumChannels)
FMOD_STUB(FMOD_Sound_GetMusicSpeed)
FMOD_STUB(FMOD_Sound_GetName)
FMOD_STUB(FMOD_Sound_GetNumSubSounds)
FMOD_STUB(FMOD_Sound_GetNumSyncPoints)
FMOD_STUB(FMOD_Sound_GetNumTags)
FMOD_STUB(FMOD_Sound_GetOpenState)
FMOD_STUB(FMOD_Sound_GetSoundGroup)
FMOD_STUB(FMOD_Sound_GetSubSound)
FMOD_STUB(FMOD_Sound_GetSyncPoint)
FMOD_STUB(FMOD_Sound_GetSyncPointInfo)
FMOD_STUB(FMOD_Sound_GetSystemObject)
FMOD_STUB(FMOD_Sound_GetTag)
FMOD_STUB(FMOD_Sound_GetUserData)
FMOD_STUB(FMOD_Sound_GetVariations)
FMOD_STUB(FMOD_Sound_Lock)
FMOD_STUB(FMOD_Sound_ReadData)
FMOD_STUB(FMOD_Sound_SeekData)
FMOD_STUB(FMOD_Sound_Set3DConeSettings)
FMOD_STUB(FMOD_Sound_Set3DCustomRolloff)
FMOD_STUB(FMOD_Sound_Set3DMinMaxDistance)
FMOD_STUB(FMOD_Sound_SetLoopPoints)
FMOD_STUB(FMOD_Sound_SetMode)
FMOD_STUB(FMOD_Sound_SetMusicChannelVolume)
FMOD_STUB(FMOD_Sound_SetMusicSpeed)
FMOD_STUB(FMOD_Sound_SetSoundGroup)
FMOD_STUB(FMOD_Sound_SetSubSound)
FMOD_STUB(FMOD_Sound_SetSubSoundSentence)
FMOD_STUB(FMOD_Sound_SetUserData)
FMOD_STUB(FMOD_Sound_SetVariations)
FMOD_STUB(FMOD_Sound_Unlock)
FMOD_STUB(FMOD_System_AddDSP)
FMOD_STUB(FMOD_System_AttachFileSystem)
FMOD_STUB(FMOD_System_Close)
FMOD_STUB(FMOD_System_CreateCodec)
FMOD_STUB(FMOD_System_CreateGeometry)
FMOD_STUB(FMOD_System_CreateReverb)
FMOD_STUB(FMOD_System_CreateSoundGroup)
FMOD_STUB(FMOD_System_Get3DListenerAttributes)
FMOD_STUB(FMOD_System_Get3DNumListeners)
FMOD_STUB(FMOD_System_Get3DSettings)
FMOD_STUB(FMOD_System_Get3DSpeakerPosition)
FMOD_STUB(FMOD_System_GetAdvancedSettings)
FMOD_STUB(FMOD_System_GetCDROMDriveName)
FMOD_STUB(FMOD_System_GetCPUUsage)
FMOD_STUB(FMOD_System_GetChannel)
FMOD_STUB(FMOD_System_GetChannelsPlaying)
FMOD_STUB(FMOD_System_GetGeometryOcclusion)
FMOD_STUB(FMOD_System_GetHardwareChannels)
FMOD_STUB(FMOD_System_GetMasterChannelGroup)
FMOD_STUB(FMOD_System_GetMasterSoundGroup)
FMOD_STUB(FMOD_System_GetMemoryInfo)
FMOD_STUB(FMOD_System_GetNetworkProxy)
FMOD_STUB(FMOD_System_GetNetworkTimeout)
FMOD_STUB(FMOD_System_GetNumCDROMDrives)
FMOD_STUB(FMOD_System_GetNumPlugins)
FMOD_STUB(FMOD_System_GetOutput)
FMOD_STUB(FMOD_System_GetOutputByPlugin)
FMOD_STUB(FMOD_System_GetOutputHandle)
FMOD_STUB(FMOD_System_GetPluginHandle)
FMOD_STUB(FMOD_System_GetPluginInfo)
FMOD_STUB(FMOD_System_GetReverbAmbientProperties)
FMOD_STUB(FMOD_System_GetReverbProperties)
FMOD_STUB(FMOD_System_GetSoftwareChannels)
FMOD_STUB(FMOD_System_GetSpectrum)
FMOD_STUB(FMOD_System_GetStreamBufferSize)
FMOD_STUB(FMOD_System_GetUserData)
FMOD_STUB(FMOD_System_GetWaveData)
FMOD_STUB(FMOD_System_LoadGeometry)
FMOD_STUB(FMOD_System_LoadPlugin)
FMOD_STUB(FMOD_System_LockDSP)
FMOD_STUB(FMOD_System_PlayDSP)
FMOD_STUB(FMOD_System_RegisterCodec)
FMOD_STUB(FMOD_System_RegisterDSP)
FMOD_STUB(FMOD_System_RegisterOutput)
FMOD_STUB(FMOD_System_Set3DListenerAttributes)
FMOD_STUB(FMOD_System_Set3DNumListeners)
FMOD_STUB(FMOD_System_Set3DSettings)
FMOD_STUB(FMOD_System_Set3DSpeakerPosition)
FMOD_STUB(FMOD_System_SetAdvancedSettings)
FMOD_STUB(FMOD_System_SetCallback)
FMOD_STUB(FMOD_System_SetDSPBufferSize)
FMOD_STUB(FMOD_System_SetDriver)
FMOD_STUB(FMOD_System_SetFileSystem)
FMOD_STUB(FMOD_System_SetGeometrySettings)
FMOD_STUB(FMOD_System_SetHardwareChannels)
FMOD_STUB(FMOD_System_SetNetworkProxy)
FMOD_STUB(FMOD_System_SetNetworkTimeout)
FMOD_STUB(FMOD_System_SetOutput)
FMOD_STUB(FMOD_System_SetOutputByPlugin)
FMOD_STUB(FMOD_System_SetPluginPath)
FMOD_STUB(FMOD_System_SetRecordDriver)
FMOD_STUB(FMOD_System_SetReverbAmbientProperties)
FMOD_STUB(FMOD_System_SetReverbProperties)
FMOD_STUB(FMOD_System_SetSoftwareChannels)
FMOD_STUB(FMOD_System_SetSoftwareFormat)
FMOD_STUB(FMOD_System_SetSpeakerMode)
FMOD_STUB(FMOD_System_SetStreamBufferSize)
FMOD_STUB(FMOD_System_SetUserData)
FMOD_STUB(FMOD_System_UnloadPlugin)
FMOD_STUB(FMOD_System_UnlockDSP)
FMOD_STUB(FMOD_System_Update)
FMOD_STUB(FMOD_System_UpdateFinished)
