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

#define CHAT_BG_COLOR (Color){0x0f,0x0f,0x0f,0xff}

#define MSG_BG_COLOR          (Color){0x21,0x21,0x21,0xff}
#define MSG_FG_COLOR          WHITE
#define MSG_REC_ROUNDNESS     20
#define MSG_REC_SEGMENT_COUNT 20
#define MSG_TEXT_MARGIN       10
#define MSG_TEXT_PADDING      15
#define MSG_AUTHOR_NAME_COLOR GREEN

#define TED_BG_COLOR     MSG_BG_COLOR
#define TED_FG_COLOR     WHITE
#define TED_CURSOR_CODE  L'▏'
#define TED_CURSOR_COLOR WHITE

#define EMACS_KEYMAP

// mappings
#define KEY(k) (IsKeyPressed(KEY_ ## k) || IsKeyPressedRepeat(KEY_ ## k))
#ifdef EMACS_KEYMAP
#   define KEYMAP_MOVE_FORWARD       (IsKeyDown(KEY_LEFT_CONTROL) && KEY(F))
#   define KEYMAP_MOVE_BACKWARD      (IsKeyDown(KEY_LEFT_CONTROL) && KEY(B))
#   define KEYMAP_MOVE_FORWARD_WORD  (IsKeyDown(KEY_LEFT_ALT)     && KEY(F))
#   define KEYMAP_MOVE_BACKWARD_WORD (IsKeyDown(KEY_LEFT_ALT)     && KEY(B))
#   define KEYMAP_DELETE             (IsKeyDown(KEY_LEFT_CONTROL) && KEY(H))
#   define KEYMAP_MOVE_UP            (IsKeyDown(KEY_LEFT_CONTROL) && KEY(P))
#   define KEYMAP_MOVE_DOWN          (IsKeyDown(KEY_LEFT_CONTROL) && KEY(N))
#else
#   define KEYMAP_MOVE_FORWARD       (KEY(RIGHT))
#   define KEYMAP_MOVE_BACKWARD      (KEY(LEFT))
#   define KEYMAP_MOVE_FORWARD_WORD  (IsKeyDown(KEY_LEFT_CONTROL) && KEY(LEFT))
#   define KEYMAP_MOVE_BACKWARD_WORD (IsKeyDown(KEY_LEFT_CONTROL) && KEY(RIGHT))
#   define KEYMAP_DELETE             (KEY(BACKSPACE))
#   define KEYMAP_MOVE_UP            (KEY(UP))
#   define KEYMAP_MOVE_DOWN          (KEY(DOWN))
#endif

typedef enum {
    MOTION_FORWARD_WORD,
    MOTION_BACKWARD_WORD,
    MOTION_FORWARD,
    MOTION_BACKWARD,
    MOTION_UP,
    MOTION_DOWN,
} Motion;

typedef struct {
    int *text;
    size_t len;
    size_t trim_whitespace_count;
} Line;

typedef struct {
    int *text;
    size_t text_len;
    Line *items;
    size_t len;
    size_t cap;
} Lines;

typedef struct {
    size_t col;
    size_t row;
} TextEditorPos;

#define MAX_MSG_LEN 4096
typedef struct {
    Lines lines;
    TextEditorPos cursor_pos;
    size_t max_line_len;
} TextEditor;

typedef struct {
    float width;
    float height;
} Size;

typedef struct {
    Lines lines;
    WStr author_name;
} Message;

#define MAX_MESSAGE_COUNT 128
typedef struct {
    Font font;
    float glyph_width;
    size_t max_msg_line_len;
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

bool grow_line(Lines *lines)
{
    if (lines->len < lines->cap) {
        lines->len += 1;
        return true;
    }

    Line *new_buf = (Line *) malloc((lines->cap+1) * sizeof(Line));
    if (new_buf == NULL) {
        fprintf(stderr, "ERROR: Could not grow one line: no memory\n");
        return false;
    }


    memcpy(new_buf, lines->items, lines->len * sizeof(Line));
    new_buf[lines->len] = (Line){0};

    lines->cap += 1;
    lines->len += 1;
    if (lines->items != NULL) {
        free(lines->items);
    }
    lines->items = new_buf;

    return true;
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

    tpilot.max_msg_line_len =
        floor((WIDTH-2*MSG_TEXT_PADDING-2*MSG_TEXT_MARGIN) / tpilot.glyph_width);

    INFO(L"Max msg line length: %zu\n", tpilot.max_msg_line_len);

    static int ted_buffer[4096];
    tpilot.editor.lines = (Lines){ .text = ted_buffer };
    grow_line(&tpilot.editor.lines);
    tpilot.editor.lines.items[0].text = ted_buffer;
    tpilot.editor.lines.items[0].len = 0;

    tpilot.editor.max_line_len = floor(WIDTH/tpilot.glyph_width); 

    return tpilot;
}

void tpilot_draw_text(
        Tpilot *self,
        Vector2 pos,
        Lines lines,
        Color color)
{
    size_t begin_x = pos.x;
    for (size_t i = 0; i < lines.len; i++) {
        DrawTextCodepoints(
                self->font,
                lines.items[i].text,
                lines.items[i].len,
                pos, FONT_SIZE, SPACING, color);
        pos.y += FONT_SIZE;
        pos.x = begin_x;
    }
}

bool recalc_lines(Lines *lines, size_t max_line)
{
    // clear all lines
    lines->len = 0;
    memset(lines->items, 0x0, lines->cap*sizeof(Line));
    if (!grow_line(lines)) return false;
    lines->items[0].text = lines->text;

    size_t last_word_begin = 0;
    Line *curr_line = &lines->items[0];
    for (size_t i = 0; i < lines->text_len; i++) {
        if (curr_line->len+1 > max_line) {
            if (lines->text[i] == ' ') {
                do {
                    curr_line->trim_whitespace_count += 1;
                    if (++i >= lines->text_len) return true;
                } while (lines->text[i] == ' ');
                if (!grow_line(lines)) return false;
                curr_line = &lines->items[lines->len-1];
                curr_line->len = 1;
                curr_line->text = &lines->text[i];
            } else if (i - last_word_begin >= max_line) {
                if (!grow_line(lines)) return false;
                curr_line = &lines->items[lines->len-1];
                curr_line->len = 1;
                curr_line->text = &lines->text[i];
            } else {
                curr_line->len -= i - last_word_begin + 1;
                if (!grow_line(lines)) return false;
                curr_line = &lines->items[lines->len-1];
                curr_line->text = &lines->text[last_word_begin];
                curr_line->len = i - last_word_begin + 1;
            }
        } else {
            curr_line->len += 1;
            if (lines->text[i] == ' ') last_word_begin = i+1;
        }
    }


    return true;
}

bool tpilot_push_message(
        Tpilot *self,
        const int *msg,
        size_t msg_len,
        WStr author_name)
{
    assert(msg_len > 0);
    assert(self->message_count < MAX_MSG_LEN);

    size_t message_size = msg_len * sizeof(int);
    Lines lines = { .text = MemAlloc(message_size), .text_len = msg_len };
    memcpy(lines.text, msg, message_size);
    if (!recalc_lines(&lines, self->max_msg_line_len)) return false;

    self->messages[self->message_count++] = (Message) {
        .lines = lines,
        .author_name = author_name
    };

    return true;
}

size_t calc_max_line(Lines lines)
{
    size_t result = 0;
    for (size_t i = 0; i < lines.len; i++) {
        if (lines.items[i].len > result)
            result = lines.items[i].len;
    }

    return result;
}

void tpilot_render(Tpilot self)
{
    ClearBackground(CHAT_BG_COLOR);

    Size widget_size;
    Vector2 widget_pos;
    { // render text editor
        widget_pos = (Vector2){ .y = HEIGHT - self.editor.lines.len*FONT_SIZE };
        DrawRectangle(0, widget_pos.y, WIDTH, HEIGHT, TED_BG_COLOR);
        tpilot_draw_text(
                &self,
                widget_pos,
                self.editor.lines,
                TED_FG_COLOR);

        // render cursor
        Vector2 cursor_pos = {
            self.editor.cursor_pos.col*self.glyph_width+1,
            widget_pos.y + self.editor.cursor_pos.row*FONT_SIZE
        };
        DrawTextCodepoint(
                self.font, TED_CURSOR_CODE,
                cursor_pos, FONT_SIZE,
                TED_CURSOR_COLOR);
    }

    { // render messages
        widget_pos.x = MSG_TEXT_MARGIN;
        for (int i = self.message_count-1; i >= 0; i--) {
            widget_size.height = self.messages[i].lines.len*FONT_SIZE;
            widget_size.height += FONT_SIZE; // reserve place for 'username'
            widget_size.height += 2*MSG_TEXT_PADDING;

            // calculate width
            widget_size.width = calc_max_line(self.messages[i].lines)*self.glyph_width;
            size_t author_name_width = self.messages[i].author_name.count*self.glyph_width;
            if (widget_size.width < author_name_width)
                widget_size.width = author_name_width;
            widget_size.width += 2*MSG_TEXT_PADDING;

            widget_pos.y -= MSG_TEXT_MARGIN + widget_size.height;

            // draw message background
            DrawRectangleRounded(
                    (Rectangle){widget_pos.x, widget_pos.y, widget_size.width, widget_size.height},
                    MSG_REC_ROUNDNESS/widget_size.height, MSG_REC_SEGMENT_COUNT,
                    MSG_BG_COLOR);

            // draw username
            DrawTextCodepoints(
                    self.font,
                    self.messages[i].author_name.data,
                    self.messages[i].author_name.count,
                    (Vector2){ widget_pos.x+MSG_TEXT_PADDING, widget_pos.y+MSG_TEXT_PADDING},
                    FONT_SIZE, SPACING, MSG_AUTHOR_NAME_COLOR);

            // draw message
            tpilot_draw_text(
                    &self,
                    (Vector2){ widget_pos.x+MSG_TEXT_PADDING, widget_pos.y+MSG_TEXT_PADDING+FONT_SIZE },
                    self.messages[i].lines,
                    MSG_FG_COLOR);
        }
    }
}

bool ted_update(TextEditor *te)
{
    return recalc_lines(&te->lines, te->max_line_len);
}

void ted_move_cursor_to_ptr(TextEditor *te, int *ptr)
{
    TextEditorPos pos = { te->cursor_pos.row > 0 ? te->cursor_pos.row-1 : 0, 0 };
    Line line = te->lines.items[pos.row];
    size_t real_line_len = line.len + line.trim_whitespace_count;
    while (&line.text[pos.col] != ptr) {
        if (pos.col >= real_line_len) {
            if (++pos.row >= te->lines.len) return;
            line = te->lines.items[pos.row];
            real_line_len = line.len + line.trim_whitespace_count;
            pos.col = 0;
        } else {
            pos.col += 1;
        }
    }

    te->cursor_pos = pos;
}

void ted_try_move_cursor(TextEditor *te, Motion dir)
{
    TextEditorPos p = te->cursor_pos;
    int *curr_text_ptr, *ptr;
    switch (dir) {
        case MOTION_BACKWARD:
            if (p.col == 0) {
                if (p.row > 0) {
                    p.row -= 1;
                    p.col = te->lines.items[p.row].len;
                }
            } else {
                p.col -= 1;
            }
            break;

        case MOTION_FORWARD:
            if (p.col == te->lines.items[p.row].len) {
                if (p.row+1 < te->lines.len) {
                    p.row += 1;
                    p.col = 0;
                }
            } else {
                p.col += 1;
            }
            break;

        case MOTION_FORWARD_WORD:;
            curr_text_ptr = &te->lines.items[p.row].text[p.col];
            ptr = &te->lines.text[te->lines.text_len-1]; // set to the end text ptr
            while (curr_text_ptr != ptr && *curr_text_ptr == ' ') curr_text_ptr++;  // move to the word begin
            while (curr_text_ptr != ptr && *curr_text_ptr != ' ') curr_text_ptr++;  // move to the word end
            ted_move_cursor_to_ptr(te, curr_text_ptr+1);
            return;

        case MOTION_BACKWARD_WORD:;
            curr_text_ptr = &te->lines.items[p.row].text[p.col];
            ptr = te->lines.text-1; // set to the begin text ptr
            if (curr_text_ptr[-1] == ' ') curr_text_ptr--;
            while (curr_text_ptr != ptr && *curr_text_ptr == ' ') curr_text_ptr--; // move to the word end
            while (curr_text_ptr != ptr && *curr_text_ptr != ' ') curr_text_ptr--; // move to the word begin
            ted_move_cursor_to_ptr(te, curr_text_ptr+1);
            return;

        case MOTION_UP:
            if (p.row > 0) {
                p.row -= 1;
                if (p.col >= te->lines.items[p.row].len) {
                    p.col = te->lines.items[p.row].len;
                }
            }
            break;

        case MOTION_DOWN:
            if (p.row+1 < te->lines.len) {
                p.row += 1;
                if (p.col >= te->lines.items[p.row].len) {
                    p.col = te->lines.items[p.row].len;
                }
            }
            break;

        default: assert(0 && "not yet implemented");
    }

    te->cursor_pos = p;
}

bool ted_insert_symbol(TextEditor *te, int symbol)
{
    if (te->lines.text_len+1 >= MAX_MSG_LEN) return true;

    // move text after cursor
    int *text_curr_ptr = &te->lines.items[te->cursor_pos.row].text[te->cursor_pos.col];
    int *text_end_ptr = &te->lines.text[te->lines.text_len];
    size_t size = (text_end_ptr - text_curr_ptr) * sizeof(int);
    memmove(text_curr_ptr+1, text_curr_ptr, size);

    *text_curr_ptr = symbol;
    te->lines.text_len += 1;

    if (!recalc_lines(&te->lines, te->max_line_len)) return false;
    ted_move_cursor_to_ptr(te, text_curr_ptr+1);

    return true;
}

bool ted_delete_symbol(TextEditor *te)
{
    // move text after cursor
    int *text_curr_ptr = &te->lines.items[te->cursor_pos.row].text[te->cursor_pos.col];
    int *text_end_ptr = &te->lines.text[te->lines.text_len];
    size_t size = (text_end_ptr - text_curr_ptr) * sizeof(int);
    memmove(text_curr_ptr-1, text_curr_ptr, size);

    te->lines.text_len -= 1;

    if (!recalc_lines(&te->lines, te->max_line_len)) return false;
    ted_move_cursor_to_ptr(te, text_curr_ptr-1);

    return true;
}

size_t empty_read(char *b, size_t s, size_t n, void *ud) {(void) b; (void) ud; return s*n; }
int gui_thread(char *chat_id)
{
    CURLcode  curl_err;
    CURLUcode curlu_err;

    // this message is necessary without it programm enter the 'ascii print system'
    // In short you cannot use print functions from <stdio> and <wchar.h> simultaneously
    SetTraceLogLevel(LOG_ERROR);

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

    ByteBuffer buffer = {0};
    Tpilot tpilot = tpilot_new();

    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        if (KEYMAP_MOVE_FORWARD) {
            ted_try_move_cursor(&tpilot.editor, MOTION_FORWARD);
        } else if (KEYMAP_MOVE_BACKWARD) {
            ted_try_move_cursor(&tpilot.editor, MOTION_BACKWARD);
        } else if (KEYMAP_MOVE_FORWARD_WORD) {
            ted_try_move_cursor(&tpilot.editor, MOTION_FORWARD_WORD);
        } else if (KEYMAP_MOVE_BACKWARD_WORD) {
            ted_try_move_cursor(&tpilot.editor, MOTION_BACKWARD_WORD);
        } else if (KEYMAP_MOVE_UP) {
            ted_try_move_cursor(&tpilot.editor, MOTION_UP);
        } else if (KEYMAP_MOVE_DOWN) {
            ted_try_move_cursor(&tpilot.editor, MOTION_DOWN);
        } else if (tpilot.editor.lines.text_len > 0 && KEYMAP_DELETE) {
            ted_delete_symbol(&tpilot.editor);
        } else if (IsKeyReleased(KEY_ENTER) && tpilot.editor.lines.text_len > 0) {
            // render message on client
            tpilot_push_message(
                    &tpilot,
                    tpilot.editor.lines.text,
                    tpilot.editor.lines.text_len,
                    (WStr)WS_LIT(L"You"));

            if (!url_append_field(
                        url, &buffer,
                        SV("chat_id"),
                        sv_from_cstr(chat_id))) return 1;

            char *editor_text_utf8 =
                LoadUTF8(tpilot.editor.lines.text, tpilot.editor.lines.text_len);
            if (!url_append_field(
                        url, &buffer,
                        SV("text"),
                        sv_from_cstr(editor_text_utf8))) return 1;
            UnloadUTF8(editor_text_utf8);

            // get result
            char *data;
            curlu_err = curl_url_get(url, CURLUPART_URL, &data, 0);
            if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

            // send message
            curl_easy_setopt(curl_sender, CURLOPT_URL, data);
            curl_err = curl_easy_perform(curl_sender);
            if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);

            // clear the editor buffer
            tpilot.editor.lines.text_len = 0;
            tpilot.editor.lines.len = 0;
            tpilot.editor.cursor_pos = (TextEditorPos){0};

            // clear 'url' buffer
            curlu_err = curl_url_set(url, CURLUPART_QUERY, NULL, 0);
            if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);
            curl_free(data);
        } else { // insert char
            int symbol = GetCharPressed();
            if (symbol != 0) {
                ted_insert_symbol(&tpilot.editor, symbol);
            }
        }

        // handle input message if it appears
        if (mtx_trylock(&recv_msg_mtx) == thrd_success) {
            WStr username = tg_msg.has_username ? tg_msg.username : (WStr)WS_LIT(L"Not a user");
            WStr text = tg_msg.has_text ? tg_msg.text : (WStr)WS_LIT(L"[Not a text]");

            tpilot_push_message(&tpilot, text.data, text.count, username);

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
    free(buffer.data);
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

    // get last message id
    CURLcode err = curl_easy_perform(curl);
    if (err != CURLE_OK) CURL_PERFORM_ERR(err);
    if (!parse_tg_response(resp_text, &tg_msg)) return 1;
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
        if (!parse_tg_response(resp_text, &tg_msg)) exit(1);
        if (tg_msg.id > last_msg_id) {
            last_msg_id = tg_msg.id;

            wprintf(L"=============================\n");
            wprintf(L"Message id: "SV_Fmt"\n", SV_Arg(tg_msg.id_str));
            wprintf(L"Chat id:    "SV_Fmt"\n", SV_Arg(tg_msg.chat_id_str));

            if (tg_msg.has_text) {
                wprintf(L"Text:       "WS_FMT"\n", WS_ARG(tg_msg.text));
            } else {
                wprintf(L"Text:       [None]\n");
            }

            if (tg_msg.has_username) {
                wprintf(L"Username:   "WS_FMT"\n", WS_ARG(tg_msg.username));
            } else {
                wprintf(L"Username:   [None]\n");
            }

            wprintf(L"=============================\n");

            if (cnd_wait(&recv_msg_cnd, &recv_msg_mtx) != thrd_success) {
                fprintf(stderr, "ERROR: Could not wait for the condition\n");
                exit(1);
            }
        }
    }
}
