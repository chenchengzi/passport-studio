// examples/t3-status/main/t3_logic.c
#include "t3_logic.h"

#include <string.h>

#define MAX_COUNT 9999

// ---------------------------------------------------------------------------
// Bridge JSON: one flat object of booleans, non-negative integers, strings,
// and nulls. Nested values are rejected; the bridge never sends them.
// ---------------------------------------------------------------------------
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// Cut a NUL-terminated UTF-8 string of length n back to a character boundary.
static void utf8_trim(char *s, size_t n)
{
    size_t end = n;
    while (end > 0 && ((unsigned char)s[end - 1] & 0xC0) == 0x80) end--;
    if (end == 0) {
        s[0] = '\0';
        return;
    }
    unsigned char lead = (unsigned char)s[end - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    s[n - (end - 1) >= need ? n : end - 1] = '\0';
}

// Returns the character after the closing quote, or NULL. Copies the content
// into out (cut at a UTF-8 boundary) when out is not NULL. Escapes keep the
// escaped character; the bridge never sends \u escapes.
static const char *read_string(const char *p, char *out, size_t cap)
{
    if (*p != '"') return NULL;
    p++;
    size_t n = 0;
    bool cut = false;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) return NULL;
        }
        if (out && n + 1 < cap) out[n++] = *p;
        else cut = true;
        p++;
    }
    if (out && cap) {
        out[n] = '\0';
        if (cut) utf8_trim(out, n);
    }
    return *p == '"' ? p + 1 : NULL;
}

static const char *read_uint(const char *p, int64_t *out)
{
    if (*p < '0' || *p > '9') return NULL;
    int64_t v = 0;
    while (*p >= '0' && *p <= '9') {
        if (v > (INT64_MAX - 9) / 10) return NULL;
        v = v * 10 + (*p++ - '0');
    }
    if (*p == '.' || *p == 'e' || *p == 'E') return NULL;
    *out = v;
    return p;
}

bool t3_parse_status(const char *json, t3_status_t *out)
{
    if (!json || !out) return false;
    t3_status_t s = {0};
    bool have_online = false, have_run = false, have_wait = false;
    const char *p = skip_ws(json);
    if (*p++ != '{') return false;
    p = skip_ws(p);
    if (*p == '}') return false;
    for (;;) {
        char key[16];
        p = read_string(skip_ws(p), key, sizeof(key));
        if (!p) return false;
        p = skip_ws(p);
        if (*p++ != ':') return false;
        p = skip_ws(p);

        if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
            bool v = *p == 't';
            p += v ? 4 : 5;
            if (!strcmp(key, "online")) {
                s.online = v;
                have_online = true;
            }
        } else if (!strncmp(p, "null", 4)) {
            p += 4;
        } else if (*p == '"') {
            bool title = !strcmp(key, "done_title");
            p = read_string(p, title ? s.done_title : NULL, title ? sizeof(s.done_title) : 0);
            if (!p) return false;
        } else {
            int64_t v;
            p = read_uint(p, &v);
            if (!p) return false;
            bool count = !strcmp(key, "run") || !strcmp(key, "wait") || !strcmp(key, "threads");
            if (count && v > MAX_COUNT) return false;
            if (!strcmp(key, "run")) {
                s.run = (int)v;
                have_run = true;
            } else if (!strcmp(key, "wait")) {
                s.wait = (int)v;
                have_wait = true;
            } else if (!strcmp(key, "threads")) {
                s.threads = (int)v;
            } else if (!strcmp(key, "done_at")) {
                s.done_at = v;
            }
        }

        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '}') return false;
        if (*skip_ws(p + 1)) return false;
        break;
    }
    if (!have_online || !have_run || !have_wait) return false;
    *out = s;
    return true;
}

bool t3_done_changed(int64_t prev, int64_t now)
{
    return prev > 0 && now > prev;
}

// ---------------------------------------------------------------------------
// Task list
// ---------------------------------------------------------------------------
// Copy [p, end) into out, cut at a UTF-8 character boundary to fit cap.
static void copy_field(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = (size_t)(end - p);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)p[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

// Split [p, end) at tabs into at most max fields; the last takes the rest.
static int split_tabs(const char *p, const char *end, const char **f, const char **fe, int max)
{
    int n = 0;
    while (n < max) {
        f[n] = p;
        if (n == max - 1) {
            fe[n++] = end;
            break;
        }
        while (p < end && *p != '\t') p++;
        fe[n++] = p;
        if (p == end) break;
        p++;
    }
    return n;
}

static bool field_is(const char *p, const char *end, const char *word)
{
    size_t n = strlen(word);
    return (size_t)(end - p) == n && !memcmp(p, word, n);
}

static bool parse_count(const char *p, const char *end, int *out)
{
    if (p == end || end - p > 4) return false;
    int v = 0;
    for (; p < end; p++) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + (*p - '0');
    }
    *out = v;
    return true;
}

static t3_req_kind_t request_kind(const char *p, const char *end)
{
    static const struct { const char *name; t3_req_kind_t kind; } KINDS[] = {
        {"command", T3_REQ_COMMAND},          {"file-read", T3_REQ_FILE_READ},
        {"file-change", T3_REQ_FILE_CHANGE},  {"permission", T3_REQ_PERMISSION},
        {"user_input", T3_REQ_QUESTION},      {"mcp-elicitation", T3_REQ_MCP},
        {"dynamic_tool_call", T3_REQ_TOOL},   {"auth_refresh", T3_REQ_AUTH},
    };
    for (size_t i = 0; i < sizeof(KINDS) / sizeof(KINDS[0]); i++) {
        if (field_is(p, end, KINDS[i].name)) return KINDS[i].kind;
    }
    return T3_REQ_OTHER;
}

bool t3_parse_detail(const char *text, t3_detail_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!text) return false;
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        const char *f[3], *fe[3];
        int n = split_tabs(p, end, f, fe, 3);
        if (field_is(f[0], fe[0], "req")) {
            if (n != 3) return false;
            out->has_request = true;
            out->request_kind = request_kind(f[1], fe[1]);
            copy_field(f[2], fe[2], out->request, sizeof(out->request));
        } else if (field_is(f[0], fe[0], "msg")) {
            if (n < 2) return false;
            copy_field(f[1], end, out->message, sizeof(out->message));
        } else if (field_is(f[0], fe[0], "bg")) {
            if (n != 3 || !parse_count(f[1], fe[1], &out->background)) return false;
            copy_field(f[2], fe[2], out->background_text, sizeof(out->background_text));
        }
        p = *end ? end + 1 : end;
    }
    return true;
}

static bool parse_task(const char *p, const char *end, t3_task_t *t)
{
    const char *f[5];
    const char *fe[5];
    for (int i = 0; i < 5; i++) {
        f[i] = p;
        while (p < end && *p != '\t') p++;
        fe[i] = p;
        if (i < 4) {
            if (p == end) return false;
            p++;
        }
    }
    if (p != end) return false;   // more than five fields

    if (fe[0] - f[0] != 1 || (*f[0] != 'w' && *f[0] != 'r')) return false;
    int64_t started = 0;
    if (fe[1] == f[1] || fe[1] - f[1] > 12) return false;
    for (const char *c = f[1]; c < fe[1]; c++) {
        if (*c < '0' || *c > '9') return false;
        started = started * 10 + (*c - '0');
    }
    if (fe[2] == f[2] || fe[2] - f[2] > T3_TASK_ID_LEN) return false;
    for (const char *c = f[2]; c < fe[2]; c++) {   // it goes into a URL path
        if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z'))) return false;
    }

    t->waiting = *f[0] == 'w';
    t->started = started;
    copy_field(f[2], fe[2], t->id, sizeof(t->id));
    copy_field(f[3], fe[3], t->project, sizeof(t->project));
    copy_field(f[4], fe[4], t->title, sizeof(t->title));
    return true;
}

int t3_parse_tasks(const char *text, t3_task_t *out, int max)
{
    if (!text || !out || max < 0) return -1;
    int n = 0;
    const char *p = text;
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        if (end > p && n < max) {
            if (!parse_task(p, end, &out[n])) return -1;
            n++;
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

#define CLOCK_SET_AFTER 1672531200   // 2023-01-01: earlier means not synced

int64_t t3_elapsed_min(int64_t started, int64_t now)
{
    if (started <= 0 || now < CLOCK_SET_AFTER || now < started - 60) return -1;
    return now > started ? (now - started) / 60 : 0;
}

// ---------------------------------------------------------------------------
// Task pages
// ---------------------------------------------------------------------------
static void nav_select(t3_nav_t *nav, int page, const t3_task_t *tasks, int count)
{
    nav->page = page;
    if (page > 0 && page <= count) memcpy(nav->id, tasks[page - 1].id, sizeof(nav->id));
    else nav->id[0] = '\0';
}

bool t3_nav_key(t3_nav_t *nav, t3_key_t key, const t3_task_t *tasks, int count)
{
    int page = nav->page;
    if (page == 0) {
        if (key == T3_KEY_DOWN) page = 1;
    } else if (key == T3_KEY_OK) {
        page = 0;
    } else if (key == T3_KEY_UP) {
        page--;
    } else if (page < count) {
        page++;
    }
    if (page == nav->page) return false;
    nav_select(nav, page, tasks, count);
    return true;
}

void t3_nav_sync(t3_nav_t *nav, const t3_task_t *tasks, int count)
{
    if (nav->page == 0) return;
    for (int i = 0; nav->id[0] && i < count; i++) {
        if (!strcmp(tasks[i].id, nav->id)) {
            nav->page = i + 1;
            return;
        }
    }
    int page = nav->page;
    if (page > count) page = count > 0 ? count : 1;
    nav_select(nav, page, tasks, count);
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------
void t3_health_reset(t3_health_t *h)
{
    memset(h, 0, sizeof(*h));
}

void t3_health_ok(t3_health_t *h, const t3_status_t *s)
{
    h->last = *s;
    h->have_last = true;
    h->fails = 0;
}

void t3_health_fail(t3_health_t *h)
{
    if (h->fails < 1000) h->fails++;
}

t3_view_t t3_health_view(const t3_health_t *h)
{
    if (h->fails >= T3_FAILS_UNREACHABLE) return T3_VIEW_UNREACHABLE;
    if (!h->have_last) return T3_VIEW_CONNECTING;
    return h->last.online ? T3_VIEW_LIVE : T3_VIEW_T3_OFFLINE;
}

// ---------------------------------------------------------------------------
// Bridge address
// ---------------------------------------------------------------------------
static bool host_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
}

bool t3_parse_address(const char *text, char *host, size_t cap, uint16_t *port)
{
    if (!text || !host || cap < 2 || !port) return false;
    while (*text == ' ') text++;
    size_t end = strlen(text);
    while (end > 0 && text[end - 1] == ' ') end--;

    size_t n = 0;
    while (n < end && host_char(text[n])) n++;
    if (n == 0 || n >= cap || text[0] == '.' || text[0] == '-') return false;

    uint32_t p = T3_DEFAULT_PORT;
    if (n < end) {
        if (text[n] != ':' || n + 1 == end || end - (n + 1) > 5) return false;
        p = 0;
        for (size_t i = n + 1; i < end; i++) {
            if (text[i] < '0' || text[i] > '9') return false;
            p = p * 10 + (uint32_t)(text[i] - '0');
        }
        if (p == 0 || p > 65535) return false;
    }
    memcpy(host, text, n);
    host[n] = '\0';
    *port = (uint16_t)p;
    return true;
}

// ---------------------------------------------------------------------------
// Wi-Fi candidates
// ---------------------------------------------------------------------------
size_t t3_candidates(const t3_network_t *list, size_t list_len, const t3_network_t *driver,
                     t3_candidate_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < list_len && n < max; i++) {
        if (!list[i].ssid[0]) continue;
        out[n].net = list[i];
        out[n].rank = (uint8_t)(i + 1);
        n++;
    }
    if (driver && driver->ssid[0] && n < max) {
        for (size_t i = 0; i < list_len; i++) {
            if (!strncmp(list[i].ssid, driver->ssid, sizeof(list[i].ssid))) return n;
        }
        out[n].net = *driver;
        out[n].rank = 0;
        n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Clock text
// ---------------------------------------------------------------------------
// Days since 1970-01-01 to month and day (Howard Hinnant's civil_from_days).
static void civil(int64_t days, int *month, int *day)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
}

static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

void t3_format_done(int64_t done_at, int64_t now, int32_t tz_min, char out[6])
{
    if (done_at <= 0) {
        memcpy(out, "--:--", 6);
        return;
    }
    int64_t local = done_at + (int64_t)tz_min * 60;
    int64_t day = floor_div(local, 86400);
    bool same_day = now < CLOCK_SET_AFTER || floor_div(now + (int64_t)tz_min * 60, 86400) == day;
    int a, b;
    if (same_day) {
        int64_t secs = local - day * 86400;
        a = (int)(secs / 3600);
        b = (int)(secs / 60 % 60);
    } else {
        civil(day, &a, &b);
    }
    out[0] = (char)('0' + a / 10);
    out[1] = (char)('0' + a % 10);
    out[2] = same_day ? ':' : '-';
    out[3] = (char)('0' + b / 10);
    out[4] = (char)('0' + b % 10);
    out[5] = '\0';
}
