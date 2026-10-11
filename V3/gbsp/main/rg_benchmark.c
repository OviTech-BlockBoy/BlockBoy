/* V3 successor to Nieuwe map/V2/components/retro-go/rg_benchmark.c.
 * No benchmark SD writes during the measured interval. GBA-only allocation. */
#include "rg_benchmark.h"
#include "gba_profile.h"
#include "cpu_jit.h"
#include "sound.h"
#include <rg_system.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define BENCH_DIR RG_BASE_PATH "/benchmarks"
#define ROWS 122
typedef struct {
    uint32_t elapsed_us, interval_us, frames, drawn, offloaded;
    uint64_t execute_us, output_us, pacing_us, wait_us;
    uint32_t max_frame_us, over20, over33, over50;
    uint32_t offload_us, offload_submit_us;
    gba_profile_counter profile[GP_COUNT];
} bench_row;
typedef struct {
    gba_profile_data profile;
    bench_row rows[ROWS], bucket;
    gba_profile_counter prev[GP_COUNT];
    uint32_t histogram[1001]; // 1 ms bins; >=1000 ms in last bin
    uint32_t offload_us, offload_submit_us, prev_offload, prev_submit;
    int row_count, duration, mode;
    int64_t start, last, warm_until;
    FILE *file;
    char path[192];
    bool settings_applied;
} bench_state;
static bench_state *bench; // the large buffer is explicitly in PSRAM
static int duration = 60;
static int pending;
static bool active;
static const char *const names[GP_COUNT] = {
    "classic", "jit", "compile", "rom_swap", "cpuset", "cpufastset", "lz77", "live_render", "m4a_mixer"
};

bool gba_benchmark_active(void) { return __atomic_load_n(&active, __ATOMIC_ACQUIRE); }
bool gba_benchmark_detail(void)
{
    return gba_benchmark_active() && bench->mode != 3;
}
void gba_benchmark_offload(uint32_t render_us, uint32_t submit_us)
{
    // Separate single-writer counters: never read a torn 64-bit core1 value.
    __atomic_fetch_add(&bench->offload_us, render_us, __ATOMIC_RELAXED);
    __atomic_fetch_add(&bench->offload_submit_us, submit_us, __ATOMIC_RELAXED);
}

const char *rg_benchmark_menu_label(void)
{
    return rg_gui_get_option_visible("GbaBench", false) ? "GBA benchmark" : NULL;
}
void rg_benchmark_menu(void)
{
    if (!rg_benchmark_menu_label()) return;
    const rg_gui_option_t options[] = {
        {1, "Clean + timings", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {2, "Current + timings", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {3, "Clean FPS only", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {30, "Duration: 30 seconds", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {60, "Duration: 60 seconds", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {90, "Duration: 90 seconds", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        {120, "Duration: 120 seconds", NULL, RG_DIALOG_FLAG_NORMAL, NULL},
        RG_DIALOG_END
    };
    for (;;) {
        char title[48];
        snprintf(title, sizeof(title), "GBA benchmark (%d s)", duration);
        int choice = rg_gui_dialog(title, options, 0);
        if (choice >= 30 && choice <= 120) { duration = choice; continue; }
        if (choice >= 1 && choice <= 3) pending = choice;
        return;
    }
}

static void collect_row(int64_t now)
{
    if (bench->row_count >= ROWS || now <= bench->last) return;
    bench_row *r = &bench->rows[bench->row_count++];
    *r = bench->bucket;
    memset(&bench->bucket, 0, sizeof(bench->bucket));
    r->elapsed_us = now - bench->start;
    r->interval_us = now - bench->last;
    bench->last = now;
    for (int i = 0; i < GP_COUNT; i++) {
        r->profile[i].cycles = bench->profile.counter[i].cycles - bench->prev[i].cycles;
        r->profile[i].calls = bench->profile.counter[i].calls - bench->prev[i].calls;
        bench->prev[i] = bench->profile.counter[i];
    }
    uint32_t ro = __atomic_load_n(&bench->offload_us, __ATOMIC_RELAXED);
    uint32_t so = __atomic_load_n(&bench->offload_submit_us, __ATOMIC_RELAXED);
    r->offload_us = ro - bench->prev_offload;
    r->offload_submit_us = so - bench->prev_submit;
    bench->prev_offload = ro;
    bench->prev_submit = so;
}

void gba_benchmark_stop(const char *reason)
{
    pending = 0;
    if (!bench || !bench->file) return;
    bool measured = gba_benchmark_active();
    bool drained = gba_benchmark_drain();
    __atomic_store_n(&active, false, __ATOMIC_RELEASE);
    gba_profile_current = NULL;
    if (measured) collect_row(rg_system_timer());
    FILE *f = bench->file;
    fprintf(f, "# stop=%s\n# offload_drained=%d\n", reason, drained);
    fprintf(f, "elapsed_s,interval_s,frames,drawn,offloaded,fps_emu,fps_drawn,execute_ms,output_ms,pacing_ms,wait_ms,max_frame_ms,frames_gt20ms,frames_gt33ms,frames_gt50ms,offload_render_ms,offload_submit_ms");
    for (int i = 0; i < GP_COUNT; i++) fprintf(f, ",%s_ms,%s_calls", names[i], names[i]);
    fprintf(f, "\n");
    uint32_t frames = 0, drawn = 0, max_us = 0;
    for (int j = 0; j < bench->row_count; j++) {
        const bench_row *r = &bench->rows[j];
        frames += r->frames; drawn += r->drawn;
        if (r->max_frame_us > max_us) max_us = r->max_frame_us;
        fprintf(f, "%.6f,%.6f,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%.3f,%.3f",
            r->elapsed_us / 1e6, r->interval_us / 1e6, (unsigned)r->frames,
            (unsigned)r->drawn, (unsigned)r->offloaded,
            r->frames * 1e6 / r->interval_us, r->drawn * 1e6 / r->interval_us,
            r->execute_us / 1000.0, r->output_us / 1000.0, r->pacing_us / 1000.0,
            r->wait_us / 1000.0, r->max_frame_us / 1000.0,
            (unsigned)r->over20, (unsigned)r->over33, (unsigned)r->over50,
            r->offload_us / 1000.0, r->offload_submit_us / 1000.0);
        for (int i = 0; i < GP_COUNT; i++) fprintf(f, ",%.3f,%u",
            r->profile[i].cycles / (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000.0),
            (unsigned)r->profile[i].calls);
        fprintf(f, "\n");
    }
    unsigned cumulative = 0, p95 = 0;
    if (frames) for (p95 = 0; p95 < 1000; p95++) {
        cumulative += bench->histogram[p95];
        if (cumulative >= (frames * 95 + 99) / 100) break;
    }
    double seconds = measured ? (bench->last - bench->start) / 1e6 : 0;
    fprintf(f, "# summary_seconds=%.6f\n# summary_fps_emu=%.3f\n# summary_fps_drawn=%.3f\n# frame_p95_bin_ms=%u\n# frame_max_ms=%.3f\n",
        seconds, seconds > 0 ? frames / seconds : 0, seconds > 0 ? drawn / seconds : 0,
        p95, max_us / 1000.0);
    for (int i = 0; i < 1024; i++) if (bench->profile.pages[i])
        fprintf(f, "# rom_page,%u,%u\n", (unsigned)i, (unsigned)bench->profile.pages[i]);
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    bench->file = NULL;
    if (bench->settings_applied) {
        gba_benchmark_settings(false);
        bench->settings_applied = false;
    }
    printf("[BENCH] %s: %s (%.2f s, %.2f FPS)\n", ok ? "saved" : "WRITE FAILED", bench->path,
           seconds, seconds > 0 ? frames / seconds : 0);
    if (!ok) rg_gui_alert("Benchmark write failed", "Check SD card/free space. CSV may be incomplete.");
}

void gba_benchmark_begin_frame(bool boot_ready)
{
    if (pending) {
        int mode = pending; pending = 0;
        if (!boot_ready) { rg_gui_alert("Benchmark", "Wait until the game has finished booting."); return; }
        if (!gba_benchmark_drain()) { rg_gui_alert("Benchmark", "Render task did not become idle."); return; }
        if (!bench) bench = heap_caps_calloc(1, sizeof(*bench), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!bench) { rg_gui_alert("Benchmark", "Not enough PSRAM for measurement buffer."); return; }
        memset(bench, 0, sizeof(*bench));
        bench->mode = mode; bench->duration = duration;
        rg_storage_mkdir(BENCH_DIR);
        // An uptime-based unique name also works without a real-time clock.
        unsigned stamp = (unsigned)(rg_system_timer() / 1000);
        do { snprintf(bench->path, sizeof(bench->path), BENCH_DIR "/gba_%u.csv", stamp++); }
        while (access(bench->path, F_OK) == 0);
        bench->file = fopen(bench->path, "w");
        if (!bench->file) { rg_gui_alert("Benchmark", "Cannot create CSV on SD card."); return; }
        if (mode != 2) { gba_benchmark_settings(true); bench->settings_applied = true; }
        const rg_app_t *app = rg_system_get_app();
        const esp_app_desc_t *desc = esp_app_get_description();
        fprintf(bench->file, "# schema=gba-benchmark-v3-1\n# mode=%s\n# timings=%d\n# requested_seconds=%d\n# rom=%s\n# version=%s\n# build=%s %s\n# elf_sha256=",
            mode == 2 ? "current" : "clean", mode != 3, duration, app->romPath,
            app->version, desc->date, desc->time);
        for (unsigned i = 0; i < sizeof(desc->app_elf_sha256); i++) fprintf(bench->file, "%02x", desc->app_elf_sha256[i]);
        fprintf(bench->file, "\n# cpu_mhz=%d\n# jit=%d\n# frameskip=%d\n# speed=%.3f\n# audio=%d\n# scaling=%d\n# filter=%d\n# buffer_bytes=%u\n# free_internal=%u\n# free_psram=%u\n",
            CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, jit_enabled, gba_benchmark_frameskip(), app->speed,
            sound_master_enable, rg_display_get_scaling(), rg_display_get_filter(), (unsigned)sizeof(*bench),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        fprintf(bench->file, "# scope_times=inclusive host elapsed; nested/parallel columns overlap; do not sum\n# drawn=frames rendered or queued for offload; not physical LCD presentation\n# warmup_seconds=2\n# p95=1ms histogram floor; bin 1000 means >=1000ms\n# ccount_scope_limit_seconds=17.8 at 240MHz; preemption included\n");
        if (fflush(bench->file) || ferror(bench->file)) { gba_benchmark_stop("header_write_error"); return; }
        bench->warm_until = rg_system_timer() + 2000000;
        printf("[BENCH] warmup 2s -> %ds, %s\n", duration, bench->path);
    }
    if (bench && bench->file && !gba_benchmark_active() && rg_system_timer() >= bench->warm_until) {
        if (!gba_benchmark_drain()) { gba_benchmark_stop("start_drain_timeout"); return; }
        bench->start = bench->last = rg_system_timer();
        gba_profile_current = bench->mode == 3 ? NULL : &bench->profile;
        __atomic_store_n(&active, true, __ATOMIC_RELEASE);
    }
}

void gba_benchmark_end_frame(uint32_t frame_us, bool drawn, bool offloaded,
                             uint32_t execute_us, uint32_t output_us,
                             uint32_t pacing_us, uint32_t wait_us)
{
    if (!gba_benchmark_active()) return;
    bench_row *r = &bench->bucket;
    r->frames++; r->drawn += drawn; r->offloaded += offloaded;
    r->execute_us += execute_us; r->output_us += output_us;
    r->pacing_us += pacing_us; r->wait_us += wait_us;
    if (frame_us > r->max_frame_us) r->max_frame_us = frame_us;
    r->over20 += frame_us > 20000; r->over33 += frame_us > 33000; r->over50 += frame_us > 50000;
    bench->histogram[frame_us / 1000 < 1000 ? frame_us / 1000 : 1000]++;
    int64_t now = rg_system_timer();
    if (now - bench->start >= bench->duration * 1000000LL) gba_benchmark_stop("duration");
    else if (now - bench->last >= 1000000) collect_row(now);
}
