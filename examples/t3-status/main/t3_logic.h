// examples/t3-status/main/t3_logic.h
// Pure logic for the T3 Code status display: the bridge's JSON and task list,
// connection health, task page navigation, the bridge address setting, Wi-Fi
// candidates, and clock text.
// No ESP-IDF headers; covered by tests/test_t3_logic.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define T3_DEFAULT_PORT     8787
#define T3_MAX_CANDIDATES   6     // five saved networks plus the driver's own setting
#define T3_FAILS_UNREACHABLE 3    // consecutive failed polls before "unreachable"
#define T3_MAX_TASKS        8     // the bridge sends at most this many
#define T3_TASK_ID_LEN      8

// GET /status from bridge/bridge.mjs:
//   {"online":true,"run":2,"wait":1,"done_at":1795425160,"done_title":"Fix login","threads":93}
typedef struct {
    bool online;        // the bridge is subscribed to T3 Code
    int run;            // threads with a running turn
    int wait;           // threads waiting for the user's approval
    int64_t done_at;    // Unix seconds of the latest completed run; 0 if none
    char done_title[121];   // that thread's title (40 characters), UTF-8
    int threads;        // active (not archived) threads
} t3_status_t;

// Parse the bridge's flat JSON object. Unknown keys are ignored; "online",
// "run", and "wait" are required. Counts must be 0..9999. Strings are cut at
// a UTF-8 character boundary to fit. false leaves *out unchanged.
bool t3_parse_status(const char *json, t3_status_t *out);

// A run finished since the previous poll: done_at moved forward from a known
// value. The first answer after start or a bridge change (prev 0) does not
// count, nor does a bridge restart that forgot older completions.
bool t3_done_changed(int64_t prev, int64_t now);

// GET /tasks from bridge/bridge.mjs: one line per running or waiting thread,
// waiting first, five tab-separated fields:
//   w|r <TAB> started (Unix seconds, 0 if unknown) <TAB> id (1-8 letters and
//   digits) <TAB> project <TAB> title
typedef struct {
    bool waiting;
    int64_t started;
    char id[T3_TASK_ID_LEN + 1];
    char project[73];     // UTF-8; longer values are cut at a character boundary
    char title[181];
} t3_task_t;

// Number of tasks parsed (at most max; extra lines are ignored), or -1 when
// any line is malformed. Empty text is an empty list.
int t3_parse_tasks(const char *text, t3_task_t *out, int max);

// What a pending request asks for (the bridge's "req" kind).
typedef enum {
    T3_REQ_OTHER = 0,
    T3_REQ_COMMAND,       // run a command
    T3_REQ_FILE_READ,
    T3_REQ_FILE_CHANGE,
    T3_REQ_PERMISSION,
    T3_REQ_QUESTION,      // user_input: the agent asks the user
    T3_REQ_MCP,           // mcp-elicitation
    T3_REQ_TOOL,          // dynamic_tool_call
    T3_REQ_AUTH,          // auth_refresh
} t3_req_kind_t;

// GET /task/<id>: lines "req<TAB>kind<TAB>text", "msg<TAB>text", and
// "bg<TAB>count<TAB>text", each optional; unknown lines are ignored.
typedef struct {
    bool has_request;
    t3_req_kind_t request_kind;
    char request[241];    // 80 characters
    char message[361];    // 120 characters: the latest assistant reply
    int background;       // background tasks still running
    char background_text[121];
} t3_detail_t;

// false for a malformed known line; *out is cleared first either way.
bool t3_parse_detail(const char *text, t3_detail_t *out);

// Whole minutes since started, or -1 when unknown (no start time, clock not
// set, or a start in the future beyond a minute of clock skew).
int64_t t3_elapsed_min(int64_t started, int64_t now);

// Task pages. page 0 is the home screen; page i (1..count) shows task i-1.
// The selected task is followed by id across refreshes.
typedef enum { T3_KEY_UP, T3_KEY_DOWN, T3_KEY_OK } t3_key_t;

typedef struct {
    int page;
    char id[T3_TASK_ID_LEN + 1];
} t3_nav_t;

// Home: DOWN opens the first task page (an empty page when there are none).
// Task page: DOWN next (stays on the last), UP previous (home from the
// first), OK home. Returns true when the page changed.
bool t3_nav_key(t3_nav_t *nav, t3_key_t key, const t3_task_t *tasks, int count);

// After a refresh: stay on the selected task, or on the same position (clamped)
// when it is gone. An empty list keeps one empty task page.
void t3_nav_sync(t3_nav_t *nav, const t3_task_t *tasks, int count);

typedef enum {
    T3_VIEW_CONNECTING = 0,   // no answer yet since start or since the address changed
    T3_VIEW_LIVE,             // bridge answered and T3 Code is subscribed
    T3_VIEW_T3_OFFLINE,       // bridge answered, but it cannot reach T3 Code
    T3_VIEW_UNREACHABLE,      // the bridge stopped answering
} t3_view_t;

// Poll results folded into what the screen should say. A single lost reply
// keeps the last good status; T3_FAILS_UNREACHABLE in a row do not.
typedef struct {
    t3_status_t last;
    bool have_last;
    unsigned fails;
} t3_health_t;

void t3_health_reset(t3_health_t *h);
void t3_health_ok(t3_health_t *h, const t3_status_t *s);
void t3_health_fail(t3_health_t *h);
t3_view_t t3_health_view(const t3_health_t *h);

// "192.168.1.10", "mac.local:9000", " 10.0.0.2:8787 " -> host and port.
// Host: letters, digits, '.', '-'; at most cap-1 bytes. Port 1..65535,
// T3_DEFAULT_PORT when omitted. false for an empty or malformed value.
bool t3_parse_address(const char *text, char *host, size_t cap, uint16_t *port);

typedef struct {
    char ssid[33];
    char password[65];
} t3_network_t;

typedef struct {
    t3_network_t net;
    uint8_t rank;     // 1-based position in the saved list; 0 for the driver setting
} t3_candidate_t;

// Saved networks first, in priority order, then the driver's setting when it
// names a network the list does not contain. Empty SSIDs are skipped.
size_t t3_candidates(const t3_network_t *list, size_t list_len, const t3_network_t *driver,
                     t3_candidate_t *out, size_t max);

// Text for "latest completed": "HH:MM" when it is the same local day as now,
// "MM-DD" otherwise, "--:--" when done_at is 0. tz_min is the local offset
// from UTC. When the clock is not set (now before 2023) only "HH:MM" is used.
// out needs 6 bytes.
void t3_format_done(int64_t done_at, int64_t now, int32_t tz_min, char out[6]);
