#include <stdio.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>
#include <locale.h>
#include <assert.h>

#include <curl/curl.h>

#define SV_IMPLEMENTATION
#include <sv.h>

#ifndef BOT_TOKEN
 #error BOT_TOKEN is not specifed
#endif

#define INFO(fmt, ...) wprintf("[INFO]: "fmt, __VA_ARGS__)

#define CURL_INIT_ERR()                         \
{                                               \
    fputs("ERROR: Creating `curl`\n", stderr);  \
    return 1;                                   \
}

#define CURL_PERFORM_ERR(code)                                                  \
{                                                                               \
    fprintf(stderr, "ERROR: Sending request: %s\n", curl_easy_strerror(code));  \
    return 1;                                                                   \
}

#define CURLU_ERR(code)                                                    \
{                                                                          \
    fprintf(stderr, "ERROR: Building url: %s\n", curl_url_strerror(code)); \
    return 1;                                                              \
}

#define WS_LIT(lit) { (sizeof(lit)/sizeof(wchar_t))-1, (lit) }
#define WS_FMT "%.*ls"
#define WS_ARG(ws) (ws).count, (ws).data

typedef struct {
    String_View name;
    String_View value;
} Field;

typedef struct {
    size_t count;
    char *data;
} Str;

typedef struct {
    size_t count;
    wchar_t *data;
} WStr;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ByteBuffer;



static struct {
    size_t id;
    String_View id_str;
    String_View chat_id;
    ByteBuffer buffer;
    WStr text;
} received_message = {0};

#define TRIGGERS_COUNT (sizeof(triggers)/sizeof(triggers[0]))
static struct {
    WStr on_word;
    String_View answer;
} triggers[] = {
    {
        .on_word = WS_LIT(L"как"),
        .answer  = SV_STATIC("Жопой об косяк"),
    },
    {
        .on_word = WS_LIT(L"да"),
        .answer  = SV_STATIC("Пизда"),
    },
    {
        .on_word = WS_LIT(L"нет"),
        .answer  = SV_STATIC("Минет")
    },
    {
        .on_word = WS_LIT(L"чо"),
        .answer  = SV_STATIC("Хуй через плечо")
    },
    {
        .on_word = WS_LIT(L"алло"),
        .answer  = SV_STATIC("Хуем по лбу не дало?")
    },
    {
        .on_word = WS_LIT(L"я"),
        .answer  = SV_STATIC("Головка от хуя")
    },
    {
        .on_word = WS_LIT(L"а"),
        .answer  = SV_STATIC("Хуйна")
    },
    {
        .on_word = WS_LIT(L"ну"),
        .answer  = SV_STATIC("Хуй гну")
    },
};



size_t parse_int(String_View text)
{
    size_t num = 0;
    for (size_t i = 0; i < text.count; i++) {
        num = num*10 + text.data[i]-'0';
    }
    return num;
}

size_t parse_hex(String_View text)
{
    size_t result = 0;
    for (size_t i = 0; i < text.count; i++) {
        if (text.data[i] > '9') {
            result = result*16 + text.data[i]-87;
        } else {
            result = result*16 + text.data[i]-'0';
        }
    }

    return result;
}

bool bb_reserve(ByteBuffer *bb, size_t amount)
{
    amount += 1; // '\0' at the end
    if (amount <= bb->cap) return true;
    bb->data = realloc(bb->data, amount);
    if (bb->data == NULL) {
        fputs("ERROR: Could not allocate memory\n", stderr);
        return false;
    }

    bb->cap = amount;

    return true;
}

void bb_append_byte(ByteBuffer *bb, char byte)
{
    assert(bb->len+1 < bb->cap);
    bb->data[bb->len++] = byte;
    bb->data[bb->len] = '\0';
}

void bb_append_slice(ByteBuffer *bb, const char *bytes, size_t count)
{
    assert(bb->len+count < bb->cap);
    memcpy(&bb->data[bb->len], bytes, count);
    bb->len += count;
    bb->data[bb->len] = '\0';
}

bool init_msg_text(String_View str)
{
    // allocating memory if it needs
    // TODO: count `wchar_t` considering this sequences: `\u0425`
    received_message.buffer.len = 0;
    if (!bb_reserve(&received_message.buffer, str.count*sizeof(wchar_t)))
        return false;

    // iterating over str converting ascii unicode sequences. Example: `\u0425` -> `Х`
    WStr new_wstr = { 0, (wchar_t *) received_message.buffer.data };
    for (size_t i = 0; i < str.count; i++, new_wstr.count++) {
        if (str.data[i] != '\\' || str.data[i+1] != 'u') {
            new_wstr.data[new_wstr.count] = str.data[i];
            continue;
        } else {
            wchar_t wc = (wchar_t) parse_hex(sv_from_parts(&str.data[i+2], 4));
            new_wstr.data[new_wstr.count] = wc;
            i += 5;
        }
    }

    received_message.text = new_wstr;
    return true;
}

bool is_not_double_quote(char s) { return s != '\"'; }
bool is_not_field_end(char s) { return s != ',' && s != '}'; }
bool next_field(String_View *source, Field *result)
{
    if (!sv_try_chop_by_delim(source, '\"', NULL)) return false;

    result->name = *source;
    sv_chop_by_delim(source, ':');
    result->name.count -= source->count+2;

    result->value = *source;
    switch (*source->data) {
    case '{':;
        size_t depth = 1;
        sv_chop_left(source, 1);
        do {
            switch (*source->data) {
                case '{': depth += 1; break;
                case '}': depth -= 1; break;
            }
            sv_chop_left(source, 1);
        } while (depth != 0);
        break;

    case '"':
        sv_chop_left(source, 1);
        while (*source->data != '\"' || source->data[-1] == '\\') {
            sv_chop_left(source, 1);
        }
        break;

    default:
        sv_chop_left_while(source, is_not_field_end);
        break;
    }

    result->value.count -= source->count;

    return true;
}

size_t handle_data(char *buf, size_t is, size_t ni, void *something)
{
    (void) something;

    size_t count = is*ni;
    String_View source = sv_from_parts(buf, count);

    // jump to useful data
    sv_chop_by_delim(&source, '{');
    sv_chop_by_delim(&source, '{');
    sv_chop_by_delim(&source, '{');

    Field field;
    for (;;) {
        if (!next_field(&source, &field)) {
            fputs("ERROR: `message_id` field is not found\n", stderr);
            return 0;
        } 
        if (sv_eq(field.name, (String_View)SV_STATIC("message_id"))) {
            received_message.id_str = field.value;
            received_message.id = parse_int(field.value);
            break;
        }
    }

    for (;;) {
        if (!next_field(&source, &field)) {
            fputs("ERROR: `chat` field is not found\n", stderr);
            return 0;
        } 
        if (sv_eq(field.name, (String_View)SV_STATIC("chat"))) {
            Field chat_field;
            for (;;) {
                if (!next_field(&field.value, &chat_field)) {
                    fputs("ERROR: `chat_id` field is not found\n", stderr);
                    return 0;
                } 
                if (sv_eq(chat_field.name, (String_View)SV_STATIC("id"))) {
                    received_message.chat_id = chat_field.value;
                    break;
                }
            }
            break;
        }
    }

    for (;;) {
        if (!next_field(&source, &field)) {
            received_message.text = (WStr){0};
            break;
        } 
        if (sv_eq(field.name, (String_View)SV_STATIC("text"))) {
            field.value.data++;
            field.value.count -= 2;
            if (!init_msg_text(field.value)) return 1;
            break;
        }
    }

    return count;
}

bool next_word(wchar_t **text, wchar_t **result)
{
    wchar_t *new_text = *text;
    do {
        if (*new_text == '\0') return false;
    } while (!iswalpha(*new_text++));

    *result = new_text-1;
    while (iswalpha(*new_text)) new_text++;
    *new_text = '\0';

    *text = new_text+1;
    return true;
}

bool get_last_word(WStr text, WStr *result)
{
    if (text.count == 0) return false;

    size_t i = text.count;
    while (!iswalpha(text.data[--i])) {
        if (&text.data[i] == text.data)
            return false;
    }

    result->count = 1;
    while (&text.data[--i] >= text.data &&
           iswalpha(text.data[i])) {
        result->count += 1;
    }

    result->data = &text.data[i+1];
    return true;
}

bool field_to_str(ByteBuffer *bb, Field field)
{
    // clean the buffer
    bb->len = 0;

    // allocating place
    if (!bb_reserve(bb, field.name.count + 1 + field.value.count))
        return false;

    bb_append_slice(bb,  field.name.data, field.name.count);
    bb_append_byte(bb, '=');
    bb_append_slice(bb,  field.value.data, field.value.count);

    return true;
}

bool fieldobj_to_str(ByteBuffer *bb, String_View name, Field f)
{
    // clear the buffer
    bb->len = 0;

    // allocating place
    if (!bb_reserve(bb, name.count + 6 + f.name.count + f.value.count))
        return false;

    bb_append_slice(bb, name.data, name.count);
    bb_append_slice(bb, "={\"", 3);
    bb_append_slice(bb, f.name.data, f.name.count);
    bb_append_slice(bb, "\":", 2);
    bb_append_slice(bb, f.value.data, f.value.count);
    bb_append_byte(bb, '}');

    return true;
}

bool wstr_eq_ignorecase(WStr wstr0, WStr wstr1)
{
    if (wstr0.count != wstr1.count) return false;
    for (size_t i = 0; i < wstr0.count; i++) {
        if (towlower(wstr0.data[i]) != towlower(wstr1.data[i])) {
            return false;
        }
    }

    return true;
}

bool calc_answer(WStr word, String_View *result)
{
    for (size_t i = 0; i < TRIGGERS_COUNT; i++) {
        if (wstr_eq_ignorecase(word, triggers[i].on_word)) {
            *result = triggers[i].answer;
            return true;
        }
    }

    return false;
}

size_t empty_read(char *b, size_t s, size_t n, void *ud) {(void) b; (void) ud; return s*n; }
int main(void)
{
    setlocale(LC_ALL, "");
    CURLcode  curl_err;
    CURLUcode curlu_err;

    CURL *input = curl_easy_init();
    if (input == NULL) CURL_INIT_ERR();
    CURL *output = curl_easy_init();
    if (output == NULL) CURL_INIT_ERR();

    CURLU *url = curl_url();
    if (url == NULL) {
        fputs("ERROR: Creating `url builder`\n", stderr);
        return 1;
    }

    curl_easy_setopt(input, CURLOPT_URL, "https://api.telegram.org/bot"BOT_TOKEN"/getUpdates?offset=-1&allowed_updates=[\"message\"]");
    curl_easy_setopt(input, CURLOPT_WRITEFUNCTION, handle_data);
    curl_easy_setopt(output, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(output, CURLOPT_WRITEFUNCTION, empty_read);

    curlu_err = curl_url_set(url, CURLUPART_URL, "https://api.telegram.org/bot"BOT_TOKEN"/sendMessage", 0);
    if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

    curl_err = curl_easy_perform(input);
    if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);
    size_t last_msg_id = received_message.id;

    ByteBuffer reply_msg, chat_id, text = {0};
    for (;;) {
        curl_err = curl_easy_perform(input);
        if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);

        if (received_message.id <= last_msg_id) continue;
        last_msg_id = received_message.id;

        INFO(L"Received message: ["WS_FMT"]\n", WS_ARG(received_message.text));

        WStr last_word;
        if (!get_last_word(received_message.text, &last_word)) {
            INFO(L"Message doesn't contain words%s\n", "");
            continue;
        }

        INFO(L"Last word: ["WS_FMT"]\n", WS_ARG(last_word));

        String_View answer;
        if (!calc_answer(last_word, &answer)) continue;

        Field field = {
            .name = SV_STATIC("text"),
            .value = answer
        };
        if (!field_to_str(&text, field)) return 1;

        field = (Field) {
            .name = SV_STATIC("chat_id"),
            .value = received_message.chat_id
        };
        if (!field_to_str(&chat_id, field)) return 1;

        field = (Field) {
            .name = SV_STATIC("message_id"),
            .value = received_message.id_str
        };
        if (!fieldobj_to_str(&reply_msg, (String_View)SV_STATIC("reply_parameters"), field)) return 1;

        curlu_err = curl_url_set(url, CURLUPART_QUERY, text.data, CURLU_APPENDQUERY | CURLU_URLENCODE);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);
        curlu_err = curl_url_set(url, CURLUPART_QUERY, chat_id.data, CURLU_APPENDQUERY);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);
        curlu_err = curl_url_set(url, CURLUPART_QUERY, reply_msg.data, CURLU_APPENDQUERY);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        char *data;
        curlu_err = curl_url_get(url, CURLUPART_URL, &data, 0);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        INFO(L"Sending message:  ["SV_Fmt"]\n", SV_Arg(answer));

        curl_easy_setopt(output, CURLOPT_URL, data);
        curl_err = curl_easy_perform(output);
        if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);

        curlu_err = curl_url_set(url, CURLUPART_QUERY, NULL, 0);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        curl_free(data);
    }

    free(text.data);
    free(chat_id.data);
    free(reply_msg.data);
    curl_url_cleanup(url);
    curl_easy_cleanup(output);
    curl_easy_cleanup(input);
    return 0;
}
