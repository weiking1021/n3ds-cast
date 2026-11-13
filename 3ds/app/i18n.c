#include "i18n.h"
#include <string.h>
#include <stdio.h>
const NativeTranslation native_translations[] = {
    {"R: capture  J: JPEG  T: send", "R 擷取  J 編碼  T 傳送"},
    {"START: exit", "START：關閉"},
    {"Save [X]", "儲存 [X]"},
    {"Start [Y]", "開始串流 [Y]"},
    {"Stop [Y]", "停止串流 [Y]"},
    {"Find TV [SELECT]", "搜尋電視 [SELECT]"},
    {"Select TV [A]", "選取電視 [A]"},
    {"%lu FPS / Q%d / target %d", "%lu FPS / 品質%d / 目標%d"},
    {"LIVE", "串流中"},
    {"READY FOR GAME", "請開啟遊戲"},
    {"CONNECTING", "連線中"},
    {"CONNECTION ERROR", "連線錯誤"},
    {"READY", "準備就緒"},
    {"CONTROL", "控制"},
    {"DEVICES", "電視"},
    {"SETTINGS", "設定"},
    {"Selected TV", "目前選取的電視"},
    {"Searching...", "搜尋中…"},
    {"No TV found. SELECT to scan again.", "找不到電視，按 SELECT 重新搜尋。"},
    {"TV IP", "電視 IP"},
    {"Screens", "畫面配置"},
    {"Filter", "濾鏡"},
    {"Aspect", "顯示比例"},
    {"Quality", "JPEG 品質"},
    {"Target FPS", "目標幀率"},
    {"TV OSD", "電視資訊"},
    {"Top", "上螢幕"},
    {"Bottom", "下螢幕"},
    {"Vertical", "上下排列"},
    {"Side by side", "左右排列"},
    {"PiP", "子母畫面"},
    {"Smooth", "平滑"},
    {"Pixelated", "銳利像素"},
    {"Stretch", "填滿畫面"},
    {"Keep aspect", "維持比例"},
    {"On", "開啟"},
    {"Off", "關閉"},
    {"Game audio", "遊戲音訊"},
    {"Audio rate", "音訊位元率"},
    {"Language", "介面語言"},
    {"Game mix", "遊戲混音"},
    {"Audio: off", "音訊：關閉"},
    {"Audio: waiting for game DSP", "音訊：等待遊戲 DSP"},
    {"Audio: DSP layout not identified", "音訊：無法辨識遊戲 DSP 佈局"},
    {"Audio: streaming game mix", "音訊：正在串流遊戲混音"},
    {"Find your TV, then start streaming", "請選取電視，再開始串流"},
    {"Ready; no game hooks or plugins", "準備就緒，請開始串流"},
    {"Preparing direct Google TV stream", "正在準備電視串流"},
    {"Connecting directly to Google TV", "正在連線至 Google TV"},
    {"Ready. Open a 3DS game from HOME Menu", "準備完成，請從 HOME 選單開啟遊戲"},
    {"Waiting for an active 3DS game", "正在等待遊戲啟動"},
    {"Waiting for a readable game framebuffer", "正在等待可擷取的遊戲畫面"},
    {"Streaming game video directly to TV", "正在將遊戲畫面投放電視"},
    {"Streaming stopped", "串流已停止"},
    {"Stopping stream...", "正在停止串流…"},
    {"Frame exceeds capacity; lower JPEG quality", "畫面資料過大，請降低 JPEG 品質"},
    {"Frame send failed", "畫面傳送失敗"},
    {"Audio send failed", "音訊傳送失敗"},
    {"Not enough background memory", "背景程序記憶體不足"},
    {"Network unavailable; check Wi-Fi", "無法使用網路，請確認 Wi-Fi"},
    {"Searching for Google Cast TVs...", "正在搜尋 Google Cast 電視…"},
    {"Could not start network scan", "無法啟動網路搜尋"},
    {"TV selected. Y starts streaming; X saves.", "已選取電視，Y 開始串流，X 儲存。"},
    {"Stop streaming before editing settings.", "請先停止串流，再修改設定。"},
    {"Google TV IPv4 address", "Google TV 的 IPv4 位址"},
    {"Settings saved", "設定已儲存"},
    {"Could not save settings", "無法儲存設定"},
    {"Check IPv4 address and 8-digit hex App ID.", "請確認 IPv4 位址與八碼 App ID。"},
    {"Starting own background stream service...", "正在啟動背景串流…"},
    {"Start request failed", "無法開始串流"},
    {"Wait for READY, then HOME > open your game.", "待準備完成後，從 HOME 選單啟動遊戲。"},
    {"Started; could not save settings.", "串流已開始，但無法儲存設定。"},
    {"Found %u TV(s). Select in Devices.", "找到 %u 台電視，請至電視頁選取。"},
    {"No TV: %u replies. SELECT retries.", "找不到電視，收到 %u 個回覆，SELECT 重試。"},
    {"No mDNS replies. Check same LAN / isolation.", "未收到探索回覆，請確認同區網與 AP 隔離。"},
    {"Network scan failed (%d). SELECT retries.", "網路搜尋失敗（%d），SELECT 重試。"},
    {"Stop request failed: %08lX", "停止串流失敗：%08lX"},
    {"Background service ended: %08lX", "背景程序已結束：%08lX"},
    {"CPU core", "CPU 核心"},
    {"Capture", "擷取模式"},
    {"Auto", "自動偵測"},
    {"Core 1", "核心 1"},
    {"Core 2", "核心 2"},
    {"Always", "持續擷取"},
    {"Games only", "僅限遊戲"},
    {"Encoder: core %lu", "編碼核心：%lu"},
    {"Encoder: core 1 (core 2 unavailable)", "編碼核心：1（核心 2 無法使用）"},
    {"Ready. Streaming the current screen", "準備完成，正在擷取目前畫面"},
    {"Waiting for a readable screen", "正在等待可擷取的畫面"},
    {"Streaming screen directly to TV", "正在將畫面投放電視"},
    {"Could not start encoder thread", "無法啟動編碼執行緒"},
};
const unsigned native_translation_count = sizeof(native_translations) / sizeof(*native_translations);
const char *native_tr(unsigned language, const char *english) {
    if (!english) {
        return "";
    }
    if (language == NATIVE_LANG_ZH_TW) {
        for (unsigned i = 0; i < native_translation_count; i++) {
            if (!strcmp(english, native_translations[i].en)) {
                return native_translations[i].zh;
            }
        }
    }
    return english;
}
void native_text_split(const char *value, char *first, size_t first_size, char *second, size_t second_size) {
    size_t at = 0;
    unsigned columns = 0;
    while (value[at]) {
        unsigned char c = value[at];
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (columns + (n == 1 ? 1 : 2) > 44 || at + n >= first_size) {
            break;
        }
        for (size_t i = 1; i < n; i++) {
            if (!value[at + i] || (value[at + i] & 0xC0) != 0x80) {
                n = 1;
                break;
            }
        }
        at += n;
        columns += n == 1 ? 1 : 2;
    }
    if (first_size) {
        memcpy(first, value, at);
        first[at] = 0;
    }
    if (second_size) {
        snprintf(second, second_size, "%s", value + at);
    }
}
