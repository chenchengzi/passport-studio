// Host tests for components/passport_link/link_core.c.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "link_core.h"

static uint8_t s_buf[LINK_MSG_MAX + 1];

static link_rx_result_t send_all(link_rx_t *rx, const uint8_t *msg, size_t len, size_t max_frame,
                                 size_t *frames)
{
    uint8_t frame[256];
    size_t offset = 0, n;
    link_rx_result_t r = LINK_RX_PARTIAL;
    *frames = 0;
    while ((n = link_tx_next(msg, len, &offset, max_frame, frame)) > 0) {
        assert(n <= max_frame);
        assert(((frame[0] & LINK_FLAG_FIRST) != 0) == (*frames == 0));
        r = link_rx_feed(rx, frame, n);
        (*frames)++;
    }
    return r;
}

static void test_frames(void)
{
    link_rx_t rx;
    link_rx_init(&rx, s_buf, sizeof(s_buf));
    size_t frames;

    // Split into 20-byte frames and reassemble.
    const char *msg = "{\"t\":\"push\",\"ch\":\"location\",\"d\":{\"lat\":39.98,\"lon\":116.31}}";
    size_t len = strlen(msg);
    assert(send_all(&rx, (const uint8_t *)msg, len, 20, &frames) == LINK_RX_COMPLETE);
    assert(frames == (len + 18) / 19 && rx.len == len && !strcmp((char *)s_buf, msg));

    // A single frame carries both flags; nothing follows it.
    uint8_t frame[64];
    size_t offset = 0;
    assert(link_tx_next((const uint8_t *)"{}", 2, &offset, 64, frame) == 3);
    assert(frame[0] == (LINK_FLAG_FIRST | LINK_FLAG_LAST));
    assert(link_tx_next((const uint8_t *)"{}", 2, &offset, 64, frame) == 0);
    assert(link_rx_feed(&rx, frame, 3) == LINK_RX_COMPLETE && !strcmp((char *)s_buf, "{}"));
    offset = 0;
    assert(link_tx_next((const uint8_t *)"{}", 2, &offset, 1, frame) == 0);   // no room

    // Continuation without a start, unknown flag bits, empty frame.
    const uint8_t cont[] = {0x00, 'x'}, last[] = {LINK_FLAG_LAST, 'x'}, bad[] = {0x80, 'x'};
    assert(link_rx_feed(&rx, cont, 2) == LINK_RX_DROPPED);
    assert(link_rx_feed(&rx, last, 2) == LINK_RX_DROPPED);
    assert(link_rx_feed(&rx, bad, 2) == LINK_RX_DROPPED);
    assert(link_rx_feed(&rx, bad, 0) == LINK_RX_DROPPED);
    const uint8_t empty[] = {LINK_FLAG_FIRST | LINK_FLAG_LAST};
    assert(link_rx_feed(&rx, empty, 1) == LINK_RX_DROPPED);

    // A new first frame restarts a half-received message.
    const uint8_t first[] = {LINK_FLAG_FIRST, 'a', 'b'}, single[] = {0x03, 'o', 'k'};
    assert(link_rx_feed(&rx, first, 3) == LINK_RX_PARTIAL);
    assert(link_rx_feed(&rx, single, 3) == LINK_RX_COMPLETE && !strcmp((char *)s_buf, "ok"));

    // Exactly the limit is accepted; one byte more is dropped, and the next
    // message is received normally.
    static uint8_t big[LINK_MSG_MAX + 1];
    memset(big, 'x', sizeof(big));
    assert(send_all(&rx, big, LINK_MSG_MAX, 200, &frames) == LINK_RX_COMPLETE && rx.len == LINK_MSG_MAX);
    assert(send_all(&rx, big, LINK_MSG_MAX + 1, 200, &frames) == LINK_RX_DROPPED);
    assert(send_all(&rx, (const uint8_t *)msg, len, 200, &frames) == LINK_RX_COMPLETE);
    puts("  frames: PASS");
}

static const passport_link_setting_t SETTINGS[] = {
    PASSPORT_LINK_ENUM("units", "Units", "C", "C", "F"),
    PASSPORT_LINK_INT("refresh", "Refresh (min)", 30, 5, 180),
    PASSPORT_LINK_BOOL_SETTING("wind", "Show wind", 1),
    PASSPORT_LINK_TEXT("city", "City name", "", 32),
};

static void test_settings(void)
{
    const size_t n = sizeof(SETTINGS) / sizeof(SETTINGS[0]);
    assert(link_schema_valid(SETTINGS, n));
    assert(SETTINGS[0].option_count == 2);
    assert(link_setting_find(SETTINGS, n, "refresh") == 1 && link_setting_find(SETTINGS, n, "x") < 0);

    assert(link_setting_str_ok(&SETTINGS[0], "F") && !link_setting_str_ok(&SETTINGS[0], "K"));
    assert(!link_setting_str_ok(&SETTINGS[0], NULL) && !link_setting_int_ok(&SETTINGS[0], 1));
    assert(link_setting_int_ok(&SETTINGS[1], 5) && link_setting_int_ok(&SETTINGS[1], 180));
    assert(!link_setting_int_ok(&SETTINGS[1], 4) && !link_setting_int_ok(&SETTINGS[1], 181));
    assert(link_setting_int_ok(&SETTINGS[2], 0) && !link_setting_int_ok(&SETTINGS[2], 2));
    assert(link_setting_str_ok(&SETTINGS[3], "") && !link_setting_int_ok(&SETTINGS[3], 0));
    char long_text[40];
    memset(long_text, 'a', 33);
    long_text[33] = '\0';
    assert(!link_setting_str_ok(&SETTINGS[3], long_text));
    long_text[32] = '\0';
    assert(link_setting_str_ok(&SETTINGS[3], long_text));

    // Broken schemas.
    const passport_link_setting_t dup[] = {
        PASSPORT_LINK_INT("a", "A", 1, 0, 2), PASSPORT_LINK_INT("a", "B", 1, 0, 2)};
    const passport_link_setting_t bad_def[] = {PASSPORT_LINK_INT("a", "A", 9, 0, 2)};
    const passport_link_setting_t bad_enum[] = {PASSPORT_LINK_ENUM("a", "A", "z", "x", "y")};
    const passport_link_setting_t long_key[] = {PASSPORT_LINK_INT("abcdefghijklmnop", "A", 1, 0, 2)};
    assert(!link_schema_valid(dup, 2) && !link_schema_valid(bad_def, 1));
    assert(!link_schema_valid(bad_enum, 1) && !link_schema_valid(long_key, 1));
    assert(link_schema_valid(NULL, 0));
    puts("  settings: PASS");
}

int main(void)
{
    test_frames();
    test_settings();
    puts("passport link tests: PASS");
    return 0;
}
