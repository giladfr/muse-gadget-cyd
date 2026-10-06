/*
 * Host test of the live-quotes module (dash_quotes.c): one full round against
 * canned Nasdaq responses (fixtures from DeskPulse) fed in random-sized
 * chunks. Checks asset-class fallback, "UNCH", extended-hours prices, chart
 * streaming into the sparkline, pushed stocks being ignored, and unknown
 * symbols backing off. Run by run.sh.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static time_t g_now = 1791301320;  // Tue 2026-10-06 10:42 CDT = 11:42 ET (open)
time_t fake_time(time_t *t) { if (t) *t = g_now; return g_now; }
int64_t g_fake_us = 5000000;
#include "dash_quotes.c"

bool dash_clock_valid(void) { return true; }
void dash_clock_seed(int64_t s) {}
bool dash_clock_local(struct tm *o) { return false; }
static int updated;
void dashboard_data_updated(void) { updated++; }

// ---- fake HTTP: canned responses by URL, fed in random chunk sizes
static http_event_handle_cb s_h; static void *s_ud; static char s_url[256]; static int s_status;
static int s_requests;
static const char *QUOTE_NVDA = "{\"data\":{\"symbol\":\"NVDA\",\"companyName\":\"NVIDIA Corporation Common Stock\",\"marketStatus\":\"Open\",\"primaryData\":{\"lastSalePrice\":\"$123.45\",\"netChange\":\"-1.50\",\"percentageChange\":\"-1.20%\",\"bidPrice\":\"$123.40\",\"volume\":\"1,000,000\",\"isRealTime\":true}},\"message\":null,\"status\":{\"rCode\":200}}";
static const char *QUOTE_SPY = "{\"data\":{\"symbol\":\"SPY\",\"marketStatus\":\"Open\",\"primaryData\":{\"lastSalePrice\":\"$671.02\",\"netChange\":\"UNCH\",\"percentageChange\":\"\"}},\"status\":{\"rCode\":200}}";
static const char *QUOTE_AMD_CLOSED = "{\"data\":{\"marketStatus\":\"Closed\",\"primaryData\":{\"lastSalePrice\":\"$150.00\",\"netChange\":\"+2.00\",\"percentageChange\":\"+1.35%\"},\"secondaryData\":{\"lastSalePrice\":\"$151.00\",\"netChange\":\"+1.00\",\"percentageChange\":\"+0.67%\"}}}";
static const char *EMPTY = "{\"data\":null,\"message\":\"Symbol not exists\",\"status\":{\"rCode\":400}}";
static const char *NA = "{\"data\":{\"primaryData\":{\"lastSalePrice\":\"N/A\"}}}";
static char CHART[60000];

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) { s_h = c->event_handler; s_ud = c->user_data; return (void *)1; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *u) { snprintf(s_url, sizeof s_url, "%s", u); return 0; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) { return 0; }
static void feed(const char *body) {
    int len = (int)strlen(body), off = 0;
    while (off < len) {
        int n = 1 + rand() % 700; if (off + n > len) n = len - off;
        esp_http_client_event_t e = {.event_id = HTTP_EVENT_ON_DATA, .data = (void *)(body + off), .data_len = n, .user_data = s_ud};
        s_h(&e); off += n;
    }
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    s_requests++;
    s_status = 200;
    const char *b = EMPTY;
    if (strstr(s_url, "/chart?")) b = CHART;
    else if (strstr(s_url, "/NVDA/info?assetclass=stocks")) b = QUOTE_NVDA;
    else if (strstr(s_url, "/SPY/info?assetclass=etf")) b = QUOTE_SPY;       // ETF: only "etf" answers
    else if (strstr(s_url, "/AMD/info?assetclass=stocks")) b = QUOTE_AMD_CLOSED;
    else if (strstr(s_url, "/XYZ/")) b = NA;
    feed(b);
    return 0;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return s_status; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { return 0; }
esp_err_t esp_crt_bundle_attach(void *c) { return 0; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { return -1; }
void nvs_close(nvs_handle_t h) {}
esp_err_t nvs_commit(nvs_handle_t h) { return 0; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *l) { return -1; }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return 0; }
void vTaskDelete(TaskHandle_t t) {}
static TaskFunction_t s_task;
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t st, void *a, UBaseType_t p, TaskHandle_t *h) { s_task = f; return 1; }
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } else printf("ok: %s\n", #c); } while (0)

int main(void) {
    // A day's chart: 400 points rising 100 -> 140, with "z" objects around.
    int o = sprintf(CHART, "{\"data\":{\"symbol\":\"NVDA\",\"previousClose\":\"$100.00\",\"chart\":[");
    for (int i = 0; i < 400; i++)
        o += sprintf(CHART + o, "%s{\"z\":{\"time\":\"%d\",\"price\":\"$1\"},\"x\":%lld,\"y\":%.2f}", i ? "," : "", i, 1790000000000LL + i * 60000LL, 100 + i * 0.1);
    sprintf(CHART + o, "]}}");
    srand(7);

    const char *err = NULL;
    dash_quotes_init();
    CHECK(dash_quotes_configure("nvda, spy,AMD,xyz", &err), "configure: %s", err);
    CHECK(!dash_quotes_configure("TOO,MANY,SYMBOLS,IN,THIS,LIST,FOR,THE,BOARD", &err), "9 symbols rejected");
    CHECK(!dash_quotes_configure("bad$sym", &err), "bad symbol rejected");

    dash_quotes_tick(true);
    CHECK(s_task != NULL, "round started");
    s_task(NULL);
    printf("requests: %d\n", s_requests);
    dash_store_t st;
    dash_store_snapshot(&st);
    CHECK(st.n_stocks == 3, "3 quotes (XYZ dropped): %d", st.n_stocks);
    CHECK(strcmp(st.stocks[0].symbol, "NVDA") == 0 && st.stocks[0].price == 123.45 && st.stocks[0].change == -1.5 && st.stocks[0].change_pct == -1.2, "NVDA parsed");
    CHECK(strcmp(st.stocks[0].market, "Open") == 0, "NVDA market %s", st.stocks[0].market);
    CHECK(strcmp(st.stocks[1].symbol, "SPY") == 0 && st.stocks[1].price == 671.02 && st.stocks[1].change == 0, "SPY via etf class, UNCH -> 0");
    CHECK(st.stocks[2].price == 151.0 && st.stocks[2].change == 1.0, "AMD closed -> extended price %.2f", st.stocks[2].price);
    CHECK(s_syms[1].asset == 1, "SPY asset class remembered (etf): %d", s_syms[1].asset);
    CHECK(st.stocks[0].spark && st.stocks[0].history_n == 32, "NVDA sparkline from chart (%d pts)", st.stocks[0].history_n);
    CHECK(st.stocks[0].history[0] > 99.9 && st.stocks[0].history[0] < 100.1, "chart start %.2f", st.stocks[0].history[0]);
    CHECK(st.stocks[0].history[31] == (float)123.45, "tip at live price %.2f", st.stocks[0].history[31]);
    CHECK(st.stocks[0].history[30] > 138 && st.stocks[0].history[30] < 140, "chart end %.2f", st.stocks[0].history[30]);
    CHECK(updated == 1, "dashboard notified");

    // Pushed stocks are ignored while the board owns the quotes.
    cJSON *push = cJSON_Parse("{\"stocks\":[{\"symbol\":\"TSLA\",\"price\":1}],\"weather\":{\"temp\":70}}");
    dash_store_set_bridge(push); cJSON_Delete(push);
    dash_store_snapshot(&st);
    CHECK(st.n_stocks == 3 && st.weather_valid && st.weather.temp == 70, "push: stocks ignored, weather taken");

    // Second round: known classes, chart not refetched (< 5 min).
    int before = s_requests; s_now = true; s_task = NULL;
    dash_quotes_tick(true); s_task(NULL);
    printf("second round requests: %d\n", s_requests - before);
    CHECK(s_requests - before == 3, "one request per known symbol, unknown XYZ skipped");
    char status[160]; dash_quotes_status(status, sizeof status); printf("%s\n", status);
    printf("ALL OK\n");
}
