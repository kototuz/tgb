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

#define MAX_MSG_LEN 4096
typedef struct {
    int text[MAX_MSG_LEN];
    int text_len;
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

void tpilot_render(Tpilot self)
{
    ClearBackground(BG_COLOR);

    Size widget_size;
    Vector2 widget_pos;
    { // render text editor
        widget_size = calc_text_block_size(
                self.glyph_width,
                0, WIDTH,
                self.editor.text,
                self.editor.text_len);

        widget_pos = (Vector2){ .y = HEIGHT - widget_size.height };
        DrawRectangle(0, widget_pos.y, WIDTH, widget_size.height, TED_BG_COLOR);
        Vector2 end_pos = tpilot_draw_text(
                &self, widget_pos,
                self.editor.text,
                self.editor.text_len,
                TED_FG_COLOR);

        // render cursor
        DrawTextCodepoint(self.font, L'█', end_pos, FONT_SIZE, TED_CURSOR_COLOR);
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

void tpilot_send_message(Tpilot *self, CURL *curl, CURLU *url, ByteBuffer *bb)
{
    // render message on client
    tpilot_push_message(
            self,
            self->editor.text,
            self->editor.text_len,
            (AuthorName){ L"You", 3 });

    // create query field 'text' with text form the editor
    char *editor_text_as_utf8 = LoadUTF8(self->editor.text, self->editor.text_len);
    if (!field_to_str(bb, (Field) {
            .name = SV_STATIC("text"),
            .value = sv_from_cstr(editor_text_as_utf8)})) exit(1);
    UnloadUTF8(editor_text_as_utf8);


    INFO(L"Sending: %s\n", bb->data);
}

size_t empty_read(char *b, size_t s, size_t n, void *ud) {(void) b; (void) ud; return s*n; }
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <chat_id>\n", argv[0]);
        return 1;
    }

    setlocale(LC_ALL, "");

    CURLcode  curl_err;
    CURLUcode curlu_err;
    CURLU *url;
    CURL *input, *output;
    size_t last_msg_id;

    // this message is necessary without it programm enter the 'ascii print system'
    // In short you cannot use print functions from <stdio> and <wchar.h> simultaneously
    INFO(L"Hello from Tpilot :)%s\n", "");

    { // init curl input and curl output
        input = curl_easy_init();
        if (input == NULL) CURL_INIT_ERR();
        output = curl_easy_init();
        if (output == NULL) CURL_INIT_ERR();

        url = curl_url();
        if (url == NULL) {
            fputs("ERROR: Creating `url builder`\n", stderr);
            return 1;
        }

        curlu_err = curl_url_set(
                url, CURLUPART_URL,
                "https://api.telegram.org/bot"BOT_TOKEN"/sendMessage", 0);
        if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

        curl_easy_setopt(input, CURLOPT_URL, "https://api.telegram.org/bot"BOT_TOKEN"/getUpdates?offset=-1&allowed_updates=[\"message\"]");
        curl_easy_setopt(input, CURLOPT_WRITEFUNCTION, handle_data);
        curl_easy_setopt(output, CURLOPT_VERBOSE, 0L);
        curl_easy_setopt(output, CURLOPT_WRITEFUNCTION, empty_read);

        // get last message id
        curl_err = curl_easy_perform(input);
        if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);
        last_msg_id = received_message.id;
    }

    InitWindow(WIDTH, HEIGHT, "Tpilot");

    ByteBuffer text_buffer, chat_id_buffer;
    Tpilot tpilot = tpilot_new();
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
                            .value = sv_from_cstr(editor_text_as_utf8)})) return 1;
                UnloadUTF8(editor_text_as_utf8);

                // create query field 'chat_id' with the argument from command line
                if (!field_to_str(&chat_id_buffer, (Field){
                            .name = SV_STATIC("chat_id"),
                            .value = sv_from_cstr(argv[1])})) return 1;

                // append field 'chat_id' to url
                curlu_err = curl_url_set(url, CURLUPART_QUERY, chat_id_buffer.data, CURLU_APPENDQUERY);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                // append field 'text' to url
                curlu_err = curl_url_set(url, CURLUPART_QUERY, text_buffer.data, CURLU_APPENDQUERY | CURLU_URLENCODE);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                char *data;
                curlu_err = curl_url_get(url, CURLUPART_URL, &data, 0);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                curl_easy_setopt(output, CURLOPT_URL, data);
                curl_err = curl_easy_perform(output);
                if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);
                curl_free(data);

                // clear the editor buffer
                tpilot.editor.text_len = 0;

                // clear 'url' buffer
                curlu_err = curl_url_set(url, CURLUPART_QUERY, NULL, 0);
                if (curlu_err != CURLUE_OK) CURLU_ERR(curlu_err);

                break;

            case KEY_BACKSPACE:
                tpilot.editor.text_len -= 1;
                break;

            default:
                key = GetCharPressed();
                if (key != 0) {
                    tpilot.editor.text[tpilot.editor.text_len++] = key;
                }
                break;
        }

        // handle input message if it appears
        curl_err = curl_easy_perform(input);
        if (curl_err != CURLE_OK) CURL_PERFORM_ERR(curl_err);
        if (received_message.id > last_msg_id) {
            last_msg_id = received_message.id;
            INFO(L"Received message: ["WS_FMT"]\n", WS_ARG(received_message.text));
            tpilot_push_message(
                    &tpilot,
                    received_message.text.data,
                    received_message.text.count,
                    (AuthorName){ L"Anon", 4 });
        }

        BeginDrawing();
            tpilot_render(tpilot);
        EndDrawing();
    }
    CloseWindow();

    // cleanup all related to 'curl'
    free(text_buffer.data);
    curl_url_cleanup(url);
    curl_easy_cleanup(output);
    curl_easy_cleanup(input);

    return 0;
}

// TODO: the program is slow. Maybe something with memory allocating or it happens because one thread
// TODO: segfaults happen
// TODO: lines in the editor. It will be more efficient and convinient to implement operations using cursor
