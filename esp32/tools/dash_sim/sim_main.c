/*
 * CYD dashboard simulator: the real dashboard firmware (main/dashboard/) in a
 * window on your computer. The mouse is your finger (it drives a simulated
 * XPT2046, so the real touch driver and gestures run); the console takes the
 * same commands Muse sends. Quotes come live from Nasdaq through libcurl.
 *
 *   dash_sim [--scale N] [--headless] [--script FILE] [--nvs FILE]
 *            [--swap-touch] [--invert-x] [--invert-y] [--chat-fail] [--slow-spi]
 *            [--listen PORT] [--muse-socket PATH] [--no-register]
 *
 * Console (stdin or --script), one per line:
 *   <muse command> [JSON params]   e.g. dashboard.card {"id":"x","title":"Hi"}
 *   tap X Y | swipe left|right | hold X Y MS | sleep MS
 *   ldr RAW | link on|off | wifi RSSI|off | chat ok|fail
 *   ota PCT|fail | image FILE.bmp | snap FILE.bmp | help | quit
 *   record MS PREFIX   (in the background, a screenshot every 40 ms:
 *                       PREFIX_0000.bmp, ...)
 *   expect TEXT   (scripts: fail the run unless the last output contains TEXT)
 *
 * Connected to a real Muse (tools/dash_sim/mac_gadget.sh): --listen takes
 * Muse's commands from `musegadget dash-sim` on 127.0.0.1:PORT, one JSON line
 * {"command": ..., "params": {...}} per connection, and --muse-socket sends
 * card button taps to Muse through that service's local socket.
 *
 * --no-register: the Link session never registers, so the dashboard starts
 * only on its 20 s fallback.
 *
 * Keys: Left/Right swipe, C calibrate, B/D bright/dark room, L link toggle,
 * S screenshot (sim-NNN.bmp), Q quit.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "dashboard.h"
#include "sim.h"

static int s_scale = 2;
static volatile bool s_quit;
static char s_last[8192];  // the last command's output, for "expect"
static int s_failures;
// The console and the Muse listener each run commands.
static pthread_mutex_t s_cmd_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- screenshots -------------------------------------------------------------

static void rgb_of(uint16_t be, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint16_t v = (uint16_t)((be >> 8) | (be << 8));
    *r = (uint8_t)(((v >> 11) & 31) * 255 / 31);
    *g = (uint8_t)(((v >> 5) & 63) * 255 / 63);
    *b = (uint8_t)((v & 31) * 255 / 31);
}

// 24-bit BMP of the panel (what the LCD shows, before the backlight).
static bool snap_buf(const char *path, bool shown) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const int w = 320, h = 240, row = w * 3, size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    hdr[2] = size & 255; hdr[3] = (size >> 8) & 255; hdr[4] = (size >> 16) & 255;
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = w & 255; hdr[19] = w >> 8;
    hdr[22] = h & 255; hdr[23] = h >> 8;
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    uint8_t line[320 * 3];
    sim_fb_lock();
    if (shown) sim_fb_settle();
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            rgb_of(shown ? g_sim.shown[y][x] : g_sim.fb[y][x], &r, &g, &b);
            line[x * 3] = b; line[x * 3 + 1] = g; line[x * 3 + 2] = r;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    sim_fb_unlock();
    fclose(f);
    return true;
}

static bool snap(const char *path) {
    return snap_buf(path, false);
}

// ---- gestures (on their own thread so the window keeps drawing) ---------------

static void pen_path(int x0, int y0, int x1, int y1, int ms) {
    int steps = ms / 15 > 1 ? ms / 15 : 1;
    sim_pen(true, x0, y0);
    for (int i = 1; i <= steps; i++) {
        usleep(15000);
        sim_pen(true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
    }
    usleep(30000);
    sim_pen(false, x1, y1);
    usleep(80000);
}

static void tap(int x, int y) { pen_path(x, y, x, y, 60); }
static void swipe(int dir) {
    if (dir > 0) pen_path(260, 120, 60, 120, 120);
    else pen_path(60, 120, 260, 120, 120);
}

static void *swipe_thread(void *p) {
    swipe((int)(intptr_t)p);
    return NULL;
}

// ---- the commands Muse sends (the dashboard ones, as app.c handles them) -------

static cJSON *ok(void) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return r;
}

static cJSON *fail(const char *msg) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg ? msg : "failed");
    return r;
}

static void show_image(const char *path) {
    SDL_Surface *img = SDL_LoadBMP(path);
    if (!img) {
        printf("can't load %s: %s\n", path, SDL_GetError());
        return;
    }
    SDL_Surface *rgb = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_RGB888, 0);
    SDL_FreeSurface(img);
    if (!rgb) return;
    dashboard_takeover_prepare();
    sim_fb_lock();
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 320; x++) {
            int sx = x * rgb->w / 320, sy = y * rgb->h / 240;
            uint32_t p = ((uint32_t *)((uint8_t *)rgb->pixels + sy * rgb->pitch))[sx];
            uint8_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            uint16_t v = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
            g_sim.fb[y][x] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
    sim_fb_unlock();
    SDL_FreeSurface(rgb);
    dashboard_takeover_begin();
}

static cJSON *muse_command(const char *cmd, cJSON *params) {
    const char *err = "bad parameters";
    // dashboard.card / notify take the fields, or all of them in "json" (an
    // object or a JSON string, as Muse sends it), like app.c.
    cJSON *json = cJSON_GetObjectItem(params, "json");
    if (!strcmp(cmd, "dashboard.card") || !strcmp(cmd, "dashboard.notify")) {
        cJSON *parsed = cJSON_IsString(json) ? cJSON_Parse(json->valuestring) : NULL;
        const cJSON *obj = parsed ? parsed : cJSON_IsObject(json) ? json : params;
        bool r = !strcmp(cmd, "dashboard.card") ? dashboard_card(obj, &err)
                                                : dashboard_notify(obj, &err);
        cJSON_Delete(parsed);
        return r ? ok() : fail(err);
    }
    if (!strcmp(cmd, "dashboard.data")) {
        cJSON *screen = cJSON_GetObjectItem(params, "screen");
        cJSON *data = cJSON_GetObjectItem(params, "data");
        if (!cJSON_IsObject(data)) data = json;
        if (!cJSON_IsString(screen)) return fail("screen is required");
        bool r = cJSON_IsObject(data) ? dashboard_data_set(screen->valuestring, data)
               : cJSON_IsString(data) ? dashboard_data_set_json(screen->valuestring,
                                                                 data->valuestring)
               : false;
        return r ? ok() : fail("unknown screen or bad JSON");
    }
    if (!strcmp(cmd, "dashboard.events")) {
        cJSON *r = ok();
        cJSON_AddItemToObject(r, "events",
                              dashboard_events(!cJSON_IsTrue(cJSON_GetObjectItem(params, "peek"))));
        return r;
    }
    if (!strcmp(cmd, "dashboard.stocks")) {
        cJSON *s = cJSON_GetObjectItem(params, "symbols");
        if (cJSON_IsString(s) && !dashboard_quotes_configure(s->valuestring, &err)) {
            return fail(err);
        }
        char status[200];
        dashboard_quotes_status(status, sizeof(status));
        cJSON *r = ok();
        cJSON_AddStringToObject(r, "status", status);
        return r;
    }
    if (!strcmp(cmd, "dashboard.calibrate")) {
        dashboard_calibrate(cJSON_IsTrue(cJSON_GetObjectItem(params, "reset")));
        return ok();
    }
    if (!strcmp(cmd, "dashboard.debug")) {
        char dbg[512];
        dashboard_debug(dbg, sizeof(dbg));
        cJSON *r = ok();
        cJSON_AddStringToObject(r, "debug", dbg);
        return r;
    }
    if (!strcmp(cmd, "display.show_animation")) {
        dashboard_takeover_end();
        return ok();
    }
    return NULL;
}

typedef struct {
    int ms;
    char prefix[201];
} record_t;

static void *record_thread(void *p) {
    record_t *r = p;
    int n = 0;
    for (int t = 0; t < r->ms && !s_quit; t += 40, n++) {
        char path[280];
        snprintf(path, sizeof(path), "%.200s_%04d.bmp", r->prefix, n % 10000);
        snap_buf(path, true);  // whole frames only
        usleep(40000);
    }
    printf("recorded %d frames\n", n);
    free(r);
    return NULL;
}

static void help(void) {
    printf("Muse commands: dashboard.card|notify|data|events|stocks|calibrate|debug,\n"
           "  display.show_animation, each followed by JSON params.\n"
           "Test commands: tap X Y | swipe left|right | hold X Y MS | sleep MS |\n"
           "  ldr RAW | link on|off | wifi RSSI|off | chat ok|fail | ota PCT|fail |\n"
           "  image FILE.bmp | snap FILE.bmp | record MS PREFIX | expect TEXT | quit\n");
}

static void run_line(char *line) {
    line[strcspn(line, "\r\n")] = '\0';
    while (*line == ' ') line++;
    if (!*line || *line == '#') return;
    char cmd[64] = "";
    int used = 0;
    sscanf(line, "%63s%n", cmd, &used);
    char *rest = line + used;
    while (*rest == ' ') rest++;
    int a = 0, b = 0, c = 0;
    char word[256] = "";
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        s_quit = true;
    } else if (!strcmp(cmd, "help")) {
        help();
    } else if (!strcmp(cmd, "tap") && sscanf(rest, "%d %d", &a, &b) == 2) {
        tap(a, b);
    } else if (!strcmp(cmd, "hold") && sscanf(rest, "%d %d %d", &a, &b, &c) == 3) {
        pen_path(a, b, a, b, c);
    } else if (!strcmp(cmd, "swipe")) {
        swipe(strncmp(rest, "left", 4) == 0 ? 1 : -1);
    } else if (!strcmp(cmd, "sleep") && sscanf(rest, "%d", &a) == 1) {
        usleep((useconds_t)a * 1000);
    } else if (!strcmp(cmd, "ldr") && sscanf(rest, "%d", &a) == 1) {
        g_sim.ldr = a;
    } else if (!strcmp(cmd, "link")) {
        g_sim.link = strncmp(rest, "on", 2) == 0;
        dashboard_set_link(g_sim.link);
    } else if (!strcmp(cmd, "wifi")) {
        g_sim.rssi = strncmp(rest, "off", 3) == 0 ? 0 : atoi(rest);
    } else if (!strcmp(cmd, "chat")) {
        g_sim.chat_ok = strncmp(rest, "fail", 4) != 0;
    } else if (!strcmp(cmd, "ota")) {
        dashboard_ota_progress(strncmp(rest, "fail", 4) == 0 ? -1 : atoi(rest));
    } else if (!strcmp(cmd, "image") && sscanf(rest, "%255s", word) == 1) {
        show_image(word);
    } else if (!strcmp(cmd, "expect")) {
        bool pass = strstr(s_last, rest) != NULL;
        if (!pass) s_failures++;
        printf("%s: expect \"%s\"%s%s\n", pass ? "PASS" : "FAIL", rest,
               pass ? "" : " in: ", pass ? "" : s_last);
    } else if (!strcmp(cmd, "record") && sscanf(rest, "%d %200s", &a, word) == 2) {
        // Frames as the panel shows them, e.g. for a GIF, while the script
        // goes on driving the dashboard.
        record_t *r = malloc(sizeof(*r));
        pthread_t th;
        if (r) {
            r->ms = a;
            snprintf(r->prefix, sizeof(r->prefix), "%.200s", word);
            if (pthread_create(&th, NULL, record_thread, r) == 0) pthread_detach(th);
            else free(r);
        }    } else if (!strcmp(cmd, "snap") && sscanf(rest, "%255s", word) == 1) {
        usleep(150000);  // let the frame in progress land
        printf("%s %s\n", snap(word) ? "saved" : "can't write", word);
    } else {
        cJSON *params = *rest ? cJSON_Parse(rest) : cJSON_CreateObject();
        if (!params) {
            printf("bad JSON params\n");
            return;
        }
        pthread_mutex_lock(&s_cmd_lock);
        cJSON *r = muse_command(cmd, params);
        pthread_mutex_unlock(&s_cmd_lock);
        cJSON_Delete(params);
        if (!r) {
            printf("unknown command '%s' (try help)\n", cmd);
            return;
        }
        char *out = cJSON_PrintUnformatted(r);
        printf("%s\n", out);
        snprintf(s_last, sizeof(s_last), "%s", out);
        free(out);
        cJSON_Delete(r);
    }
    fflush(stdout);
}

typedef struct {
    FILE *in;
    bool quit_at_end;
} console_t;

static void *console_thread(void *p) {
    console_t *con = p;
    char line[8192];
    while (!s_quit && fgets(line, sizeof(line), con->in)) run_line(line);
    if (con->quit_at_end) s_quit = true;
    return NULL;
}

// ---- Muse, through `musegadget dash-sim` -------------------------------------

static void serve_client(int fd) {
    FILE *f = fdopen(fd, "r+");
    if (!f) {
        close(fd);
        return;
    }
    static char line[16384];
    cJSON *r = NULL;
    if (fgets(line, sizeof(line), f)) {
        cJSON *req = cJSON_Parse(line);
        cJSON *cmd = cJSON_GetObjectItem(req, "command");
        cJSON *params = cJSON_GetObjectItem(req, "params");
        if (!cJSON_IsString(cmd)) {
            r = fail("expected {\"command\": ..., \"params\": {...}}");
        } else {
            if (!cJSON_IsObject(params)) {
                cJSON_DeleteItemFromObject(req, "params");
                params = cJSON_AddObjectToObject(req, "params");
            }
            char *shown = cJSON_PrintUnformatted(params);
            printf("[muse] %s %s\n", cmd->valuestring, shown ? shown : "");
            free(shown);
            pthread_mutex_lock(&s_cmd_lock);
            r = muse_command(cmd->valuestring, params);
            pthread_mutex_unlock(&s_cmd_lock);
            if (!r) r = fail("unknown command");
        }
        cJSON_Delete(req);
    }
    if (r) {
        char *out = cJSON_PrintUnformatted(r);
        if (out) fprintf(f, "%s\n", out);
        free(out);
        cJSON_Delete(r);
    }
    fclose(f);
}

static void *listen_thread(void *p) {
    int srv = (int)(intptr_t)p;
    while (!s_quit) {
        int fd = accept(srv, NULL, NULL);
        if (fd >= 0) serve_client(fd);
    }
    return NULL;
}

// Localhost only: anything that can connect can drive the dashboard.
static bool start_listener(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (srv < 0 || bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0
        || listen(srv, 4) != 0) {
        fprintf(stderr, "can't listen on 127.0.0.1:%d\n", port);
        if (srv >= 0) close(srv);
        return false;
    }
    printf("taking Muse commands on 127.0.0.1:%d\n", port);
    pthread_t th;
    pthread_create(&th, NULL, listen_thread, (void *)(intptr_t)srv);
    pthread_detach(th);
    return true;
}

// ---- window -------------------------------------------------------------------

static void present(SDL_Renderer *ren, SDL_Texture *tex) {
    // The LCD lit by the backlight: never fully black, so a dimmed screen is
    // still visible here.
    float light = 0.18f + 0.82f * (float)g_sim.backlight / 1023.0f;
    if (light > 1) light = 1;
    uint32_t *px;
    int pitch;
    if (SDL_LockTexture(tex, NULL, (void **)&px, &pitch) != 0) return;
    sim_fb_lock();
    for (int y = 0; y < 240; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)px + y * pitch);
        for (int x = 0; x < 320; x++) {
            uint8_t r, g, b;
            rgb_of(g_sim.fb[y][x], &r, &g, &b);
            row[x] = 0xff000000u | (uint32_t)(r * light) << 16
                     | (uint32_t)(g * light) << 8 | (uint32_t)(b * light);
        }
    }
    sim_fb_unlock();
    SDL_UnlockTexture(tex);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

static void title(SDL_Window *w) {
    char t[160];
    snprintf(t, sizeof(t), "CYD dashboard sim - backlight %d%%  light sensor %d  link %s",
             g_sim.backlight * 100 / 1023, g_sim.ldr, g_sim.link ? "up" : "down");
    SDL_SetWindowTitle(w, t);
}

int main(int argc, char **argv) {
    bool headless = false, chat_fail = false;
    const char *script = NULL;
    int listen_port = 0;
    bool register_link = true;
    snprintf(g_sim.nvs_path, sizeof(g_sim.nvs_path), "dash_sim_nvs.txt");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) s_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--nvs") && i + 1 < argc)
            snprintf(g_sim.nvs_path, sizeof(g_sim.nvs_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--swap-touch")) g_sim.touch_swap = true;
        else if (!strcmp(argv[i], "--invert-x")) g_sim.touch_invert_x = true;
        else if (!strcmp(argv[i], "--invert-y")) g_sim.touch_invert_y = true;
        else if (!strcmp(argv[i], "--chat-fail")) chat_fail = true;
        else if (!strcmp(argv[i], "--slow-spi")) g_sim.slow_spi = true;
        else if (!strcmp(argv[i], "--listen") && i + 1 < argc) listen_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-register")) register_link = false;
        else if (!strcmp(argv[i], "--muse-socket") && i + 1 < argc)
            snprintf(g_sim.muse_socket, sizeof(g_sim.muse_socket), "%s", argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--scale N] [--headless] [--script FILE] [--nvs FILE]\n"
                    "  [--swap-touch] [--invert-x] [--invert-y] [--chat-fail] [--slow-spi]\n"
                    "  [--listen PORT] [--muse-socket PATH] [--no-register]\n",
                    argv[0]);
            return 2;
        }
    }
    g_sim.chat_ok = !chat_fail;
    signal(SIGPIPE, SIG_IGN);  // a Muse client that hangs up mustn't end the sim
    setvbuf(stdout, NULL, _IOLBF, 0);
    sim_nvs_load();

    // Boot: the dashboard starts, then the Link session comes up.
    dashboard_init();
    dashboard_set_link(true);
    if (register_link) dashboard_link_registered();  // link.register went out
    dashboard_set_paired(true);
    if (listen_port && !start_listener(listen_port)) return 1;

    console_t con = {stdin, false};
    if (script) {
        con.in = fopen(script, "r");
        if (!con.in) {
            fprintf(stderr, "can't open %s\n", script);
            return 1;
        }
        con.quit_at_end = true;
    } else {
        help();
    }
    pthread_t th;
    pthread_create(&th, NULL, console_thread, &con);

    if (headless) {
        while (!s_quit) usleep(20000);
        return s_failures ? 1 : 0;
    }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL: %s (use --headless)\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("CYD dashboard sim", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 320 * s_scale,
                                       240 * s_scale, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, 320, 240);
    if (!win || !ren || !tex) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    int shots = 0;
    while (!s_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) s_quit = true;
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                sim_pen(true, e.button.x / s_scale, e.button.y / s_scale);
            } else if (e.type == SDL_MOUSEMOTION && g_sim.pen) {
                sim_pen(true, e.motion.x / s_scale, e.motion.y / s_scale);
            } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
                sim_pen(false, e.button.x / s_scale, e.button.y / s_scale);
            } else if (e.type == SDL_KEYDOWN) {
                pthread_t g;
                switch (e.key.keysym.sym) {
                    case SDLK_LEFT:
                    case SDLK_RIGHT:
                        pthread_create(&g, NULL, swipe_thread,
                                       (void *)(intptr_t)(e.key.keysym.sym == SDLK_RIGHT ? 1 : -1));
                        pthread_detach(g);
                        break;
                    case SDLK_c: dashboard_calibrate(false); break;
                    case SDLK_b: g_sim.ldr = 20; break;
                    case SDLK_d: g_sim.ldr = 900; break;
                    case SDLK_l:
                        g_sim.link = !g_sim.link;
                        dashboard_set_link(g_sim.link);
                        break;
                    case SDLK_s: {
                        char name[32];
                        snprintf(name, sizeof(name), "sim-%03d.bmp", shots++);
                        printf("%s %s\n", snap(name) ? "saved" : "can't write", name);
                        break;
                    }
                    case SDLK_q: s_quit = true; break;
                    default: break;
                }
            }
        }
        present(ren, tex);
        title(win);
        SDL_Delay(16);
    }
    SDL_Quit();
    return s_failures ? 1 : 0;
}
