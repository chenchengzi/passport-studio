<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# T3 Status

在 FoloToy AI Passport 上显示 [T3 Code](https://t3.codes) 状态的桌面小屏：正在执行和
等待批准的任务数、最近一次完成的时间，以及 Mac 是否可达。按下键可以逐个查看正在执行和
等待批准的任务：等待批准的具体内容、最新一条回复和后台任务。任务完成时主页会高亮提示。

```text
Mac: T3 Code (:3773) ──ws──> bridge.mjs (:8787) <──HTTP /status── Passport
```

桥接脚本和 T3 Code 一起运行在 Mac 上，持有只读的 T3 令牌，在局域网内提供计数和
进行中任务的详情。令牌不会发到设备上。回复和请求内容只在单个线程的详细数据里，因此
桥接脚本会额外订阅每个正在执行或等待批准的线程（`orchestration.subscribeThread`），
线程空闲后取消订阅。

## Mac 上的桥接脚本

需要 Node.js 22 或更高版本（使用内置 `WebSocket`）。

1. 签发只读会话令牌并妥善保存：

   ```bash
   mkdir -p ~/.config/passport-t3
   ~/.t3/bin/t3 auth session issue --scope orchestration:read \
     --label passport-status --ttl 30d --token-only > ~/.config/passport-t3/token
   chmod 600 ~/.config/passport-t3/token
   ```

   过期后重新签发。不要提交到仓库，也不要复制到设备。

2. 启动并检查：

   ```bash
   node examples/t3-status/bridge/bridge.mjs
   curl http://127.0.0.1:8787/status
   # {"online":true,"run":2,"wait":0,"done_at":1795425160,"threads":93}
   ```

   | 环境变量 | 默认值 |
   | --- | --- |
   | `T3_URL` | `http://127.0.0.1:3773` |
   | `T3_TOKEN_FILE` | `~/.config/passport-t3/token` |
   | `BRIDGE_PORT` | `8787` |
   | `BRIDGE_HOST` | `0.0.0.0` |
   | `BRIDGE_TASKS` | `1`；设为 `0` 关闭 `/tasks` 和 `/task/<id>` |

3. 用 `ipconfig getifaddr en0` 查看 Mac 的局域网地址。如果开启了 macOS 防火墙，
   在弹窗中允许 `node` 接受传入连接（系统设置 → 网络 → 防火墙）。

所有接口都是明文 HTTP，同一网络内的任何人都能读取。`/status` 包含计数、时间戳和最近
完成的线程标题；`/tasks` 和 `/task/<id>` 还包含线程标题、项目名、回复，以及等待批准的
命令或问题。设置 `BRIDGE_TASKS=0` 可不在网络上提供这些内容，设置
`BRIDGE_HOST=127.0.0.1` 可只允许本机访问。

| 字段 | 含义 |
| --- | --- |
| `online` | 桥接脚本已订阅 T3 Code |
| `run` | `status` 或 `activityRunStatus` 为 `running` 的线程数 |
| `wait` | 有 `pendingRuntimeRequest` 的线程数 |
| `done_at` | 最近一次完成的时间，Unix 秒（0 表示没有）；线程开始新一轮运行时，保留它上一次的完成时间 |
| `done_title` | 这个线程的标题，最多 40 个字符 |

`GET /tasks` 每行一个正在执行或等待批准的线程，等待批准的在前，其余按开始时间从新到旧，
最多 8 行。每行 5 个字段，用制表符分隔：`w` 或 `r`、本轮开始时间（Unix 秒，未知为 0）、
短 id（线程 id 中最后 8 个字母和数字）、项目名（最多 24 个字符）、标题（最多 60 个字符）。

`GET /task/<id>` 描述列表中的一个线程，每种信息一行（可省略），字段用制表符分隔；
线程不在列表中时返回 404：

| 行 | 字段 |
| --- | --- |
| `req` | 请求类型（`command`、`file-change`、`user_input` 等）、请求内容（最多 80 个字符，未知时为空） |
| `msg` | 最新一条助手回复，最多 120 个字符 |
| `bg` | 后台任务数、它们的描述（最多 40 个字符） |

## 设备

1. 在 iPhone App 中添加 Wi-Fi 网络，然后通过 App 安装 `build/T3-Status-merged.bin`。
2. 在 App 的设备连接页面，把 **桥接地址** 设为 Mac 的地址，例如 `192.168.1.10` 或
   `192.168.1.10:8787`。

| 设置项 | 默认值 | |
| --- | --- | --- |
| `bridge` | 空 | Mac 地址，可加 `:端口`（默认 8787） |
| `screen` | 10 | 自动关屏的分钟数，0 为常亮 |
| `wake` | 开 | 有新的待批准或任务完成时亮屏 |

时间频道提供时区，用于显示 **最近完成**（当天显示 `HH:MM`，否则显示 `MM-DD`）。
设备每 2 秒轮询一次。连续三次轮询失败后，状态行显示桥接服务不可达。

| 按键 | 主页 | 任务页 |
| --- | --- | --- |
| 下 | 进入任务页 | 下一个任务 |
| 上 | | 上一个任务；在第一个任务时返回主页 |
| OK | 立即刷新 | 返回主页 |

任务页依次显示请求类型（或「正在执行」）、本轮已运行的时间、项目、标题（最多两行），
然后是等待批准的具体内容（琥珀色）或正在执行任务的最新回复。有后台任务时，底部多一行
后台任务信息。一分钟没有按键会自动回到主页。

任务完成时，主页的「最近完成」卡片显示蓝色边框，页脚显示该任务的标题，持续十分钟或
直到按下任意键。屏幕关闭时按下的键只负责亮屏。两种中文字体都包含 GB2312 一级汉字
（3755 个），更少见的字和 emoji 显示为占位方框。

## 构建

```bash
cd examples/t3-status
idf.py build
```

`./tools/validate.sh --firmware` 会和其他应用一起构建它并生成合并镜像。中文使用
Noto Sans SC 子集；修改 `main/t3_text.c` 中的任何文字后，运行 `fonts/generate_fonts.py`。
