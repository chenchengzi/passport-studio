// examples/t3-status/main/t3_text.h
// Every fixed screen string. fonts/generate_fonts.py subsets the Chinese
// fonts from the non-ASCII characters of t3_text.c, so keep all Chinese UI
// text there.
#pragma once

#define T3_TXT_HEADER          "T3 CODE"
extern const char T3_TXT_LIVE[];          // status line: subscribed
extern const char T3_TXT_IDLE[];          // live, nothing running or waiting
extern const char T3_TXT_T3_OFFLINE[];    // bridge up, T3 Code not reachable
extern const char T3_TXT_UNREACHABLE[];   // bridge not answering
extern const char T3_TXT_CONNECTING[];    // first poll pending
extern const char T3_TXT_RUN[];           // card labels
extern const char T3_TXT_WAIT[];
extern const char T3_TXT_DONE[];
extern const char T3_TXT_STARTING[];
extern const char T3_TXT_WIFI_FAILED[];
extern const char T3_TXT_NO_WIFI[];
extern const char T3_TXT_NO_WIFI_HINT[];
extern const char T3_TXT_JOINING[];       // "%s": SSID
extern const char T3_TXT_CANT_JOIN[];
extern const char T3_TXT_CANT_JOIN_HINT[];
extern const char T3_TXT_NO_ADDRESS[];
extern const char T3_TXT_NO_ADDRESS_HINT[];
extern const char T3_TXT_BAD_ADDRESS[];
extern const char T3_TXT_UNREACHABLE_HINT[];   // "%s": host:port
extern const char T3_TXT_OFFLINE_HINT[];
extern const char T3_TXT_TASK_PAGE[];      // "%d/%d": page, count
extern const char T3_TXT_TASKS[];          // status line of the empty task page
extern const char T3_TXT_TASKS_NONE[];
extern const char T3_TXT_TASKS_LOADING[];
extern const char T3_TXT_TASKS_FAILED[];   // /tasks failed or is turned off
extern const char T3_TXT_TASK_HINT[];
extern const char T3_TXT_ELAPSED_NEW[];
extern const char T3_TXT_ELAPSED_MIN[];    // "%d": minutes
extern const char T3_TXT_ELAPSED_HOUR[];   // "%d %d": hours, minutes
extern const char T3_TXT_REQ_COMMAND[];    // task page: what a request asks for
extern const char T3_TXT_REQ_FILE_READ[];
extern const char T3_TXT_REQ_FILE_CHANGE[];
extern const char T3_TXT_REQ_PERMISSION[];
extern const char T3_TXT_REQ_QUESTION[];
extern const char T3_TXT_REQ_MCP[];
extern const char T3_TXT_REQ_TOOL[];
extern const char T3_TXT_REQ_AUTH[];
extern const char T3_TXT_DETAIL_LOADING[];
extern const char T3_TXT_NO_REPLY[];
extern const char T3_TXT_BACKGROUND[];     // "%d": background tasks
extern const char T3_TXT_JUST_DONE[];      // footer, "%s": the finished task's title
extern const char T3_TXT_PAIR_TITLE[];
extern const char T3_TXT_PAIR_HINT[];
