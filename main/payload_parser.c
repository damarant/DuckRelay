#include "payload_parser.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

/* === LAYOUT TASTIERA TARGET ===
 * Runtime: impostato da payload_set_layout().
 * Default: IT (sovrascritto dal boot trigger o dalla web UI).
 */
static bool s_layout_it = true;

/* Fallback per versioni di TinyUSB che non definiscono il tasto ISO < > */
#ifndef HID_KEY_NON_US_BACKSLASH
#define HID_KEY_NON_US_BACKSLASH 0x64
#endif

static const char *TAG = "payload";

static uint32_t s_default_delay = 10;

#define PAYLOAD_MAX_SIZE 8192

typedef struct {
    uint8_t keycode;
    uint8_t modifier;
} hid_map_t;

static const hid_map_t s_ascii_map[95] = {
    /* 32  ' '  */ { HID_KEY_SPACE,           0 },
    /* 33  '!'  */ { HID_KEY_1,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 34  '"'  */ { HID_KEY_APOSTROPHE,      KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 35  '#'  */ { HID_KEY_3,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 36  '$'  */ { HID_KEY_4,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 37  '%'  */ { HID_KEY_5,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 38  '&'  */ { HID_KEY_7,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 39  '\'' */ { HID_KEY_APOSTROPHE,      0 },
    /* 40  '('  */ { HID_KEY_9,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 41  ')'  */ { HID_KEY_0,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 42  '*'  */ { HID_KEY_8,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 43  '+'  */ { HID_KEY_EQUAL,           KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 44  ','  */ { HID_KEY_COMMA,           0 },
    /* 45  '-'  */ { HID_KEY_MINUS,           0 },
    /* 46  '.'  */ { HID_KEY_PERIOD,          0 },
    /* 47  '/'  */ { HID_KEY_SLASH,           0 },
    /* 48  '0'  */ { HID_KEY_0,               0 },
    /* 49  '1'  */ { HID_KEY_1,               0 },
    /* 50  '2'  */ { HID_KEY_2,               0 },
    /* 51  '3'  */ { HID_KEY_3,               0 },
    /* 52  '4'  */ { HID_KEY_4,               0 },
    /* 53  '5'  */ { HID_KEY_5,               0 },
    /* 54  '6'  */ { HID_KEY_6,               0 },
    /* 55  '7'  */ { HID_KEY_7,               0 },
    /* 56  '8'  */ { HID_KEY_8,               0 },
    /* 57  '9'  */ { HID_KEY_9,               0 },
    /* 58  ':'  */ { HID_KEY_SEMICOLON,       KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 59  ';'  */ { HID_KEY_SEMICOLON,       0 },
    /* 60  '<'  */ { HID_KEY_COMMA,           KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 61  '='  */ { HID_KEY_EQUAL,           0 },
    /* 62  '>'  */ { HID_KEY_PERIOD,          KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 63  '?'  */ { HID_KEY_SLASH,           KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 64  '@'  */ { HID_KEY_2,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 65  'A'  */ { HID_KEY_A,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 66  'B'  */ { HID_KEY_B,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 67  'C'  */ { HID_KEY_C,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 68  'D'  */ { HID_KEY_D,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 69  'E'  */ { HID_KEY_E,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 70  'F'  */ { HID_KEY_F,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 71  'G'  */ { HID_KEY_G,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 72  'H'  */ { HID_KEY_H,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 73  'I'  */ { HID_KEY_I,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 74  'J'  */ { HID_KEY_J,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 75  'K'  */ { HID_KEY_K,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 76  'L'  */ { HID_KEY_L,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 77  'M'  */ { HID_KEY_M,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 78  'N'  */ { HID_KEY_N,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 79  'O'  */ { HID_KEY_O,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 80  'P'  */ { HID_KEY_P,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 81  'Q'  */ { HID_KEY_Q,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 82  'R'  */ { HID_KEY_R,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 83  'S'  */ { HID_KEY_S,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 84  'T'  */ { HID_KEY_T,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 85  'U'  */ { HID_KEY_U,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 86  'V'  */ { HID_KEY_V,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 87  'W'  */ { HID_KEY_W,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 88  'X'  */ { HID_KEY_X,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 89  'Y'  */ { HID_KEY_Y,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 90  'Z'  */ { HID_KEY_Z,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 91  '['  */ { HID_KEY_BRACKET_LEFT,    0 },
    /* 92  '\\' */ { HID_KEY_BACKSLASH,       0 },
    /* 93  ']'  */ { HID_KEY_BRACKET_RIGHT,   0 },
    /* 94  '^'  */ { HID_KEY_6,               KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 95  '_'  */ { HID_KEY_MINUS,           KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 96  '`'  */ { HID_KEY_GRAVE,           0 },
    /* 97  'a'  */ { HID_KEY_A,               0 },
    /* 98  'b'  */ { HID_KEY_B,               0 },
    /* 99  'c'  */ { HID_KEY_C,               0 },
    /* 100 'd'  */ { HID_KEY_D,               0 },
    /* 101 'e'  */ { HID_KEY_E,               0 },
    /* 102 'f'  */ { HID_KEY_F,               0 },
    /* 103 'g'  */ { HID_KEY_G,               0 },
    /* 104 'h'  */ { HID_KEY_H,               0 },
    /* 105 'i'  */ { HID_KEY_I,               0 },
    /* 106 'j'  */ { HID_KEY_J,               0 },
    /* 107 'k'  */ { HID_KEY_K,               0 },
    /* 108 'l'  */ { HID_KEY_L,               0 },
    /* 109 'm'  */ { HID_KEY_M,               0 },
    /* 110 'n'  */ { HID_KEY_N,               0 },
    /* 111 'o'  */ { HID_KEY_O,               0 },
    /* 112 'p'  */ { HID_KEY_P,               0 },
    /* 113 'q'  */ { HID_KEY_Q,               0 },
    /* 114 'r'  */ { HID_KEY_R,               0 },
    /* 115 's'  */ { HID_KEY_S,               0 },
    /* 116 't'  */ { HID_KEY_T,               0 },
    /* 117 'u'  */ { HID_KEY_U,               0 },
    /* 118 'v'  */ { HID_KEY_V,               0 },
    /* 119 'w'  */ { HID_KEY_W,               0 },
    /* 120 'x'  */ { HID_KEY_X,               0 },
    /* 121 'y'  */ { HID_KEY_Y,               0 },
    /* 122 'z'  */ { HID_KEY_Z,               0 },
    /* 123 '{'  */ { HID_KEY_BRACKET_LEFT,    KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 124 '|'  */ { HID_KEY_BACKSLASH,       KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 125 '}'  */ { HID_KEY_BRACKET_RIGHT,   KEYBOARD_MODIFIER_LEFTSHIFT },
    /* 126 '~'  */ { HID_KEY_GRAVE,           KEYBOARD_MODIFIER_LEFTSHIFT },
};

void payload_set_layout(bool italian)
{
    s_layout_it = italian;
}

bool payload_get_layout(void)
{
    return s_layout_it;
}

/* Correzioni per il layout italiano */
static void apply_it_overrides(char c, uint8_t *mod, uint8_t *kc)
{
    switch (c) {
    case '\\': *mod = 0;                          *kc = HID_KEY_GRAVE;          break;
    case '|':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_GRAVE;          break;
    case ':':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_PERIOD;         break;
    case ';':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_COMMA;          break;
    case '?':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_MINUS;          break;
    case '\'': *mod = 0;                          *kc = HID_KEY_MINUS;          break;
    case '"':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_2;              break;
    case '!':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_1;              break;
    case '&':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_6;              break;
    case '/':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_7;              break;
    case '(':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_8;              break;
    case ')':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_9;              break;
    case '=':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_0;              break;
    case '$':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_4;              break;
    case '%':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_5;              break;
    case '+':  *mod = 0;                          *kc = HID_KEY_BRACKET_RIGHT;  break;
    case '*':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_BRACKET_RIGHT; break;
    case '[':  *mod = KEYBOARD_MODIFIER_RIGHTALT; *kc = HID_KEY_BRACKET_LEFT;   break;
    case ']':  *mod = KEYBOARD_MODIFIER_RIGHTALT; *kc = HID_KEY_BRACKET_RIGHT;  break;
    case '{':  *mod = KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_BRACKET_LEFT;  break;
    case '}':  *mod = KEYBOARD_MODIFIER_RIGHTALT | KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_BRACKET_RIGHT; break;
    case '@':  *mod = KEYBOARD_MODIFIER_RIGHTALT; *kc = HID_KEY_SEMICOLON;      break;
    case '#':  *mod = KEYBOARD_MODIFIER_RIGHTALT; *kc = HID_KEY_APOSTROPHE;     break;
    case '^':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_EQUAL;         break;
    case '<':  *mod = 0;                          *kc = HID_KEY_NON_US_BACKSLASH; break;
    case '>':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_NON_US_BACKSLASH; break;
    case '-':  *mod = 0;                          *kc = HID_KEY_SLASH;          break;
    case '_':  *mod = KEYBOARD_MODIFIER_LEFTSHIFT; *kc = HID_KEY_SLASH;         break;
    default: break;
    }
}


static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *kc)
{
    uint8_t u = (uint8_t)c;
    if (u < 32 || u > 126) return false;
    *mod = s_ascii_map[u - 32].modifier;
    *kc  = s_ascii_map[u - 32].keycode;
    if (s_layout_it) {
        apply_it_overrides(c, mod, kc);
    }
    return true;
}

static void type_key(uint8_t keycode, uint8_t modifier)
{
    uint32_t timeout = 100;
    while (!tud_hid_ready() && timeout--) vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t keys[6] = { keycode, 0, 0, 0, 0, 0 };
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, modifier, keys);
    vTaskDelay(pdMS_TO_TICKS(s_default_delay));
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(s_default_delay));
}

static void type_char(char c)
{
    uint8_t mod = 0, kc = 0;
    if (!ascii_to_hid(c, &mod, &kc)) return;
    type_key(kc, mod);
}

static void type_string(const char *s)
{
    while (*s) { type_char(*s); s++; }
}

static esp_err_t process_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;

    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' ||
                       line[len-1] == ' '  || line[len-1] == '\t')) {
        line[--len] = '\0';
    }

    if (*line == '\0' || *line == ';' || strncmp(line, "//", 2) == 0) return ESP_OK;
    if (strncasecmp(line, "REM ", 4) == 0 || strcasecmp(line, "REM") == 0) return ESP_OK;

    char *cmd = line;
    char *arg = line;
    while (*arg && *arg != ' ' && *arg != '\t') arg++;
    if (*arg) {
        *arg = '\0';
        arg++;
        while (*arg == ' ' || *arg == '\t') arg++;
    }

    ESP_LOGD(TAG, "cmd='%s' arg='%s'", cmd, arg);

    if (strcasecmp(cmd, "STRING") == 0) {
        type_string(arg);
    } else if (strcasecmp(cmd, "DELAY") == 0) {
        int ms = atoi(arg);
        if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
    } else if (strcasecmp(cmd, "DEFAULTDELAY") == 0 ||
               strcasecmp(cmd, "DEFAULT_DELAY") == 0) {
        int ms = atoi(arg);
        if (ms >= 0) s_default_delay = (uint32_t)ms;
    } else if (strcasecmp(cmd, "ENTER") == 0 || strcasecmp(cmd, "RETURN") == 0) {
        type_key(HID_KEY_ENTER, 0);
    } else if (strcasecmp(cmd, "TAB") == 0) {
        type_key(HID_KEY_TAB, 0);
    } else if (strcasecmp(cmd, "ESC") == 0 || strcasecmp(cmd, "ESCAPE") == 0) {
        type_key(HID_KEY_ESCAPE, 0);
    } else if (strcasecmp(cmd, "SPACE") == 0) {
        type_key(HID_KEY_SPACE, 0);
    } else if (strcasecmp(cmd, "BACKSPACE") == 0) {
        type_key(HID_KEY_BACKSPACE, 0);
    } else if (strcasecmp(cmd, "DELETE") == 0) {
        type_key(HID_KEY_DELETE, 0);
    } else if (strcasecmp(cmd, "GUI") == 0 || strcasecmp(cmd, "WINDOWS") == 0) {
        if (*arg) {
            uint8_t mod = 0, kc = 0;
            if (ascii_to_hid(arg[0], &mod, &kc)) {
                type_key(kc, mod | KEYBOARD_MODIFIER_LEFTGUI);
            }
        } else {
            type_key(0, KEYBOARD_MODIFIER_LEFTGUI);
        }
    } else if (strcasecmp(cmd, "CTRL") == 0 || strcasecmp(cmd, "CONTROL") == 0) {
        if (*arg) {
            uint8_t mod = 0, kc = 0;
            if (ascii_to_hid(arg[0], &mod, &kc)) {
                type_key(kc, mod | KEYBOARD_MODIFIER_LEFTCTRL);
            }
        }
    } else if (strcasecmp(cmd, "ALT") == 0) {
        if (*arg) {
            uint8_t mod = 0, kc = 0;
            if (ascii_to_hid(arg[0], &mod, &kc)) {
                type_key(kc, mod | KEYBOARD_MODIFIER_LEFTALT);
            }
        }
    } else if (strcasecmp(cmd, "SHIFT") == 0) {
        if (*arg) {
            uint8_t mod = 0, kc = 0;
            if (ascii_to_hid(arg[0], &mod, &kc)) {
                type_key(kc, mod | KEYBOARD_MODIFIER_LEFTSHIFT);
            }
        }
    } else {
        ESP_LOGW(TAG, "Comando sconosciuto: %s", cmd);
    }
    return ESP_OK;
}

esp_err_t payload_parser_init(void)
{
    s_default_delay = 20;
    s_layout_it = true;   /* default IT */
    return ESP_OK;
}

esp_err_t payload_execute_string(const char *script)
{
    if (!script) return ESP_ERR_INVALID_ARG;
    if (!tud_mounted()) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Executing payload (%u bytes)", (unsigned)strlen(script));

    const char *p = script;
    char line_buf[256];
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t line_len = end ? (size_t)(end - p) : strlen(p);
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        memcpy(line_buf, p, line_len);
        line_buf[line_len] = '\0';
        process_line(line_buf);
        p = end ? end + 1 : p + strlen(p);
    }
    ESP_LOGI(TAG, "Payload done");
    return ESP_OK;
}

esp_err_t payload_execute_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Cannot open %s (disco MSC montato su host?)", path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > PAYLOAD_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid payload size: %ld", sz);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    ESP_LOGI(TAG, "Loaded %u bytes from %s", (unsigned)n, path);
    esp_err_t ret = payload_execute_string(buf);
    free(buf);
    return ret;
}