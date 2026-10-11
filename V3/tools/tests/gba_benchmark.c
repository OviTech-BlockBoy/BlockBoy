/* Host mocks around the actual rg_benchmark.c, not a reimplementation. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gba_profile.h"
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define RG_BASE_PATH "/sd/BlockBoy"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define RG_DIALOG_FLAG_NORMAL 1
#define RG_DIALOG_END {0,0,0,0,0}
#define RG_DISPLAY_SCALING_OFF 0
#define F_OK 0
typedef struct { int id; const char *label, *value; int flags; void *cb; } rg_gui_option_t;
typedef struct { const char *romPath, *version; float speed; } rg_app_t;
typedef struct { char date[16], time[16]; unsigned char app_elf_sha256[32]; } esp_app_desc_t;
typedef int FILE;
static int64_t clock_us;
static uint32_t cycle_clock;
static int writes_during_run, applied, restored, alloc_fail, open_fail, io_fail, close_fail, drain_fail;
static unsigned char arena[100000] __attribute__((aligned(16)));
static rg_app_t app = {"emerald.gba", "test", 1.0f};
static esp_app_desc_t desc;
static unsigned jit_enabled = 1, sound_master_enable = 1;
static bool benchmark_visible;
static bool rg_gui_get_option_visible(const char *key, bool fallback) { (void)key;(void)fallback;return benchmark_visible; }
gba_profile_data *gba_profile_current;
uint32_t gba_profile_test_clock(void) { return cycle_clock; }
bool gba_benchmark_active(void);
void *memset(void *p, int v, size_t n) { unsigned char *s=p; while(n--) *s++=v; return p; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *a=d; const unsigned char *b=s; while(n--) *a++=*b++; return d; }
static int64_t rg_system_timer(void) { return clock_us; }
static void *heap_caps_calloc(size_t n, size_t s, int flags) { (void)flags; if(alloc_fail || n*s>sizeof(arena)) return NULL; return memset(arena,0,n*s); }
static unsigned heap_caps_get_free_size(int flags) { (void)flags; return 100000; }
static void rg_gui_alert(const char *a, const char *b) { (void)a;(void)b; }
static int rg_gui_dialog(const char *a, const rg_gui_option_t *b, int c) { (void)a;(void)b;(void)c;return -1; }
static bool rg_storage_mkdir(const char *p) { (void)p;return true; }
static int access(const char *p,int m) { (void)p;(void)m;return -1; }
static FILE *fopen(const char *p,const char *m) { (void)p;(void)m;return open_fail?NULL:(FILE*)arena; }
static int fprintf(FILE *f,const char *fmt,...) { (void)f;(void)fmt;if(gba_benchmark_active())writes_during_run++;return io_fail?-1:0; }
static int fflush(FILE *f) { (void)f;if(gba_benchmark_active())writes_during_run++;return io_fail?-1:0; }
static int ferror(FILE *f) { (void)f;return io_fail; }
static int fclose(FILE *f) { (void)f;return close_fail?-1:0; }
static int snprintf(char *p,size_t n,const char *fmt,...) { (void)fmt;if(n)*p=0;return 0; }
static int printf(const char *fmt,...) { (void)fmt;return 0; }
static const rg_app_t *rg_system_get_app(void) { return &app; }
static const esp_app_desc_t *esp_app_get_description(void) { return &desc; }
static int rg_display_get_scaling(void) { return 0; }
static int rg_display_get_filter(void) { return 0; }
bool gba_benchmark_drain(void) { return !drain_fail; }
void gba_benchmark_settings(bool apply) { if(apply) applied++;else restored++; }
int gba_benchmark_frameskip(void) { return -1; }
#include "benchmark_under_test.h"
#define CHECK(c) do { if (!(c)) return __LINE__; } while(0)
static void reset(void) {
    bench=NULL;pending=0;active=false;gba_profile_current=NULL;clock_us=0;
    writes_during_run=applied=restored=alloc_fail=open_fail=io_fail=close_fail=drain_fail=0;
}
static void start(int mode) { pending=mode;duration=30;gba_benchmark_begin_frame(true);clock_us+=2000000;gba_benchmark_begin_frame(true); }
__declspec(dllexport) int test_benchmark(void)
{
    benchmark_visible=false;CHECK(rg_benchmark_menu_label()==NULL);
    benchmark_visible=true;CHECK(rg_benchmark_menu_label()!=NULL);
    benchmark_visible=false;
    reset(); alloc_fail=1;start(1); CHECK(!active && !applied);
    reset(); open_fail=1;start(1); CHECK(!active && !applied);
    reset(); io_fail=1;start(1); CHECK(!active && applied==1 && restored==1 && !bench->file);
    reset(); drain_fail=1;start(1); CHECK(!active && !applied);
    reset(); pending=1;gba_benchmark_begin_frame(false);CHECK(!active && !applied);
    reset(); pending=1;gba_benchmark_begin_frame(true);CHECK(!active && applied==1);
    gba_benchmark_stop("menu");CHECK(restored==1 && !bench->file);
    reset();start(1);CHECK(active && gba_profile_current && applied==1);
    for(int i=0;i<60;i++) { clock_us+=20000;gba_benchmark_end_frame(20000,true,true,9000,2000,8000,1000); }
    CHECK(bench->row_count==1 && bench->rows[0].frames==50 && bench->rows[0].interval_us==1000000);
    gba_benchmark_offload(1234,5678);gba_benchmark_stop("menu");
    CHECK(!active && restored==1 && writes_during_run==0);
    CHECK(bench->row_count==2 && bench->rows[1].frames==10 && bench->rows[1].interval_us==200000);
    CHECK(bench->rows[1].offload_us==1234 && bench->rows[1].offload_submit_us==5678);
    CHECK(bench->histogram[20]==60);
    start(3); CHECK(active && !gba_profile_current && bench->row_count==0);
    for(int i=0;i<30;i++) {clock_us+=1000000;gba_benchmark_end_frame(1000000,false,false,0,0,0,0);}
    CHECK(!active && restored==2 && bench->row_count==30 && writes_during_run==0);
    reset();start(2);CHECK(active && !applied);gba_benchmark_stop("menu");CHECK(!restored);
    reset();start(1);close_fail=1;gba_benchmark_stop("menu");CHECK(restored==1 && !bench->file);
    reset();start(1);bench->duration=120;
    for(int i=0;i<120;i++) {clock_us+=1001000;gba_benchmark_end_frame(1001000,true,false,1,2,3,4);}
    CHECK(!active && bench->row_count==120 && writes_during_run==0);
    return 0;
}
__declspec(dllexport) int test_profile(void)
{
    static gba_profile_data data;
    memset(&data,0,sizeof(data));gba_profile_current=&data;
    cycle_clock=0xfffffff0u;uint32_t start_cycle=gba_profile_begin();
    cycle_clock=0x20;gba_profile_end(GP_CLASSIC,start_cycle);
    CHECK(data.counter[GP_CLASSIC].cycles==48 && data.counter[GP_CLASSIC].calls==1);
    cycle_clock=0;start_cycle=gba_profile_begin();cycle_clock=240;gba_profile_end(GP_CLASSIC,start_cycle);
    CHECK(data.counter[GP_CLASSIC].cycles==288 && data.counter[GP_CLASSIC].calls==2);
    gba_profile_current=NULL;gba_profile_end(GP_CLASSIC,0);CHECK(data.counter[GP_CLASSIC].calls==2);
    return 0;
}
