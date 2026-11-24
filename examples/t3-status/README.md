<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# T3 Status

A desk display for [T3 Code](https://t3.codes) on the FoloToy AI Passport. It
shows how many threads are running and how many wait for approval, when the
last run finished, and whether the Mac is reachable. Press DOWN to page
through the running and waiting threads: what each one asks for approval, its
latest reply, and its background work. A finished run is highlighted on the
home screen.

```text
Mac: T3 Code (:3773) ──ws──> bridge.mjs (:8787) <──HTTP /status── Passport
```

The bridge runs on the Mac next to T3 Code, holds a read-only T3 token, and
serves counts and details of active threads on the LAN. The token never
reaches the device. Replies and request contents are only in a thread's own
projection, so the bridge also subscribes to each running or waiting thread
(`orchestration.subscribeThread`) and drops that subscription when the thread
goes idle.

## Bridge on the Mac

Requires Node.js 22 or later (built-in `WebSocket`).

1. Issue a read-only session token and keep it private:

   ```bash
   mkdir -p ~/.config/passport-t3
   ~/.t3/bin/t3 auth session issue --scope orchestration:read \
     --label passport-status --ttl 30d --token-only > ~/.config/passport-t3/token
   chmod 600 ~/.config/passport-t3/token
   ```

   Issue a new one when it expires. Never commit it or copy it to the device.

2. Start the bridge and check it:

   ```bash
   node examples/t3-status/bridge/bridge.mjs
   curl http://127.0.0.1:8787/status
   # {"online":true,"run":2,"wait":0,"done_at":1795425160,"threads":93}
   ```

   | Variable | Default |
   | --- | --- |
   | `T3_URL` | `http://127.0.0.1:3773` |
   | `T3_TOKEN_FILE` | `~/.config/passport-t3/token` |
   | `BRIDGE_PORT` | `8787` |
   | `BRIDGE_HOST` | `0.0.0.0` |
   | `BRIDGE_TASKS` | `1`; `0` turns `/tasks` and `/task/<id>` off |

3. Find the Mac's LAN address with `ipconfig getifaddr en0`. If the macOS
   firewall is on, allow incoming connections for `node` when prompted
   (System Settings → Network → Firewall).

Every endpoint is plain HTTP and readable by anyone on the same network.
`/status` carries counts, a timestamp, and the title of the last finished
thread; `/tasks` and `/task/<id>` also carry thread and project titles,
replies, and the commands or questions waiting for approval. Set
`BRIDGE_TASKS=0` to keep those off the network, or `BRIDGE_HOST=127.0.0.1` to
keep everything local.

| Field | Meaning |
| --- | --- |
| `online` | the bridge is subscribed to T3 Code |
| `run` | threads with `status` or `activityRunStatus` `running` |
| `wait` | threads with a `pendingRuntimeRequest` |
| `done_at` | latest completed run, Unix seconds (0: none); a thread keeps its last completion while a new run is in progress |
| `done_title` | that thread's title, up to 40 characters |

`GET /tasks` returns one line per running or waiting thread, waiting first and
then the most recently started, at most 8, with five tab-separated fields:
`w` or `r`, run start (Unix seconds, 0 if unknown), a short id (the last 8
letters and digits of the thread id), project (up to 24 characters), and title
(up to 60).

`GET /task/<id>` describes one listed thread, one optional line each,
tab-separated; 404 when the thread is not listed:

| Line | Fields |
| --- | --- |
| `req` | request kind (`command`, `file-change`, `user_input`, ...), what it asks (up to 80 characters; empty when unknown) |
| `msg` | latest assistant reply, up to 120 characters |
| `bg` | number of background tasks, their descriptions (up to 40 characters) |

## Device

1. Add the Wi-Fi network in the iPhone app, then install
   `build/T3-Status-merged.bin` through the app.
2. On the app's Device Link page, set **bridge** to the Mac's address, such as
   `192.168.1.10` or `192.168.1.10:8787`.

| Setting | Default | |
| --- | --- | --- |
| `bridge` | empty | Mac address, optional `:port` (8787) |
| `screen` | 10 | minutes before the screen turns off; 0 keeps it on |
| `wake` | on | turn the screen on for a new approval request or a finished run |

The time channel provides the time zone for **Last finished** (`HH:MM` today,
`MM-DD` otherwise). The display polls every 2 seconds. After three failed polls
in a row the status line shows the bridge as unreachable.

| Key | Home | Task page |
| --- | --- | --- |
| DOWN | open the task pages | next task |
| UP | | previous task; home from the first |
| OK | poll now | home |

A task page shows the request kind (or "running"), how long the current run
has been going, the project, the title (up to two lines), and then what a
waiting thread asks for, in amber, or a running thread's latest reply. A line
for background tasks appears at the bottom when there are any. Task pages
return home after a minute without a key press.

When a run finishes, the home screen outlines **Last finished** in blue and
the footer shows the thread's title, for ten minutes or until a key press.
A key press that turns the screen on does nothing else. Both Chinese fonts
cover the 3755 GB2312 level-1 characters; rarer characters and emoji show as
placeholder boxes.

## Build

```bash
cd examples/t3-status
idf.py build
```

`./tools/validate.sh --firmware` builds it with the other applications and
writes the merged image. Chinese text uses Noto Sans SC subsets; after changing
any string in `main/t3_text.c`, run `fonts/generate_fonts.py`.
