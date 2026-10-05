/*
 * Music PM Hotkeys — TeamSpeak 3 client plugin.
 *
 * Global hotkeys (the same hook TeamSpeak uses for push-to-talk) send a
 * private message "!1" .. "!20" to one chosen client.
 */

#if defined(WIN32) || defined(__WIN32__) || defined(_WIN32)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "ts3_functions.h"

#define PLUGINS_EXPORTDLL __declspec(dllexport)
#define PLUGIN_API_VERSION 26

#define CMD_COUNT 20
#define UID_CAP 160
#define NICK_CAP 256
#define PREFIX_CAP 12
#define PATH_CAP 512
#define MSG_CAP 32
#define DEBOUNCE_MS 250

static struct TS3Functions ts3Functions;
static char* g_plugin_id = NULL;

static CRITICAL_SECTION g_lock;
static int g_lock_ready = 0;

typedef struct TargetConfig {
    char uid[UID_CAP];
    char nick[NICK_CAP];
    char prefix[PREFIX_CAP];
} TargetConfig;

static TargetConfig g_cfg;
static wchar_t g_ini_path[PATH_CAP];
static ULONGLONG g_last_sent[CMD_COUNT + 1];

static struct {
    int active;
    int sent;
    int cmd;
    uint64 schid;
    char uid[UID_CAP];
    char message[MSG_CAP];
} g_pending;

static HWND g_dialog = NULL;
static volatile LONG g_dialog_open = 0;
static HINSTANCE g_module = NULL;

enum {
    MENU_SET_TARGET = 1,
    MENU_SHOW_TARGET = 2
};

static void copy_str(char* dest, size_t cap, const char* src)
{
    size_t i;
    if (cap == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < cap && src[i]; ++i) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

static void plugin_log(enum LogLevel level, const char* message, uint64 schid)
{
    if (ts3Functions.logMessage) {
        ts3Functions.logMessage(message, level, "MusicPM", schid);
    }
}

static void chat(const char* message)
{
    if (ts3Functions.printMessageToCurrentTab && message) {
        ts3Functions.printMessageToCurrentTab(message);
    }
}

static int utf8_iequals(const char* a, const char* b)
{
    wchar_t wa[NICK_CAP];
    wchar_t wb[NICK_CAP];
    if (!a || !b) {
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a, -1, wa, NICK_CAP) <= 0) {
        return strcmp(a, b) == 0;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, b, -1, wb, NICK_CAP) <= 0) {
        return strcmp(a, b) == 0;
    }
    return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, wa, -1, wb, -1) == CSTR_EQUAL;
}

static void trim_inplace(char* s)
{
    char* start;
    size_t n;
    if (!s) {
        return;
    }
    start = s;
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static int prefix_ok(const char* s)
{
    size_t i;
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n >= PREFIX_CAP) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c < 33 || c > 126) {
            return 0;
        }
    }
    return 1;
}

static void config_defaults(TargetConfig* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    copy_str(cfg->prefix, sizeof(cfg->prefix), "!");
}

static void strip_breaks(char* s)
{
    char* r = s;
    char* w = s;
    if (!s) {
        return;
    }
    while (*r) {
        if (*r != '\r' && *r != '\n') {
            *w++ = *r;
        }
        ++r;
    }
    *w = '\0';
}

static void config_sanitize(TargetConfig* cfg)
{
    strip_breaks(cfg->uid);
    strip_breaks(cfg->nick);
    trim_inplace(cfg->uid);
    trim_inplace(cfg->nick);
    trim_inplace(cfg->prefix);
    if (!prefix_ok(cfg->prefix)) {
        copy_str(cfg->prefix, sizeof(cfg->prefix), "!");
    }
}

static void config_snapshot(TargetConfig* out)
{
    EnterCriticalSection(&g_lock);
    *out = g_cfg;
    LeaveCriticalSection(&g_lock);
}

static void ini_set_path_from_utf8(const char* utf8)
{
    g_ini_path[0] = L'\0';
    if (!utf8 || !utf8[0]) {
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, g_ini_path, PATH_CAP);
}

static int save_config_file(const TargetConfig* cfg)
{
    FILE* file;
    TargetConfig clean = *cfg;
    config_sanitize(&clean);
    if (!g_ini_path[0]) {
        return 0;
    }
    file = _wfopen(g_ini_path, L"wb");
    if (!file) {
        return 0;
    }
    fputs("# Music PM Hotkeys\r\n", file);
    fprintf(file, "uid=%s\r\n", clean.uid);
    fprintf(file, "nick=%s\r\n", clean.nick);
    fprintf(file, "prefix=%s\r\n", clean.prefix);
    fclose(file);
    return 1;
}

static void load_config_file(void)
{
    FILE* file;
    char line[512];
    TargetConfig cfg;
    config_defaults(&cfg);
    if (!g_ini_path[0]) {
        EnterCriticalSection(&g_lock);
        g_cfg = cfg;
        LeaveCriticalSection(&g_lock);
        return;
    }
    file = _wfopen(g_ini_path, L"rb");
    if (!file) {
        EnterCriticalSection(&g_lock);
        g_cfg = cfg;
        LeaveCriticalSection(&g_lock);
        return;
    }
    while (fgets(line, sizeof(line), file)) {
        char* eq;
        trim_inplace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        trim_inplace(line);
        trim_inplace(eq + 1);
        if (strcmp(line, "uid") == 0) {
            copy_str(cfg.uid, sizeof(cfg.uid), eq + 1);
        } else if (strcmp(line, "nick") == 0) {
            copy_str(cfg.nick, sizeof(cfg.nick), eq + 1);
        } else if (strcmp(line, "prefix") == 0) {
            copy_str(cfg.prefix, sizeof(cfg.prefix), eq + 1);
        }
    }
    fclose(file);
    config_sanitize(&cfg);
    EnterCriticalSection(&g_lock);
    g_cfg = cfg;
    LeaveCriticalSection(&g_lock);
}

static void config_commit(const TargetConfig* cfg)
{
    TargetConfig clean = *cfg;
    config_sanitize(&clean);
    EnterCriticalSection(&g_lock);
    g_cfg = clean;
    LeaveCriticalSection(&g_lock);
    if (!save_config_file(&clean)) {
        plugin_log(LogLevel_ERROR, "Failed to save music_pm_hotkeys.ini", 0);
    }
}

static void format_command(const TargetConfig* cfg, int cmd, char* out, size_t cap)
{
    snprintf(out, cap, "%s%d", cfg->prefix, cmd);
}

static int keyword_to_cmd(const char* keyword)
{
    int tens;
    int ones;
    if (!keyword || strncmp(keyword, "music_pm_", 9) != 0) {
        return 0;
    }
    if (keyword[9] < '0' || keyword[9] > '9' || keyword[10] < '0' || keyword[10] > '9' || keyword[11] != '\0') {
        return 0;
    }
    tens = keyword[9] - '0';
    ones = keyword[10] - '0';
    tens = tens * 10 + ones;
    if (tens < 1 || tens > CMD_COUNT) {
        return 0;
    }
    return tens;
}

static void bb_escape(const char* src, char* dest, size_t cap)
{
    size_t o = 0;
    size_t i;
    if (cap == 0) {
        return;
    }
    for (i = 0; src && src[i] && o + 1 < cap; ++i) {
        char c = src[i];
        if (c == '[' || c == ']') {
            c = '\'';
        }
        dest[o++] = c;
    }
    dest[o] = '\0';
}

static void chatf_target_saved(const TargetConfig* cfg)
{
    char nick[NICK_CAP];
    char line[640];
    bb_escape(cfg->nick[0] ? cfg->nick : cfg->uid, nick, sizeof(nick));
    snprintf(line, sizeof(line),
             "[color=green]Music PM:[/color] получатель лички — [b]%s[/b]. "
             "Клавиши: Инструменты → Опции → Горячие клавиши → Добавить → «Отправить %s1 в личку».",
             nick, cfg->prefix);
    chat(line);
}

static anyID find_visible(uint64 schid, const TargetConfig* cfg, int* nick_ambiguous)
{
    anyID* list = NULL;
    anyID found_uid = 0;
    anyID found_nick = 0;
    int nick_hits = 0;
    size_t i;

    *nick_ambiguous = 0;
    if (!ts3Functions.getClientList || ts3Functions.getClientList(schid, &list) != ERROR_ok || !list) {
        return 0;
    }

    for (i = 0; list[i] != 0 && i < 4096; ++i) {
        char* uid = NULL;
        char* nick = NULL;
        if (cfg->uid[0] &&
            ts3Functions.getClientVariableAsString(schid, list[i], CLIENT_UNIQUE_IDENTIFIER, &uid) == ERROR_ok &&
            uid && strcmp(uid, cfg->uid) == 0) {
            found_uid = list[i];
        }
        if (!found_uid && cfg->nick[0] &&
            ts3Functions.getClientVariableAsString(schid, list[i], CLIENT_NICKNAME, &nick) == ERROR_ok &&
            nick && utf8_iequals(nick, cfg->nick)) {
            ++nick_hits;
            found_nick = list[i];
        }
        if (uid) {
            ts3Functions.freeMemory(uid);
        }
        if (nick) {
            ts3Functions.freeMemory(nick);
        }
        if (found_uid) {
            break;
        }
    }
    ts3Functions.freeMemory(list);

    if (found_uid) {
        return found_uid;
    }
    /* A saved unique id must not fall back to a different person with the same nick. */
    if (cfg->uid[0]) {
        return 0;
    }
    if (nick_hits > 1) {
        *nick_ambiguous = 1;
        return 0;
    }
    return found_nick;
}

static void deliver(uint64 schid, anyID target, const char* message)
{
    unsigned int error;
    char line[256];
    if (!message || !message[0] || target == 0) {
        return;
    }
    error = ts3Functions.requestSendPrivateTextMsg(schid, message, target, NULL);
    if (error == ERROR_ok) {
        snprintf(line, sizeof(line), "Sent private message \"%s\"", message);
        plugin_log(LogLevel_INFO, line, schid);
        return;
    }
    if (ts3Functions.getErrorMessage) {
        char* err = NULL;
        if (ts3Functions.getErrorMessage(error, &err) == ERROR_ok && err) {
            snprintf(line, sizeof(line), "[color=red]Music PM:[/color] не удалось отправить %s (%s)", message, err);
            chat(line);
            ts3Functions.freeMemory(err);
            return;
        }
    }
    snprintf(line, sizeof(line), "[color=red]Music PM:[/color] не удалось отправить %s (код %u)", message, error);
    chat(line);
}

static void begin_uid_lookup(uint64 schid, const TargetConfig* cfg, int cmd, const char* message)
{
    unsigned int error;
    EnterCriticalSection(&g_lock);
    if (g_pending.active) {
        g_pending.cmd = cmd;
        g_pending.schid = schid;
        copy_str(g_pending.message, sizeof(g_pending.message), message);
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_pending.active = 1;
    g_pending.sent = 0;
    g_pending.cmd = cmd;
    g_pending.schid = schid;
    copy_str(g_pending.uid, sizeof(g_pending.uid), cfg->uid);
    copy_str(g_pending.message, sizeof(g_pending.message), message);
    LeaveCriticalSection(&g_lock);

    error = ts3Functions.requestClientIDs(schid, cfg->uid, NULL);
    if (error != ERROR_ok) {
        EnterCriticalSection(&g_lock);
        g_pending.active = 0;
        LeaveCriticalSection(&g_lock);
        chat("[color=red]Music PM:[/color] не удалось запросить id получателя у сервера.");
    }
}

static void send_numbered_command(uint64 schid, int cmd)
{
    TargetConfig cfg;
    char message[MSG_CAP];
    anyID target;
    int ambiguous = 0;
    ULONGLONG now;

    if (cmd < 1 || cmd > CMD_COUNT) {
        return;
    }
    if (schid == 0 && ts3Functions.getCurrentServerConnectionHandlerID) {
        schid = ts3Functions.getCurrentServerConnectionHandlerID();
    }
    if (schid == 0) {
        chat("[color=red]Music PM:[/color] нет подключения к серверу.");
        return;
    }

    now = GetTickCount64();
    EnterCriticalSection(&g_lock);
    if (now - g_last_sent[cmd] < DEBOUNCE_MS) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_last_sent[cmd] = now;
    cfg = g_cfg;
    LeaveCriticalSection(&g_lock);

    if (!cfg.uid[0] && !cfg.nick[0]) {
        chat("[color=red]Music PM:[/color] получатель не задан. Правый клик по боту → «Назначить получателем команд !1-!20».");
        return;
    }

    format_command(&cfg, cmd, message, sizeof(message));
    target = find_visible(schid, &cfg, &ambiguous);
    if (ambiguous) {
        chat("[color=red]Music PM:[/color] на сервере несколько людей с таким ником. Назначьте получателя правым кликом, чтобы запомнить Unique ID.");
        return;
    }
    if (target != 0) {
        EnterCriticalSection(&g_lock);
        g_pending.active = 0;
        LeaveCriticalSection(&g_lock);
        deliver(schid, target, message);
        return;
    }
    if (cfg.uid[0] && ts3Functions.requestClientIDs) {
        begin_uid_lookup(schid, &cfg, cmd, message);
        return;
    }
    chat("[color=red]Music PM:[/color] получатель не виден в списке. Зайдите в его канал или назначьте его правым кликом, когда он онлайн.");
}

static int wide_to_utf8(const wchar_t* src, char* dest, size_t cap)
{
    if (!dest || cap == 0) {
        return 0;
    }
    dest[0] = '\0';
    if (!src) {
        return 1;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, src, -1, dest, (int)cap, NULL, NULL) <= 0) {
        dest[0] = '\0';
        return 0;
    }
    return 1;
}

static void read_control_utf8(HWND hwnd, int id, char* dest, size_t cap)
{
    wchar_t wide[NICK_CAP];
    GetDlgItemTextW(hwnd, id, wide, NICK_CAP);
    wide_to_utf8(wide, dest, cap);
    trim_inplace(dest);
}

#define IDC_NICK 101
#define IDC_UID 102
#define IDC_PREFIX 103

static INT_PTR CALLBACK config_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        TargetConfig cfg;
        wchar_t wide[NICK_CAP];
        config_snapshot(&cfg);
        g_dialog = hwnd;
        InterlockedExchange(&g_dialog_open, 1);
        MultiByteToWideChar(CP_UTF8, 0, cfg.nick, -1, wide, NICK_CAP);
        SetDlgItemTextW(hwnd, IDC_NICK, wide);
        MultiByteToWideChar(CP_UTF8, 0, cfg.uid, -1, wide, NICK_CAP);
        SetDlgItemTextW(hwnd, IDC_UID, wide);
        MultiByteToWideChar(CP_UTF8, 0, cfg.prefix, -1, wide, NICK_CAP);
        SetDlgItemTextW(hwnd, IDC_PREFIX, wide);
        SendDlgItemMessageW(hwnd, IDC_NICK, EM_SETLIMITTEXT, 64, 0);
        SendDlgItemMessageW(hwnd, IDC_UID, EM_SETLIMITTEXT, UID_CAP - 1, 0);
        SendDlgItemMessageW(hwnd, IDC_PREFIX, EM_SETLIMITTEXT, 8, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            TargetConfig cfg;
            config_defaults(&cfg);
            read_control_utf8(hwnd, IDC_NICK, cfg.nick, sizeof(cfg.nick));
            read_control_utf8(hwnd, IDC_UID, cfg.uid, sizeof(cfg.uid));
            read_control_utf8(hwnd, IDC_PREFIX, cfg.prefix, sizeof(cfg.prefix));
            if (!cfg.nick[0] && !cfg.uid[0]) {
                MessageBoxW(hwnd, L"Укажите ник или Unique ID получателя.", L"Music PM", MB_ICONWARNING);
                return TRUE;
            }
            if (!prefix_ok(cfg.prefix)) {
                MessageBoxW(hwnd, L"Префикс: 1–8 символов без пробелов, например !", L"Music PM", MB_ICONWARNING);
                return TRUE;
            }
            config_commit(&cfg);
            chatf_target_saved(&cfg);
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    case WM_DESTROY:
        g_dialog = NULL;
        InterlockedExchange(&g_dialog_open, 0);
        break;
    default:
        break;
    }
    return FALSE;
}

typedef struct DlgBuf {
    unsigned char data[8192];
    size_t size;
} DlgBuf;

static void dlg_align(DlgBuf* buf)
{
    while (buf->size % 4) {
        buf->data[buf->size++] = 0;
    }
}

static void dlg_bytes(DlgBuf* buf, const void* src, size_t n)
{
    if (buf->size + n > sizeof(buf->data)) {
        return;
    }
    memcpy(buf->data + buf->size, src, n);
    buf->size += n;
}

static void dlg_word(DlgBuf* buf, WORD value)
{
    dlg_bytes(buf, &value, sizeof(value));
}

static void dlg_dword(DlgBuf* buf, DWORD value)
{
    dlg_bytes(buf, &value, sizeof(value));
}

static void dlg_short(DlgBuf* buf, short value)
{
    dlg_bytes(buf, &value, sizeof(value));
}

static void dlg_wstr(DlgBuf* buf, const wchar_t* text)
{
    const wchar_t* s = text ? text : L"";
    dlg_bytes(buf, s, (wcslen(s) + 1) * sizeof(wchar_t));
}

static void dlg_item(DlgBuf* buf, DWORD style, short x, short y, short cx, short cy, WORD id, WORD klass, const wchar_t* title)
{
    dlg_align(buf);
    dlg_dword(buf, style);
    dlg_dword(buf, 0);
    dlg_short(buf, x);
    dlg_short(buf, y);
    dlg_short(buf, cx);
    dlg_short(buf, cy);
    dlg_word(buf, id);
    dlg_word(buf, 0xFFFF);
    dlg_word(buf, klass);
    dlg_wstr(buf, title);
    dlg_word(buf, 0);
}

static void show_config_dialog(void)
{
    DlgBuf buf;
    DWORD style;
    memset(&buf, 0, sizeof(buf));
    style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_MODALFRAME | DS_CENTER;
    dlg_dword(&buf, style);
    dlg_dword(&buf, 0);
    dlg_word(&buf, 9);
    dlg_short(&buf, 0);
    dlg_short(&buf, 0);
    dlg_short(&buf, 320);
    dlg_short(&buf, 214);
    dlg_word(&buf, 0);
    dlg_word(&buf, 0);
    dlg_wstr(&buf, L"Music PM — кому отправлять !1 … !20");
    dlg_word(&buf, 9);
    dlg_wstr(&buf, L"Segoe UI");

    dlg_item(&buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 8, 304, 10, 0xFFFF, 0x0082, L"Ник получателя");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 18, 304, 14, IDC_NICK, 0x0081, L"");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 38, 304, 10, 0xFFFF, 0x0082, L"Unique ID (надежнее ника, заполняется правым кликом)");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 48, 304, 14, IDC_UID, 0x0081, L"");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 68, 304, 10, 0xFFFF, 0x0082, L"Префикс. Сообщение = префикс + номер, по умолчанию !1");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 8, 78, 48, 14, IDC_PREFIX, 0x0081, L"!");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | SS_LEFT, 8, 100, 304, 62, 0xFFFF, 0x0082,
             L"Клавиши: Инструменты, Опции, вкладка Горячие клавиши, кнопка Добавить. В списке действий — «Отправить !1 в личку» и дальше до !20. Это тот же перехват, что и «Нажать и говорить», поэтому сочетания работают в игре.");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 168, 176, 70, 16, IDOK, 0x0080, L"Сохранить");
    dlg_item(&buf, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 244, 176, 68, 16, IDCANCEL, 0x0080, L"Отмена");

    if (DialogBoxIndirectParamW(g_module ? g_module : GetModuleHandleW(NULL), (LPCDLGTEMPLATEW)buf.data, NULL, config_dlg_proc, 0) == -1) {
        MessageBoxW(NULL, L"Не удалось открыть окно настроек Music PM.", L"Music PM", MB_ICONERROR);
    }
}

static struct PluginMenuItem* create_menu_item(enum PluginMenuType type, int id, const char* text)
{
    struct PluginMenuItem* item = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
    if (!item) {
        return NULL;
    }
    item->type = type;
    item->id = id;
    copy_str(item->text, PLUGIN_MENU_BUFSZ, text);
    item->icon[0] = '\0';
    return item;
}

static struct PluginHotkey* create_hotkey(const char* keyword, const char* description)
{
    struct PluginHotkey* hotkey = (struct PluginHotkey*)malloc(sizeof(struct PluginHotkey));
    if (!hotkey) {
        return NULL;
    }
    copy_str(hotkey->keyword, PLUGIN_HOTKEY_BUFSZ, keyword);
    copy_str(hotkey->description, PLUGIN_HOTKEY_BUFSZ, description);
    return hotkey;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
        config_defaults(&g_cfg);
    }
    return TRUE;
}

PLUGINS_EXPORTDLL const char* ts3plugin_name(void)
{
    return "Музыка ЛС !1-!20";
}

PLUGINS_EXPORTDLL const char* ts3plugin_version(void)
{
    return "1.0.0";
}

PLUGINS_EXPORTDLL int ts3plugin_apiVersion(void)
{
    return PLUGIN_API_VERSION;
}

PLUGINS_EXPORTDLL const char* ts3plugin_author(void)
{
    return "Music PM Hotkeys";
}

PLUGINS_EXPORTDLL const char* ts3plugin_description(void)
{
    return "По глобальным горячим клавишам отправляет в личку выбранному человеку команды !1 … !20. "
           "Клавиши работают так же, как «Нажать и говорить», в том числе во время игры.";
}

PLUGINS_EXPORTDLL void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
}

PLUGINS_EXPORTDLL int ts3plugin_init(void)
{
    char config_path[PATH_CAP];
    char ini_utf8[PATH_CAP];
    size_t n;

    config_path[0] = '\0';
    if (ts3Functions.getConfigPath) {
        ts3Functions.getConfigPath(config_path, PATH_CAP);
    }
    n = strlen(config_path);
    if (n > 0 && config_path[n - 1] != '\\' && config_path[n - 1] != '/') {
        if (n + 1 < PATH_CAP) {
            config_path[n] = '\\';
            config_path[n + 1] = '\0';
        }
    }
    snprintf(ini_utf8, sizeof(ini_utf8), "%smusic_pm_hotkeys.ini", config_path);
    ini_set_path_from_utf8(ini_utf8);
    load_config_file();
    plugin_log(LogLevel_INFO, "Music PM hotkeys loaded", 0);
    return 0;
}

PLUGINS_EXPORTDLL void ts3plugin_shutdown(void)
{
    HWND dialog;
    int spins = 0;

    dialog = g_dialog;
    if (dialog) {
        PostMessageW(dialog, WM_CLOSE, 0, 0);
    }
    while (g_dialog_open && spins < 50) {
        Sleep(20);
        ++spins;
    }

    if (g_plugin_id) {
        free(g_plugin_id);
        g_plugin_id = NULL;
    }
    if (g_lock_ready) {
        DeleteCriticalSection(&g_lock);
        g_lock_ready = 0;
    }
}

PLUGINS_EXPORTDLL int ts3plugin_offersConfigure(void)
{
    return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD;
}

PLUGINS_EXPORTDLL void ts3plugin_configure(void* handle, void* qParentWidget)
{
    (void)handle;
    (void)qParentWidget;
    show_config_dialog();
}

PLUGINS_EXPORTDLL void ts3plugin_registerPluginID(const char* id)
{
    size_t n = strlen(id) + 1;
    g_plugin_id = (char*)malloc(n);
    if (g_plugin_id) {
        memcpy(g_plugin_id, id, n);
    }
}

PLUGINS_EXPORTDLL const char* ts3plugin_commandKeyword(void)
{
    return "musicpm";
}

PLUGINS_EXPORTDLL int ts3plugin_processCommand(uint64 serverConnectionHandlerID, const char* command)
{
    const char* p = command ? command : "";
    char line[640];
    TargetConfig cfg;
    int cmd = 0;

    while (*p == ' ') {
        ++p;
    }
    if (*p == '\0' || strcmp(p, "help") == 0 || strcmp(p, "who") == 0) {
        config_snapshot(&cfg);
        if (cfg.nick[0] || cfg.uid[0]) {
            char nick[NICK_CAP];
            bb_escape(cfg.nick[0] ? cfg.nick : "(без ника)", nick, sizeof(nick));
            snprintf(line, sizeof(line),
                     "[b]Music PM[/b] получатель: %s. Команда: /musicpm 1 … /musicpm 20 отправит %s1 … %s20 в личку.",
                     nick, cfg.prefix, cfg.prefix);
        } else {
            snprintf(line, sizeof(line),
                     "[b]Music PM[/b] получатель не задан. Правый клик по человеку → назначить. Затем /musicpm 1 или горячая клавиша.");
        }
        chat(line);
        return 0;
    }
    if (p[0] >= '0' && p[0] <= '9') {
        cmd = atoi(p);
        if (cmd >= 1 && cmd <= CMD_COUNT) {
            send_numbered_command(serverConnectionHandlerID, cmd);
            return 0;
        }
    }
    chat("[color=red]Music PM:[/color] используйте /musicpm 1 … /musicpm 20 или /musicpm who");
    return 0;
}

PLUGINS_EXPORTDLL const char* ts3plugin_infoTitle(void)
{
    return "Music PM";
}

PLUGINS_EXPORTDLL void ts3plugin_infoData(uint64 serverConnectionHandlerID, uint64 id, enum PluginItemType type, char** data)
{
    TargetConfig cfg;
    char* text;
    char nick[NICK_CAP];
    (void)serverConnectionHandlerID;
    (void)id;
    (void)type;

    text = (char*)malloc(512);
    if (!text) {
        *data = NULL;
        return;
    }
    config_snapshot(&cfg);
    if (cfg.nick[0]) {
        bb_escape(cfg.nick, nick, sizeof(nick));
        snprintf(text, 512, "Личка: [b]%s[/b] → %s1 … %s20", nick, cfg.prefix, cfg.prefix);
    } else if (cfg.uid[0]) {
        snprintf(text, 512, "Личка по Unique ID → %s1 … %s20", cfg.prefix, cfg.prefix);
    } else {
        snprintf(text, 512, "Получатель не задан");
    }
    *data = text;
}

PLUGINS_EXPORTDLL void ts3plugin_freeMemory(void* data)
{
    free(data);
}

PLUGINS_EXPORTDLL int ts3plugin_requestAutoload(void)
{
    return 1;
}

PLUGINS_EXPORTDLL void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon)
{
    *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * 3);
    if (!*menuItems) {
        *menuIcon = NULL;
        return;
    }
    (*menuItems)[0] = create_menu_item(PLUGIN_MENU_TYPE_CLIENT, MENU_SET_TARGET, "Назначить получателем команд !1-!20");
    (*menuItems)[1] = create_menu_item(PLUGIN_MENU_TYPE_GLOBAL, MENU_SHOW_TARGET, "Показать получателя лички");
    (*menuItems)[2] = NULL;
    *menuIcon = NULL;
}

PLUGINS_EXPORTDLL void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys)
{
    TargetConfig cfg;
    int i;
    config_snapshot(&cfg);
    *hotkeys = (struct PluginHotkey**)malloc(sizeof(struct PluginHotkey*) * (CMD_COUNT + 1));
    if (!*hotkeys) {
        return;
    }
    for (i = 1; i <= CMD_COUNT; ++i) {
        char keyword[PLUGIN_HOTKEY_BUFSZ];
        char description[PLUGIN_HOTKEY_BUFSZ];
        snprintf(keyword, sizeof(keyword), "music_pm_%02d", i);
        snprintf(description, sizeof(description), "Отправить %s%d в личку", cfg.prefix, i);
        (*hotkeys)[i - 1] = create_hotkey(keyword, description);
    }
    (*hotkeys)[CMD_COUNT] = NULL;
}

PLUGINS_EXPORTDLL void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID)
{
    if (type == PLUGIN_MENU_TYPE_CLIENT && menuItemID == MENU_SET_TARGET) {
        anyID client_id = (anyID)selectedItemID;
        char* uid = NULL;
        char* nick = NULL;
        TargetConfig cfg;
        config_snapshot(&cfg);
        if (ts3Functions.getClientVariableAsString(serverConnectionHandlerID, client_id, CLIENT_UNIQUE_IDENTIFIER, &uid) != ERROR_ok) {
            chat("[color=red]Music PM:[/color] не удалось прочитать Unique ID.");
            return;
        }
        if (ts3Functions.getClientVariableAsString(serverConnectionHandlerID, client_id, CLIENT_NICKNAME, &nick) != ERROR_ok) {
            nick = NULL;
        }
        copy_str(cfg.uid, sizeof(cfg.uid), uid ? uid : "");
        copy_str(cfg.nick, sizeof(cfg.nick), nick ? nick : "");
        if (uid) {
            ts3Functions.freeMemory(uid);
        }
        if (nick) {
            ts3Functions.freeMemory(nick);
        }
        config_commit(&cfg);
        chatf_target_saved(&cfg);
        return;
    }

    if (type == PLUGIN_MENU_TYPE_GLOBAL && menuItemID == MENU_SHOW_TARGET) {
        char line[640];
        char nick[NICK_CAP];
        TargetConfig cfg;
        config_snapshot(&cfg);
        if (!cfg.nick[0] && !cfg.uid[0]) {
            chat("[b]Music PM[/b] получатель не задан. Правый клик по боту в списке клиентов.");
            return;
        }
        bb_escape(cfg.nick[0] ? cfg.nick : cfg.uid, nick, sizeof(nick));
        snprintf(line, sizeof(line), "[b]Music PM[/b] личка уходит к [b]%s[/b], команды %s1 … %s20.", nick, cfg.prefix, cfg.prefix);
        chat(line);
    }
}

PLUGINS_EXPORTDLL void ts3plugin_onHotkeyEvent(const char* keyword)
{
    int cmd = keyword_to_cmd(keyword);
    uint64 schid = 0;
    if (!cmd) {
        return;
    }
    if (ts3Functions.getCurrentServerConnectionHandlerID) {
        schid = ts3Functions.getCurrentServerConnectionHandlerID();
    }
    send_numbered_command(schid, cmd);
}

PLUGINS_EXPORTDLL void ts3plugin_onClientIDsEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, anyID clientID, const char* clientName)
{
    char message[MSG_CAP];
    int take = 0;
    (void)clientName;

    EnterCriticalSection(&g_lock);
    if (g_pending.active && !g_pending.sent && g_pending.schid == serverConnectionHandlerID &&
        uniqueClientIdentifier && strcmp(uniqueClientIdentifier, g_pending.uid) == 0 && clientID != 0) {
        copy_str(message, sizeof(message), g_pending.message);
        g_pending.sent = 1;
        take = 1;
    }
    LeaveCriticalSection(&g_lock);

    if (take) {
        deliver(serverConnectionHandlerID, clientID, message);
    }
}

PLUGINS_EXPORTDLL void ts3plugin_onClientIDsFinishedEvent(uint64 serverConnectionHandlerID)
{
    int missing = 0;
    EnterCriticalSection(&g_lock);
    if (g_pending.active && g_pending.schid == serverConnectionHandlerID) {
        missing = !g_pending.sent;
        g_pending.active = 0;
    }
    LeaveCriticalSection(&g_lock);
    if (missing) {
        chat("[color=red]Music PM:[/color] получатель не в сети на этом сервере.");
    }
}

PLUGINS_EXPORTDLL const char* ts3plugin_keyPrefix(void)
{
    return "musicpm";
}
