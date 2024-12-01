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
