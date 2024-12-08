#ifndef BOT_TOKEN
# error 'BOT_TOKEN' must be provided
#endif

#include <raylib-5.5/src/raylib.h>

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <assert.h>
#include <locale.h>
#include <threads.h>
#include <math.h>

#include <curl/curl.h>

#include "../common.c"

#define WIDTH  800
#define HEIGHT 600

#define FONT_PATH        "./tpilot/JetBrainsMono-Regular.ttf"
#define FONT_SIZE        48.0
#define FONT_GLYPH_COUNT 9608

// TODO: the spacing that greater then 0 is not supported. Functions don't consider it
#define SPACING          0

#define BG_COLOR (Color){0x0f,0x0f,0x0f,0xff}

#define MSG_BG_COLOR          (Color){0x21,0x21,0x21,0xff}
#define MSG_FG_COLOR          WHITE
#define MSG_REC_ROUNDNESS     10
#define MSG_REC_SEGMENT_COUNT 20
#define MSG_LEFT_BOUND_X      10
#define MSG_RIGHT_BOUND_X     (WIDTH-10)

#define TED_BG_COLOR     MSG_BG_COLOR
#define TED_FG_COLOR     WHITE
#define TED_CURSOR_COLOR WHITE

#define MSG_DISTANCE 7

typedef struct {
    size_t begin;
    size_t end;
} Line;

typedef struct {
    size_t *items;
    size_t len;
    size_t cap;
} LineEnds;

#define MAX_MSG_LEN 4096
typedef struct {
    int text[MAX_MSG_LEN];
    size_t text_len;
    LineEnds line_ends;
    size_t last_word_begin;
    Vector2 cursor_pos;
} TextEditor;

typedef struct {
    float width;
    float height;
} Size;

typedef struct {
    int *data;
    size_t len;
} AuthorName;

typedef struct {
    int *text;
    size_t len;
    AuthorName author_name;
} Message;

#define MAX_MESSAGE_COUNT 128
typedef struct {
    Font font;
    float glyph_width;
    TextEditor editor;
    Message messages[MAX_MESSAGE_COUNT];
    size_t message_count;
} Tpilot;



static cnd_t recv_msg_cnd;
static mtx_t recv_msg_mtx;
static TgMessage tg_msg = {0};



// NOTE: the function returns text height consider 'WIDTH' of the screen
Size calc_text_block_size(
        float glyph_width,
        int lb, int rb,
        const int *data,
        int length)
{
    float x = lb;
    int word_start = 0;
    Size size = { .height = FONT_SIZE };
    for (int i = 0; i < length; i++) {
        if (x + (i-word_start+1)*glyph_width > rb) {
            size.height += FONT_SIZE;
            x = lb + (i-word_start+1) * glyph_width;
            word_start = i+1;
        } else if (data[i] == '\n') {
            size.height += FONT_SIZE;
            word_start = i+1;
            x = lb + (i-word_start) * glyph_width;
        } else if (data[i] == ' ') {
            x += (i-word_start+1) * glyph_width;
            word_start = i+1;
        } else if (i+1 == length) {
            x += (i-word_start+1) * glyph_width;
        }
        if (x > size.width) size.width = x;
    }

    return size;
}


Tpilot tpilot_new()
{
    Tpilot tpilot = {0};

    tpilot.font = LoadFontEx(
            FONT_PATH, FONT_SIZE,
            0, FONT_GLYPH_COUNT);

    // glyph_width mustn't change because we use monospaced font
    tpilot.glyph_width =
        FONT_SIZE / tpilot.font.baseSize * tpilot.font.glyphs[0].advanceX;

    // TODO: maybe 'cursor_pos' is redundant
    tpilot.editor.cursor_pos.y = HEIGHT - FONT_SIZE;

    return tpilot;
}

// NOTE: the function renders text 'down to up'. Returns the text end position
Vector2 tpilot_draw_text(Tpilot *self, Vector2 pos, const int *codepoints, int codepoint_count, Color color)
{
    int word_start = 0;
    float start_x = pos.x;
    for (int i = 0; i < codepoint_count; i++) {
        if (pos.x + (i-word_start+1)*self->glyph_width > WIDTH) {
            pos.x = start_x;
            pos.y += FONT_SIZE;
            for (; word_start <= i; word_start++) {
                DrawTextCodepoint(self->font, codepoints[word_start], pos, FONT_SIZE, color);
                pos.x += self->glyph_width + SPACING;
            }
        } else if (codepoints[i] == '\n') {
            for (;word_start < i; word_start++) {
                DrawTextCodepoint(self->font, codepoints[word_start], pos, FONT_SIZE, color);
                pos.x += self->glyph_width + SPACING;
            }
            pos.x = start_x;
            pos.y += FONT_SIZE;
            word_start += 1;
        } else if (codepoints[i] == ' ') {
            for (;word_start <= i; word_start++) {
                DrawTextCodepoint(self->font, codepoints[word_start], pos, FONT_SIZE, color);
                pos.x += self->glyph_width + SPACING;
            }
        } else if (i+1 == codepoint_count) {
            for (;word_start < codepoint_count; word_start++) {
                DrawTextCodepoint(self->font, codepoints[word_start], pos, FONT_SIZE, color);
                pos.x += self->glyph_width + SPACING;
            }
        }
    }

    return pos;
}

void tpilot_push_message(
        Tpilot *self,
        const int *msg, size_t msg_len,
        AuthorName author_name)
{
    assert(msg_len > 0);
    assert(self->message_count < MAX_MSG_LEN);

    size_t message_size = msg_len * sizeof(int);
    int *msg_text = MemAlloc(message_size);
    memcpy(msg_text, msg, message_size);

    printf("Push message #%zu: %ls\n", self->message_count, msg_text);

    self->messages[self->message_count++] = (Message) {
        .text = msg_text,
        .len = msg_len,
        .author_name = author_name
    };
}

bool grow_line_end(LineEnds *line_ends)
{
    if (line_ends->len < line_ends->cap) {
        line_ends->len += 1;
        return true;
    }

    size_t *new_buf = (size_t *) malloc((line_ends->cap+1) * sizeof(size_t));
    if (new_buf == NULL) {
        fprintf(stderr, "ERROR: Could not grow one line end: no memory\n");
        return false;
    }

    memcpy(new_buf, line_ends->items, line_ends->len * sizeof(size_t));
    new_buf[line_ends->len] = 0;

    line_ends->cap += 1;
    line_ends->len += 1;
    free(line_ends->items);
    line_ends->items = new_buf;

    return true;
}

bool recalc_line_ends(
        int *text, size_t text_len,
        LineEnds *line_ends,
        size_t max_line)
{
    // clear all lines
    line_ends->len = 0;
    memset(line_ends->items, 0x0, line_ends->cap*sizeof(size_t));
    if (!grow_line_end(line_ends)) return false;

    size_t last_word_begin = 0;
    size_t curr_line_len = 0;
    for (size_t i = 0; i < text_len; i++) {
        if (curr_line_len+1 > max_line) {
            if (text[i] == ' ') {
                line_ends->items[line_ends->len-1] = i+1;
                if (!grow_line_end(line_ends)) return false;
                line_ends->items[line_ends->len-1] = i+1;
                curr_line_len = 0;
            } else if (i - last_word_begin >= max_line) {
                line_ends->items[line_ends->len-1] = i;
                if (!grow_line_end(line_ends)) return false;
                line_ends->items[line_ends->len-1] = i+1;
                curr_line_len = 1;
            } else {
                line_ends->items[line_ends->len-1] = last_word_begin;
                if (!grow_line_end(line_ends)) return false;
                curr_line_len = i - last_word_begin + 1;
                line_ends->items[line_ends->len-1] = i+1;
            }
            continue;
        }

        if (text[i] == ' ') last_word_begin = i+1;

        line_ends->items[line_ends->len-1] += 1;
        curr_line_len += 1;
    }

    return true;
}

void tpilot_render(Tpilot self)
{
    ClearBackground(BG_COLOR);

    Size widget_size;
    Vector2 widget_pos;
    { // render text editor
        widget_pos = (Vector2){ .y = HEIGHT - self.editor.line_ends.len*FONT_SIZE };
        DrawRectangle(0, widget_pos.y, WIDTH, HEIGHT, TED_BG_COLOR);

        size_t text_i = 0;
        Vector2 symbol_pos = widget_pos;
        for (size_t i = 0; i < self.editor.line_ends.len; i++) {
            for (; text_i < self.editor.line_ends.items[i]; text_i++) {
                DrawTextCodepoint(
                        self.font,
                        self.editor.text[text_i],
                        symbol_pos, FONT_SIZE, TED_FG_COLOR);
                symbol_pos.x += self.glyph_width + SPACING;
            }
            symbol_pos.y += FONT_SIZE;
            symbol_pos.x = 0;
        }
    }

    { // render messages
        widget_pos.x = MSG_LEFT_BOUND_X;
        for (int i = self.message_count-1; i >= 0; i--) {
            widget_size = calc_text_block_size(
                    self.glyph_width,
                    MSG_LEFT_BOUND_X, MSG_RIGHT_BOUND_X,
                    self.messages[i].text,
                    self.messages[i].len);

            widget_size.height += FONT_SIZE;
            widget_pos.y -= MSG_DISTANCE + widget_size.height;

            // draw message background
            DrawRectangleRounded(
                    (Rectangle){widget_pos.x, widget_pos.y, widget_size.width, widget_size.height},
                    MSG_REC_ROUNDNESS/widget_size.height, MSG_REC_SEGMENT_COUNT,
                    MSG_BG_COLOR);

            // draw username
            DrawTextCodepoints(
                    self.font,
                    self.messages[i].author_name.data,
                    self.messages[i].author_name.len,
                    widget_pos, FONT_SIZE, SPACING, RED);

            // draw message
            tpilot_draw_text(&self,
                    (Vector2){ widget_pos.x, widget_pos.y+FONT_SIZE },
                    self.messages[i].text,
                    self.messages[i].len,
                    MSG_FG_COLOR);
        }
    }
}

size_t empty_read(char *b, size_t s, size_t n, void *ud) {(void) b; (void) ud; return s*n; }
int gui_thread(char *chat_id)
{
    CURLcode  curl_err;
    CURLUcode curlu_err;

    // this message is necessary without it programm enter the 'ascii print system'
    // In short you cannot use print functions from <stdio> and <wchar.h> simultaneously
    wprintf(L"Hello, from tpilot\n");

    CURLU *url;
    CURL *curl_sender;
    {
        curl_sender = curl_easy_init();
        if (curl_sender == NULL) CURL_INIT_ERR();

        url = curl_url();
        if (url == NULL) {
            fputs("ERROR: Creating `url builder`\n", stderr);
            exit(1);
        }

        curlu_err = curl_url_set(
                url, CURLUPART_URL,
                "https://api.telegram.org/bot"BOT_TOKEN"/sendMessage", 0);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        curl_easy_setopt(curl_sender, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(curl_sender, CURLOPT_WRITEFUNCTION, empty_read);
    }

    InitWindow(WIDTH, HEIGHT, "Tpilot");

    ByteBuffer text_buffer = {0};
    ByteBuffer chat_id_buffer = {0};
    Tpilot tpilot = tpilot_new();
    size_t max_editor_line_len = floor(WIDTH/tpilot.glyph_width);

    while (!WindowShouldClose()) {
        int key = GetKeyPressed();
        switch (key) {
            case KEY_ENTER:
                // render message on client
                tpilot_push_message(
                        &tpilot,
                        tpilot.editor.text,
                        tpilot.editor.text_len,
                        (AuthorName){ L"You", 3 });

                // create query field 'text' with text form the editor
                char *editor_text_as_utf8 = LoadUTF8(tpilot.editor.text, tpilot.editor.text_len);
                if (!field_to_str(&text_buffer, (Field) {
                            .name = SV_STATIC("text"),
                            .value = sv_from_cstr(editor_text_as_utf8)})) exit(1);
                UnloadUTF8(editor_text_as_utf8);


                // create query field 'chat_id' with the argument from command line
                if (!field_to_str(&chat_id_buffer, (Field){
                            .name = SV_STATIC("chat_id"),
                            .value = sv_from_cstr(chat_id)})) exit(1);

                // append field 'chat_id' to url
                curlu_err = curl_url_set(url, CURLUPART_QUERY, chat_id_buffer.data, CURLU_APPENDQUERY);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                // append field 'text' to url
                curlu_err = curl_url_set(url, CURLUPART_QUERY, text_buffer.data, CURLU_APPENDQUERY | CURLU_URLENCODE);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                char *data;
                curlu_err = curl_url_get(url, CURLUPART_URL, &data, 0);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                curl_easy_setopt(curl_sender, CURLOPT_URL, data);
                curl_err = curl_easy_perform(curl_sender);
                if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);
                curl_free(data);

                // clear the editor buffer
                tpilot.editor.text_len = 0;
                tpilot.editor.line_ends.len = 0;

                // clear 'url' buffer
                curlu_err = curl_url_set(url, CURLUPART_QUERY, NULL, 0);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                break;

            case KEY_BACKSPACE:
                if (tpilot.editor.text_len == 0) break;
                tpilot.editor.text_len -= 1;
                if (!recalc_line_ends(
                            tpilot.editor.text,
                            tpilot.editor.text_len,
                            &tpilot.editor.line_ends,
                            max_editor_line_len)) return 1;
                break;

            default:
                key = GetCharPressed();
                if (key != 0) {
                    tpilot.editor.text[tpilot.editor.text_len++] = key;
                    if (!recalc_line_ends(
                                tpilot.editor.text,
                                tpilot.editor.text_len,
                                &tpilot.editor.line_ends,
                                max_editor_line_len)) return 1;

                }
                break;
        }

        // handle input message if it appears
        if (mtx_trylock(&recv_msg_mtx) == thrd_success) {
            tpilot_push_message(
                    &tpilot,
                    tg_msg.text.data,
                    tg_msg.text.count,
                    (AuthorName){ L"Anon", 4 });
            if (cnd_signal(&recv_msg_cnd) != thrd_success) {
                fprintf(stderr, "ERROR: Could not signal to a condition\n");
                exit(1);
            }
            if (mtx_unlock(&recv_msg_mtx) != thrd_success) {
                fprintf(stderr, "ERROR: Could not unlock a mutex\n");
                exit(1);
            }
        }

        BeginDrawing();
            tpilot_render(tpilot);
        EndDrawing();
    }
    CloseWindow();

    // cleanup all related to 'curl'
    free(text_buffer.data);
    curl_url_cleanup(url);
    curl_easy_cleanup(curl_sender);

    exit(0);
}

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    if (argc != 2) {
        fprintf(stderr, "usage: %s <chat_id>\n", argv[0]);
        return 1;
    }

    // create 'curl'
    String_View resp_text = {0};
    CURL *curl = curl_easy_init();
    if (curl == NULL) CURL_INIT_ERR();
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.telegram.org/bot"BOT_TOKEN"/getUpdates?offset=-1&allowed_updates=[\"message\"]");
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *) &resp_text);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);

    ByteBuffer bb = {0};

    // get last message id
    CURLcode err = curl_easy_perform(curl);
    if (err != CURLE_OK) CURL_PERFORM_ERR(err);
    if (!parse_tg_response(&bb, resp_text, &tg_msg)) return 1;
    size_t last_msg_id = tg_msg.id;

    // create a new mutex and lock it
    if (mtx_init(&recv_msg_mtx, mtx_plain) != thrd_success) {
        fprintf(stderr, "ERROR: Could not create a mutex\n");
        return 1;
    }
    if (mtx_lock(&recv_msg_mtx) != thrd_success) {
        fprintf(stderr, "ERROR: Could not lock the new mutex\n");
        return 1;
    }

    // create a new condition
    if (cnd_init(&recv_msg_cnd) != thrd_success) {
        fprintf(stderr, "ERROR: Could not create a new condition\n");
        return 1;
    }

    // create and run a new thread
    thrd_t thread;
    if (thrd_create(&thread, (int (*)(void*))gui_thread, argv[1]) != thrd_success) {
        fprintf(stderr, "ERROR: Could not create a new thread\n");
        return 1;
    }

    for (;;) {
        resp_text = (String_View){0};
        err = curl_easy_perform(curl);
        if (err != CURLE_OK) CURL_PERFORM_ERR(err);
        if (!parse_tg_response(&bb, resp_text, &tg_msg)) exit(1);
        if (tg_msg.id > last_msg_id) {
            last_msg_id = tg_msg.id;

            wprintf(L"=============================\n");
            wprintf(L"Message id: "SV_Fmt"\n", SV_Arg(tg_msg.id_str));
            wprintf(L"Chat id:    "SV_Fmt"\n", SV_Arg(tg_msg.chat_id_str));
            wprintf(L"Text:       "WS_FMT"\n", WS_ARG(tg_msg.text));
            wprintf(L"=============================\n");

            if (cnd_wait(&recv_msg_cnd, &recv_msg_mtx) != thrd_success) {
                fprintf(stderr, "ERROR: Could not wait for the condition\n");
                exit(1);
            }
        }
    }
}

// TODO: refactor 'tpilot_draw_text()'
// TODO: message widget padding
// TODO: creating query parameters from fields
