// Doom-facing glue: implements DG_sound_module (SFX only) and a no-op
// DG_music_module, on top of the Arduino/I2S mixer in k10_sound.cpp.
//
// This file must stay plain C and must never include Arduino.h: doomtype.h's
// `typedef unsigned int boolean` conflicts with Arduino.h's
// `typedef bool boolean`, and the two cannot appear in the same translation
// unit. See k10_sound.h for the split.
//
// Music is not implemented -- MUS/MIDI playback would need an OPL or MIDI
// synth ported to this device, which is a separate project.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "k10_sound.h"

// i_sound.c's I_BindSoundVariables() binds these two config variables
// unconditionally (they're normally supplied by i_sdlsound.c, which this
// vendored, platform-backend-stripped copy of doomgeneric doesn't have). We
// don't do libsamplerate-quality resampling, so they're inert placeholders
// that just need to exist for the linker and the config file format.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// --- sfx module ----------------------------------------------------------

// I_InitSound() runs from D_DoomMain, well before D_DoomLoop's one-time setup
// calls I_InitGraphics() -> DG_Init(). DG_Init needs a specific, documented
// allocation order to land I_VideoBuffer in internal SRAM (it grabs a
// contiguous 64KB before anything else -- including TFT_eSPI's own buffers --
// gets a chance to fragment that heap). Bringing up I2S (DMA buffers) and the
// mixer task (its stack) here would compete for the same heap ahead of that
// claim, so defer it until the first sound actually plays -- by then DG_Init
// has long since run.
static boolean SFX_Init(boolean use_sfx_prefix)
{
    (void)use_sfx_prefix;
    return true;
}

static void SFX_Shutdown(void)
{
    // Doom never calls this in normal play; not implemented.
}

// DMX sound lump name convention: "ds" + the sfx's short name.
static int SFX_GetSfxLumpNum(sfxinfo_t *sfxinfo)
{
    char name[9];
    snprintf(name, sizeof(name), "ds%s", sfxinfo->name);
    // Some linked sfx (e.g. chgun -> pistol) have no lump of their own;
    // W_CheckNumForName (unlike W_GetNumForName) returns -1 instead of a
    // fatal error, and SFX_StartSound already treats a negative lumpnum, or
    // a missing lump, as "skip this sound".
    return W_CheckNumForName(name);
}

static void SFX_Update(void)
{
}

// s_sound.c passes sep in 0..254 with 128 ("NORM_SEP") as center, and vol in
// 0..127. Divide by 127, not 254, so a center-panned sound reaches ~unity
// gain on both channels instead of being halved -- center is the common case
// (every player weapon, pickup and pain sound uses NORM_SEP), so getting this
// wrong makes everything quiet, not just hard-panned sounds. Clamp because
// the extremes (sep=0 or sep=254) would otherwise compute up to ~2x vol.
static void PanToGains(int vol, int sep, int *volL, int *volR)
{
    int l = vol * (254 - sep) / 127;
    int r = vol * sep / 127;
    *volL = l > vol ? vol : l;
    *volR = r > vol ? vol : r;
}

static void SFX_UpdateSoundParams(int channel, int vol, int sep)
{
    int volL, volR;
    PanToGains(vol, sep, &volL, &volR);
    k10_sound_set_params(channel, volL, volR);
}

static int SFX_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep)
{
    int len, lump;
    const uint8_t *raw, *data;
    uint16_t rate;
    uint32_t nsamples, avail;
    int volL, volR;

    if (sfxinfo->lumpnum < 0)
    {
        return -1;
    }

    if (!k10_sound_init())   // no-op once already running
    {
        return -1;
    }

    lump = sfxinfo->lumpnum;
    len = W_LumpLength((unsigned int)lump);
    if (len < 9)
    {
        return -1;   // smaller than the DMX header
    }

    raw = (const uint8_t *)W_CacheLumpNum(lump, PU_STATIC);
    rate = raw[2] | (raw[3] << 8);
    nsamples = raw[4] | (raw[5] << 8) | (raw[6] << 16) | ((uint32_t)raw[7] << 24);

    data = raw + 8;
    avail = (uint32_t)(len - 8);
    if (nsamples > avail)
    {
        nsamples = avail;
    }
    // Trim the 16-sample lead-in/lead-out pad used by the original
    // PC-speaker driver's ramp; harmless to skip if too short to have one.
    if (nsamples > 32)
    {
        data += 16;
        nsamples -= 32;
    }
    if (nsamples == 0)
    {
        return -1;
    }

    PanToGains(vol, sep, &volL, &volR);

    return k10_sound_start(channel, data, nsamples, rate, volL, volR);
}

static void SFX_StopSound(int channel)
{
    k10_sound_stop(channel);
}

static boolean SFX_SoundIsPlaying(int channel)
{
    return k10_sound_is_playing(channel) ? true : false;
}

static void SFX_CacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    int i;
    // Just resolve lump numbers up front; the WAD cache pulls in the actual
    // lump data lazily on first play (SFX_StartSound), avoiding a big
    // up-front allocation for sounds that never get played in a session.
    for (i = 0; i < num_sounds; i++)
    {
        if (sounds[i].lumpnum < 0)
        {
            sounds[i].lumpnum = SFX_GetSfxLumpNum(&sounds[i]);
        }
    }
}

static snddevice_t sfx_devices[] = { SNDDEVICE_SB };

sound_module_t DG_sound_module = {
    sfx_devices,
    sizeof(sfx_devices) / sizeof(sfx_devices[0]),
    SFX_Init,
    SFX_Shutdown,
    SFX_GetSfxLumpNum,
    SFX_Update,
    SFX_UpdateSoundParams,
    SFX_StartSound,
    SFX_StopSound,
    SFX_SoundIsPlaying,
    SFX_CacheSounds,
};

// --- music module (stub -- no MUS/MIDI synth on this port) ---------------

static boolean Music_Init(void)                        { return true; }
static void    Music_Shutdown(void)                     {}
static void    Music_SetVolume(int volume)              { (void)volume; }
static void    Music_Pause(void)                        {}
static void    Music_Resume(void)                       {}
static void   *Music_RegisterSong(void *data, int len)  { (void)data; (void)len; return NULL; }
static void    Music_UnRegisterSong(void *handle)       { (void)handle; }
static void    Music_Play(void *handle, boolean looping){ (void)handle; (void)looping; }
static void    Music_Stop(void)                         {}
static boolean Music_IsPlaying(void)                    { return false; }

static snddevice_t music_devices[] = { SNDDEVICE_SB, SNDDEVICE_GENMIDI };

music_module_t DG_music_module = {
    music_devices,
    sizeof(music_devices) / sizeof(music_devices[0]),
    Music_Init,
    Music_Shutdown,
    Music_SetVolume,
    Music_Pause,
    Music_Resume,
    Music_RegisterSong,
    Music_UnRegisterSong,
    Music_Play,
    Music_Stop,
    Music_IsPlaying,
    NULL,   // Poll
};
