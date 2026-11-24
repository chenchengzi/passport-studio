// examples/t3-status/main/t3_text.c
// Run fonts/generate_fonts.py after changing any Chinese character here.
#include "t3_text.h"

const char T3_TXT_LIVE[] = "在线";
const char T3_TXT_IDLE[] = "在线 · 空闲";
const char T3_TXT_T3_OFFLINE[] = "T3 Code 未连接";
const char T3_TXT_UNREACHABLE[] = "状态不可达";
const char T3_TXT_CONNECTING[] = "正在连接桥接服务";
const char T3_TXT_RUN[] = "正在执行";
const char T3_TXT_WAIT[] = "等待批准";
const char T3_TXT_DONE[] = "最近完成";
const char T3_TXT_STARTING[] = "正在启动";
const char T3_TXT_WIFI_FAILED[] = "Wi-Fi 启动失败";
const char T3_TXT_NO_WIFI[] = "没有 Wi-Fi";
const char T3_TXT_NO_WIFI_HINT[] = "请在手机上添加网络";
const char T3_TXT_JOINING[] = "正在连接 %s";
const char T3_TXT_CANT_JOIN[] = "无法连接 Wi-Fi";
const char T3_TXT_CANT_JOIN_HINT[] = "一分钟后重试";
const char T3_TXT_NO_ADDRESS[] = "未设置桥接地址";
const char T3_TXT_NO_ADDRESS_HINT[] = "在手机的设备设置中填写";
const char T3_TXT_BAD_ADDRESS[] = "桥接地址格式错误";
const char T3_TXT_UNREACHABLE_HINT[] = "%s 无响应";
const char T3_TXT_OFFLINE_HINT[] = "请确认 Mac 上 T3 Code 已打开";
const char T3_TXT_TASK_PAGE[] = "任务 %d/%d";
const char T3_TXT_TASKS[] = "进行中的任务";
const char T3_TXT_TASKS_NONE[] = "现在没有正在执行或等待批准的任务";
const char T3_TXT_TASKS_LOADING[] = "正在读取任务";
const char T3_TXT_TASKS_FAILED[] = "读取任务失败，请确认桥接脚本已更新";
const char T3_TXT_TASK_HINT[] = "上下键切换 · OK 返回";
const char T3_TXT_ELAPSED_NEW[] = "刚开始";
const char T3_TXT_ELAPSED_MIN[] = "%d 分钟";
const char T3_TXT_ELAPSED_HOUR[] = "%d 小时 %d 分";
const char T3_TXT_REQ_COMMAND[] = "运行命令";
const char T3_TXT_REQ_FILE_READ[] = "读取文件";
const char T3_TXT_REQ_FILE_CHANGE[] = "修改文件";
const char T3_TXT_REQ_PERMISSION[] = "申请权限";
const char T3_TXT_REQ_QUESTION[] = "等你回答";
const char T3_TXT_REQ_MCP[] = "MCP 请求";
const char T3_TXT_REQ_TOOL[] = "调用工具";
const char T3_TXT_REQ_AUTH[] = "需要重新登录";
const char T3_TXT_DETAIL_LOADING[] = "正在读取详情";
const char T3_TXT_NO_REPLY[] = "还没有回复";
const char T3_TXT_BACKGROUND[] = "后台任务 %d 个";
const char T3_TXT_JUST_DONE[] = "已完成：%s";
const char T3_TXT_PAIR_TITLE[] = "蓝牙配对";
const char T3_TXT_PAIR_HINT[] = "在手机上输入此代码";
