// Host tests for examples/t3-status/main/t3_logic.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "t3_logic.h"

static t3_network_t net(const char *ssid, const char *pass)
{
    t3_network_t n = {0};
    strcpy(n.ssid, ssid);
    strcpy(n.password, pass);
    return n;
}

static void test_parse(void)
{
    t3_status_t s;
    // The bridge's actual output.
    assert(t3_parse_status("{\"online\":true,\"run\":2,\"wait\":0,\"done_at\":1795425160,\"threads\":93}", &s));
    assert(s.online && s.run == 2 && s.wait == 0 && s.done_at == 1795425160 && s.threads == 93);

    // Whitespace, key order, unknown keys of every flat type.
    assert(t3_parse_status(" { \"wait\" : 1 , \"extra\":\"a\\\"b\", \"n\":null, \"f\":false,"
                           " \"run\":0, \"online\":false }\n", &s));
    assert(!s.online && s.run == 0 && s.wait == 1 && s.done_at == 0 && s.threads == 0);

    // Failures leave the output unchanged.
    t3_status_t keep = {.online = true, .run = 7};
    s = keep;
    static const char *const bad[] = {
        "", "null", "[]", "{}", "{\"online\":true,\"run\":1}",            // missing wait
        "{\"online\":true,\"run\":-1,\"wait\":0}",                         // negative
        "{\"online\":true,\"run\":1.5,\"wait\":0}",                        // fraction
        "{\"online\":true,\"run\":10000,\"wait\":0}",                      // out of range
        "{\"online\":true,\"run\":1,\"wait\":0,\"x\":{}}",                 // nested
        "{\"online\":true,\"run\":1,\"wait\":0",                           // truncated
        "{\"online\":true,\"run\":1,\"wait\":0}x",                         // trailing data
        "{\"online\":true,\"run\":1,\"wait\":0,\"done_at\":99999999999999999999}",
        "{\"online\":true,\"run\":1,,\"wait\":0}",
        "{\"online\":true,\"run\":1,\"wait\":0,\"s\":\"unterminated}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!t3_parse_status(bad[i], &s));
        assert(s.online && s.run == 7);
    }
    assert(!t3_parse_status(NULL, &s));

    // The finished task's title; escapes; a long title cut at a character.
    assert(t3_parse_status("{\"online\":true,\"run\":0,\"wait\":0,\"done_title\":\"Fix \\\"a\\\"\"}", &s));
    assert(!strcmp(s.done_title, "Fix \"a\""));
    char json[512] = "{\"online\":true,\"run\":0,\"wait\":0,\"done_title\":\"";
    for (int i = 0; i < 50; i++) strcat(json, "\xe4\xbb\xbb");   // 150 bytes
    strcat(json, "\"}");
    assert(t3_parse_status(json, &s));
    assert(strlen(s.done_title) == 120);
    // Other string keys never fill done_title.
    assert(t3_parse_status("{\"online\":true,\"run\":0,\"wait\":0,\"x\":\"y\"}", &s) && !s.done_title[0]);

    assert(!t3_done_changed(0, 1795425160));   // first answer
    assert(t3_done_changed(1795425160, 1795425200));
    assert(!t3_done_changed(1795425160, 1795425160));
    assert(!t3_done_changed(1795425160, 1795000000));   // bridge restarted
    assert(!t3_done_changed(1795425160, 0));
}

static void test_detail(void)
{
    t3_detail_t d;
    assert(t3_parse_detail("req\tcommand\tgit push\nmsg\tDone. Next\tstep\nbg\t2\tnpm run dev / review\n", &d));
    assert(d.has_request && d.request_kind == T3_REQ_COMMAND && !strcmp(d.request, "git push"));
    assert(!strcmp(d.message, "Done. Next\tstep"));   // a reply may keep its own tabs
    assert(d.background == 2 && !strcmp(d.background_text, "npm run dev / review"));

    static const struct { const char *name; t3_req_kind_t kind; } kinds[] = {
        {"file-read", T3_REQ_FILE_READ}, {"file-change", T3_REQ_FILE_CHANGE},
        {"permission", T3_REQ_PERMISSION}, {"user_input", T3_REQ_QUESTION},
        {"mcp-elicitation", T3_REQ_MCP}, {"dynamic_tool_call", T3_REQ_TOOL},
        {"auth_refresh", T3_REQ_AUTH}, {"something-new", T3_REQ_OTHER},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        char line[64];
        snprintf(line, sizeof(line), "req\t%s\t", kinds[i].name);
        assert(t3_parse_detail(line, &d) && d.has_request && d.request_kind == kinds[i].kind && !d.request[0]);
    }

    // Empty, unknown lines, no final newline.
    assert(t3_parse_detail("", &d) && !d.has_request && !d.message[0] && d.background == 0);
    assert(t3_parse_detail("new\tfield\nmsg\thi", &d) && !strcmp(d.message, "hi"));

    // Long text cut at a character boundary.
    char text[700] = "msg\t";
    for (int i = 0; i < 200; i++) strcat(text, "\xe4\xbb\xbb");
    assert(t3_parse_detail(text, &d) && strlen(d.message) == 360);

    static const char *const bad[] = {"req\tcommand", "msg", "bg\t2", "bg\tx\tt", "bg\t12345\tt", "bg\t\tt"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!t3_parse_detail(bad[i], &d));
    }
    assert(!t3_parse_detail(NULL, &d));
}

static void test_tasks(void)
{
    t3_task_t t[T3_MAX_TASKS];
    const char *reply =
        "w\t1795425000\tab12cd34\tpassport-studio\t\xe7\xad\x89\xe6\x89\xb9\xe5\x87\x86\n"
        "r\t0\tff00\t\tUntitled\n";
    assert(t3_parse_tasks(reply, t, T3_MAX_TASKS) == 2);
    assert(t[0].waiting && t[0].started == 1795425000 && !strcmp(t[0].id, "ab12cd34"));
    assert(!strcmp(t[0].project, "passport-studio") && !strcmp(t[0].title, "\xe7\xad\x89\xe6\x89\xb9\xe5\x87\x86"));
    assert(!t[1].waiting && t[1].started == 0 && !strcmp(t[1].project, "") && !strcmp(t[1].title, "Untitled"));

    // Empty list, blank lines, no final newline, more lines than room.
    assert(t3_parse_tasks("", t, T3_MAX_TASKS) == 0);
    assert(t3_parse_tasks("\nr\t1\ta\tp\tt", t, T3_MAX_TASKS) == 1);
    assert(t3_parse_tasks(reply, t, 1) == 1 && t[0].waiting);

    // A long title is cut before a split UTF-8 character (3 bytes each).
    char line[600] = "r\t1\tid\tp\t";
    for (int i = 0; i < 100; i++) strcat(line, "\xe4\xbb\xbb");
    assert(t3_parse_tasks(line, t, 1) == 1);
    assert(strlen(t[0].title) == 180);

    static const char *const bad[] = {
        "x\t1\tid\tp\tt",          // state
        "r\t\tid\tp\tt",           // no start
        "r\t-1\tid\tp\tt",         // negative
        "r\t1\t\tp\tt",            // no id
        "r\t1\t123456789\tp\tt",   // id too long
        "r\t1\ta/b\tp\tt",         // id not alphanumeric
        "r\t1\tid\tp",              // four fields
        "r\t1\tid\tp\tt\textra",  // six fields
        "r\t1\tid\tp\tt\nbad",    // a later line
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(t3_parse_tasks(bad[i], t, T3_MAX_TASKS) == -1);
    }
    assert(t3_parse_tasks(NULL, t, T3_MAX_TASKS) == -1);

    const int64_t now = 1795425160;
    assert(t3_elapsed_min(now - 125, now) == 2);
    assert(t3_elapsed_min(now + 30, now) == 0);      // small skew
    assert(t3_elapsed_min(now + 120, now) == -1);
    assert(t3_elapsed_min(0, now) == -1);
    assert(t3_elapsed_min(100, 200) == -1);           // clock not set
}

static t3_task_t task(const char *id)
{
    t3_task_t t = {0};
    strcpy(t.id, id);
    return t;
}

static void test_nav(void)
{
    t3_task_t t[3] = {task("a"), task("b"), task("c")};
    t3_nav_t nav = {0};

    assert(!t3_nav_key(&nav, T3_KEY_UP, t, 3) && nav.page == 0);
    assert(!t3_nav_key(&nav, T3_KEY_OK, t, 3) && nav.page == 0);
    assert(t3_nav_key(&nav, T3_KEY_DOWN, t, 3) && nav.page == 1 && !strcmp(nav.id, "a"));
    assert(t3_nav_key(&nav, T3_KEY_DOWN, t, 3) && nav.page == 2 && !strcmp(nav.id, "b"));
    assert(t3_nav_key(&nav, T3_KEY_DOWN, t, 3) && nav.page == 3);
    assert(!t3_nav_key(&nav, T3_KEY_DOWN, t, 3) && nav.page == 3);   // stays on the last
    assert(t3_nav_key(&nav, T3_KEY_UP, t, 3) && nav.page == 2);
    assert(t3_nav_key(&nav, T3_KEY_OK, t, 3) && nav.page == 0 && !nav.id[0]);
    assert(t3_nav_key(&nav, T3_KEY_DOWN, t, 3) && t3_nav_key(&nav, T3_KEY_UP, t, 3) && nav.page == 0);

    // Follow the selection when the list reorders.
    t3_nav_key(&nav, T3_KEY_DOWN, t, 3);
    t3_nav_key(&nav, T3_KEY_DOWN, t, 3);   // "b"
    t3_task_t moved[3] = {task("x"), task("a"), task("b")};
    t3_nav_sync(&nav, moved, 3);
    assert(nav.page == 3 && !strcmp(nav.id, "b"));
    // Gone: same position, clamped.
    t3_task_t fewer[2] = {task("x"), task("a")};
    t3_nav_sync(&nav, fewer, 2);
    assert(nav.page == 2 && !strcmp(nav.id, "a"));
    // Empty: one empty page, then the first task once one appears.
    t3_nav_sync(&nav, NULL, 0);
    assert(nav.page == 1 && !nav.id[0]);
    assert(!t3_nav_key(&nav, T3_KEY_DOWN, NULL, 0) && nav.page == 1);
    t3_nav_sync(&nav, t, 3);
    assert(nav.page == 1 && !strcmp(nav.id, "a"));
    // Home is left alone.
    nav = (t3_nav_t){0};
    t3_nav_sync(&nav, t, 3);
    assert(nav.page == 0);
    // Empty list from home: an empty page.
    assert(t3_nav_key(&nav, T3_KEY_DOWN, NULL, 0) && nav.page == 1 && !nav.id[0]);
}

static void test_health(void)
{
    t3_health_t h;
    t3_health_reset(&h);
    assert(t3_health_view(&h) == T3_VIEW_CONNECTING);
    t3_health_fail(&h);
    t3_health_fail(&h);
    assert(t3_health_view(&h) == T3_VIEW_CONNECTING);
    t3_health_fail(&h);
    assert(t3_health_view(&h) == T3_VIEW_UNREACHABLE);

    t3_status_t live = {.online = true, .run = 1};
    t3_health_ok(&h, &live);
    assert(t3_health_view(&h) == T3_VIEW_LIVE);
    // Brief losses keep the last good status on screen.
    t3_health_fail(&h);
    t3_health_fail(&h);
    assert(t3_health_view(&h) == T3_VIEW_LIVE && h.last.run == 1);
    t3_health_fail(&h);
    assert(t3_health_view(&h) == T3_VIEW_UNREACHABLE);

    t3_status_t offline = {.online = false};
    t3_health_ok(&h, &offline);
    assert(t3_health_view(&h) == T3_VIEW_T3_OFFLINE);
    for (int i = 0; i < 5000; i++) t3_health_fail(&h);   // saturates, no wrap
    assert(t3_health_view(&h) == T3_VIEW_UNREACHABLE);
}

static void test_address(void)
{
    char host[32];
    uint16_t port;
    assert(t3_parse_address("192.168.1.10", host, sizeof(host), &port));
    assert(!strcmp(host, "192.168.1.10") && port == T3_DEFAULT_PORT);
    assert(t3_parse_address("  my-mac.local:9000 ", host, sizeof(host), &port));
    assert(!strcmp(host, "my-mac.local") && port == 9000);
    assert(t3_parse_address("h:65535", host, sizeof(host), &port) && port == 65535);

    static const char *const bad[] = {
        "", "   ", ":8787", "host:", "host:0", "host:65536", "host:123456", "host:80a",
        "http://host", "host/status", "ho st", "-host", ".host", "host:80:81",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(!t3_parse_address(bad[i], host, sizeof(host), &port));
    }
    char tiny[4];
    assert(!t3_parse_address("abcd", tiny, sizeof(tiny), &port));
    assert(t3_parse_address("abc", tiny, sizeof(tiny), &port));
}

static void test_candidates(void)
{
    t3_candidate_t out[T3_MAX_CANDIDATES];
    t3_network_t list[3] = {net("Home", "password1"), net("", ""), net("Lab", "")};
    t3_network_t driver = net("Office", "officepass");
    size_t n = t3_candidates(list, 3, &driver, out, T3_MAX_CANDIDATES);
    assert(n == 3);
    assert(!strcmp(out[0].net.ssid, "Home") && out[0].rank == 1);
    assert(!strcmp(out[1].net.ssid, "Lab") && out[1].rank == 3);
    assert(!strcmp(out[2].net.ssid, "Office") && out[2].rank == 0);

    driver = net("Lab", "");
    assert(t3_candidates(list, 3, &driver, out, T3_MAX_CANDIDATES) == 2);
    driver = net("Office", "officepass");
    assert(t3_candidates(NULL, 0, &driver, out, T3_MAX_CANDIDATES) == 1);
    assert(t3_candidates(NULL, 0, NULL, out, T3_MAX_CANDIDATES) == 0);
    assert(t3_candidates(list, 3, &driver, out, 1) == 1);
}

static void test_clock(void)
{
    char out[6];
    // 2026-11-23 09:12:40 UTC.
    const int64_t done = 1795425160;
    t3_format_done(0, done, 0, out);
    assert(!strcmp(out, "--:--"));
    t3_format_done(done, done + 60, 0, out);
    assert(!strcmp(out, "09:12"));
    t3_format_done(done, done + 60, 480, out);     // UTC+8
    assert(!strcmp(out, "17:12"));
    t3_format_done(done, done + 86400, 480, out);  // a day later
    assert(!strcmp(out, "11-23"));
    t3_format_done(done, done + 14 * 3600, 0, out);  // 23:12 same UTC day
    assert(!strcmp(out, "09:12"));
    t3_format_done(done, done + 15 * 3600, 0, out);  // 00:12 next day
    assert(!strcmp(out, "11-23"));
    t3_format_done(done, done, -600, out);         // UTC-10: 23:12 on the 22nd
    assert(!strcmp(out, "23:12"));
    t3_format_done(done, done + 3600, -600, out);  // now 00:12 on the 23rd
    assert(!strcmp(out, "11-22"));
    t3_format_done(done, 5, 480, out);             // clock not set yet
    assert(!strcmp(out, "17:12"));
    t3_format_done(1709208000, 1795425160, 0, out);  // 2024-02-29 12:00 UTC
    assert(!strcmp(out, "02-29"));
}

int main(void)
{
    test_parse();
    test_detail();
    test_tasks();
    test_nav();
    test_health();
    test_address();
    test_candidates();
    test_clock();
    puts("t3 status logic tests: PASS");
    return 0;
}
