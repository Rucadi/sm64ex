#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>  //Header file for sleep(). man 3 sleep for details.
#include <time.h>
#include <errno.h>
#include <stdbool.h>

#ifdef TARGET_WEB
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"

#include "gfx/gfx_opengl.h"
#include "gfx/gfx_direct3d11.h"
#include "gfx/gfx_direct3d12.h"

#include "gfx/gfx_dxgi.h"
#include "gfx/gfx_sdl.h"

#include "audio/audio_api.h"
#include "audio/audio_sdl.h"
#include "audio/audio_null.h"

#include "pc_main.h"
#include "thread.h"
#include "cliopts.h"
#include "configfile.h"
#include "controller/controller_api.h"
#include "controller/controller_keyboard.h"
#include "fs/fs.h"

#include "game/game_init.h"
#include "game/main.h"
#include "game/thread6.h"
#include "game/display.h"

#ifdef DISCORDRPC
#include "pc/discord/discordrpc.h"
#endif

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

s32 gRumblePakPfs;
struct RumbleData gRumbleDataQueue[3];
struct StructSH8031D9B0 gCurrRumbleSettings;

static struct AudioAPI *audio_api;
static struct GfxWindowManagerAPI *wm_api;
static struct GfxRenderingAPI *rendering_api;

sys_thread_t pcthread_audio_id;
sys_mutex_t pcthread_audio_mutex = SYS_MUTEX_INITIALIZER;
bool pcthread_audio_init = false;
bool pcthread_audio_rendering = false;
sys_sem_t pcthread_audio_sema;

sys_mutex_t pcthread_game_mutex = SYS_MUTEX_INITIALIZER;
bool pcthread_game_loop_iterating = false;
bool pcthread_game_reset_sound = false;
bool pcthread_wait_for_gameloop = false;
sys_sem_t pcthread_game_sema;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);
extern void audio_game_loop_tick(void);
extern void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);
void* audio_thread();

void dispatch_audio_sptask(struct SPTask *spTask) {
}

void set_vblank_handler(s32 index, struct VblankHandler *handler, OSMesgQueue *queue, OSMesg *msg) {
}

// Send a request for non-game threads to wait for the game thread to finish
void pc_request_gameloop_wait(void) {
    sys_mutex_lock(&pcthread_game_mutex);
    pcthread_wait_for_gameloop = true;
    sys_mutex_unlock(&pcthread_game_mutex);
}

// Wait for the audio thread to finish rendering audio
void pc_wait_for_audio(void) {
    sys_semaphore_wait(&pcthread_audio_sema);
}

// Check if the audio thread is currently rendering audio
bool pc_check_audio_rendering(void) {
    sys_mutex_lock(&pcthread_audio_mutex);
    bool rendering = pcthread_audio_rendering;
    sys_mutex_unlock(&pcthread_audio_mutex);
    return rendering;
}

// Check if the game thread should finish before continuing
bool pc_check_gameloop_wait(void) {
    sys_mutex_lock(&pcthread_game_mutex);
    bool waiting = pcthread_wait_for_gameloop;
    sys_mutex_unlock(&pcthread_game_mutex);
    return waiting;
}

static bool inited = false;

void send_display_list(struct SPTask *spTask) {
    if (!inited) return;
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#ifdef VERSION_EU
#define SAMPLES_HIGH 656
#define SAMPLES_LOW 640
#else
#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528
#endif

void produce_one_frame(void) {
    gfx_start_frame();

    game_loop_one_iteration();

    // Post the game thread semaphore if the game thread requested it
    sys_mutex_lock(&pcthread_game_mutex); 
    if (pcthread_wait_for_gameloop) {
        sys_semaphore_post(&pcthread_game_sema);
        pcthread_wait_for_gameloop = false; 
    }
    sys_mutex_unlock(&pcthread_game_mutex);

    thread6_rumble_loop(NULL);

    gfx_end_frame();
}

// Seperate the audio thread from the main thread so that your ears won't bleed at a low framerate
void* audio_thread() {
    // Keep track of the time in microseconds
    const f64 frametime_micro = 16666.666;   // 16.666666 ms = 60Hz; run this thread 60 times a second like the original game
    f64 start_time;
    f64 end_time;
    bool doloop = true;
    sys_semaphore_wait(&pcthread_game_sema);
    while(doloop) {
        start_time = sys_profile_time();
        const f32 master_mod = (f32)configMasterVolume / 127.0f;
        set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * master_mod);
        set_sequence_player_volume(SEQ_PLAYER_SFX, (f32)configSfxVolume / 127.0f * master_mod);
        set_sequence_player_volume(SEQ_PLAYER_ENV, (f32)configEnvVolume / 127.0f * master_mod);

        int samples_left = audio_api->buffered();
        u32 num_audio_samples = samples_left < audio_api->get_desired_buffered() ? SAMPLES_HIGH : SAMPLES_LOW;
        // printf("Audio samples: %d %u\n", samples_left, num_audio_samples);
        s16 audio_buffer[SAMPLES_HIGH * 2];
        /*if (audio_cnt-- == 0) {
            audio_cnt = 2;
        }
        u32 num_audio_samples = audio_cnt < 2 ? 528 : 544;*/

        if (!pc_check_gameloop_wait()) {
            sys_mutex_lock(&pcthread_audio_mutex);
            pcthread_audio_rendering = true;
            sys_mutex_unlock(&pcthread_audio_mutex);
            create_next_audio_buffer(audio_buffer, num_audio_samples);
            sys_semaphore_post(&pcthread_audio_sema);
            sys_mutex_lock(&pcthread_audio_mutex);
            pcthread_audio_rendering = false;
            sys_mutex_unlock(&pcthread_audio_mutex);
        } /* else {
            printf("Audio thread: dropped frame\n");
        } */

        // printf("Audio samples before submitting: %d\n", audio_api->buffered());
        audio_api->play((u8 *)audio_buffer, num_audio_samples * 4);

        end_time = sys_profile_time();

        // Sleep for the remaining time
        f64 nap_time = frametime_micro - (end_time - start_time);
        // printf("Audio thread nap time: %f\n", nap_time);
        if (nap_time > 0.0) sys_sleep(nap_time);

        // Check if the game thread is still running
        sys_mutex_lock(&pcthread_audio_mutex);
        doloop = pcthread_audio_init;
        sys_mutex_unlock(&pcthread_audio_mutex);
    }
    return NULL;
}

void audio_thread_init() {
    pcthread_audio_init = true;
    sys_semaphore_init(&pcthread_audio_sema, 0, 1);
    sys_thread_create(&pcthread_audio_id, audio_thread, NULL);
}

void audio_shutdown(void) {
    // Tell the audio thread to stop
    sys_mutex_lock(&pcthread_audio_mutex);
    pcthread_audio_init = false;
    sys_mutex_unlock(&pcthread_audio_mutex);
    sys_semaphore_wait(&pcthread_audio_sema); // Wait for the audio thread to finish rendering audio, then destroy it all
    sys_thread_join(&pcthread_audio_id, NULL);
    sys_semaphore_destroy(&pcthread_audio_sema);

    if (audio_api) {
        if (audio_api->shutdown) audio_api->shutdown();
        audio_api = NULL;
    }
}

void game_deinit(void) {
#ifdef DISCORDRPC
    discord_shutdown();
#endif
    configfile_save(configfile_name());
    controller_shutdown();
    audio_shutdown();
    gfx_shutdown();
    sys_semaphore_destroy(&pcthread_game_sema);
    inited = false;
}

void game_exit(void) {
    game_deinit();
#ifndef TARGET_WEB
    exit(0);
#endif
}

#ifdef TARGET_WEB
static void em_main_loop(void) {
}

static void request_anim_frame(void (*func)(double time)) {
    EM_ASM(requestAnimationFrame(function(time) {
        dynCall("vd", $0, [time]);
    }), func);
}

static void on_anim_frame(double time) {
    static double target_time;

    time *= 0.03; // milliseconds to frame count (33.333 ms -> 1)

    if (time >= target_time + 10.0) {
        // We are lagging 10 frames behind, probably due to coming back after inactivity,
        // so reset, with a small margin to avoid potential jitter later.
        target_time = time - 0.010;
    }

    for (int i = 0; i < 2; i++) {
        // If refresh rate is 15 Hz or something we might need to generate two frames
        if (time >= target_time) {
            produce_one_frame();
            target_time = target_time + 1.0;
        }
    }

    if (inited) // only continue if the init flag is still set
        request_anim_frame(on_anim_frame);
}
#endif

void main_func(void) {
    const char *gamedir = gCLIOpts.GameDir[0] ? gCLIOpts.GameDir : FS_BASEDIR;
    const char *userpath = gCLIOpts.SavePath[0] ? gCLIOpts.SavePath : sys_user_path();
    fs_init(sys_ropaths, gamedir, userpath);

    configfile_load(configfile_name());

    if (gCLIOpts.FullScreen == 1)
        configWindow.fullscreen = true;
    else if (gCLIOpts.FullScreen == 2)
        configWindow.fullscreen = false;

    const size_t poolsize = gCLIOpts.PoolSize ? gCLIOpts.PoolSize : DEFAULT_POOL_SIZE;
    u64 *pool = malloc(poolsize);
    if (!pool) sys_fatal("Could not alloc %u bytes for main pool.\n", poolsize);
    main_pool_init(pool, pool + poolsize / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    #if defined(WAPI_SDL1) || defined(WAPI_SDL2)
    wm_api = &gfx_sdl;
    #elif defined(WAPI_DXGI)
    wm_api = &gfx_dxgi;
    #else
    #error No window API!
    #endif

    #if defined(RAPI_D3D11)
    rendering_api = &gfx_direct3d11_api;
    # define RAPI_NAME "DirectX 11"
    #elif defined(RAPI_D3D12)
    rendering_api = &gfx_direct3d12_api;
    # define RAPI_NAME "DirectX 12"
    #elif defined(RAPI_GL) || defined(RAPI_GL_LEGACY)
    rendering_api = &gfx_opengl_api;
    # ifdef USE_GLES
    #  define RAPI_NAME "OpenGL ES"
    # else
    #  define RAPI_NAME "OpenGL"
    # endif
    #else
    #error No rendering API!
    #endif

    char window_title[96] =
    "Super Mario 64 EX (" RAPI_NAME ")"
    #ifdef NIGHTLY
    " nightly " GIT_HASH
    #endif
    ;

    gfx_init(wm_api, rendering_api, window_title);
    wm_api->set_keyboard_callbacks(keyboard_on_key_down, keyboard_on_key_up, keyboard_on_all_keys_up);

    #if defined(AAPI_SDL1) || defined(AAPI_SDL2)
    if (audio_api == NULL && audio_sdl.init()) 
        audio_api = &audio_sdl;
    #endif

    if (audio_api == NULL) {
        audio_api = &audio_null;
    }

    audio_init();
    sound_init();

    thread5_game_loop(NULL);

    inited = true;

#ifdef EXTERNAL_DATA
    // precache data if needed
    if (configPrecacheRes) {
        fprintf(stdout, "precaching data\n");
        fflush(stdout);
        gfx_precache_textures();
    }
#endif

#ifdef DISCORDRPC
    discord_init();
#endif

#ifdef TARGET_WEB
    emscripten_set_main_loop(em_main_loop, 0, 0);
    request_anim_frame(on_anim_frame);
#else
    // initialize multithreading
    sys_semaphore_init(&pcthread_game_sema, 0, 0);
    audio_thread_init();

    while (true) {
        wm_api->main_loop(produce_one_frame);
#ifdef DISCORDRPC
        discord_update_rich_presence();
#endif
    }
#endif
}

int main(int argc, char *argv[]) {
    parse_cli_opts(argc, argv);
    main_func();
    return 0;
}
