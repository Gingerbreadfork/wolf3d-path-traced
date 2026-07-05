// platform_audio.cpp
//
// SDL2-backed implementation of the tiny Mix_* audio bridge declared in
// mixer_compat.h. Replaces SDL_mixer for the original id_sd.cpp sound manager.
//
// Output is 16-bit signed stereo. Each audio callback:
//   1. fills the buffer with OPL music via the registered music hook, then
//   2. mixes any playing 16-bit *mono* sound chunks on top with per-channel
//      left/right panning volumes.

#include "mixer_compat.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

namespace {

struct Channel {
    const Sint16 *data = nullptr;  // mono samples (borrowed from Mix_Chunk)
    Uint32        nsamples = 0;
    Uint32        pos = 0;
    bool          playing = false;
    int           tag = -1;
    Uint8         left = 255, right = 255;
    Uint64        startSeq = 0;    // for "oldest" selection
    Mix_Chunk    *chunk = nullptr;
};

SDL_AudioDeviceID   g_dev = 0;
SDL_AudioSpec       g_spec;
Channel             g_chan[MIX_CHANNELS];
int                 g_reserved = 0;
Uint64              g_seq = 1;

Mix_MusicHook            g_musicHook = nullptr;
void                    *g_musicArg = nullptr;
Mix_ChannelFinishedHook  g_finishedHook = nullptr;

// Channels that finished during the callback; the finished hook is invoked
// afterwards on the main thread-ish (from the audio thread here, matching
// SDL_mixer behaviour which also calls it from the mixer).
int   g_finished[MIX_CHANNELS];
int   g_finishedCount = 0;

inline Sint16 clamp16(int v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (Sint16)v;
}

void AudioCallback(void * /*ud*/, Uint8 *stream, int len) {
    // 1. Base music layer.
    if (g_musicHook)
        g_musicHook(g_musicArg, stream, len);
    else
        memset(stream, 0, len);

    Sint16 *out = (Sint16 *)stream;
    int frames = len / (2 * sizeof(Sint16)); // stereo frames

    g_finishedCount = 0;

    // 2. Mix sound effect channels.
    for (int c = 0; c < MIX_CHANNELS; ++c) {
        Channel &ch = g_chan[c];
        if (!ch.playing || !ch.data) continue;

        for (int f = 0; f < frames; ++f) {
            if (ch.pos >= ch.nsamples) {
                ch.playing = false;
                if (g_finishedCount < MIX_CHANNELS)
                    g_finished[g_finishedCount++] = c;
                break;
            }
            int s = ch.data[ch.pos++];
            int l = out[f * 2 + 0] + (s * ch.left) / 255;
            int r = out[f * 2 + 1] + (s * ch.right) / 255;
            out[f * 2 + 0] = clamp16(l);
            out[f * 2 + 1] = clamp16(r);
        }
    }

    for (int i = 0; i < g_finishedCount; ++i)
        if (g_finishedHook) g_finishedHook(g_finished[i]);
}

} // namespace

extern "C" {

int Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize) {
    (void)format; (void)channels;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) return -1;

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = frequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)chunksize;
    want.callback = AudioCallback;

    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &g_spec, 0);
    if (g_dev == 0) {
        printf("platform_audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return -1;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    return 0;
}

void Mix_CloseAudio(void) {
    if (g_dev) { SDL_CloseAudioDevice(g_dev); g_dev = 0; }
}

const char *Mix_GetError(void) { return SDL_GetError(); }

int Mix_ReserveChannels(int num) {
    g_reserved = num;
    return num;
}

int Mix_GroupChannels(int from, int to, int tag) {
    if (from < 0) from = 0;
    if (to >= MIX_CHANNELS) to = MIX_CHANNELS - 1;
    for (int c = from; c <= to; ++c) g_chan[c].tag = tag;
    return to - from + 1;
}

int Mix_GroupAvailable(int tag) {
    if (g_dev == 0) return -1;
    SDL_LockAudioDevice(g_dev);
    int found = -1;
    for (int c = 0; c < MIX_CHANNELS; ++c) {
        if (g_chan[c].tag == tag && !g_chan[c].playing) { found = c; break; }
    }
    SDL_UnlockAudioDevice(g_dev);
    return found;
}

int Mix_GroupOldest(int tag) {
    if (g_dev == 0) return -1;
    SDL_LockAudioDevice(g_dev);
    int found = -1;
    Uint64 oldest = ~0ull;
    for (int c = 0; c < MIX_CHANNELS; ++c) {
        if (g_chan[c].tag == tag && g_chan[c].playing && g_chan[c].startSeq < oldest) {
            oldest = g_chan[c].startSeq;
            found = c;
        }
    }
    SDL_UnlockAudioDevice(g_dev);
    return found;
}

int Mix_SetPanning(int channel, Uint8 left, Uint8 right) {
    if (channel < 0 || channel >= MIX_CHANNELS || g_dev == 0) return 0;
    SDL_LockAudioDevice(g_dev);
    g_chan[channel].left = left;
    g_chan[channel].right = right;
    SDL_UnlockAudioDevice(g_dev);
    return 1;
}

// Parse the in-memory 16-bit mono PCM WAV that id_sd.cpp builds.
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc) {
    SDL_AudioSpec spec;
    Uint8 *buf = nullptr;
    Uint32 buflen = 0;
    if (SDL_LoadWAV_RW(src, freesrc, &spec, &buf, &buflen) == nullptr)
        return nullptr;

    Mix_Chunk *chunk = (Mix_Chunk *)calloc(1, sizeof(Mix_Chunk));
    chunk->allocated = 1;
    chunk->abuf = buf;         // SDL-allocated; freed with SDL_FreeWAV
    chunk->alen = buflen;
    chunk->volume = MIX_MAX_VOLUME;
    return chunk;
}

int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops) {
    (void)loops;
    if (!chunk || g_dev == 0) return -1;

    SDL_LockAudioDevice(g_dev);
    if (channel < 0) {
        for (int c = g_reserved; c < MIX_CHANNELS; ++c)
            if (!g_chan[c].playing) { channel = c; break; }
        if (channel < 0) { SDL_UnlockAudioDevice(g_dev); return -1; }
    }
    Channel &ch = g_chan[channel];
    ch.data = (const Sint16 *)chunk->abuf;
    ch.nsamples = chunk->alen / sizeof(Sint16);
    ch.pos = 0;
    ch.playing = true;
    ch.chunk = chunk;
    ch.startSeq = g_seq++;
    SDL_UnlockAudioDevice(g_dev);
    return channel;
}

int Mix_HaltChannel(int channel) {
    if (g_dev == 0) return 0;
    SDL_LockAudioDevice(g_dev);
    if (channel < 0) {
        for (int c = 0; c < MIX_CHANNELS; ++c) g_chan[c].playing = false;
    } else if (channel < MIX_CHANNELS) {
        g_chan[channel].playing = false;
    }
    SDL_UnlockAudioDevice(g_dev);
    return 0;
}

int Mix_Playing(int channel) {
    if (channel < 0 || channel >= MIX_CHANNELS) return 0;
    return g_chan[channel].playing ? 1 : 0;
}

void Mix_FreeChunk(Mix_Chunk *chunk) {
    if (!chunk) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    for (int c = 0; c < MIX_CHANNELS; ++c)
        if (g_chan[c].chunk == chunk) { g_chan[c].playing = false; g_chan[c].chunk = nullptr; }
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    if (chunk->allocated && chunk->abuf) SDL_FreeWAV(chunk->abuf);
    free(chunk);
}

void Mix_HookMusic(Mix_MusicHook mix_func, void *arg) {
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_musicHook = mix_func;
    g_musicArg = arg;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

void Mix_ChannelFinished(Mix_ChannelFinishedHook channel_finished) {
    g_finishedHook = channel_finished;
}

} // extern "C"
