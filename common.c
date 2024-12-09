#define SV_IMPLEMENTATION
#include "../external/sv.h"

#include <stdio.h>
#include <assert.h>
#include <wchar.h>

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
    wchar_t *data;
} WStr;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} ByteBuffer;

typedef struct {
    char *response;
    size_t size;
} Memory;

typedef struct {
    size_t id;
    String_View id_str;

    size_t chat_id;
    String_View chat_id_str;

    ByteBuffer text_buffer;
    WStr text;
    bool has_text;

    ByteBuffer username_buffer;
    WStr username;
    bool has_username;
} TgMessage;



static struct {
    size_t id;
    String_View id_str;
    String_View chat_id;
    ByteBuffer buffer;
    WStr text;
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

bool asciiutf8_to_wstr(ByteBuffer *bb, String_View str, WStr *result)
{
    if (!bb_reserve(bb, str.count*sizeof(wchar_t))) return false;

    result->count = 0;
    result->data = (wchar_t *) bb->data;
    for (size_t i = 0; i < str.count; i++, result->count++) {
        if (str.data[i] != '\\' || str.data[i+1] != 'u') {
            result->data[result->count] = str.data[i];
        } else {
            wchar_t wc = (wchar_t) parse_hex(sv_from_parts(&str.data[i+2], 4));
            result->data[result->count] = wc;
            i += 5;
        }
    }

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

    size_t depth = 1;
    result->value = *source;
    switch (*source->data) {
    case '{':;
        sv_chop_left(source, 1);
        do {
            switch (*source->data) {
                case '{': depth += 1; break;
                case '}': depth -= 1; break;
            }
            sv_chop_left(source, 1);
        } while (depth != 0);
        break;

    case '[':;
        sv_chop_left(source, 1);
        do {
            switch (*source->data) {
                case '[': depth += 1; break;
                case ']': depth -= 1; break;
            }
            sv_chop_left(source, 1);
        } while (depth != 0);
        break;

    case '"':
        sv_chop_left(source, 1);
        while (*source->data != '\"' || source->data[-1] == '\\') {
            sv_chop_left(source, 1);
        }
        result->value.data += 1;
        result->value.count -= 1;
        break;

    default:
        sv_chop_left_while(source, is_not_field_end);
        break;
    }

    result->value.count -= source->count;

    return true;
}

bool skip_fields_until(
        String_View *src,
        String_View field_name,
        Field *result)
{
    for (;;) {
        if (!next_field(src, result)) return false;
        if (sv_eq(result->name, field_name)) return true;
    }
}

size_t write_cb(char *data, size_t size, size_t nmemb, void *clientp)
{
    size_t realsize = size * nmemb;
    Memory *mem = (Memory *) clientp;
 
    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(!ptr) return 0;  /* out of memory */
 
    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), data, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;
 
    return realsize;
}

#define FIELD_FMT    "["SV_Fmt":"SV_Fmt"]"
#define FIELD_ARG(f) SV_Arg(f.name), SV_Arg(f.value)
bool parse_tg_response(String_View src, TgMessage *result)
{
    String_View src2;
    Field field = {0};

    // clear result
    result->text_buffer.len = 0;
    result->username_buffer.len = 0;
    result->has_text = false;
    result->has_username = false;

    // check if the response is ok
    assert(next_field(&src, &field));
    assert(sv_eq(field.name, SV("ok")));
    if (!sv_eq(field.value, SV("true"))) {
        fprintf(stderr, "ERROR: TgMessage is not ok\n");
        return false;
    }

    // get result of the response
    assert(next_field(&src, &field));
    assert(sv_eq(field.name, SV("result")));

    src = field.value;
    *result = (TgMessage){0};

    // check if the result is not empty and skip 'update_id'
    if (!next_field(&src, &field)) return true;

    // check if the next field is 'message'
    if (!next_field(&src, &field) || !sv_eq(field.name, SV("message")))
        return true;


    src = field.value;

    // get 'id'
    assert(next_field(&src, &field));
    result->id_str = field.value;
    result->id = parse_int(field.value);

    // get username if it exists
    for (;;) {
        assert(next_field(&src, &field));
        if (sv_eq(field.name, SV("from"))) {
            src2 = field.value;

            // skip that we don't need
            assert(next_field(&src2, &field));
            assert(next_field(&src2, &field));

            assert(next_field(&src2, &field));
            result->has_username = true;
            if (!asciiutf8_to_wstr(
                        &result->username_buffer,
                        field.value,
                        &result->username)) return false;

            assert(skip_fields_until(&src, SV("chat"), &field));
            break;
        } else if (sv_eq(field.name, SV("chat"))) break;
    }

    // get 'chat_id'
    src2 = field.value;
    assert(next_field(&src2, &field));
    result->chat_id_str = field.value;
    result->chat_id = parse_int(field.value);

    // get 'text'
    if (!skip_fields_until(&src, SV("text"), &field)) return true;
    if (!asciiutf8_to_wstr(
                &result->text_buffer,
                field.value,
                &result->text)) return false;
    result->has_text = true;

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
            if (!asciiutf8_to_wstr(
                        &received_message.buffer,
                        field.value,
                        &received_message.text)) return 1;
            break;
        }
    }

    return count;
}

bool url_append_field(
        CURLU *url,
        ByteBuffer *bb,
        String_View field_name,
        String_View field_value)
{
    // clear the buffer
    bb->len = 0;

    // allocate needed memory
    if (!bb_reserve(bb, field_name.count + 1 + field_value.count))
        return false;

    bb_append_slice(bb, field_name.data, field_name.count);
    bb_append_byte(bb, '=');
    bb_append_slice(bb, field_value.data, field_value.count);

    CURLUcode curlu_err = curl_url_set(url, CURLUPART_QUERY, bb->data, CURLU_APPENDQUERY | CURLU_URLENCODE);
    if (curlu_err != CURLUE_OK) {
        fprintf(stderr, "ERROR: Could not append field: %s\n", curl_url_strerror(curlu_err));
        return false;
    }

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
