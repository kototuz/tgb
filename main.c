#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <assert.h>

#include <curl/curl.h>

#define SV_IMPLEMENTATION
#include "sv.h"

#ifndef BOT_TOKEN
 #error BOT_TOKEN is not specifed
#endif

#define TG_HOSTNAME "api.telegram.org"

typedef struct {
    String_View name;
    String_View value;
} Field;

typedef struct {
    size_t count;
    char *data;
} Str;

static struct {
    size_t id;
    String_View id_str;
    String_View chat_id;
    String_View text;
} received_message = {0};



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

void convert_unicode(Str *s)
{
    size_t converted = 0;
    for (size_t i = 0; i < s->count; i++) {
        if (s->data[i] != '\\' || s->data[i+1] != 'u') {
            s->data[converted] = s->data[i];
            converted += 1;
            continue;
        }

        // TODO: handling of the russian language is hardcoded
        wchar_t wc = (wchar_t) parse_hex(sv_from_parts(&s->data[i+2], 4));
        if (L'А' <= wc && wc <= L'Я') {
            wc += 32;
        }

        wctomb(&s->data[converted], wc);

        i += 5;
        converted += 2;
    }

    s->count = converted;
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
    if (*source->data == '{') {
        size_t depth = 1;
        sv_chop_left(source, 1);
        do {
            switch (*source->data) {
                case '{': depth += 1; break;
                case '}': depth -= 1; break;
            }
            sv_chop_left(source, 1);
        } while (depth != 0);
    } else {
        sv_chop_left_while(source, is_not_field_end);
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
            received_message.text = SV_NULL;
            break;
        } 
        if (sv_eq(field.name, (String_View)SV_STATIC("text"))) {
            received_message.text.data = field.value.data+1;
            received_message.text.count = field.value.count-2;
            convert_unicode((Str *)&received_message.text);
            break;
        }
    }

    return count;
}

char *field_to_str(Field field)
{
    char *buf = (char *) malloc(field.name.count + 2 + field.value.count);
    if (buf == NULL) {
        perror("ERROR: Allocating memory");
        return NULL;
    }

    memcpy(buf, field.name.data, field.name.count);
    buf[field.name.count] = '=';
    memcpy(&buf[field.name.count+1], field.value.data, field.value.count);
    buf[field.name.count+field.value.count+1] = '\0';

    return buf;
}

// TODO: field builder
char *fieldobj_to_str(String_View name, Field f)
{
    size_t bufsize = name.count + 7 + f.name.count + f.value.count;
    char *buf = (char *) malloc(bufsize);
    if (buf == NULL) {
        perror("ERROR: Allocating memory");
        return NULL;
    }

    memcpy(buf, name.data, name.count);
    buf[name.count]   = '=';
    buf[name.count+1] = '{';
    buf[name.count+2] = '\"';
    memcpy(&buf[name.count+3], f.name.data, f.name.count);

    size_t idx = name.count+3+f.name.count;
    buf[idx]   = '\"';
    buf[idx+1] = ':';
    memcpy(&buf[idx+2], f.value.data, f.value.count);
    buf[idx+2+f.value.count] = '}';
    buf[idx+3+f.value.count] = '\0';

    return buf;
}

#define ANSWERS_COUNT (sizeof(answers)/sizeof(answers[0]))
static struct {
    String_View on_text;
    String_View answer;
} answers[] = {
    {
        .on_text = SV_STATIC("да"),
        .answer  = SV_STATIC("Пизда"),
    },
    {
        .on_text = SV_STATIC("нет"),
        .answer  = SV_STATIC("Минет")
    },
    {
        .on_text = SV_STATIC("чо?"),
        .answer  = SV_STATIC("Хуй через плечо")
    },
    {
        .on_text = SV_STATIC("алло"),
        .answer  = SV_STATIC("Хуем по лбу не дало?")
    },
    {
        .on_text = SV_STATIC("я"),
        .answer  = SV_STATIC("Головка от хуя")
    },
    {
        .on_text = SV_STATIC("а?"),
        .answer  = SV_STATIC("Хуйна")
    },
    {
        .on_text = SV_STATIC("ну"),
        .answer  = SV_STATIC("Хуй гну")
    },
};

bool calc_answer(String_View s, String_View *result)
{
    for (size_t i = 0; i < ANSWERS_COUNT; i++) {
        if (sv_eq(s, answers[i].on_text)) {
            *result = answers[i].answer;
            return true;
        }
    }

    return false;
}

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
    for (;;) {
        curl_err = curl_easy_perform(input);
        if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);

        if (received_message.id <= last_msg_id) continue;
        last_msg_id = received_message.id;

        printf("Received message: ["SV_Fmt"]\n", SV_Arg(received_message.text));

        String_View answer;
        if (!calc_answer(received_message.text, &answer)) continue;

        char *new_text_field = field_to_str((Field) {
            .name = SV_STATIC("text"),
            .value = answer
        });
        if (new_text_field == NULL) return 1;

        char *new_chatid_field = field_to_str((Field) {
            .name = SV_STATIC("chat_id"),
            .value = received_message.chat_id
        });
        if (new_text_field == NULL) return 1;

        char *new_reply_message_field = fieldobj_to_str((String_View)SV_STATIC("reply_parameters"), (Field) {
            .name = SV_STATIC("message_id"),
            .value = received_message.id_str
        });

        if (new_text_field == NULL) return 1;

        curlu_err = curl_url_set(url, CURLUPART_QUERY, new_text_field, CURLU_APPENDQUERY | CURLU_URLENCODE);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);
        curlu_err = curl_url_set(url, CURLUPART_QUERY, new_chatid_field, CURLU_APPENDQUERY);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);
        curlu_err = curl_url_set(url, CURLUPART_QUERY, new_reply_message_field, CURLU_APPENDQUERY);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        char *data;
        curlu_err = curl_url_get(url, CURLUPART_URL, &data, 0);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        printf("Sending message:  ["SV_Fmt"]\n", SV_Arg(answer));

        curl_easy_setopt(output, CURLOPT_URL, data);
        curl_err = curl_easy_perform(output);
        if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);

        curlu_err = curl_url_set(url, CURLUPART_QUERY, NULL, 0);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        free(new_reply_message_field);
        free(new_chatid_field);
        free(new_text_field);
        curl_free(data);
    }

    curl_url_cleanup(url);
    curl_easy_cleanup(output);
    curl_easy_cleanup(input);
    return 0;
}
