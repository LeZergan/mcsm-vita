/*
 * OpenSL ES to SceAudioOut audio bridge for FMOD Studio on PS Vita.
 *
 * FMOD's Android output opens libOpenSLES.so at runtime, resolves
 * slCreateEngine, creates an AudioPlayer with an Android simple buffer queue,
 * and feeds signed 16-bit PCM through Enqueue. This file provides that OpenSL
 * surface and sends the queued PCM to sceAudioOutOutput.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "utils/logger.h"

extern void audio_close_port(void);
extern int  audio_open_port(int sample_rate, int channels, int desired_frames);
extern void audio_output_i16_frames(const int16_t *samples, int frames, int channels);

typedef struct OpenslPlayer {
    const struct SLObjectItf_ *obj_itf;
    const struct SLPlayItf_ *play_itf;
    const struct SLAndroidSimpleBufferQueueItf_ *android_bq_itf;
    const struct SLBufferQueueItf_ *bq_itf;
    const struct SLAndroidConfigurationItf_ *config_itf;
    const struct SLVolumeItf_ *volume_itf;

    struct SLObjectItf_ obj_vt;
    struct SLPlayItf_ play_vt;
    struct SLAndroidSimpleBufferQueueItf_ android_bq_vt;
    struct SLBufferQueueItf_ bq_vt;
    struct SLAndroidConfigurationItf_ config_vt;
    struct SLVolumeItf_ volume_vt;

    slAndroidSimpleBufferQueueCallback android_callback;
    slBufferQueueCallback bq_callback;
    void *callback_ctx;

    int sample_rate;
    int channels;
    int bytes_per_sample;
    volatile int playing;
    volatile int port_open;
    volatile int thread_stop;
    volatile int in_callback;
    volatile unsigned enqueue_serial;
    int thread_started;
    pthread_t thread;

    SLmillibel volume_level;
    SLboolean muted;
    unsigned enqueue_log_count;
    unsigned callback_log_count;
} OpenslPlayer;

static OpenslPlayer *g_player = NULL;

static struct SLObjectItf_ g_engine_obj_vt;
static const struct SLObjectItf_ *g_engine_obj_itf = &g_engine_obj_vt;
static const SLObjectItf g_engine_obj = (SLObjectItf)&g_engine_obj_itf;
static struct SLEngineItf_ g_engine_vt;
static const struct SLEngineItf_ *g_engine_itf = &g_engine_vt;

static struct SLObjectItf_ g_mix_obj_vt;
static const struct SLObjectItf_ *g_mix_obj_itf = &g_mix_obj_vt;
static const SLObjectItf g_mix_obj = (SLObjectItf)&g_mix_obj_itf;
static struct SLOutputMixItf_ g_mix_vt;
static const struct SLOutputMixItf_ *g_mix_itf = &g_mix_vt;

static int iid_equal(SLInterfaceID a, SLInterfaceID b) {
    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }
    return memcmp(a, b, sizeof(*a)) == 0;
}

static const char *iid_name(SLInterfaceID iid) {
    if (iid_equal(iid, SL_IID_ENGINE)) return "ENGINE";
    if (iid_equal(iid, SL_IID_OUTPUTMIX)) return "OUTPUTMIX";
    if (iid_equal(iid, SL_IID_PLAY)) return "PLAY";
    if (iid_equal(iid, SL_IID_RECORD)) return "RECORD";
    if (iid_equal(iid, SL_IID_VOLUME)) return "VOLUME";
    if (iid_equal(iid, SL_IID_BUFFERQUEUE)) return "BUFFERQUEUE";
    if (iid_equal(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) return "ANDROIDSIMPLEBUFFERQUEUE";
    if (iid_equal(iid, SL_IID_ANDROIDCONFIGURATION)) return "ANDROIDCONFIGURATION";
    return "?";
}

static SLresult unsupported_required_interfaces(SLuint32 count,
                                                const SLInterfaceID *ids,
                                                const SLboolean *required) {
    for (SLuint32 i = 0; i < count; ++i) {
        const int supported =
            iid_equal(ids[i], SL_IID_PLAY) ||
            iid_equal(ids[i], SL_IID_VOLUME) ||
            iid_equal(ids[i], SL_IID_BUFFERQUEUE) ||
            iid_equal(ids[i], SL_IID_ANDROIDSIMPLEBUFFERQUEUE) ||
            iid_equal(ids[i], SL_IID_ANDROIDCONFIGURATION);
        const SLboolean req = required ? required[i] : SL_BOOLEAN_FALSE;
        l_info("OpenSL: requested player interface[%u]=%s required=%u",
               (unsigned)i, iid_name(ids[i]), (unsigned)req);
        if (!supported && req) {
            return SL_RESULT_FEATURE_UNSUPPORTED;
        }
    }
    return SL_RESULT_SUCCESS;
}

static void opensl_call_queue_callback(OpenslPlayer *p) {
    if (!p || p->thread_stop) {
        return;
    }
    if (p->android_callback) {
        if (p->callback_log_count++ < 8U) {
            l_info("OpenSL: invoking Android buffer queue callback");
        }
        p->android_callback((SLAndroidSimpleBufferQueueItf)&p->android_bq_itf, p->callback_ctx);
    } else if (p->bq_callback) {
        if (p->callback_log_count++ < 8U) {
            l_info("OpenSL: invoking generic buffer queue callback");
        }
        p->bq_callback((SLBufferQueueItf)&p->bq_itf, p->callback_ctx);
    }
}

static void *opensl_audio_thread(void *arg) {
    OpenslPlayer *p = (OpenslPlayer *)arg;
    l_info("OpenSL: audio feed thread started");
    while (p && !p->thread_stop) {
        if (p->playing && p->port_open && (p->android_callback || p->bq_callback)) {
            const unsigned before = p->enqueue_serial;
            p->in_callback++;
            opensl_call_queue_callback(p);
            p->in_callback--;
            if (p->enqueue_serial == before) {
                sceKernelDelayThread(1000);
            }
        } else {
            sceKernelDelayThread(5000);
        }
    }
    l_info("OpenSL: audio feed thread stopped");
    return NULL;
}

static void opensl_start_thread(OpenslPlayer *p) {
    if (!p || p->thread_started) {
        return;
    }
    p->thread_stop = 0;
    if (pthread_create(&p->thread, NULL, opensl_audio_thread, p) == 0) {
        p->thread_started = 1;
    } else {
        l_error("OpenSL: failed to start audio feed thread");
    }
}

static void opensl_stop_thread(OpenslPlayer *p) {
    if (!p || !p->thread_started) {
        return;
    }
    p->thread_stop = 1;
    if (pthread_equal(pthread_self(), p->thread)) {
        pthread_detach(p->thread);
    } else {
        pthread_join(p->thread, NULL);
    }
    p->thread_started = 0;
}

static SLresult player_enqueue_common(OpenslPlayer *p, const void *buffer, SLuint32 size) {
    if (!p || !buffer || size == 0) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    if (p->bytes_per_sample != 2 || p->channels <= 0) {
        l_error("OpenSL: unsupported PCM format ch=%d bytes=%d",
                p->channels, p->bytes_per_sample);
        return SL_RESULT_CONTENT_UNSUPPORTED;
    }

    const int frame_bytes = p->channels * p->bytes_per_sample;
    const int frames = (int)(size / (SLuint32)frame_bytes);
    if (frames <= 0) {
        return SL_RESULT_PARAMETER_INVALID;
    }

    if (!p->port_open) {
        audio_open_port(p->sample_rate, p->channels, 1024);
        p->port_open = 1;
    }

    if (p->enqueue_log_count < 8U) {
        const int16_t *samples = (const int16_t *)buffer;
        int has_audio = 0;
        int check = frames * p->channels;
        if (check > 256) {
            check = 256;
        }
        for (int i = 0; i < check; ++i) {
            if (samples[i] != 0) {
                has_audio = 1;
                break;
            }
        }
        l_info("OpenSL Enqueue frames=%d ch=%d rate=%d has_audio=%d",
               frames, p->channels, p->sample_rate, has_audio);
        p->enqueue_log_count++;
    }

    p->enqueue_serial++;
    audio_output_i16_frames((const int16_t *)buffer, frames, p->channels);
    return SL_RESULT_SUCCESS;
}

static SLresult player_android_bq_enqueue(SLAndroidSimpleBufferQueueItf self,
                                          const void *buffer,
                                          SLuint32 size) {
    (void)self;
    return player_enqueue_common(g_player, buffer, size);
}

static SLresult player_android_bq_clear(SLAndroidSimpleBufferQueueItf self) {
    (void)self;
    return SL_RESULT_SUCCESS;
}

static SLresult player_android_bq_get_state(SLAndroidSimpleBufferQueueItf self,
                                            SLAndroidSimpleBufferQueueState *state) {
    (void)self;
    if (state) {
        state->count = 0;
        state->index = g_player ? g_player->enqueue_serial : 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_android_bq_register_callback(SLAndroidSimpleBufferQueueItf self,
                                                    slAndroidSimpleBufferQueueCallback callback,
                                                    void *context) {
    (void)self;
    if (g_player) {
        g_player->android_callback = callback;
        g_player->callback_ctx = context;
        l_info("OpenSL Android buffer queue callback registered @%p", (void *)callback);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_bq_enqueue(SLBufferQueueItf self, const void *buffer, SLuint32 size) {
    (void)self;
    return player_enqueue_common(g_player, buffer, size);
}

static SLresult player_bq_clear(SLBufferQueueItf self) {
    (void)self;
    return SL_RESULT_SUCCESS;
}

static SLresult player_bq_get_state(SLBufferQueueItf self, SLBufferQueueState *state) {
    (void)self;
    if (state) {
        state->count = 0;
        state->playIndex = g_player ? g_player->enqueue_serial : 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_bq_register_callback(SLBufferQueueItf self,
                                            slBufferQueueCallback callback,
                                            void *context) {
    (void)self;
    if (g_player) {
        g_player->bq_callback = callback;
        g_player->callback_ctx = context;
        l_info("OpenSL buffer queue callback registered @%p", (void *)callback);
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_set_state(SLPlayItf self, SLuint32 state) {
    (void)self;
    OpenslPlayer *p = g_player;
    if (!p) {
        return SL_RESULT_SUCCESS;
    }

    l_info("OpenSL PlayState -> %s",
           state == SL_PLAYSTATE_PLAYING ? "PLAYING" :
           state == SL_PLAYSTATE_PAUSED ? "PAUSED" : "STOPPED");

    if (state == SL_PLAYSTATE_PLAYING) {
        p->playing = 1;
        if (!p->port_open) {
            audio_open_port(p->sample_rate, p->channels, 1024);
            p->port_open = 1;
        }
        opensl_start_thread(p);
    } else if (state == SL_PLAYSTATE_PAUSED) {
        p->playing = 0;
    } else {
        p->playing = 0;
        opensl_stop_thread(p);
        if (p->port_open) {
            audio_close_port();
            p->port_open = 0;
        }
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_get_state(SLPlayItf self, SLuint32 *state) {
    (void)self;
    if (state) {
        *state = (g_player && g_player->playing) ? SL_PLAYSTATE_PLAYING : SL_PLAYSTATE_STOPPED;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_get_duration(SLPlayItf self, SLmillisecond *msec) {
    (void)self;
    if (msec) {
        *msec = SL_TIME_UNKNOWN;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_get_position(SLPlayItf self, SLmillisecond *msec) {
    (void)self;
    if (msec) {
        *msec = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_register_callback(SLPlayItf self,
                                              slPlayCallback callback,
                                              void *context) {
    (void)self;
    (void)callback;
    (void)context;
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_set_events(SLPlayItf self, SLuint32 event_flags) {
    (void)self;
    (void)event_flags;
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_get_events(SLPlayItf self, SLuint32 *event_flags) {
    (void)self;
    if (event_flags) {
        *event_flags = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_marker(SLPlayItf self, SLmillisecond msec) {
    (void)self;
    (void)msec;
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_clear_marker(SLPlayItf self) {
    (void)self;
    return SL_RESULT_SUCCESS;
}

static SLresult player_play_get_marker(SLPlayItf self, SLmillisecond *msec) {
    (void)self;
    if (msec) {
        *msec = SL_TIME_UNKNOWN;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult player_config_set(SLAndroidConfigurationItf self,
                                  const SLchar *key,
                                  const void *value,
                                  SLuint32 value_size) {
    (void)self;
    (void)value;
    l_info("OpenSL: AndroidConfiguration.Set(%s, %u)",
           key ? (const char *)key : "(null)", (unsigned)value_size);
    return SL_RESULT_SUCCESS;
}

static SLresult player_config_get(SLAndroidConfigurationItf self,
                                  const SLchar *key,
                                  SLuint32 *value_size,
                                  void *value) {
    (void)self;
    (void)key;
    if (value_size) {
        if (value && *value_size >= sizeof(SLint32)) {
            memset(value, 0, sizeof(SLint32));
            *value_size = sizeof(SLint32);
        } else {
            *value_size = sizeof(SLint32);
        }
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_set_level(SLVolumeItf self, SLmillibel level) {
    (void)self;
    if (g_player) {
        g_player->volume_level = level;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_level(SLVolumeItf self, SLmillibel *level) {
    (void)self;
    if (level) {
        *level = g_player ? g_player->volume_level : 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_max(SLVolumeItf self, SLmillibel *level) {
    (void)self;
    if (level) {
        *level = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_set_mute(SLVolumeItf self, SLboolean mute) {
    (void)self;
    if (g_player) {
        g_player->muted = mute;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_mute(SLVolumeItf self, SLboolean *mute) {
    (void)self;
    if (mute) {
        *mute = g_player ? g_player->muted : SL_BOOLEAN_FALSE;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_enable_stereo(SLVolumeItf self, SLboolean enable) {
    (void)self;
    (void)enable;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_stereo_enabled(SLVolumeItf self, SLboolean *enable) {
    (void)self;
    if (enable) {
        *enable = SL_BOOLEAN_FALSE;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult volume_set_stereo_position(SLVolumeItf self, SLpermille position) {
    (void)self;
    (void)position;
    return SL_RESULT_SUCCESS;
}

static SLresult volume_get_stereo_position(SLVolumeItf self, SLpermille *position) {
    (void)self;
    if (position) {
        *position = 0;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult obj_realize(SLObjectItf self, SLboolean async) {
    (void)self;
    (void)async;
    l_info("OpenSL: Object Realize");
    return SL_RESULT_SUCCESS;
}

static SLresult obj_resume(SLObjectItf self, SLboolean async) {
    (void)self;
    (void)async;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_get_state(SLObjectItf self, SLuint32 *state) {
    (void)self;
    if (state) {
        *state = SL_OBJECT_STATE_REALIZED;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult obj_get_interface(SLObjectItf self, const SLInterfaceID iid, void *iface) {
    if (!iface) {
        return SL_RESULT_PARAMETER_INVALID;
    }

    if (self == g_engine_obj && iid_equal(iid, SL_IID_ENGINE)) {
        *(SLEngineItf *)iface = (SLEngineItf)&g_engine_itf;
        l_info("OpenSL: GetInterface ENGINE");
        return SL_RESULT_SUCCESS;
    }

    if (self == g_mix_obj && iid_equal(iid, SL_IID_OUTPUTMIX)) {
        *(SLOutputMixItf *)iface = (SLOutputMixItf)&g_mix_itf;
        l_info("OpenSL: GetInterface OUTPUTMIX");
        return SL_RESULT_SUCCESS;
    }

    OpenslPlayer *p = g_player;
    if (p && self == (SLObjectItf)&p->obj_itf) {
        if (iid_equal(iid, SL_IID_PLAY)) {
            *(SLPlayItf *)iface = (SLPlayItf)&p->play_itf;
            return SL_RESULT_SUCCESS;
        }
        if (iid_equal(iid, SL_IID_ANDROIDSIMPLEBUFFERQUEUE)) {
            *(SLAndroidSimpleBufferQueueItf *)iface = (SLAndroidSimpleBufferQueueItf)&p->android_bq_itf;
            return SL_RESULT_SUCCESS;
        }
        if (iid_equal(iid, SL_IID_BUFFERQUEUE)) {
            *(SLBufferQueueItf *)iface = (SLBufferQueueItf)&p->bq_itf;
            return SL_RESULT_SUCCESS;
        }
        if (iid_equal(iid, SL_IID_ANDROIDCONFIGURATION)) {
            *(SLAndroidConfigurationItf *)iface = (SLAndroidConfigurationItf)&p->config_itf;
            return SL_RESULT_SUCCESS;
        }
        if (iid_equal(iid, SL_IID_VOLUME)) {
            *(SLVolumeItf *)iface = (SLVolumeItf)&p->volume_itf;
            return SL_RESULT_SUCCESS;
        }
    }

    l_warn("OpenSL: GetInterface unsupported iid=%s", iid_name(iid));
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult obj_register_callback(SLObjectItf self,
                                      slObjectCallback callback,
                                      void *context) {
    (void)self;
    (void)callback;
    (void)context;
    return SL_RESULT_SUCCESS;
}

static void obj_abort(SLObjectItf self) {
    (void)self;
}

static void obj_destroy(SLObjectItf self) {
    l_info("OpenSL: Object Destroy");
    if (g_player && self == (SLObjectItf)&g_player->obj_itf) {
        opensl_stop_thread(g_player);
        if (g_player->port_open) {
            audio_close_port();
            g_player->port_open = 0;
        }
        free(g_player);
        g_player = NULL;
    } else if (self == g_engine_obj || self == g_mix_obj) {
        if (g_player) {
            opensl_stop_thread(g_player);
            if (g_player->port_open) {
                audio_close_port();
                g_player->port_open = 0;
            }
            free(g_player);
            g_player = NULL;
        }
    }
}

static SLresult obj_set_priority(SLObjectItf self, SLint32 priority, SLboolean preemptable) {
    (void)self;
    (void)priority;
    (void)preemptable;
    return SL_RESULT_SUCCESS;
}

static SLresult obj_get_priority(SLObjectItf self, SLint32 *priority, SLboolean *preemptable) {
    (void)self;
    if (priority) {
        *priority = 0;
    }
    if (preemptable) {
        *preemptable = SL_BOOLEAN_FALSE;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult obj_set_loss_interfaces(SLObjectItf self,
                                        SLint16 num_interfaces,
                                        SLInterfaceID *interface_ids,
                                        SLboolean enabled) {
    (void)self;
    (void)num_interfaces;
    (void)interface_ids;
    (void)enabled;
    return SL_RESULT_SUCCESS;
}

static void init_object_vtable(struct SLObjectItf_ *vt) {
    memset(vt, 0, sizeof(*vt));
    vt->Realize = obj_realize;
    vt->Resume = obj_resume;
    vt->GetState = obj_get_state;
    vt->GetInterface = obj_get_interface;
    vt->RegisterCallback = obj_register_callback;
    vt->AbortAsyncOperation = obj_abort;
    vt->Destroy = obj_destroy;
    vt->SetPriority = obj_set_priority;
    vt->GetPriority = obj_get_priority;
    vt->SetLossOfControlInterfaces = obj_set_loss_interfaces;
}

static void init_player_vtables(OpenslPlayer *p) {
    init_object_vtable(&p->obj_vt);
    p->obj_itf = &p->obj_vt;

    memset(&p->play_vt, 0, sizeof(p->play_vt));
    p->play_vt.SetPlayState = player_play_set_state;
    p->play_vt.GetPlayState = player_play_get_state;
    p->play_vt.GetDuration = player_play_get_duration;
    p->play_vt.GetPosition = player_play_get_position;
    p->play_vt.RegisterCallback = player_play_register_callback;
    p->play_vt.SetCallbackEventsMask = player_play_set_events;
    p->play_vt.GetCallbackEventsMask = player_play_get_events;
    p->play_vt.SetMarkerPosition = player_play_marker;
    p->play_vt.ClearMarkerPosition = player_play_clear_marker;
    p->play_vt.GetMarkerPosition = player_play_get_marker;
    p->play_vt.SetPositionUpdatePeriod = player_play_marker;
    p->play_vt.GetPositionUpdatePeriod = player_play_get_marker;
    p->play_itf = &p->play_vt;

    memset(&p->android_bq_vt, 0, sizeof(p->android_bq_vt));
    p->android_bq_vt.Enqueue = player_android_bq_enqueue;
    p->android_bq_vt.Clear = player_android_bq_clear;
    p->android_bq_vt.GetState = player_android_bq_get_state;
    p->android_bq_vt.RegisterCallback = player_android_bq_register_callback;
    p->android_bq_itf = &p->android_bq_vt;

    memset(&p->bq_vt, 0, sizeof(p->bq_vt));
    p->bq_vt.Enqueue = player_bq_enqueue;
    p->bq_vt.Clear = player_bq_clear;
    p->bq_vt.GetState = player_bq_get_state;
    p->bq_vt.RegisterCallback = player_bq_register_callback;
    p->bq_itf = &p->bq_vt;

    memset(&p->config_vt, 0, sizeof(p->config_vt));
    p->config_vt.SetConfiguration = player_config_set;
    p->config_vt.GetConfiguration = player_config_get;
    p->config_itf = &p->config_vt;

    memset(&p->volume_vt, 0, sizeof(p->volume_vt));
    p->volume_vt.SetVolumeLevel = volume_set_level;
    p->volume_vt.GetVolumeLevel = volume_get_level;
    p->volume_vt.GetMaxVolumeLevel = volume_get_max;
    p->volume_vt.SetMute = volume_set_mute;
    p->volume_vt.GetMute = volume_get_mute;
    p->volume_vt.EnableStereoPosition = volume_enable_stereo;
    p->volume_vt.IsEnabledStereoPosition = volume_get_stereo_enabled;
    p->volume_vt.SetStereoPosition = volume_set_stereo_position;
    p->volume_vt.GetStereoPosition = volume_get_stereo_position;
    p->volume_itf = &p->volume_vt;
}

static SLresult mix_get_devices(SLOutputMixItf self, SLint32 *num_devices, SLuint32 *device_ids) {
    (void)self;
    if (num_devices) {
        if (device_ids && *num_devices > 0) {
            device_ids[0] = SL_DEFAULTDEVICEID_AUDIOOUTPUT;
        }
        *num_devices = 1;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult mix_register_device_callback(SLOutputMixItf self,
                                             slMixDeviceChangeCallback callback,
                                             void *context) {
    (void)self;
    (void)callback;
    (void)context;
    return SL_RESULT_SUCCESS;
}

static SLresult mix_reroute(SLOutputMixItf self, SLint32 num_devices, SLuint32 *device_ids) {
    (void)self;
    (void)num_devices;
    (void)device_ids;
    return SL_RESULT_SUCCESS;
}

static void init_mix_vtables(void) {
    init_object_vtable(&g_mix_obj_vt);
    memset(&g_mix_vt, 0, sizeof(g_mix_vt));
    g_mix_vt.GetDestinationOutputDeviceIDs = mix_get_devices;
    g_mix_vt.RegisterDeviceChangeCallback = mix_register_device_callback;
    g_mix_vt.ReRoute = mix_reroute;
}

static SLresult eng_create_audio_player(SLEngineItf self,
                                        SLObjectItf *player,
                                        SLDataSource *audio_src,
                                        SLDataSink *audio_sink,
                                        SLuint32 num_interfaces,
                                        const SLInterfaceID *interface_ids,
                                        const SLboolean *interface_required) {
    (void)self;
    (void)audio_sink;
    if (!player) {
        return SL_RESULT_PARAMETER_INVALID;
    }

    SLresult req_result = unsupported_required_interfaces(num_interfaces, interface_ids, interface_required);
    if (req_result != SL_RESULT_SUCCESS) {
        return req_result;
    }

    int rate = 48000;
    int channels = 2;
    int bytes_per_sample = 2;
    if (audio_src && audio_src->pFormat) {
        SLDataFormat_PCM *pcm = (SLDataFormat_PCM *)audio_src->pFormat;
        if (pcm->formatType == SL_DATAFORMAT_PCM) {
            if (pcm->samplesPerSec > 0) {
                rate = (int)((SLuint32)pcm->samplesPerSec / 1000U);
            }
            channels = (int)pcm->numChannels;
            bytes_per_sample = (int)(pcm->bitsPerSample / 8U);
        }
    }
    if (rate <= 0) rate = 48000;
    if (channels != 1 && channels != 2) channels = 2;
    if (bytes_per_sample <= 0) bytes_per_sample = 2;

    OpenslPlayer *p = calloc(1, sizeof(*p));
    if (!p) {
        return SL_RESULT_MEMORY_FAILURE;
    }

    p->sample_rate = rate;
    p->channels = channels;
    p->bytes_per_sample = bytes_per_sample;
    p->volume_level = 0;
    p->muted = SL_BOOLEAN_FALSE;
    init_player_vtables(p);

    if (g_player) {
        opensl_stop_thread(g_player);
        if (g_player->port_open) {
            audio_close_port();
        }
        free(g_player);
    }
    g_player = p;
    *player = (SLObjectItf)&p->obj_itf;

    l_info("OpenSL CreateAudioPlayer rate=%d ch=%d bits=%d object=%p",
           rate, channels, bytes_per_sample * 8, (void *)*player);
    return SL_RESULT_SUCCESS;
}

static SLresult eng_create_output_mix(SLEngineItf self,
                                      SLObjectItf *mix,
                                      SLuint32 num_interfaces,
                                      const SLInterfaceID *interface_ids,
                                      const SLboolean *interface_required) {
    (void)self;
    (void)interface_required;
    if (!mix) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    for (SLuint32 i = 0; i < num_interfaces; ++i) {
        l_info("OpenSL: requested output mix interface[%u]=%s",
               (unsigned)i, iid_name(interface_ids[i]));
    }
    init_mix_vtables();
    *mix = g_mix_obj;
    l_info("OpenSL: CreateOutputMix object=%p", (void *)*mix);
    return SL_RESULT_SUCCESS;
}

static SLresult eng_create_audio_recorder(SLEngineItf self,
                                          SLObjectItf *recorder,
                                          SLDataSource *audio_src,
                                          SLDataSink *audio_snk,
                                          SLuint32 num_interfaces,
                                          const SLInterfaceID *interface_ids,
                                          const SLboolean *interface_required) {
    (void)self;
    (void)recorder;
    (void)audio_src;
    (void)audio_snk;
    for (SLuint32 i = 0; i < num_interfaces; ++i) {
        l_info("OpenSL CreateAudioRecorder probe interface[%u]=%s required=%u",
               (unsigned)i,
               iid_name(interface_ids[i]),
               interface_required ? (unsigned)interface_required[i] : 0U);
    }
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult eng_query_num_interfaces(SLEngineItf self,
                                         SLuint32 object_id,
                                         SLuint32 *num_interfaces) {
    (void)self;
    if (!num_interfaces) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    switch (object_id) {
        case SL_OBJECTID_ENGINE:
            *num_interfaces = 1;
            break;
        case SL_OBJECTID_AUDIOPLAYER:
            *num_interfaces = 5;
            break;
        case SL_OBJECTID_OUTPUTMIX:
            *num_interfaces = 1;
            break;
        default:
            *num_interfaces = 0;
            break;
    }
    return SL_RESULT_SUCCESS;
}

static SLresult eng_query_interface(SLEngineItf self,
                                    SLuint32 object_id,
                                    SLuint32 index,
                                    SLInterfaceID *interface_id) {
    (void)self;
    if (!interface_id) {
        return SL_RESULT_PARAMETER_INVALID;
    }
    if (object_id == SL_OBJECTID_ENGINE && index == 0) {
        *interface_id = SL_IID_ENGINE;
        return SL_RESULT_SUCCESS;
    }
    if (object_id == SL_OBJECTID_OUTPUTMIX && index == 0) {
        *interface_id = SL_IID_OUTPUTMIX;
        return SL_RESULT_SUCCESS;
    }
    if (object_id == SL_OBJECTID_AUDIOPLAYER) {
        SLInterfaceID ids[] = {
            SL_IID_PLAY,
            SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
            SL_IID_BUFFERQUEUE,
            SL_IID_ANDROIDCONFIGURATION,
            SL_IID_VOLUME,
        };
        if (index < sizeof(ids) / sizeof(ids[0])) {
            *interface_id = ids[index];
            return SL_RESULT_SUCCESS;
        }
    }
    return SL_RESULT_PARAMETER_INVALID;
}

static SLresult eng_query_extensions(SLEngineItf self, SLuint32 *num_extensions) {
    (void)self;
    if (num_extensions) {
        *num_extensions = 0;
    }
    return SL_RESULT_SUCCESS;
}

SLresult slCreateEngine_soloader_opensl(SLObjectItf *engine,
                                        SLuint32 num_options,
                                        const SLEngineOption *engine_options,
                                        SLuint32 num_interfaces,
                                        const SLInterfaceID *interface_ids,
                                        const SLboolean *interface_required) {
    (void)num_options;
    (void)engine_options;
    (void)interface_required;
    if (!engine) {
        return SL_RESULT_PARAMETER_INVALID;
    }

    l_info("OpenSL slCreateEngine ifaces=%u", (unsigned)num_interfaces);
    for (SLuint32 i = 0; i < num_interfaces; ++i) {
        l_info("OpenSL: requested engine interface[%u]=%s",
               (unsigned)i, iid_name(interface_ids[i]));
    }

    init_object_vtable(&g_engine_obj_vt);
    memset(&g_engine_vt, 0, sizeof(g_engine_vt));
    g_engine_vt.CreateAudioPlayer = eng_create_audio_player;
    g_engine_vt.CreateAudioRecorder = eng_create_audio_recorder;
    g_engine_vt.CreateOutputMix = eng_create_output_mix;
    g_engine_vt.QueryNumSupportedInterfaces = eng_query_num_interfaces;
    g_engine_vt.QuerySupportedInterfaces = eng_query_interface;
    g_engine_vt.QueryNumSupportedExtensions = eng_query_extensions;

    *engine = g_engine_obj;
    l_info("OpenSL slCreateEngine -> SUCCESS engine=%p", (void *)*engine);
    return SL_RESULT_SUCCESS;
}
