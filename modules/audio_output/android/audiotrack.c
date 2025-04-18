/*****************************************************************************
 * audiotrack.c: Android Java AudioTrack audio output module
 *****************************************************************************
 * Copyright © 2012-2015 VLC authors and VideoLAN, VideoLabs
 *
 * Authors: Thomas Guillem <thomas@gllm.fr>
 *          Ming Hu <tewilove@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <jni.h>
#include <dlfcn.h>
#include <stdbool.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_aout.h>
#include "../../video_output/android/env.h"
#include "device.h"
#include "audioformat_jni.h"
#include "dynamicsprocessing_jni.h"

#define SMOOTHPOS_SAMPLE_COUNT 10
#define SMOOTHPOS_INTERVAL_US VLC_TICK_FROM_MS(30) // 30ms

#define AUDIOTIMESTAMP_INTERVAL_US VLC_TICK_FROM_MS(500) // 500ms

#define AUDIOTRACK_MAX_BUFFER_US VLC_TICK_FROM_MS(750) // 750ms

#define TIMING_REPORT_DELAY_TICKS VLC_TICK_FROM_MS(1000)

#define JARRAY_SIZE_US VLC_TICK_FROM_MS(50)

typedef struct
{
    jobject p_audiotrack; /* AudioTrack ref */
    jobject p_dp;
    float volume;
    bool mute;

    audio_sample_format_t fmt; /* fmt setup by Start */

    struct {
        unsigned int i_rate;
        int i_channel_config;
        int i_format;
        int i_size;
    } audiotrack_args;

    /* Used by AudioTrack_getPlaybackHeadPosition */
    struct {
        uint64_t i_wrap_count;
        uint32_t i_last;
    } headpos;

    /* Used by AudioTrack_GetTimestampPositionUs */
    struct {
        jobject p_obj; /* AudioTimestamp ref */

        jlong i_frame_post_last;
        uint64_t i_frame_wrap_count;
    } timestamp;

    uint32_t i_max_audiotrack_samples;
    long long i_encoding_flags;
    bool b_passthrough;
    uint8_t i_chans_to_reorder; /* do we need channel reordering */
    uint8_t p_chan_table[AOUT_CHAN_MAX];

    enum {
        WRITE_BYTEARRAY,
        WRITE_BYTEARRAYV23,
        WRITE_SHORTARRAYV23,
        WRITE_FLOATARRAY
    } i_write_type;

    vlc_thread_t thread;    /* AudioTrack_Thread */
    vlc_mutex_t lock;
    vlc_cond_t thread_cond; /* cond owned by AudioTrack_Thread */

    /* These variables need locking on read and write */
    bool b_thread_running;  /* Set to false by aout to stop the thread */
    bool b_thread_paused;   /* If true, the thread won't process any data, see
                             * Pause() */

    uint64_t i_samples_written; /* Number of samples written since last flush */
    bool b_audiotrack_exception; /* True if audiotrack threw an exception */
    bool b_error; /* generic error */

    struct {
        size_t offset;
        size_t size;
        size_t maxsize;
        jobject array;
    } jbuffer;

    vlc_frame_t *frame_chain;
    vlc_frame_t **frame_last;

    /* Bytes written since the last timing report */
    size_t timing_report_last_written_bytes;
    /* Number of bytes to write before sending a timing report */
    size_t timing_report_delay_bytes;
    vlc_tick_t first_pts;
} aout_sys_t;


// Don't use Float for now since 5.1/7.1 Float is down sampled to Stereo Float
//#define AUDIOTRACK_USE_FLOAT

/* Get AudioTrack native sample rate: if activated, most of  the resampling
 * will be done by VLC */
#define AUDIOTRACK_NATIVE_SAMPLERATE

#define THREAD_NAME "android_audiotrack"
#define GET_ENV() android_getEnv( VLC_OBJECT(stream), THREAD_NAME )

static struct
{
    struct {
        jclass clazz;
        jmethodID ctor;
        bool has_ctor_21;
        jmethodID release;
        jmethodID getState;
        jmethodID play;
        jmethodID stop;
        jmethodID flush;
        jmethodID pause;
        jmethodID write;
        jmethodID writeV23;
        jmethodID writeShortV23;
        jmethodID writeFloat;
        jmethodID getAudioSessionId;
        jmethodID getBufferSizeInFrames;
        jmethodID getLatency;
        jmethodID getPlaybackHeadPosition;
        jmethodID getTimestamp;
        jmethodID getMinBufferSize;
        jmethodID getNativeOutputSampleRate;
        jmethodID setVolume;
        jmethodID setStereoVolume;
        jint STATE_INITIALIZED;
        jint MODE_STREAM;
        jint ERROR;
        jint ERROR_BAD_VALUE;
        jint ERROR_INVALID_OPERATION;
        jint WRITE_NON_BLOCKING;
    } AudioTrack;
    struct {
        jint USAGE_MEDIA;
        jint CONTENT_TYPE_MOVIE;
        jint CONTENT_TYPE_MUSIC;
    } AudioAttributes;
    struct {
        jclass clazz;
        jmethodID ctor;
        jmethodID build;
        jmethodID setLegacyStreamType;
        jmethodID setUsage;
        jmethodID setContentType;
    } AudioAttributes_Builder;
    struct {
        jint ENCODING_PCM_8BIT;
        jint ENCODING_PCM_16BIT;
        jint ENCODING_PCM_FLOAT;
        bool has_ENCODING_PCM_FLOAT;
        jint ENCODING_IEC61937;
        bool has_ENCODING_IEC61937;
        jint CHANNEL_OUT_MONO;
        jint CHANNEL_OUT_STEREO;
        jint CHANNEL_OUT_FRONT_LEFT;
        jint CHANNEL_OUT_FRONT_RIGHT;
        jint CHANNEL_OUT_BACK_LEFT;
        jint CHANNEL_OUT_BACK_RIGHT;
        jint CHANNEL_OUT_FRONT_CENTER;
        jint CHANNEL_OUT_LOW_FREQUENCY;
        jint CHANNEL_OUT_BACK_CENTER;
        jint CHANNEL_OUT_5POINT1;
        jint CHANNEL_OUT_SIDE_LEFT;
        jint CHANNEL_OUT_SIDE_RIGHT;
        bool has_CHANNEL_OUT_SIDE;
    } AudioFormat;
    struct {
        jclass clazz;
        jmethodID ctor;
        jmethodID build;
        jmethodID setChannelMask;
        jmethodID setEncoding;
        jmethodID setSampleRate;
    } AudioFormat_Builder;
    struct {
        jint ERROR_DEAD_OBJECT;
        bool has_ERROR_DEAD_OBJECT;
        jint STREAM_MUSIC;
    } AudioManager;
    struct {
        jclass clazz;
        jmethodID ctor;
        jfieldID framePosition;
        jfieldID nanoTime;
    } AudioTimestamp;

} jfields;

/* init all jni fields.
 * Done only one time during the first initialisation */
static int
AudioTrack_InitJNI( vlc_object_t *p_aout)
{
    static vlc_mutex_t lock = VLC_STATIC_MUTEX;
    static int i_init_state = -1;
    jclass clazz;
    jfieldID field;
    JNIEnv *env = android_getEnv( p_aout, THREAD_NAME );

    if( env == NULL )
        return VLC_EGENERIC;

    vlc_mutex_lock( &lock );

    if( i_init_state != -1 )
        goto end;

#define CHECK_EXCEPTION( what, critical ) do { \
    if( (*env)->ExceptionCheck( env ) ) \
    { \
        msg_Err( p_aout, "%s failed", what ); \
        (*env)->ExceptionClear( env ); \
        if( (critical) ) \
        { \
            i_init_state = 0; \
            goto end; \
        } \
    } \
} while( 0 )
#define GET_CLASS( str, critical ) do { \
    clazz = (*env)->FindClass( env, (str) ); \
    CHECK_EXCEPTION( "FindClass(" str ")", critical ); \
} while( 0 )
#define GET_ID( get, id, str, args, critical ) do { \
    jfields.id = (*env)->get( env, clazz, (str), (args) ); \
    CHECK_EXCEPTION( #get "(" #id ")", critical ); \
} while( 0 )
#define GET_CONST_INT( id, str, critical ) do { \
    field = NULL; \
    field = (*env)->GetStaticFieldID( env, clazz, (str), "I" ); \
    CHECK_EXCEPTION( "GetStaticFieldID(" #id ")", critical ); \
    if( field ) \
    { \
        jfields.id = (*env)->GetStaticIntField( env, clazz, field ); \
        CHECK_EXCEPTION( #id, critical ); \
    } \
} while( 0 )

    /* AudioTrack class init */
    GET_CLASS( "android/media/AudioTrack", true );
    jfields.AudioTrack.clazz = (jclass) (*env)->NewGlobalRef( env, clazz );
    CHECK_EXCEPTION( "NewGlobalRef", true );

    GET_ID( GetMethodID, AudioTrack.ctor, "<init>",
            "(Landroid/media/AudioAttributes;Landroid/media/AudioFormat;III)V", false );
    jfields.AudioTrack.has_ctor_21 = jfields.AudioTrack.ctor != NULL;
    if( !jfields.AudioTrack.has_ctor_21 )
        GET_ID( GetMethodID, AudioTrack.ctor, "<init>", "(IIIIIII)V", true );
    GET_ID( GetMethodID, AudioTrack.release, "release", "()V", true );
    GET_ID( GetMethodID, AudioTrack.getState, "getState", "()I", true );
    GET_ID( GetMethodID, AudioTrack.play, "play", "()V", true );
    GET_ID( GetMethodID, AudioTrack.stop, "stop", "()V", true );
    GET_ID( GetMethodID, AudioTrack.flush, "flush", "()V", true );
    GET_ID( GetMethodID, AudioTrack.pause, "pause", "()V", true );

    GET_ID( GetMethodID, AudioTrack.writeV23, "write", "([BIII)I", false );
    GET_ID( GetMethodID, AudioTrack.writeShortV23, "write", "([SIII)I", false );

    if( jfields.AudioTrack.writeV23 )
    {
        GET_CONST_INT( AudioTrack.WRITE_NON_BLOCKING, "WRITE_NON_BLOCKING", true );
#ifdef AUDIOTRACK_USE_FLOAT
        GET_ID( GetMethodID, AudioTrack.writeFloat, "write", "([FIII)I", true );
#endif
    } else
        GET_ID( GetMethodID, AudioTrack.write, "write", "([BII)I", true );

    GET_ID( GetMethodID, AudioTrack.getAudioSessionId,
            "getAudioSessionId", "()I", true );

    GET_ID( GetMethodID, AudioTrack.getBufferSizeInFrames,
            "getBufferSizeInFrames", "()I", false );

    GET_ID( GetMethodID, AudioTrack.getLatency, "getLatency", "()I", false );

    GET_ID( GetMethodID, AudioTrack.getTimestamp,
            "getTimestamp", "(Landroid/media/AudioTimestamp;)Z", false );
    GET_ID( GetMethodID, AudioTrack.getPlaybackHeadPosition,
            "getPlaybackHeadPosition", "()I", true );

    GET_ID( GetStaticMethodID, AudioTrack.getMinBufferSize, "getMinBufferSize",
            "(III)I", true );
#ifdef AUDIOTRACK_NATIVE_SAMPLERATE
    GET_ID( GetStaticMethodID, AudioTrack.getNativeOutputSampleRate,
            "getNativeOutputSampleRate",  "(I)I", true );
#endif
    GET_ID( GetMethodID, AudioTrack.setVolume,
            "setVolume",  "(F)I", false );
    if( !jfields.AudioTrack.setVolume )
        GET_ID( GetMethodID, AudioTrack.setStereoVolume,
                "setStereoVolume",  "(FF)I", true );
    GET_CONST_INT( AudioTrack.STATE_INITIALIZED, "STATE_INITIALIZED", true );
    GET_CONST_INT( AudioTrack.MODE_STREAM, "MODE_STREAM", true );
    GET_CONST_INT( AudioTrack.ERROR, "ERROR", true );
    GET_CONST_INT( AudioTrack.ERROR_BAD_VALUE , "ERROR_BAD_VALUE", true );
    GET_CONST_INT( AudioTrack.ERROR_INVALID_OPERATION,
                   "ERROR_INVALID_OPERATION", true );

    if( jfields.AudioTrack.has_ctor_21 )
    {
        /* AudioAttributes_Builder class init */
        GET_CLASS( "android/media/AudioAttributes$Builder", true );
        jfields.AudioAttributes_Builder.clazz = (jclass) (*env)->NewGlobalRef( env, clazz );
        CHECK_EXCEPTION( "NewGlobalRef", true );
        GET_ID( GetMethodID, AudioAttributes_Builder.ctor, "<init>",
                "()V", true );
        GET_ID( GetMethodID, AudioAttributes_Builder.build, "build",
                "()Landroid/media/AudioAttributes;", true );
        GET_ID( GetMethodID, AudioAttributes_Builder.setLegacyStreamType, "setLegacyStreamType",
                "(I)Landroid/media/AudioAttributes$Builder;", true );
        GET_ID( GetMethodID, AudioAttributes_Builder.setUsage, "setUsage",
                "(I)Landroid/media/AudioAttributes$Builder;", true );
        GET_ID( GetMethodID, AudioAttributes_Builder.setContentType, "setContentType",
                "(I)Landroid/media/AudioAttributes$Builder;", true );

        /* AudioAttributes class init */
        GET_CLASS( "android/media/AudioAttributes", true );
        GET_CONST_INT( AudioAttributes.USAGE_MEDIA, "USAGE_MEDIA", true );
        GET_CONST_INT( AudioAttributes.CONTENT_TYPE_MUSIC, "CONTENT_TYPE_MUSIC", true );
        GET_CONST_INT( AudioAttributes.CONTENT_TYPE_MOVIE, "CONTENT_TYPE_MOVIE", true );


        /* AudioFormat_Builder class init */
        GET_CLASS( "android/media/AudioFormat$Builder", true );
        jfields.AudioFormat_Builder.clazz = (jclass) (*env)->NewGlobalRef( env, clazz );
        CHECK_EXCEPTION( "NewGlobalRef", true );
        GET_ID( GetMethodID, AudioFormat_Builder.ctor, "<init>",
                "()V", true );
        GET_ID( GetMethodID, AudioFormat_Builder.build, "build",
                "()Landroid/media/AudioFormat;", true );
        GET_ID( GetMethodID, AudioFormat_Builder.setChannelMask, "setChannelMask",
                "(I)Landroid/media/AudioFormat$Builder;", true );
        GET_ID( GetMethodID, AudioFormat_Builder.setEncoding, "setEncoding",
                "(I)Landroid/media/AudioFormat$Builder;", true );
        GET_ID( GetMethodID, AudioFormat_Builder.setSampleRate, "setSampleRate",
                "(I)Landroid/media/AudioFormat$Builder;", true );
    }

    /* AudioTimestamp class init (if any) */
    if( jfields.AudioTrack.getTimestamp )
    {
        GET_CLASS( "android/media/AudioTimestamp", true );
        jfields.AudioTimestamp.clazz = (jclass) (*env)->NewGlobalRef( env,
                                                                      clazz );
        CHECK_EXCEPTION( "NewGlobalRef", true );

        GET_ID( GetMethodID, AudioTimestamp.ctor, "<init>", "()V", true );
        GET_ID( GetFieldID, AudioTimestamp.framePosition,
                "framePosition", "J", true );
        GET_ID( GetFieldID, AudioTimestamp.nanoTime,
                "nanoTime", "J", true );
    }

    /* AudioFormat class init */
    GET_CLASS( "android/media/AudioFormat", true );
    GET_CONST_INT( AudioFormat.ENCODING_PCM_8BIT, "ENCODING_PCM_8BIT", true );
    GET_CONST_INT( AudioFormat.ENCODING_PCM_16BIT, "ENCODING_PCM_16BIT", true );

#ifdef AUDIOTRACK_USE_FLOAT
    GET_CONST_INT( AudioFormat.ENCODING_PCM_FLOAT, "ENCODING_PCM_FLOAT",
                   false );
    jfields.AudioFormat.has_ENCODING_PCM_FLOAT = field != NULL &&
                                                 jfields.AudioTrack.writeFloat;
#else
    jfields.AudioFormat.has_ENCODING_PCM_FLOAT = false;
#endif

    if( jfields.AudioTrack.writeShortV23 )
    {
        GET_CONST_INT( AudioFormat.ENCODING_IEC61937, "ENCODING_IEC61937", false );
        jfields.AudioFormat.has_ENCODING_IEC61937 = field != NULL;
    }
    else
        jfields.AudioFormat.has_ENCODING_IEC61937 = false;

    GET_CONST_INT( AudioFormat.CHANNEL_OUT_MONO, "CHANNEL_OUT_MONO", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_STEREO, "CHANNEL_OUT_STEREO", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_FRONT_LEFT, "CHANNEL_OUT_FRONT_LEFT", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_FRONT_RIGHT, "CHANNEL_OUT_FRONT_RIGHT", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_5POINT1, "CHANNEL_OUT_5POINT1", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_BACK_LEFT, "CHANNEL_OUT_BACK_LEFT", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_BACK_RIGHT, "CHANNEL_OUT_BACK_RIGHT", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_FRONT_CENTER, "CHANNEL_OUT_FRONT_CENTER", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_LOW_FREQUENCY, "CHANNEL_OUT_LOW_FREQUENCY", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_BACK_CENTER, "CHANNEL_OUT_BACK_CENTER", true );
    GET_CONST_INT( AudioFormat.CHANNEL_OUT_SIDE_LEFT, "CHANNEL_OUT_SIDE_LEFT", false );
    if( field != NULL )
    {
        GET_CONST_INT( AudioFormat.CHANNEL_OUT_SIDE_RIGHT, "CHANNEL_OUT_SIDE_RIGHT", true );
        jfields.AudioFormat.has_CHANNEL_OUT_SIDE = true;
    } else
        jfields.AudioFormat.has_CHANNEL_OUT_SIDE = false;

    /* AudioManager class init */
    GET_CLASS( "android/media/AudioManager", true );
    GET_CONST_INT( AudioManager.ERROR_DEAD_OBJECT, "ERROR_DEAD_OBJECT", false );
    jfields.AudioManager.has_ERROR_DEAD_OBJECT = field != NULL;
    GET_CONST_INT( AudioManager.STREAM_MUSIC, "STREAM_MUSIC", true );

#undef CHECK_EXCEPTION
#undef GET_CLASS
#undef GET_ID
#undef GET_CONST_INT

    i_init_state = 1;
end:
    vlc_mutex_unlock( &lock );
    return i_init_state == 1 ? VLC_SUCCESS : VLC_EGENERIC;
}

static inline bool
check_exception( JNIEnv *env, aout_stream_t *stream,
                 const char *class, const char *method )
{
    if( (*env)->ExceptionCheck( env ) )
    {
        aout_sys_t *p_sys = stream->sys;

        p_sys->b_audiotrack_exception = true;
        p_sys->b_error = true;
        (*env)->ExceptionDescribe( env );
        (*env)->ExceptionClear( env );
        msg_Err( stream, "%s.%s triggered an exception !", class, method );
        return true;
    } else
        return false;
}

#define CHECK_EXCEPTION( class, method ) check_exception( env, stream, class, method )
#define CHECK_AT_EXCEPTION( method ) check_exception( env, stream, "AudioTrack", method )

#define JNI_CALL( what, obj, method, ... ) (*env)->what( env, obj, method, ##__VA_ARGS__ )

#define JNI_CALL_INT( obj, method, ... ) JNI_CALL( CallIntMethod, obj, method, ##__VA_ARGS__ )
#define JNI_CALL_BOOL( obj, method, ... ) JNI_CALL( CallBooleanMethod, obj, method, ##__VA_ARGS__ )
#define JNI_CALL_OBJECT( obj, method, ... ) JNI_CALL( CallObjectMethod, obj, method, ##__VA_ARGS__ )
#define JNI_CALL_VOID( obj, method, ... ) JNI_CALL( CallVoidMethod, obj, method, ##__VA_ARGS__ )
#define JNI_CALL_STATIC_INT( clazz, method, ... ) JNI_CALL( CallStaticIntMethod, clazz, method, ##__VA_ARGS__ )

#define JNI_AT_NEW( ... ) JNI_CALL( NewObject, jfields.AudioTrack.clazz, jfields.AudioTrack.ctor, ##__VA_ARGS__ )
#define JNI_AT_CALL_INT( method, ... ) JNI_CALL_INT( p_sys->p_audiotrack, jfields.AudioTrack.method, ##__VA_ARGS__ )
#define JNI_AT_CALL_BOOL( method, ... ) JNI_CALL_BOOL( p_sys->p_audiotrack, jfields.AudioTrack.method, ##__VA_ARGS__ )
#define JNI_AT_CALL_VOID( method, ... ) JNI_CALL_VOID( p_sys->p_audiotrack, jfields.AudioTrack.method, ##__VA_ARGS__ )
#define JNI_AT_CALL_STATIC_INT( method, ... ) JNI_CALL( CallStaticIntMethod, jfields.AudioTrack.clazz, jfields.AudioTrack.method, ##__VA_ARGS__ )

#define JNI_AUDIOTIMESTAMP_GET_LONG( field ) JNI_CALL( GetLongField, p_sys->timestamp.p_obj, jfields.AudioTimestamp.field )

static inline vlc_tick_t
frames_to_us( aout_sys_t *p_sys, uint64_t i_nb_frames )
{
    return  vlc_tick_from_samples(i_nb_frames, p_sys->fmt.i_rate);
}
#define FRAMES_TO_US(x) frames_to_us( p_sys, (x) )

static inline uint64_t
bytes_to_frames( aout_sys_t *p_sys, size_t i_bytes )
{
    return i_bytes * p_sys->fmt.i_frame_length / p_sys->fmt.i_bytes_per_frame;
}
#define BYTES_TO_FRAMES(x) bytes_to_frames( p_sys, (x) )
#define BYTES_TO_US(x) frames_to_us( p_sys, bytes_to_frames( p_sys, (x) ) )

static inline size_t
frames_to_bytes( aout_sys_t *p_sys, uint64_t i_frames )
{
    return i_frames * p_sys->fmt.i_bytes_per_frame / p_sys->fmt.i_frame_length;
}
#define FRAMES_TO_BYTES(x) frames_to_bytes( p_sys, (x) )

static inline uint64_t
us_to_frames( aout_sys_t *p_sys, vlc_tick_t i_us )
{
    return samples_from_vlc_tick( i_us, p_sys->fmt.i_rate );
}
#define US_TO_FRAMES(x) us_to_frames( p_sys, x)
#define US_TO_BYTES(x) frames_to_bytes( p_sys, us_to_frames( p_sys, x ) )

/**
 * Get the AudioTrack position
 *
 * The doc says that the position is reset to zero after flush but it's not
 * true for all devices or Android versions.
 */
static uint64_t
AudioTrack_getPlaybackHeadPosition( JNIEnv *env, aout_stream_t *stream )
{
    /* Android doc:
     * getPlaybackHeadPosition: Returns the playback head position expressed in
     * frames. Though the "int" type is signed 32-bits, the value should be
     * reinterpreted as if it is unsigned 32-bits. That is, the next position
     * after 0x7FFFFFFF is (int) 0x80000000. This is a continuously advancing
     * counter. It will wrap (overflow) periodically, for example approximately
     * once every 27:03:11 hours:minutes:seconds at 44.1 kHz. It is reset to
     * zero by flush(), reload(), and stop().
     */

    aout_sys_t *p_sys = stream->sys;
    uint32_t i_pos;

    /* int32_t to uint32_t */
    i_pos = 0xFFFFFFFFL & JNI_AT_CALL_INT( getPlaybackHeadPosition );

    /* uint32_t to uint64_t */
    if( p_sys->headpos.i_last > i_pos )
        p_sys->headpos.i_wrap_count++;
    p_sys->headpos.i_last = i_pos;
    return p_sys->headpos.i_last + (p_sys->headpos.i_wrap_count << 32);
}

/**
 * Reset AudioTrack position
 *
 * Called after flush, or start
 */
static void
AudioTrack_ResetWrapCount( JNIEnv *env, aout_stream_t *stream )
{
    (void) env;
    aout_sys_t *p_sys = stream->sys;

    p_sys->headpos.i_last = 0;
    p_sys->headpos.i_wrap_count = 0;

    p_sys->timestamp.i_frame_post_last = 0;
    p_sys->timestamp.i_frame_wrap_count = 0;
}

/**
 * Reset all AudioTrack positions and internal state
 */
static void
AudioTrack_Reset( JNIEnv *env, aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;

    AudioTrack_ResetWrapCount( env, stream );
    p_sys->i_samples_written = 0;
    p_sys->timing_report_last_written_bytes = 0;
    p_sys->timing_report_delay_bytes = 0;
    p_sys->first_pts = VLC_TICK_INVALID;
}

static vlc_tick_t
AudioTrack_GetLatencyUs( JNIEnv *env, aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;

    if( jfields.AudioTrack.getLatency )
    {
        int i_latency_ms = JNI_AT_CALL_INT( getLatency );

        /* getLatency() includes the latency due to AudioTrack buffer size,
         * AudioMixer (if any) and audio hardware driver. We only need the
         * audio hardware latency */
        if( i_latency_ms > 0 )
        {
            vlc_tick_t i_latency_us = VLC_TICK_FROM_MS( i_latency_ms )
                - FRAMES_TO_US( p_sys->i_max_audiotrack_samples );
            return i_latency_us >= 0 ? i_latency_us : 0;
        }
    }

    return 0;
}

struct role {
    char vlc[16];
};

static const struct role video_roles[] = {
    { "production" },
    { "test" },
    { "video" },
};

static int role_cmp(const void *r1, const void *entry) {
    const struct role *role = entry;
    return strcmp(r1, role->vlc);
}

static inline bool RoleHasVideoContent( char *role ) {
    char *res = bsearch(role, video_roles, ARRAY_SIZE(video_roles), sizeof(*video_roles), role_cmp);

    return res != NULL;
}

static jobject
AudioTrack_New21( JNIEnv *env, aout_stream_t *stream, unsigned int i_rate,
                  int i_channel_config, int i_format, int i_size,
                  jint session_id )
{
    jobject p_audiotrack = NULL;
    jobject p_aattr_builder = NULL;
    jobject p_audio_attributes = NULL;
    jobject p_afmt_builder = NULL;
    jobject p_audio_format = NULL;
    jobject ref;

    p_aattr_builder =
        JNI_CALL( NewObject,
                  jfields.AudioAttributes_Builder.clazz,
                  jfields.AudioAttributes_Builder.ctor );
    if( !p_aattr_builder )
        return NULL;

    bool hasVideo = false;

    char *vlc_role = var_InheritString(stream, "role");
    if (vlc_role != NULL) {
    hasVideo = RoleHasVideoContent(vlc_role);
    free(vlc_role);
    }

    ref = JNI_CALL_OBJECT( p_aattr_builder,
                         jfields.AudioAttributes_Builder.setUsage,
                         jfields.AudioAttributes.USAGE_MEDIA );
    (*env)->DeleteLocalRef( env, ref );

    ref = JNI_CALL_OBJECT( p_aattr_builder,
                         jfields.AudioAttributes_Builder.setContentType,
                         hasVideo ?
                         jfields.AudioAttributes.CONTENT_TYPE_MOVIE :
                         jfields.AudioAttributes.CONTENT_TYPE_MUSIC );
    (*env)->DeleteLocalRef( env, ref );

    p_audio_attributes =
        JNI_CALL_OBJECT( p_aattr_builder,
                         jfields.AudioAttributes_Builder.build );
    if( !p_audio_attributes )
        goto del_local_refs;

    p_afmt_builder = JNI_CALL( NewObject,
                               jfields.AudioFormat_Builder.clazz,
                               jfields.AudioFormat_Builder.ctor );
    if( !p_afmt_builder )
        goto del_local_refs;

    ref = JNI_CALL_OBJECT( p_afmt_builder,
                           jfields.AudioFormat_Builder.setChannelMask,
                           i_channel_config );
    if( CHECK_EXCEPTION( "AudioFormat.Builder", "setChannelMask" ) )
    {
        (*env)->DeleteLocalRef( env, ref );
        goto del_local_refs;
    }
    (*env)->DeleteLocalRef( env, ref );

    ref = JNI_CALL_OBJECT( p_afmt_builder,
                           jfields.AudioFormat_Builder.setEncoding,
                           i_format );
    if( CHECK_EXCEPTION( "AudioFormat.Builder", "setEncoding" ) )
    {
        (*env)->DeleteLocalRef( env, ref );
        goto del_local_refs;
    }
    (*env)->DeleteLocalRef( env, ref );

    ref = JNI_CALL_OBJECT( p_afmt_builder,
                           jfields.AudioFormat_Builder.setSampleRate,
                           i_rate );
    if( CHECK_EXCEPTION( "AudioFormat.Builder", "setSampleRate" ) )
    {
        (*env)->DeleteLocalRef( env, ref );
        goto del_local_refs;
    }
    (*env)->DeleteLocalRef( env, ref );

    p_audio_format = JNI_CALL_OBJECT( p_afmt_builder,
                                      jfields.AudioFormat_Builder.build );
    if(!p_audio_format)
        goto del_local_refs;

    p_audiotrack = JNI_AT_NEW( p_audio_attributes, p_audio_format, i_size,
                               jfields.AudioTrack.MODE_STREAM, session_id );

del_local_refs:
    (*env)->DeleteLocalRef( env, p_aattr_builder );
    (*env)->DeleteLocalRef( env, p_audio_attributes );
    (*env)->DeleteLocalRef( env, p_afmt_builder );
    (*env)->DeleteLocalRef( env, p_audio_format );
    return p_audiotrack;
}

static jobject
AudioTrack_NewLegacy( JNIEnv *env, aout_stream_t *stream, unsigned int i_rate,
                      int i_channel_config, int i_format, int i_size,
                      jint session_id )
{
    VLC_UNUSED( stream );
    return JNI_AT_NEW( jfields.AudioManager.STREAM_MUSIC, i_rate,
                       i_channel_config, i_format, i_size,
                       jfields.AudioTrack.MODE_STREAM, session_id );
}

/**
 * Create an Android AudioTrack.
 * returns -1 on error, 0 on success.
 */
static int
AudioTrack_New( JNIEnv *env, aout_stream_t *stream, unsigned int i_rate,
                int i_channel_config, int i_format, int i_size )
{
    aout_sys_t *p_sys = stream->sys;
    jint session_id = var_InheritInteger( stream, "audiotrack-session-id" );

    jobject p_audiotrack;
    if( jfields.AudioTrack.has_ctor_21 )
        p_audiotrack = AudioTrack_New21( env, stream, i_rate, i_channel_config,
                                         i_format, i_size, session_id );
    else
        p_audiotrack = AudioTrack_NewLegacy( env, stream, i_rate,
                                             i_channel_config, i_format, i_size,
                                             session_id );
    if( CHECK_AT_EXCEPTION( "AudioTrack<init>" ) || !p_audiotrack )
    {
        msg_Warn( stream, "AudioTrack Init failed" );
        return -1;
    }
    if( JNI_CALL_INT( p_audiotrack, jfields.AudioTrack.getState )
        != jfields.AudioTrack.STATE_INITIALIZED )
    {
        JNI_CALL_VOID( p_audiotrack, jfields.AudioTrack.release );
        (*env)->DeleteLocalRef( env, p_audiotrack );
        msg_Err( stream, "AudioTrack getState failed" );
        return -1;
    }

    p_sys->p_audiotrack = (*env)->NewGlobalRef( env, p_audiotrack );
    (*env)->DeleteLocalRef( env, p_audiotrack );
    if( !p_sys->p_audiotrack )
        return -1;

    if( !p_sys->b_passthrough )
    {
        if (session_id == 0 )
            session_id = JNI_AT_CALL_INT( getAudioSessionId );

        if( session_id != 0 )
            p_sys->p_dp = DynamicsProcessing_New( stream, session_id );
    }

    return 0;
}

/**
 * Destroy and recreate an Android AudioTrack using the same arguments.
 * returns -1 on error, 0 on success.
 */
static int
AudioTrack_Recreate( JNIEnv *env, aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;

    JNI_AT_CALL_VOID( release );
    (*env)->DeleteGlobalRef( env, p_sys->p_audiotrack );
    p_sys->p_audiotrack = NULL;

    int ret = AudioTrack_New( env, stream, p_sys->audiotrack_args.i_rate,
                              p_sys->audiotrack_args.i_channel_config,
                              p_sys->audiotrack_args.i_format,
                              p_sys->audiotrack_args.i_size );

    if( ret == 0 )
    {
        stream->volume_set(stream, p_sys->volume);
        if (p_sys->mute)
            stream->mute_set(stream, true);
    }

    return ret;
}

/**
 * Configure and create an Android AudioTrack.
 * returns -1 on configuration error, 0 on success.
 */
static int
AudioTrack_Create( JNIEnv *env, aout_stream_t *stream,
                   unsigned int i_rate,
                   int i_format,
                   uint16_t i_physical_channels )
{
    aout_sys_t *p_sys = stream->sys;
    int i_size, i_min_buffer_size, i_channel_config;

    switch( i_physical_channels )
    {
        case AOUT_CHANS_7_1:
            /* bitmask of CHANNEL_OUT_7POINT1 doesn't correspond to 5POINT1 and
             * SIDES */
            i_channel_config = jfields.AudioFormat.CHANNEL_OUT_5POINT1 |
                               jfields.AudioFormat.CHANNEL_OUT_SIDE_LEFT |
                               jfields.AudioFormat.CHANNEL_OUT_SIDE_RIGHT;
            break;
        case AOUT_CHANS_5_1:
            i_channel_config = jfields.AudioFormat.CHANNEL_OUT_5POINT1;
            break;
        case AOUT_CHAN_LEFT:
            i_channel_config = jfields.AudioFormat.CHANNEL_OUT_MONO;
            break;
        case AOUT_CHANS_STEREO:
            i_channel_config = jfields.AudioFormat.CHANNEL_OUT_STEREO;
            break;
        default:
            vlc_assert_unreachable();
    }

    i_min_buffer_size = JNI_AT_CALL_STATIC_INT( getMinBufferSize, i_rate,
                                                i_channel_config, i_format );
    if( i_min_buffer_size <= 0 )
    {
        msg_Warn( stream, "getMinBufferSize returned an invalid size" ) ;
        return -1;
    }
    i_size = i_min_buffer_size * 2;

    /* Clip the buffer size to a maximum safe size since audiotrack init will
     * fail of the requested size is too big. */
    const int i_max_size = US_TO_BYTES( AUDIOTRACK_MAX_BUFFER_US );
    if( i_size > i_max_size )
        i_size = i_max_size;

    /* create AudioTrack object */
    if( AudioTrack_New( env, stream, i_rate, i_channel_config,
                        i_format , i_size ) != 0 )
        return -1;

    p_sys->audiotrack_args.i_rate = i_rate;
    p_sys->audiotrack_args.i_channel_config = i_channel_config;
    p_sys->audiotrack_args.i_format = i_format;
    p_sys->audiotrack_args.i_size = i_size;

    return 0;
}

static void
AudioTrack_ConsumeFrame(aout_stream_t *stream, vlc_frame_t *f, size_t size)
{
    aout_sys_t *p_sys = stream->sys;
    assert(f != NULL && f == p_sys->frame_chain);

    f->i_buffer -= size;
    if (f->i_buffer > 0)
    {
        f->p_buffer += size;
        return;
    }

    p_sys->frame_chain = f->p_next;
    if (p_sys->frame_chain == NULL)
        p_sys->frame_last = &p_sys->frame_chain;

    vlc_frame_Release(f);
}

static int
AudioTrack_AllocJArray(JNIEnv *env, aout_stream_t *stream, unsigned sample_size,
                       jarray (*new)(JNIEnv *env, jsize size))
{
    aout_sys_t *p_sys = stream->sys;

    size_t size = US_TO_BYTES(JARRAY_SIZE_US) / sample_size;

    jobject array = new(env, size);
    if (array == NULL)
        return jfields.AudioTrack.ERROR;

    p_sys->jbuffer.array = (*env)->NewGlobalRef(env, array);
    (*env)->DeleteLocalRef(env, array);
    if (p_sys->jbuffer.array == NULL)
        return jfields.AudioTrack.ERROR;

    p_sys->jbuffer.maxsize = size;
    p_sys->jbuffer.size = 0;
    p_sys->jbuffer.offset = 0;

    return 0;
}

/**
 * Non blocking write function, run from AudioTrack_Thread.
 * Do a calculation between current position and audiotrack position and assure
 * that we won't wait in AudioTrack.write() method.
 */
static int
AudioTrack_WriteByteArray( JNIEnv *env, aout_stream_t *stream, bool b_force )
{
    aout_sys_t *p_sys = stream->sys;
    uint64_t i_samples;
    uint64_t i_audiotrack_pos;
    uint64_t i_samples_pending;
    int ret;

    if (p_sys->jbuffer.size == 0)
    {
        vlc_frame_t *f = p_sys->frame_chain;
        assert(f != NULL);

        size_t size = f->i_buffer > p_sys->jbuffer.maxsize ?
                      p_sys->jbuffer.maxsize : f->i_buffer;
        p_sys->jbuffer.size = size;
        p_sys->jbuffer.offset = 0;
        (*env)->SetByteArrayRegion(env, p_sys->jbuffer.array, 0,
                                   size, (jbyte *)f->p_buffer);
        AudioTrack_ConsumeFrame(stream, f, size);
    }
    assert(p_sys->jbuffer.size > 0);
    assert(p_sys->jbuffer.size > p_sys->jbuffer.offset);

    i_audiotrack_pos = AudioTrack_getPlaybackHeadPosition( env, stream );

    assert( i_audiotrack_pos <= p_sys->i_samples_written );
    if( i_audiotrack_pos > p_sys->i_samples_written )
    {
        msg_Err( stream, "audiotrack position is ahead. Should NOT happen" );
        p_sys->i_samples_written = 0;
        p_sys->b_error = true;
        return 0;
    }
    i_samples_pending = p_sys->i_samples_written - i_audiotrack_pos;

    /* check if audiotrack buffer is not full before writing on it. */
    if( b_force )
    {
        msg_Warn( stream, "Force write. It may block..." );
        i_samples_pending = 0;
    } else if( i_samples_pending >= p_sys->i_max_audiotrack_samples )
        return 0;

    i_samples = __MIN( p_sys->i_max_audiotrack_samples - i_samples_pending,
                       BYTES_TO_FRAMES(p_sys->jbuffer.size - p_sys->jbuffer.offset));

    ret = JNI_AT_CALL_INT( write, p_sys->jbuffer.array, p_sys->jbuffer.offset,
                           FRAMES_TO_BYTES( i_samples ) );
    if (ret > 0)
    {
        p_sys->jbuffer.offset += ret;
        if (p_sys->jbuffer.offset == p_sys->jbuffer.size)
            p_sys->jbuffer.size = 0;
    }

    return ret;
}

/**
 * Non blocking write function for Android M and after, run from
 * AudioTrack_Thread. It calls a new write method with WRITE_NON_BLOCKING
 * flags.
 */
static int
AudioTrack_WriteByteArrayV23(JNIEnv *env, aout_stream_t *stream)
{
    aout_sys_t *p_sys = stream->sys;
    int ret;

    if (p_sys->jbuffer.size == 0)
    {
        vlc_frame_t *f = p_sys->frame_chain;
        assert(f != NULL);

        size_t size = f->i_buffer > p_sys->jbuffer.maxsize ?
                      p_sys->jbuffer.maxsize : f->i_buffer;
        p_sys->jbuffer.size = size;
        p_sys->jbuffer.offset = 0;
        (*env)->SetByteArrayRegion(env, p_sys->jbuffer.array, 0,
                                   size, (jbyte *)f->p_buffer);
        AudioTrack_ConsumeFrame(stream, f, size);
    }
    assert(p_sys->jbuffer.size > 0);
    assert(p_sys->jbuffer.size > p_sys->jbuffer.offset);

    ret = JNI_AT_CALL_INT( writeV23, p_sys->jbuffer.array,
                           p_sys->jbuffer.offset,
                           p_sys->jbuffer.size - p_sys->jbuffer.offset,
                           jfields.AudioTrack.WRITE_NON_BLOCKING );
    if (ret > 0)
    {
        p_sys->jbuffer.offset += ret;
        if (p_sys->jbuffer.offset == p_sys->jbuffer.size)
            p_sys->jbuffer.size = 0;
    }

    return ret;
}

/**
 * Non blocking short write function for Android M and after, run from
 * AudioTrack_Thread. It calls a new write method with WRITE_NON_BLOCKING
 * flags.
 */
static int
AudioTrack_WriteShortArrayV23(JNIEnv *env, aout_stream_t *stream)
{
    aout_sys_t *p_sys = stream->sys;
    int ret;

    if (p_sys->jbuffer.size == 0)
    {
        vlc_frame_t *f = p_sys->frame_chain;
        assert(f != NULL);

        size_t buffer_size_short = f->i_buffer / 2;
        size_t size = buffer_size_short > p_sys->jbuffer.maxsize ?
                      p_sys->jbuffer.maxsize : buffer_size_short;
        p_sys->jbuffer.size = size;
        p_sys->jbuffer.offset = 0;
        (*env)->SetShortArrayRegion(env, p_sys->jbuffer.array, 0,
                                    size, (jshort *)f->p_buffer);
        AudioTrack_ConsumeFrame(stream, f, size * 2);
    }
    assert(p_sys->jbuffer.size > 0);
    assert(p_sys->jbuffer.size > p_sys->jbuffer.offset);

    ret = JNI_AT_CALL_INT( writeShortV23, p_sys->jbuffer.array,
                           p_sys->jbuffer.offset,
                           p_sys->jbuffer.size - p_sys->jbuffer.offset,
                           jfields.AudioTrack.WRITE_NON_BLOCKING );
    if (ret > 0)
    {
        p_sys->jbuffer.offset += ret;
        if (p_sys->jbuffer.offset == p_sys->jbuffer.size)
            p_sys->jbuffer.size = 0;
        ret *= 2;
    }

    return ret;
}

/**
 * Non blocking play float function for Lollipop and after, run from
 * AudioTrack_Thread. It calls a new write method with WRITE_NON_BLOCKING
 * flags.
 */
static int
AudioTrack_WriteFloatArray(JNIEnv *env, aout_stream_t *stream)
{
    aout_sys_t *p_sys = stream->sys;
    int ret;

    if (p_sys->jbuffer.size == 0)
    {
        vlc_frame_t *f = p_sys->frame_chain;
        assert(f != NULL);

        size_t buffer_size_float = f->i_buffer / 4;
        size_t size = buffer_size_float > p_sys->jbuffer.maxsize ?
                      p_sys->jbuffer.maxsize : buffer_size_float;
        p_sys->jbuffer.size = size;
        p_sys->jbuffer.offset = 0;
        (*env)->SetFloatArrayRegion(env, p_sys->jbuffer.array, 0,
                                    size, (jfloat *)f->p_buffer);
        AudioTrack_ConsumeFrame(stream, f, size * 4);
    }
    assert(p_sys->jbuffer.size > 0);
    assert(p_sys->jbuffer.size > p_sys->jbuffer.offset);

    ret = JNI_AT_CALL_INT(writeFloat, p_sys->jbuffer.array,
                          p_sys->jbuffer.offset,
                          p_sys->jbuffer.size - p_sys->jbuffer.offset,
                          jfields.AudioTrack.WRITE_NON_BLOCKING);
    if (ret > 0)
    {
        p_sys->jbuffer.offset += ret;
        if (p_sys->jbuffer.offset == p_sys->jbuffer.size)
            p_sys->jbuffer.size = 0;
        ret *= 4;
    }

    return ret;
}

static int
AudioTrack_Write( JNIEnv *env, aout_stream_t *stream, bool b_force )
{
    aout_sys_t *p_sys = stream->sys;
    int i_ret;

    switch( p_sys->i_write_type )
    {
    case WRITE_BYTEARRAYV23:
        i_ret = AudioTrack_WriteByteArrayV23(env, stream);
        break;
    case WRITE_SHORTARRAYV23:
        i_ret = AudioTrack_WriteShortArrayV23(env, stream);
        break;
    case WRITE_BYTEARRAY:
        i_ret = AudioTrack_WriteByteArray(env, stream, b_force);
        break;
    case WRITE_FLOATARRAY:
        i_ret = AudioTrack_WriteFloatArray(env, stream);
        break;
    default:
        vlc_assert_unreachable();
    }

    if( i_ret < 0 ) {
        if( jfields.AudioManager.has_ERROR_DEAD_OBJECT
            && i_ret == jfields.AudioManager.ERROR_DEAD_OBJECT )
        {
            msg_Warn( stream, "ERROR_DEAD_OBJECT: "
                              "try recreating AudioTrack" );
            if( ( i_ret = AudioTrack_Recreate( env, stream ) ) == 0 )
            {
                AudioTrack_Reset( env, stream );
                JNI_AT_CALL_VOID( play );
                CHECK_AT_EXCEPTION( "play" );
            }
        } else
        {
            const char *str;
            if( i_ret == jfields.AudioTrack.ERROR_INVALID_OPERATION )
                str = "ERROR_INVALID_OPERATION";
            else if( i_ret == jfields.AudioTrack.ERROR_BAD_VALUE )
                str = "ERROR_BAD_VALUE";
            else
                str = "ERROR";
            msg_Err( stream, "Write failed: %s", str );
            p_sys->b_error = true;
        }
    } else
        p_sys->i_samples_written += BYTES_TO_FRAMES( i_ret );
    return i_ret;
}

static int
AudioTrack_ReportTiming( JNIEnv *env, aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;

    if( p_sys->timestamp.p_obj != NULL
     && JNI_AT_CALL_BOOL( getTimestamp, p_sys->timestamp.p_obj ) )
    {
        vlc_tick_t frame_date_us =
            VLC_TICK_FROM_NS(JNI_AUDIOTIMESTAMP_GET_LONG( nanoTime ));
        jlong frame_post_last = JNI_AUDIOTIMESTAMP_GET_LONG( framePosition );
        if( frame_post_last == 0 )
            return VLC_EGENERIC;

        /* the low-order 32 bits of position is in wrapping frame units
         * similar to AudioTrack#getPlaybackHeadPosition. */
        if( p_sys->timestamp.i_frame_post_last > frame_post_last )
            p_sys->timestamp.i_frame_wrap_count++;
        p_sys->timestamp.i_frame_post_last = frame_post_last;
        uint64_t frame_pos = frame_post_last
                           + (p_sys->timestamp.i_frame_wrap_count << 32);

        aout_stream_TimingReport( stream, frame_date_us,
                                  FRAMES_TO_US( frame_pos ) +
                                  p_sys->first_pts );
        return VLC_SUCCESS;
    }

    vlc_tick_t now = vlc_tick_now();
    vlc_tick_t frame_us = FRAMES_TO_US( AudioTrack_getPlaybackHeadPosition( env, stream ) );
    if( frame_us == 0 )
        return VLC_EGENERIC;

    vlc_tick_t latency_us = AudioTrack_GetLatencyUs( env, stream );
    frame_us -= latency_us;

    if( frame_us <= 0 )
        return VLC_EGENERIC;
    aout_stream_TimingReport( stream, now, frame_us + p_sys->first_pts );

    return VLC_SUCCESS;
}

/**
 * This thread will play the data coming from the jbuffer buffer.
 */
static void *
AudioTrack_Thread( void *p_data )
{
    aout_stream_t *stream = p_data;
    aout_sys_t *p_sys = stream->sys;
    JNIEnv *env = GET_ENV();
    vlc_tick_t i_last_time_blocked = 0;

    vlc_thread_set_name("vlc-audiotrack");

    if( !env )
        return NULL;

    for( ;; )
    {
        int i_ret = 0;
        bool b_forced;

        vlc_mutex_lock( &p_sys->lock );

        /* Wait for not paused state */
        while( p_sys->b_thread_running && p_sys->b_thread_paused )
        {
            i_last_time_blocked = 0;
            vlc_cond_wait( &p_sys->thread_cond, &p_sys->lock );
        }

        /* Wait for more data in the jbuffer buffer */
        while (p_sys->b_thread_running
            && p_sys->jbuffer.size == 0
            && p_sys->frame_chain == NULL)
            vlc_cond_wait( &p_sys->thread_cond, &p_sys->lock );

        if( !p_sys->b_thread_running || p_sys->b_error )
        {
            vlc_mutex_unlock( &p_sys->lock );
            break;
        }

        /* HACK: AudioFlinger can drop frames without notifying us and there is
         * no way to know it. If it happens, i_audiotrack_pos won't move and
         * the current code will be stuck because it'll assume that audiotrack
         * internal buffer is full when it's not. It may happen only after
         * Android 4.4.2 if we send frames too quickly. To fix this issue,
         * force the writing of the buffer after a certain delay. */
        if( i_last_time_blocked != 0 )
            b_forced = vlc_tick_now() - i_last_time_blocked >
                       FRAMES_TO_US( p_sys->i_max_audiotrack_samples ) * 2;
        else
            b_forced = false;

        i_ret = AudioTrack_Write( env, stream, b_forced );
        if( i_ret >= 0 )
        {
            if( i_ret == 0 )
            {
                vlc_tick_t i_now = vlc_tick_now();

                /* cf. b_forced HACK comment */
                if( p_sys->i_write_type == WRITE_BYTEARRAY && i_last_time_blocked == 0 )
                    i_last_time_blocked = i_now;

                /* Wait for free space in Audiotrack internal buffer */
                vlc_tick_t i_play_deadline = i_now + __MAX( 10000,
                        FRAMES_TO_US( p_sys->i_max_audiotrack_samples / 5 ) );

                while( p_sys->b_thread_running && i_ret == 0 )
                    i_ret = vlc_cond_timedwait( &p_sys->thread_cond, &p_sys->lock,
                                                i_play_deadline );

            }
            else
            {
                i_last_time_blocked = 0;

                if( !p_sys->b_passthrough )
                {
                    p_sys->timing_report_last_written_bytes += i_ret;

                    if( p_sys->timing_report_last_written_bytes >=
                            p_sys->timing_report_delay_bytes
                     && AudioTrack_ReportTiming( env, stream ) == VLC_SUCCESS )
                    {
                        p_sys->timing_report_last_written_bytes = 0;
                        /* From now on, fetch the timestamp every 1 seconds */
                        p_sys->timing_report_delay_bytes =
                            US_TO_BYTES( TIMING_REPORT_DELAY_TICKS );
                    }
                }
            }
        }

        vlc_mutex_unlock( &p_sys->lock );
    }

    return NULL;
}

static int
ConvertFromIEC61937( aout_stream_t *stream, block_t *p_buffer )
{
    /* This function is only used for Android API 23 when AudioTrack is
     * configured with ENCODING_ AC3/E_AC3/DTS. In that case, only the codec
     * data is needed (without the IEC61937 encapsulation). This function
     * recovers the codec data from an EC61937 frame. It is the opposite of the
     * code found in converter/tospdif.c. We could also request VLC core to
     * send us the codec data directly, but in that case, we wouldn't benefit
     * from the eac3 block merger of tospdif.c. */

    VLC_UNUSED( stream );
    uint8_t i_length_mul;

    if( p_buffer->i_buffer < 6 )
        return -1;

    switch( GetWBE( &p_buffer->p_buffer[4] ) & 0xFF )
    {
        case 0x01: /* IEC61937_AC3 */
            i_length_mul = 8;
            break;
        case 0x15: /* IEC61937_EAC3 */
            i_length_mul = 1;
            break;
        case 0x0B: /* IEC61937_DTS1 */
        case 0x0C: /* IEC61937_DTS2 */
        case 0x0D: /* IEC61937_DTS3 */
            i_length_mul = 8;
            break;
        case 0x11: /* IEC61937_DTSHD */
            i_length_mul = 1;
            break;
        default:
            vlc_assert_unreachable();
    }
    uint16_t i_length = GetWBE( &p_buffer->p_buffer[6] );
    if( i_length == 0 )
        return -1;

    i_length /= i_length_mul;
    if( i_length > p_buffer->i_buffer - 8 )
        return -1;

    p_buffer->p_buffer += 8; /* SPDIF_HEADER_SIZE */
    p_buffer->i_buffer = i_length;

    return 0;
}

static void
Play( aout_stream_t *stream, block_t *p_buffer, vlc_tick_t i_date )
{
    JNIEnv *env = NULL;
    aout_sys_t *p_sys = stream->sys;

    if( p_sys->b_passthrough && p_sys->fmt.i_format == VLC_CODEC_SPDIFB
     && ConvertFromIEC61937( stream, p_buffer ) != 0 )
    {
        block_Release(p_buffer);
        return;
    }

    vlc_mutex_lock( &p_sys->lock );

    if( p_sys->b_error || !( env = GET_ENV() ) )
    {
        block_Release( p_buffer );
        goto bailout;
    }

    if( p_sys->i_chans_to_reorder )
       aout_ChannelReorder( p_buffer->p_buffer, p_buffer->i_buffer,
                            p_sys->i_chans_to_reorder, p_sys->p_chan_table,
                            p_sys->fmt.i_format );

    if( p_sys->first_pts == VLC_TICK_INVALID )
        p_sys->first_pts = p_buffer->i_pts;

    vlc_frame_ChainLastAppend(&p_sys->frame_last, p_buffer);
    vlc_cond_signal(&p_sys->thread_cond);

bailout:
    vlc_mutex_unlock( &p_sys->lock );
    (void) i_date;
}

static void
Pause( aout_stream_t *stream, bool b_pause, vlc_tick_t i_date )
{
    aout_sys_t *p_sys = stream->sys;
    JNIEnv *env;
    VLC_UNUSED( i_date );

    vlc_mutex_lock( &p_sys->lock );

    if( p_sys->b_error || !( env = GET_ENV() ) )
        goto bailout;

    if( b_pause )
    {
        p_sys->b_thread_paused = true;
        JNI_AT_CALL_VOID( pause );
        CHECK_AT_EXCEPTION( "pause" );
    } else
    {
        p_sys->b_thread_paused = false;
        JNI_AT_CALL_VOID( play );
        CHECK_AT_EXCEPTION( "play" );
    }

bailout:
    vlc_mutex_unlock( &p_sys->lock );
}

static void
Flush( aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;
    JNIEnv *env;

    vlc_mutex_lock( &p_sys->lock );

    p_sys->jbuffer.size = p_sys->jbuffer.offset = 0;
    vlc_frame_ChainRelease(p_sys->frame_chain);
    p_sys->frame_chain = NULL;
    p_sys->frame_last = &p_sys->frame_chain;

    if( p_sys->b_error || !( env = GET_ENV() ) )
        goto bailout;

    /* Android doc:
     * stop(): Stops playing the audio data. When used on an instance created
     * in MODE_STREAM mode, audio will stop playing after the last buffer that
     * was written has been played. For an immediate stop, use pause(),
     * followed by flush() to discard audio data that hasn't been played back
     * yet.
     *
     * flush(): Flushes the audio data currently queued for playback. Any data
     * that has not been played back will be discarded.  No-op if not stopped
     * or paused, or if the track's creation mode is not MODE_STREAM.
     */
    JNI_AT_CALL_VOID( pause );
    if( CHECK_AT_EXCEPTION( "pause" ) )
        goto bailout;
    JNI_AT_CALL_VOID( flush );

    /* HACK: Before Android 4.4, the head position is not reset to zero and is
     * still moving after a flush or a stop. This prevents to get a precise
     * head position and there is no way to know when it stabilizes. Therefore
     * recreate an AudioTrack object in that case. The AudioTimestamp class was
     * added in API Level 19, so if this class is not found, the Android
     * Version is 4.3 or before */
    if( !jfields.AudioTimestamp.clazz && p_sys->i_samples_written > 0 )
    {
        if( AudioTrack_Recreate( env, stream ) != 0 )
        {
            p_sys->b_error = true;
            goto bailout;
        }
    }
    AudioTrack_Reset( env, stream );
    JNI_AT_CALL_VOID( play );
    CHECK_AT_EXCEPTION( "play" );

bailout:
    vlc_mutex_unlock( &p_sys->lock );
}

static void
AudioTrack_SetVolume( JNIEnv *env, aout_stream_t *stream, float volume )
{
    aout_sys_t *p_sys = stream->sys;

    if( jfields.AudioTrack.setVolume )
    {
        JNI_AT_CALL_INT( setVolume, volume );
        CHECK_AT_EXCEPTION( "setVolume" );
    } else
    {
        JNI_AT_CALL_INT( setStereoVolume, volume, volume );
        CHECK_AT_EXCEPTION( "setStereoVolume" );
    }
}

static void
VolumeSet( aout_stream_t *stream, float volume )
{
    aout_sys_t *p_sys = stream->sys;
    JNIEnv *env;
    float gain = 1.0f;

    p_sys->volume = volume;
    if (volume > 1.f)
    {
        gain = volume * volume * volume;
        volume = 1.f;
    }

    if( !p_sys->b_error && p_sys->p_audiotrack != NULL && ( env = GET_ENV() ) )
    {
        AudioTrack_SetVolume( env, stream, volume );

        /* Apply gain > 1.0f via DynamicsProcessing if possible */
        if( p_sys->p_dp != NULL )
        {
            if( gain <= 1.0f )
            {
                /* DynamicsProcessing is not needed anymore (using AudioTrack
                 * volume) */
                DynamicsProcessing_Disable( stream, p_sys->p_dp );
            }
            else
            {
                int ret = DynamicsProcessing_SetVolume( stream, p_sys->p_dp, gain );

                if( ret == VLC_SUCCESS )
                    gain = 1.0; /* reset sw gain */
                else
                    msg_Warn( stream, "failed to set gain via DynamicsProcessing, fallback to sw gain");
            }
        }
    }

    aout_stream_GainRequest(stream, gain);
}

static void
MuteSet( aout_stream_t *stream, bool mute )
{
    aout_sys_t *p_sys = stream->sys;
    JNIEnv *env;
    p_sys->mute = mute;

    if( !p_sys->b_error && p_sys->p_audiotrack != NULL && ( env = GET_ENV() ) )
        AudioTrack_SetVolume( env, stream, mute ? 0.0f : p_sys->volume );
}

static int GetPassthroughFmt( bool compat, audio_sample_format_t *fmt, int *at_format )
{
    if( !compat && jfields.AudioFormat.has_ENCODING_IEC61937 )
    {
        *at_format = jfields.AudioFormat.ENCODING_IEC61937;
        switch( fmt->i_format )
        {
            case VLC_CODEC_TRUEHD:
            case VLC_CODEC_MLP:
                fmt->i_rate = 192000;
                fmt->i_bytes_per_frame = 16;

                /* AudioFormat.ENCODING_IEC61937 documentation says that the
                 * channel layout must be stereo. Well, not for TrueHD
                 * apparently */
                fmt->i_physical_channels = AOUT_CHANS_7_1;
                break;
            case VLC_CODEC_DTS:
                fmt->i_bytes_per_frame = 4;
                fmt->i_physical_channels = AOUT_CHANS_STEREO;
                break;
            case VLC_CODEC_DTSHD:
                fmt->i_bytes_per_frame = 4;
                fmt->i_physical_channels = AOUT_CHANS_STEREO;
                fmt->i_rate = 192000;
                fmt->i_bytes_per_frame = 16;
                break;
            case VLC_CODEC_EAC3:
                fmt->i_rate = 192000;
            case VLC_CODEC_A52:
                fmt->i_physical_channels = AOUT_CHANS_STEREO;
                fmt->i_bytes_per_frame = 4;
                break;
            default:
                return VLC_EGENERIC;
        }
        fmt->i_frame_length = 1;
        fmt->i_channels = aout_FormatNbChannels( fmt );
        fmt->i_format = VLC_CODEC_SPDIFL;
    }
    else
    {
        if( vlc_android_AudioFormat_FourCCToEncoding( fmt->i_format, at_format )
                != VLC_SUCCESS )
            return VLC_EGENERIC;
        fmt->i_bytes_per_frame = 4;
        fmt->i_frame_length = 1;
        fmt->i_physical_channels = AOUT_CHANS_STEREO;
        fmt->i_channels = 2;
        fmt->i_format = VLC_CODEC_SPDIFB;
    }

    return VLC_SUCCESS;
}

static int
StartPassthrough( JNIEnv *env, aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;

    /* Try ENCODING_IEC61937 first, then fallback to ENCODING_[AC3|DTS|...] */
    unsigned nb_fmt = jfields.AudioFormat.has_ENCODING_IEC61937 ? 2 : 1;
    int i_ret;
    for( unsigned i = 0; i < nb_fmt; ++i )
    {
        int i_at_format;
        bool compat = i == 1;
        audio_sample_format_t fmt = p_sys->fmt;

        i_ret = GetPassthroughFmt( compat, &fmt, &i_at_format );
        if( i_ret != VLC_SUCCESS )
            return i_ret;

        p_sys->b_passthrough = true;
        i_ret = AudioTrack_Create( env, stream, fmt.i_rate, i_at_format,
                                   fmt.i_physical_channels );

        if( i_ret == VLC_SUCCESS )
        {
            msg_Dbg( stream, "Using passthrough format: %d", i_at_format );
            p_sys->i_chans_to_reorder = 0;
            p_sys->fmt = fmt;
            return VLC_SUCCESS;
        }
    }

    p_sys->b_passthrough = false;
    msg_Warn( stream, "SPDIF configuration failed" );
    return VLC_EGENERIC;
}

static int
StartPCM( JNIEnv *env, aout_stream_t *stream, unsigned i_max_channels )
{
    aout_sys_t *p_sys = stream->sys;
    unsigned i_nb_channels;
    int i_at_format, i_ret;

    if (jfields.AudioTrack.getNativeOutputSampleRate)
        p_sys->fmt.i_rate =
            JNI_AT_CALL_STATIC_INT( getNativeOutputSampleRate,
                                    jfields.AudioManager.STREAM_MUSIC );
    else
        p_sys->fmt.i_rate = VLC_CLIP( p_sys->fmt.i_rate, 4000, 48000 );

    do
    {
        /* We can only accept U8, S16N, FL32, and AC3 */
        switch( p_sys->fmt.i_format )
        {
        case VLC_CODEC_U8:
            i_at_format = jfields.AudioFormat.ENCODING_PCM_8BIT;
            break;
        case VLC_CODEC_S16N:
            i_at_format = jfields.AudioFormat.ENCODING_PCM_16BIT;
            break;
        case VLC_CODEC_FL32:
            if( jfields.AudioFormat.has_ENCODING_PCM_FLOAT )
                i_at_format = jfields.AudioFormat.ENCODING_PCM_FLOAT;
            else
            {
                p_sys->fmt.i_format = VLC_CODEC_S16N;
                i_at_format = jfields.AudioFormat.ENCODING_PCM_16BIT;
            }
            break;
        default:
            p_sys->fmt.i_format = VLC_CODEC_S16N;
            i_at_format = jfields.AudioFormat.ENCODING_PCM_16BIT;
            break;
        }

        /* Android AudioTrack supports only mono, stereo, 5.1 and 7.1.
         * Android will downmix to stereo if audio output doesn't handle 5.1 or 7.1
         */

        i_nb_channels = aout_FormatNbChannels( &p_sys->fmt );
        if( i_nb_channels == 0 )
            return VLC_EGENERIC;
        if( AOUT_FMT_LINEAR( &p_sys->fmt ) )
            i_nb_channels = __MIN( i_max_channels, i_nb_channels );
        if( i_nb_channels > 5 )
        {
            if( i_nb_channels > 7 && jfields.AudioFormat.has_CHANNEL_OUT_SIDE )
                p_sys->fmt.i_physical_channels = AOUT_CHANS_7_1;
            else
                p_sys->fmt.i_physical_channels = AOUT_CHANS_5_1;
        } else
        {
            if( i_nb_channels == 1 )
                p_sys->fmt.i_physical_channels = AOUT_CHAN_LEFT;
            else
                p_sys->fmt.i_physical_channels = AOUT_CHANS_STEREO;
        }

        /* Try to create an AudioTrack with the most advanced channel and
         * format configuration. If AudioTrack_Create fails, try again with a
         * less advanced format (PCM S16N). If it fails again, try again with
         * Stereo channels. */
        i_ret = AudioTrack_Create( env, stream, p_sys->fmt.i_rate, i_at_format,
                                   p_sys->fmt.i_physical_channels );
        if( i_ret != 0 )
        {
            if( p_sys->fmt.i_format == VLC_CODEC_FL32 )
            {
                msg_Warn( stream, "FL32 configuration failed, "
                                  "fallback to S16N PCM" );
                p_sys->fmt.i_format = VLC_CODEC_S16N;
            }
            else if( p_sys->fmt.i_physical_channels & AOUT_CHANS_5_1 )
            {
                msg_Warn( stream, "5.1 or 7.1 configuration failed, "
                                  "fallback to Stereo" );
                p_sys->fmt.i_physical_channels = AOUT_CHANS_STEREO;
            }
            else
                break;
        }
    } while( i_ret != 0 );

    if( i_ret != VLC_SUCCESS )
        return i_ret;

    uint32_t p_chans_out[AOUT_CHAN_MAX];
    AndroidDevice_GetChanOrder( p_sys->fmt.i_physical_channels, p_chans_out,
                                AOUT_CHAN_MAX );
    p_sys->i_chans_to_reorder =
        aout_CheckChannelReorder( NULL, p_chans_out,
                                  p_sys->fmt.i_physical_channels,
                                  p_sys->p_chan_table );
    aout_FormatPrepare( &p_sys->fmt );
    return VLC_SUCCESS;
}

static void
Stop( aout_stream_t *stream )
{
    aout_sys_t *p_sys = stream->sys;
    JNIEnv *env;

    if( !( env = GET_ENV() ) )
        return;

    /* Stop the AudioTrack thread */
    vlc_mutex_lock( &p_sys->lock );
    if( p_sys->b_thread_running )
    {
        p_sys->b_thread_running = false;
        vlc_cond_signal( &p_sys->thread_cond );
        vlc_mutex_unlock( &p_sys->lock );
        vlc_join( p_sys->thread, NULL );
    }
    else
        vlc_mutex_unlock(  &p_sys->lock );

    if (p_sys->jbuffer.array != NULL)
        (*env)->DeleteGlobalRef(env, p_sys->jbuffer.array);

    /* Release the AudioTrack object */
    if( p_sys->p_audiotrack )
    {
        if( !p_sys->b_audiotrack_exception )
        {
            JNI_AT_CALL_VOID( stop );
            if( !CHECK_AT_EXCEPTION( "stop" ) )
                JNI_AT_CALL_VOID( release );
        }
        (*env)->DeleteGlobalRef( env, p_sys->p_audiotrack );
    }

    if( p_sys->p_dp != NULL )
        DynamicsProcessing_Delete( stream, p_sys->p_dp );

    /* Release the timestamp object */
    if( p_sys->timestamp.p_obj )
        (*env)->DeleteGlobalRef( env, p_sys->timestamp.p_obj );

    free( p_sys );
}

static int
Start( aout_stream_t *stream, audio_sample_format_t *restrict p_fmt,
       enum android_audio_device_type adev )
{
    JNIEnv *env;
    int i_ret;
    bool b_try_passthrough;
    unsigned i_max_channels;

    if( adev == ANDROID_AUDIO_DEVICE_ENCODED )
    {
        b_try_passthrough = true;
        i_max_channels = ANDROID_AUDIO_DEVICE_MAX_CHANNELS;
    }
    else
    {
        b_try_passthrough = false;
        i_max_channels = adev == ANDROID_AUDIO_DEVICE_STEREO ? 2 : ANDROID_AUDIO_DEVICE_MAX_CHANNELS;
    }

    if( !( env = GET_ENV() ) )
        return VLC_EGENERIC;

    AudioTrack_InitJNI(VLC_OBJECT(stream));

    aout_sys_t *p_sys = stream->sys = calloc( 1, sizeof (aout_sys_t) );

    if( unlikely( p_sys == NULL ) )
        return VLC_ENOMEM;

    vlc_mutex_init(&p_sys->lock);
    vlc_cond_init(&p_sys->thread_cond);
    p_sys->jbuffer.array = NULL;

    p_sys->volume = 1.0f;
    p_sys->mute = false;

    p_sys->fmt = *p_fmt;

    aout_FormatPrint( stream, "VLC is looking for:", &p_sys->fmt );

    if (p_sys->fmt.channel_type == AUDIO_CHANNEL_TYPE_AMBISONICS)
    {
        p_sys->fmt.channel_type = AUDIO_CHANNEL_TYPE_BITMAP;

        /* TODO: detect sink channel layout */
        p_sys->fmt.i_physical_channels = AOUT_CHANS_STEREO;
        aout_FormatPrepare(&p_sys->fmt);
    }

    if( AOUT_FMT_LINEAR( &p_sys->fmt ) )
        i_ret = StartPCM( env, stream, i_max_channels );
    else if( b_try_passthrough )
        i_ret = StartPassthrough( env, stream );
    else
        i_ret = VLC_EGENERIC;

    if( i_ret != 0 )
    {
        free( p_sys );
        return VLC_EGENERIC;
    }

    if( jfields.AudioTrack.getBufferSizeInFrames )
        p_sys->i_max_audiotrack_samples = JNI_AT_CALL_INT( getBufferSizeInFrames );
    else
        p_sys->i_max_audiotrack_samples = BYTES_TO_FRAMES( p_sys->audiotrack_args.i_size );

    if( jfields.AudioTimestamp.clazz )
    {
        /* create AudioTimestamp object */
        jobject p_obj = JNI_CALL( NewObject, jfields.AudioTimestamp.clazz,
                                 jfields.AudioTimestamp.ctor );
        if( p_obj )
        {
            p_sys->timestamp.p_obj = (*env)->NewGlobalRef( env, p_obj );
            (*env)->DeleteLocalRef( env, p_obj );
        }
        if( !p_sys->timestamp.p_obj )
            goto error;
    }

    p_sys->frame_chain = NULL;
    p_sys->frame_last = &p_sys->frame_chain;
    AudioTrack_Reset( env, stream );

    if( p_sys->fmt.i_format == VLC_CODEC_FL32 )
    {
        msg_Dbg( stream, "using WRITE_FLOATARRAY");
        p_sys->i_write_type = WRITE_FLOATARRAY;
        i_ret = AudioTrack_AllocJArray(env, stream, 4, (*env)->NewFloatArray);
    }
    else if( p_sys->fmt.i_format == VLC_CODEC_SPDIFL )
    {
        assert( jfields.AudioFormat.has_ENCODING_IEC61937 );
        msg_Dbg( stream, "using WRITE_SHORTARRAYV23");
        p_sys->i_write_type = WRITE_SHORTARRAYV23;
        i_ret = AudioTrack_AllocJArray(env, stream, 2, (*env)->NewShortArray);
    }
    else if( jfields.AudioTrack.writeV23 )
    {
        msg_Dbg( stream, "using WRITE_BYTEARRAYV23");
        p_sys->i_write_type = WRITE_BYTEARRAYV23;
        i_ret = AudioTrack_AllocJArray(env, stream, 1, (*env)->NewByteArray);
    }
    else
    {
        msg_Dbg( stream, "using WRITE_BYTEARRAY");
        p_sys->i_write_type = WRITE_BYTEARRAY;
        i_ret = AudioTrack_AllocJArray(env, stream, 1, (*env)->NewByteArray);
    }

    if (i_ret != 0)
    {
        msg_Err(stream, "jbuffer allocation failed");
        goto error;
    }

    /* Run AudioTrack_Thread */
    p_sys->b_thread_running = true;
    p_sys->b_thread_paused = false;
    if ( vlc_clone( &p_sys->thread, AudioTrack_Thread, stream ) )
    {
        p_sys->b_thread_running = false;
        msg_Err(stream, "vlc clone failed");
        goto error;
    }

    JNI_AT_CALL_VOID( play );
    CHECK_AT_EXCEPTION( "play" );

    *p_fmt = p_sys->fmt;

    aout_FormatPrint( stream, "VLC will output:", &p_sys->fmt );

    stream->stop = Stop;
    stream->play = Play;
    stream->pause = Pause;
    stream->flush = Flush;
    stream->time_get = NULL;
    stream->volume_set = VolumeSet;
    stream->mute_set = MuteSet;

    msg_Dbg(stream, "using AudioTrack API");
    return VLC_SUCCESS;

error:
    Stop( stream );
    return VLC_EGENERIC;
}

vlc_module_begin ()
    set_shortname("AudioTrack")
    set_description("Android AudioTrack audio output")

    set_subcategory(SUBCAT_AUDIO_AOUT)

    set_capability("aout android stream", 180)
    set_callback(Start)
vlc_module_end ()
