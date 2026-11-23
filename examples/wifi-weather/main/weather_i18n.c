// examples/wifi-weather/main/weather_i18n.c
#include "weather_i18n.h"

#include <string.h>

#include "weather_logic.h"

static const char *const EN[WX_S_COUNT] = {
    [WX_S_HEADER] = "WEATHER",
    [WX_S_STARTING] = "Starting Wi-Fi",
    [WX_S_WIFI_FAILED] = "Wi-Fi failed",
    [WX_S_RESTART] = "Restart the device",
    [WX_S_NO_WIFI] = "No Wi-Fi saved",
    [WX_S_NO_WIFI_HINT] = "Add a network in the iPhone app, then sync it in the installer.",
    [WX_S_HOLD_UP] = "Hold UP at power-on",
    [WX_S_CONNECTING] = "Connecting %u/%u",
    [WX_S_FROM_LIST] = "  (app list #%u)",
    [WX_S_FROM_NVS] = "  (installer NVS)",
    [WX_S_CANT_CONNECT] = "Can't connect",
    [WX_S_CANT_CONNECT_HINT] = "Check the name and password in the iPhone app. Retrying in 1 min.",
    [WX_S_TRIED] = "Tried %u network(s)",
    [WX_S_UPDATING] = "Updating",
    [WX_S_WEATHER_FAILED] = "Weather unavailable",
    [WX_S_PLACE_FAILED] = "Location unavailable",
    [WX_S_REQUEST_FAILED] = "Connected, but the request failed. Press OK to retry.",
    [WX_S_DETAIL] = "H %.0f\xC2\xB0   L %.0f\xC2\xB0\nHumidity %d%%   Wind %.0f km/h",
    [WX_S_PAIR_TITLE] = "Pair with iPhone",
    [WX_S_PAIR_HINT] = "Enter this code on the phone",
};

static const char *const ZH[WX_S_COUNT] = {
    [WX_S_HEADER] = "天气",
    [WX_S_STARTING] = "正在启动 Wi-Fi",
    [WX_S_WIFI_FAILED] = "Wi-Fi 启动失败",
    [WX_S_RESTART] = "请重启设备",
    [WX_S_NO_WIFI] = "没有保存的 Wi-Fi",
    [WX_S_NO_WIFI_HINT] = "请在 iPhone App 中添加网络，再在安装器里同步。",
    [WX_S_HOLD_UP] = "开机时按住上键",
    [WX_S_CONNECTING] = "正在连接 %u/%u",
    [WX_S_FROM_LIST] = "（列表第 %u 个）",
    [WX_S_FROM_NVS] = "（安装器写入）",
    [WX_S_CANT_CONNECT] = "无法连接",
    [WX_S_CANT_CONNECT_HINT] = "请在 iPhone App 中检查网络名称和密码，1 分钟后重试。",
    [WX_S_TRIED] = "已尝试 %u 个网络",
    [WX_S_UPDATING] = "正在更新",
    [WX_S_WEATHER_FAILED] = "天气不可用",
    [WX_S_PLACE_FAILED] = "位置不可用",
    [WX_S_REQUEST_FAILED] = "已联网，但请求失败。按 OK 重试。",
    [WX_S_DETAIL] = "最高 %.0f\xC2\xB0   最低 %.0f\xC2\xB0\n湿度 %d%%   风速 %.0f km/h",
    [WX_S_PAIR_TITLE] = "与 iPhone 配对",
    [WX_S_PAIR_HINT] = "在手机上输入这个配对码",
};

const char *wx_str(wx_lang_t lang, wx_str_t id)
{
    if ((unsigned)id >= WX_S_COUNT) return "";
    return lang == WX_LANG_ZH ? ZH[id] : EN[id];
}

const char *wx_text_lang(wx_lang_t lang, int wmo)
{
    if (lang != WX_LANG_ZH) return wx_text(wmo);
    switch (wmo) {
    case 0: return "晴";
    case 1: return "大部晴朗";
    case 2: return "局部多云";
    case 3: return "阴";
    case 45: case 48: return "雾";
    case 51: case 53: case 55: return "毛毛雨";
    case 56: case 57: return "冻毛毛雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: case 67: return "冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: case 81: case 82: return "阵雨";
    case 85: case 86: return "阵雪";
    case 95: return "雷暴";
    case 96: case 99: return "雷暴伴冰雹";
    default: return "未知";
    }
}

void wx_utf8_copy(char *out, size_t cap, const char *in)
{
    if (cap == 0) return;
    size_t n = strlen(in);
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)in[n] & 0xC0) == 0x80) n--;   // back to a lead byte
    }
    memcpy(out, in, n);
    out[n] = '\0';
}
