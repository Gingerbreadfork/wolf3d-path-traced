// mixer_compat.h
//
// A compact reimplementation of the small subset of the SDL_mixer API that the
// original Wolfenstein 3D sound manager (id_sd.cpp) relies on, backed directly
// by an SDL2 audio device. This is the "modern audio bridge" from the plan: it
// lets the original id_sd.cpp code run unmodified while removing the external
// SDL_mixer dependency.
//
// Supported: an OPL music hook (Mix_HookMusic), 16-bit mono sound chunks with
// stereo panning, reserved channels, and channel groups.

#ifndef WOLFPT_MIXER_COMPAT_H
#define WOLFPT_MIXER_COMPAT_H

#include <SDL.h>

// Consistent layout whether included under wl_def.h's pack(1) or default.
#pragma pack(push, 8)

#ifdef __cplusplus
extern "C" {
#endif

#define MIX_CHANNELS        8
#define MIX_MAX_VOLUME      128
#define MIX_DEFAULT_FORMAT  AUDIO_S16SYS

typedef struct Mix_Chunk {
    int        allocated;   // owns `abuf`
    Uint8     *abuf;        // raw 16-bit mono sample bytes
    Uint32     alen;        // length in bytes
    Uint8      volume;      // 0..128
} Mix_Chunk;

typedef void (*Mix_MusicHook)(void *udata, Uint8 *stream, int len);
typedef void (*Mix_ChannelFinishedHook)(int channel);

// --- lifecycle ---
int  Mix_OpenAudio(int frequency, Uint16 format, int channels, int chunksize);
void Mix_CloseAudio(void);
const char *Mix_GetError(void);

// --- channel management ---
int  Mix_ReserveChannels(int num);
int  Mix_GroupChannels(int from, int to, int tag);
int  Mix_GroupAvailable(int tag);
int  Mix_GroupOldest(int tag);
int  Mix_SetPanning(int channel, Uint8 left, Uint8 right);

// --- playback ---
Mix_Chunk *Mix_LoadWAV_RW(SDL_RWops *src, int freesrc);
int  Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops);
int  Mix_HaltChannel(int channel);
int  Mix_Playing(int channel);
void Mix_FreeChunk(Mix_Chunk *chunk);

// --- hooks ---
void Mix_HookMusic(Mix_MusicHook mix_func, void *arg);
void Mix_ChannelFinished(Mix_ChannelFinishedHook channel_finished);

#ifdef __cplusplus
}
#endif

#pragma pack(pop)

#endif // WOLFPT_MIXER_COMPAT_H
