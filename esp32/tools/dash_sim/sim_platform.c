/*
 * The ESP32 side of the CYD, simulated on a computer: FreeRTOS tasks as
 * threads, the timer, NVS in a file, the XPT2046 touch controller on its
 * bit-banged pins (driven by the mouse), the backlight PWM and light sensor,
 * Wi-Fi status, the panel's draw calls, and Muse's Link session (chat sends
 * are printed). Everything above this layer is the real firmware code.
 */
#define _GNU_SOURCE
#include "sim.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_crt_bundle.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_status.h"
#include "noise_control.h"
#include "nvs.h"

sim_state_t g_sim = {
    .link = true,
    .rssi = -58,
    .ldr = 30,
    .chat_ok = true,
};

// ---- time ---------------------------------------------------------------------

static int64_t s_t0_us;

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t esp_timer_get_time(void) {
    if (!s_t0_us) s_t0_us = mono_us();
    return mono_us() - s_t0_us + 1000000;
}

// ---- FreeRTOS -----------------------------------------------------------------

struct sim_task {
    pthread_t thread;
    pthread_mutex_t m;
    pthread_cond_t c;
    uint32_t notes;
    TaskFunction_t fn;
    void *arg;
    char name[16];
};

static __thread struct sim_task *t_self;

static void *task_main(void *p) {
    struct sim_task *t = p;
    t_self = t;
    t->fn(t->arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t f, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *out) {
    (void)stack;
    (void)prio;
    struct sim_task *t = calloc(1, sizeof(*t));
    if (!t) return 0;
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);
    t->fn = f;
    t->arg = arg;
    snprintf(t->name, sizeof(t->name), "%s", name);
    if (out) *out = t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t->thread, &a, task_main, t);
    pthread_attr_destroy(&a);
    return rc == 0;
}

void vTaskDelete(TaskHandle_t t) {
    if (t && t != t_self) return;  // only self-deletion is used
    // A task that deletes itself (like a quote round) owns its record: nobody
    // else keeps the handle.
    struct sim_task *self = t_self;
    t_self = NULL;
    if (self) {
        pthread_mutex_destroy(&self->m);
        pthread_cond_destroy(&self->c);
        free(self);
    }
    pthread_exit(NULL);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    struct sim_task *t = t_self;
    if (!t) {
        usleep(wait == portMAX_DELAY ? 100000 : wait * 1000);
        return 0;
    }
    pthread_mutex_lock(&t->m);
    if (!t->notes) {
        if (wait == portMAX_DELAY) {
            while (!t->notes) pthread_cond_wait(&t->c, &t->m);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t ns = ts.tv_nsec + (int64_t)wait * 1000000;
            ts.tv_sec += ns / 1000000000;
            ts.tv_nsec = ns % 1000000000;
            while (!t->notes) {
                if (pthread_cond_timedwait(&t->c, &t->m, &ts) == ETIMEDOUT) break;
            }
        }
    }
    uint32_t n = t->notes;
    t->notes = clear ? 0 : (n ? n - 1 : 0);
    pthread_mutex_unlock(&t->m);
    return n;
}

BaseType_t xTaskNotifyGive(TaskHandle_t t) {
    if (!t) return 0;
    pthread_mutex_lock(&t->m);
    t->notes++;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
    return 1;
}

void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t *woken) {
    if (woken) *woken = 0;
    xTaskNotifyGive(t);
}

void vTaskDelay(TickType_t t) {
    usleep((useconds_t)t * 1000);
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t) {
    (void)t;
    return 0;  // not measurable here
}

TickType_t xTaskGetTickCount(void) {
    return (TickType_t)(esp_timer_get_time() / 1000);
}

struct sim_sem {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool binary;
    int count;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct sim_sem *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL);
    return s;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    struct sim_sem *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->binary = true;
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    (void)wait;
    if (!s->binary) return pthread_mutex_lock(&s->m) == 0;
    pthread_mutex_lock(&s->m);
    while (!s->count) pthread_cond_wait(&s->c, &s->m);
    s->count = 0;
    pthread_mutex_unlock(&s->m);
    return 1;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s->binary) return pthread_mutex_unlock(&s->m) == 0;
    pthread_mutex_lock(&s->m);
    s->count = 1;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return 1;
}

// ---- NVS: one "namespace/key=hex" line per value, in g_sim.nvs_path -----------

typedef struct {
    char key[48];
    uint8_t *val;
    size_t len;
} nvs_item_t;

static nvs_item_t s_nvs[64];
static int s_nvs_n;
static pthread_mutex_t s_nvs_m = PTHREAD_MUTEX_INITIALIZER;
static char s_nvs_ns[8][16];

static void nvs_save(void) {
    if (!g_sim.nvs_path[0]) return;
    FILE *f = fopen(g_sim.nvs_path, "w");
    if (!f) return;
    for (int i = 0; i < s_nvs_n; i++) {
        fprintf(f, "%s=", s_nvs[i].key);
        for (size_t k = 0; k < s_nvs[i].len; k++) fprintf(f, "%02x", s_nvs[i].val[k]);
        fputc('\n', f);
    }
    fclose(f);
}

void sim_nvs_load(void) {
    FILE *f = g_sim.nvs_path[0] ? fopen(g_sim.nvs_path, "r") : NULL;
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && s_nvs_n < 64) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        nvs_item_t *it = &s_nvs[s_nvs_n];
        snprintf(it->key, sizeof(it->key), "%.47s", line);
        size_t hex = strspn(eq + 1, "0123456789abcdef");
        it->len = hex / 2;
        it->val = malloc(it->len + 1);
        for (size_t k = 0; k < it->len; k++) sscanf(eq + 1 + 2 * k, "%2hhx", &it->val[k]);
        s_nvs_n++;
    }
    fclose(f);
}

static nvs_item_t *nvs_find(nvs_handle_t h, const char *k, bool create) {
    char key[48];
    snprintf(key, sizeof(key), "%s/%s", s_nvs_ns[h], k);
    for (int i = 0; i < s_nvs_n; i++) {
        if (strcmp(s_nvs[i].key, key) == 0) return &s_nvs[i];
    }
    if (!create || s_nvs_n == 64) return NULL;
    nvs_item_t *it = &s_nvs[s_nvs_n++];
    memset(it, 0, sizeof(*it));
    snprintf(it->key, sizeof(it->key), "%s", key);
    return it;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) {
    (void)m;
    for (int i = 0; i < 8; i++) {
        if (!s_nvs_ns[i][0] || strcmp(s_nvs_ns[i], ns) == 0) {
            snprintf(s_nvs_ns[i], sizeof(s_nvs_ns[i]), "%s", ns);
            *h = (nvs_handle_t)i;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len) {
    pthread_mutex_lock(&s_nvs_m);
    nvs_item_t *it = nvs_find(h, k, false);
    esp_err_t rc = ESP_FAIL;
    if (it && *len >= it->len) {
        memcpy(v, it->val, it->len);
        *len = it->len;
        rc = ESP_OK;
    }
    pthread_mutex_unlock(&s_nvs_m);
    return rc;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) {
    pthread_mutex_lock(&s_nvs_m);
    nvs_item_t *it = nvs_find(h, k, true);
    if (it) {
        free(it->val);
        it->val = malloc(len + 1);
        memcpy(it->val, v, len);
        it->len = len;
        nvs_save();
    }
    pthread_mutex_unlock(&s_nvs_m);
    return it ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len) {
    size_t n = *len;
    if (nvs_get_blob(h, k, v, &n) != ESP_OK || n >= *len) return ESP_FAIL;
    v[n] = '\0';
    *len = n + 1;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) {
    return nvs_set_blob(h, k, v, strlen(v));
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) {
    pthread_mutex_lock(&s_nvs_m);
    nvs_item_t *it = nvs_find(h, k, false);
    if (it) {
        free(it->val);
        *it = s_nvs[--s_nvs_n];
        nvs_save();
    }
    pthread_mutex_unlock(&s_nvs_m);
    return it ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) {
    (void)h;
}

// ---- XPT2046 on its bit-banged pins ---------------------------------------------
//
// Emulates the controller's serial protocol: 8 command bits latched on rising
// clock edges, then the 12-bit result shifted out MSB first on the falling
// edges after the busy clock. Readings come from g_sim's pen state, mapped
// back through the panel's raw range (optionally swapped/inverted, to test
// calibration).

static int s_cs = 1, s_mosi, s_clk, s_rise, s_cmd, s_val, s_miso;
static gpio_isr_t s_isr;
static bool s_intr_on;

static int raw_axis(int v, int size, int lo, int hi) {
    return lo + v * (hi - lo) / size;
}

static int touch_value(int cmd) {
    if (!g_sim.pen) {
        return (cmd & 0x70) == 0x40 ? 4095 : 0;  // Z2 high = no pressure
    }
    // The default calibration: raw X 200..3700 -> screen X, raw Y 240..3800 -> Y.
    int sx = g_sim.pen_x, sy = g_sim.pen_y;
    if (g_sim.touch_invert_x) sx = 319 - sx;
    if (g_sim.touch_invert_y) sy = 239 - sy;
    int rx = raw_axis(sx, 320, 200, 3700), ry = raw_axis(sy, 240, 240, 3800);
    if (g_sim.touch_swap) {
        int t = raw_axis(sy, 240, 200, 3700);
        ry = raw_axis(sx, 320, 240, 3800);
        rx = t;
    }
    switch (cmd & 0x70) {
        case 0x10: return rx;    // X channel
        case 0x50: return ry;    // Y channel
        case 0x30: return 900;   // Z1
        case 0x40: return 2800;  // Z2: firm press
        default: return 0;
    }
}

esp_err_t gpio_config(const gpio_config_t *c) {
    (void)c;
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t p, uint32_t l) {
    if (p == 33) {
        if (!l && s_cs) s_rise = 0;
        s_cs = (int)l;
    } else if (p == 32) {
        s_mosi = (int)l;
    } else if (p == 25 && !s_cs) {
        if (l && !s_clk) {
            int n = s_rise++ % 24;
            if (n < 8) {
                s_cmd = (n == 0 ? 0 : s_cmd << 1) | s_mosi;
                if (n == 7) s_val = touch_value(s_cmd);
            }
        } else if (!l && s_clk) {
            int n = (s_rise - 1) % 24 + 1;
            s_miso = (n >= 9 && n <= 20) ? (s_val >> (11 - (n - 9))) & 1 : 0;
        }
        s_clk = (int)l;
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t p) {
    if (p == 39) return s_miso;
    if (p == 36) return g_sim.pen ? 0 : 1;  // PENIRQ, active low
    return 0;
}

esp_err_t gpio_install_isr_service(int f) {
    (void)f;
    return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t p, gpio_int_type_t t) {
    (void)p;
    (void)t;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t p, gpio_isr_t h, void *a) {
    (void)p;
    (void)a;
    s_isr = h;
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t p) {
    (void)p;
    s_intr_on = true;
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t p) {
    (void)p;
    s_intr_on = false;
    return ESP_OK;
}

void sim_pen(bool down, int x, int y) {
    g_sim.pen_x = x < 0 ? 0 : x > 319 ? 319 : x;
    g_sim.pen_y = y < 0 ? 0 : y > 239 ? 239 : y;
    bool was = g_sim.pen;
    g_sim.pen = down;
    if (down && !was && s_intr_on && s_isr) s_isr(NULL);  // PENIRQ falling edge
}

// ---- backlight, light sensor, Wi-Fi, SNTP, TLS bundle ---------------------------

esp_err_t ledc_timer_config(const ledc_timer_config_t *c) { (void)c; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *c) {
    g_sim.backlight = (int)c->duty;
    return ESP_OK;
}
esp_err_t ledc_fade_func_install(int f) { (void)f; return ESP_OK; }
esp_err_t ledc_set_fade_time_and_start(ledc_mode_t m, ledc_channel_t c, uint32_t d,
                                       uint32_t ms, ledc_fade_mode_t w) {
    (void)m; (void)c; (void)ms; (void)w;
    g_sim.backlight = (int)d;
    return ESP_OK;
}
esp_err_t ledc_set_duty(ledc_mode_t m, ledc_channel_t c, uint32_t d) {
    (void)m; (void)c;
    g_sim.backlight = (int)d;
    return ESP_OK;
}
esp_err_t ledc_update_duty(ledc_mode_t m, ledc_channel_t c) { (void)m; (void)c; return ESP_OK; }

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h) {
    (void)c;
    *h = (void *)1;
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch,
                                     const adc_oneshot_chan_cfg_t *c) {
    (void)h; (void)ch; (void)c;
    return ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *out) {
    (void)h; (void)ch;
    *out = g_sim.ldr;
    return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    if (!g_sim.link && g_sim.rssi == 0) return ESP_FAIL;
    ap->rssi = (int8_t)g_sim.rssi;
    return ESP_OK;
}

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) {
    (void)c;  // the computer's clock is already right
    return ESP_OK;
}

esp_err_t esp_crt_bundle_attach(void *conf) {
    (void)conf;  // libcurl uses the system's CA certificates
    return ESP_OK;
}

bool sd_card_init(void) {
    return false;
}

// ---- panel ------------------------------------------------------------------------

static pthread_mutex_t s_fb_m = PTHREAD_MUTEX_INITIALIZER;
static bool s_active;
static const uint16_t *s_pending;
static int s_px, s_py, s_pw, s_ph;

static void blit(int x, int y, int w, int h, const uint16_t *p) {
    pthread_mutex_lock(&s_fb_m);
    for (int r = 0; r < h; r++) memcpy(&g_sim.fb[y + r][x], p + r * w, (size_t)w * 2);
    g_sim.frames++;
    pthread_mutex_unlock(&s_fb_m);
}

void sim_fb_lock(void) { pthread_mutex_lock(&s_fb_m); }
void sim_fb_unlock(void) { pthread_mutex_unlock(&s_fb_m); }

void dashboard_display_set_active(bool on) {
    if (on && !s_active) {
        sim_fb_lock();
        memset(g_sim.fb, 0, sizeof(g_sim.fb));
        sim_fb_unlock();
    }
    s_active = on;
}

bool dashboard_display_draw(int x, int y, int w, int h, const uint16_t *pixels) {
    blit(x, y, w, h, pixels);
    return true;
}

bool dashboard_display_draw_start(int x, int y, int w, int h, const uint16_t *pixels) {
    s_pending = pixels;
    s_px = x;
    s_py = y;
    s_pw = w;
    s_ph = h;
    if (g_sim.slow_spi) usleep((useconds_t)(w * h * 16 / 40) + 50);  // 40 MHz SPI
    return true;
}

void dashboard_display_draw_wait(void) {
    blit(s_px, s_py, s_pw, s_ph, s_pending);
}

// ---- Muse's Link session --------------------------------------------------------

bool noise_ctrl_is_connected(void) {
    return g_sim.link;
}

static noise_ctrl_req_cb s_req_cb;
static void *s_req_ctx;

int64_t noise_ctrl_req_open(const char *verb, const char *path, const char *const *headers,
                            bool end_body, noise_ctrl_req_cb cb, void *ctx) {
    (void)headers;
    (void)end_body;
    if (!g_sim.link) return 0;
    s_req_cb = cb;
    s_req_ctx = ctx;
    printf("[link] %s %s\n", verb, path);
    return 100;
}

bool noise_ctrl_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms) {
    (void)id;
    (void)end_body;
    (void)wait_ms;
    printf("[link] -> Muse: %.*s\n", (int)len, (const char *)data);
    fflush(stdout);
    // Muse's acknowledgement (or a refusal, with --chat-fail).
    int status = g_sim.chat_ok ? 200 : 503;
    const char *ack = g_sim.chat_ok ? "{\"id\":\"sim-msg\"}" : "{\"error\":\"unavailable\"}";
    s_req_cb(s_req_ctx, status, (const uint8_t *)ack, strlen(ack), true);
    return true;
}

void noise_ctrl_req_cancel(int64_t id) {
    (void)id;
}
