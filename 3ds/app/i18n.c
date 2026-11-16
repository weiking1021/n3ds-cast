#include "i18n.h"
#include <string.h>
#include <stdio.h>
const NativeTranslation native_translations[] = {
    {"R: capture  J: JPEG  T: send", "R 擷取  J 編碼  T 傳送", "R キャプチャ  J エンコード  T 送信"},
    {"START: exit", "START：關閉", "START：終了"},
    {"Save [X]", "儲存 [X]", "保存 [X]"},
    {"Start [Y]", "開始串流 [Y]", "配信開始 [Y]"},
    {"Stop [Y]", "停止串流 [Y]", "配信停止 [Y]"},
    {"Find TV [SELECT]", "搜尋電視 [SELECT]", "テレビを探す [SELECT]"},
    {"Select TV [A]", "選取電視 [A]", "テレビを選ぶ [A]"},
    {"%lu FPS / Q%d / target %d", "%lu FPS / 品質%d / 目標%d", "%lu FPS / 画質%d / 目標%d"},
    {"LIVE", "串流中", "配信中"},
    {"READY FOR GAME", "請開啟遊戲", "ゲームを起動してください"},
    {"CONNECTING", "連線中", "つなぎ中"},
    {"CONNECTION ERROR", "連線錯誤", "通信エラー"},
    {"READY", "準備就緒", "準備完了"},
    {"CONTROL", "控制", "操作"},
    {"DEVICES", "電視", "テレビ"},
    {"SETTINGS", "設定", "設定"},
    {"Selected TV", "目前選取的電視", "選んだテレビ"},
    {"Searching...", "搜尋中…", "探しています…"},
    {"No TV found. SELECT to scan again.", "找不到電視，按 SELECT 重新搜尋。",
     "テレビが見つかりません。SELECTでもう一度探します。"},
    {"TV IP", "電視 IP", "テレビIP"},
    {"Screens", "畫面配置", "画面配置"},
    {"Filter", "濾鏡", "フィルター"},
    {"Aspect", "顯示比例", "表示比率"},
    {"Quality", "JPEG 品質", "JPEG画質"},
    {"Target FPS", "目標幀率", "目標FPS"},
    {"TV OSD", "電視資訊", "テレビ情報"},
    {"Top", "上螢幕", "上画面"},
    {"Bottom", "下螢幕", "下画面"},
    {"Vertical", "上下排列", "上下に並べる"},
    {"Side by side", "左右排列", "左右に並べる"},
    {"PiP", "子母畫面", "子画面"},
    {"Smooth", "平滑", "なめらか"},
    {"Pixelated", "銳利像素", "ドット"},
    {"Stretch", "填滿畫面", "画面いっぱい"},
    {"Keep aspect", "維持比例", "比率を維持"},
    {"On", "開啟", "オン"},
    {"Off", "關閉", "オフ"},
    {"Game audio", "遊戲音訊", "ゲーム音声"},
    {"Audio rate", "音訊位元率", "音声レート"},
    {"Language", "介面語言", "表示言語"},
    {"Game mix", "遊戲混音", "ゲームミックス"},
    {"Audio: off", "音訊：關閉", "音声：オフ"},
    {"Audio: waiting for game DSP", "音訊：等待遊戲 DSP", "音声：ゲームのDSP待ち"},
    {"Audio: DSP layout not identified", "音訊：無法辨識遊戲 DSP 佈局", "音声：DSPの配置を特定できません"},
    {"Audio: streaming game mix", "音訊：正在串流遊戲混音", "音声：ゲームミックスを配信中"},
    {"Find your TV, then start streaming", "請選取電視，再開始串流", "テレビを選んでから配信を開始してください"},
    {"Ready; no game hooks or plugins", "準備就緒，請開始串流", "準備完了。配信を開始してください"},
    {"Preparing direct Google TV stream", "正在準備電視串流", "テレビへの配信を準備しています"},
    {"Connecting directly to Google TV", "正在連線至 Google TV", "Google TVにつないでいます"},
    {"Ready. Open a 3DS game from HOME Menu", "準備完成，請從 HOME 選單開啟遊戲",
     "準備完了。HOMEメニューからゲームを起動してください"},
    {"Waiting for an active 3DS game", "正在等待遊戲啟動", "ゲームの起動を待っています"},
    {"Waiting for a readable game framebuffer", "正在等待可擷取的遊戲畫面", "取得できるゲーム画面を待っています"},
    {"Streaming game video directly to TV", "正在將遊戲畫面投放電視", "ゲーム画面をテレビへ配信しています"},
    {"Streaming stopped", "串流已停止", "配信を停止しました"},
    {"Stopping stream...", "正在停止串流…", "配信を停止しています…"},
    {"Frame exceeds capacity; lower JPEG quality", "畫面資料過大，請降低 JPEG 品質",
     "画面データが大きすぎます。JPEG画質を下げてください"},
    {"Frame send failed", "畫面傳送失敗", "画面の送信に失敗しました"},
    {"Audio send failed", "音訊傳送失敗", "音声の送信に失敗しました"},
    {"Not enough background memory", "背景程序記憶體不足", "バックグラウンドのメモリが足りません"},
    {"Network unavailable; check Wi-Fi", "無法使用網路，請確認 Wi-Fi",
     "ネットワークが使えません。Wi-Fiを確認してください"},
    {"Searching for Google Cast TVs...", "正在搜尋 Google Cast 電視…", "Google Castのテレビを探しています…"},
    {"Could not start network scan", "無法啟動網路搜尋", "ネットワーク探索を開始できません"},
    {"TV selected. Y starts streaming; X saves.", "已選取電視，Y 開始串流，X 儲存。",
     "テレビを選びました。Yで配信開始、Xで保存。"},
    {"Stop streaming before editing settings.", "請先停止串流，再修改設定。",
     "設定を編集する前に配信を停止してください。"},
    {"Google TV IPv4 address", "Google TV 的 IPv4 位址", "Google TVのIPv4アドレス"},
    {"Settings saved", "設定已儲存", "設定を保存しました"},
    {"Could not save settings", "無法儲存設定", "設定を保存できません"},
    {"Check IPv4 address and 8-digit hex App ID.", "請確認 IPv4 位址與八碼 App ID。",
     "IPv4アドレスと8桁のアプリIDを確認してください。"},
    {"Starting own background stream service...", "正在啟動背景串流…", "バックグラウンド配信を起動しています…"},
    {"Start request failed", "無法開始串流", "配信を開始できません"},
    {"Wait for READY, then HOME > open your game.", "待準備完成後，從 HOME 選單啟動遊戲。",
     "準備完了後、HOMEメニューからゲームを起動してください。"},
    {"Started; could not save settings.", "串流已開始，但無法儲存設定。",
     "配信を開始しましたが、設定を保存できません。"},
    {"Found %u TV(s). Select in Devices.", "找到 %u 台電視，請至電視頁選取。",
     "%u台のテレビが見つかりました。テレビ画面で選んでください。"},
    {"No TV: %u replies. SELECT retries.", "找不到電視，收到 %u 個回覆，SELECT 重試。",
     "テレビなし：返信%u件。SELECTで再試行。"},
    {"No mDNS replies. Check same LAN / isolation.", "未收到探索回覆，請確認同區網與 AP 隔離。",
     "mDNSの返信がありません。同じLANか、AP分離を確認してください。"},
    {"Network scan failed (%d). SELECT retries.", "網路搜尋失敗（%d），SELECT 重試。",
     "ネットワーク探索に失敗しました（%d）。SELECTで再試行。"},
    {"Stop request failed: %08lX", "停止串流失敗：%08lX", "停止に失敗しました：%08lX"},
    {"Background service ended: %08lX", "背景程序已結束：%08lX", "バックグラウンドが終了しました：%08lX"},
    {"CPU core", "CPU 核心", "CPUコア"},
    {"Capture", "擷取模式", "画面取得"},
    {"Network", "網路", "回線"},
    {"Auto", "自動偵測", "自動"},
    {"Core 1", "核心 1", "コア1"},
    {"Core 2", "核心 2", "コア2"},
    {"Always", "持續擷取", "常時"},
    {"Games only", "僅限遊戲", "ゲームのみ"},
    {"Encoder: core %lu", "編碼核心：%lu", "エンコード：コア%lu"},
    {"Encoder: core 1 (core 2 unavailable)", "編碼核心：1（核心 2 無法使用）", "エンコード：コア1（コア2は使用不可）"},
    {"Ready. Streaming the current screen", "準備完成，正在擷取目前畫面", "準備完了。現在の画面を配信しています"},
    {"Waiting for a readable screen", "正在等待可擷取的畫面", "取得できる画面を待っています"},
    {"Streaming screen directly to TV", "正在將畫面投放電視", "画面をテレビへ配信しています"},
    {"Could not start encoder thread", "無法啟動編碼執行緒", "エンコードスレッドを開始できません"},
};
const unsigned native_translation_count = sizeof(native_translations) / sizeof(*native_translations);
const char *native_tr(unsigned language, const char *english) {
    if (!english) {
        return "";
    }
    if (language == NATIVE_LANG_ZH_TW || language == NATIVE_LANG_JA) {
        for (unsigned i = 0; i < native_translation_count; i++) {
            if (!strcmp(english, native_translations[i].en)) {
                return language == NATIVE_LANG_JA ? native_translations[i].ja : native_translations[i].zh;
            }
        }
    }
    return english;
}
const char *native_language_name(unsigned language) {
    return language == NATIVE_LANG_ZH_TW ? "繁體中文" : language == NATIVE_LANG_JA ? "日本語" : "English";
}
const char *native_language_tag(unsigned language) {
    return language == NATIVE_LANG_ZH_TW ? "zh-TW" : language == NATIVE_LANG_JA ? "ja" : "en";
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
