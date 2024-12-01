#include <stdio.h>
#include <string.h>
#include <wctype.h>
#include <wchar.h>
#include <locale.h>
#include <assert.h>

#include <curl/curl.h>

#include "../common.c"

#ifndef BOT_TOKEN
 #error BOT_TOKEN is not specifed
#endif

typedef struct {
    size_t count;
    char *data;
} Str;

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
