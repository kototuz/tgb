#include <raylib-5.5/src/raylib.h>

#include <stdio.h>
#include <locale.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#define WIDTH  800
#define HEIGHT 600

// NOTE: the function assumes monospaced font
void draw_codepoints_boxed(Font font, float font_size, int *codepoints, int codepoint_count, Rectangle box)
{
    float scale_factor = font_size/font.baseSize;
    double glyph_width = font.glyphs[0].advanceX*scale_factor;

    Vector2 pos = { .x = box.x, .y = box.y };
    for (int i = 0; i < codepoint_count; i++) {
        if (codepoints[i] == '\n') {
            pos.y += font_size;
            pos.x = 0;
            continue;
        }

        if (pos.x+glyph_width > box.width) {
            pos.y += font_size;
            pos.x = 0;
        }

        DrawTextCodepoint(font, codepoints[i], pos, font_size, WHITE);
        pos.x += glyph_width;
    }
}

int main(void)
{
    InitWindow(WIDTH, HEIGHT, "Tpilot");

    Font font = LoadFontEx("JetBrainsMono-Regular.ttf", 48, NULL, 1171);

    SetTargetFPS(60);
    SetTextLineSpacing(20);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    int buflen = 0;
    int buffer[1024];
    while (!WindowShouldClose()) {
        int key = GetKeyPressed();
        if (key == KEY_ENTER) {
            buffer[buflen++] = L'\n';
        } else if (key == KEY_BACKSPACE) {
            printf("BACKSPACE");
            buflen -= 1;
        } else {
            int key = GetCharPressed();
            if (key != 0) {
                printf("Pressed: %lc\n", key);
                if (buflen == sizeof(buffer)) break;
                buffer[buflen++] = key;
            }
        }

        BeginDrawing();
            ClearBackground(BLACK);
            /*printf("Glyph value:       %lc\n", font.glyphs[3].value);*/
            /*printf("Glyph andanceX:    %d\n", font.glyphs[3].advanceX);*/
            /*printf("Glyph rect width:  %f\n", font.recs[3].width);*/
            /*printf("Glyph rect height: %f\n", font.recs[3].height);*/
            draw_codepoints_boxed(font, 48, buffer, buflen, (Rectangle){
                .width  = WIDTH,
                .height = HEIGHT,
            });
        EndDrawing();
    }

    UnloadFont(font);
    CloseWindow();

    return 0;
}

// Text to be displayed, must be UTF-8 (save this code file as UTF-8)
// NOTE: It can contain all the required text for the game,
// this text will be scanned to get all the required codepoints
static char *text = "いろはにほへと　ちりぬるを\nわかよたれそ　つねならむ\nうゐのおくやま　けふこえて\nあさきゆめみし　ゑひもせす";

// Remove codepoint duplicates if requested
static int *CodepointRemoveDuplicates(int *codepoints, int codepointCount, int *codepointResultCount);

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main0(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "raylib [text] example - codepoints loading");

    // Get codepoints from text
    int codepointCount = 0;
    int *codepoints = LoadCodepoints(text, &codepointCount);

    // Removed duplicate codepoints to generate smaller font atlas
    int codepointsNoDupsCount = 0;
    int *codepointsNoDups = CodepointRemoveDuplicates(codepoints, codepointCount, &codepointsNoDupsCount);
    UnloadCodepoints(codepoints);

    // Load font containing all the provided codepoint glyphs
    // A texture font atlas is automatically generated
    Font font = LoadFontEx("raylib-5.5/examples/text/resources/DotGothic16-Regular.ttf", 36, codepointsNoDups, codepointsNoDupsCount);

    // Set bilinear scale filter for better font scaling
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);

    SetTextLineSpacing(20);         // Set line spacing for multiline text (when line breaks are included '\n')

    // Free codepoints, atlas has already been generated
    free(codepointsNoDups);

    bool showFontAtlas = false;

    int codepointSize = 0;
    char *ptr = text;

    SetTargetFPS(60);               // Set our game to run at 60 frames-per-second
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose())    // Detect window close button or ESC key
    {
        // Update
        //----------------------------------------------------------------------------------
        if (IsKeyPressed(KEY_SPACE)) showFontAtlas = !showFontAtlas;

        // Testing code: getting next and previous codepoints on provided text
        if (IsKeyPressed(KEY_RIGHT))
        {
            // Get next codepoint in string and move pointer
            GetCodepointNext(ptr, &codepointSize);
            ptr += codepointSize;
        }
        else if (IsKeyPressed(KEY_LEFT))
        {
            // Get previous codepoint in string and move pointer
            GetCodepointPrevious(ptr, &codepointSize);
            ptr -= codepointSize;
        }
        //----------------------------------------------------------------------------------

        // Draw
        //----------------------------------------------------------------------------------
        BeginDrawing();

            ClearBackground(RAYWHITE);

            DrawRectangle(0, 0, GetScreenWidth(), 70, BLACK);
            DrawText(TextFormat("Total codepoints contained in provided text: %i", codepointCount), 10, 10, 20, GREEN);
            DrawText(TextFormat("Total codepoints required for font atlas (duplicates excluded): %i", codepointsNoDupsCount), 10, 40, 20, GREEN);

            if (showFontAtlas)
            {
                // Draw generated font texture atlas containing provided codepoints
                DrawTexture(font.texture, 150, 100, BLACK);
                DrawRectangleLines(150, 100, font.texture.width, font.texture.height, BLACK);
            }
            else
            {
                // Draw provided text with laoded font, containing all required codepoint glyphs
                DrawTextEx(font, text, (Vector2) { 160, 110 }, 48, 5, BLACK);
            }

            DrawText("Press SPACE to toggle font atlas view!", 10, GetScreenHeight() - 30, 20, GRAY);

        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    // De-Initialization
    //--------------------------------------------------------------------------------------
    UnloadFont(font);     // Unload font

    CloseWindow();        // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}

// Remove codepoint duplicates if requested
// WARNING: This process could be a bit slow if there text to process is very long
static int *CodepointRemoveDuplicates(int *codepoints, int codepointCount, int *codepointsResultCount)
{
    int codepointsNoDupsCount = codepointCount;
    int *codepointsNoDups = (int *)calloc(codepointCount, sizeof(int));
    memcpy(codepointsNoDups, codepoints, codepointCount*sizeof(int));

    // Remove duplicates
    for (int i = 0; i < codepointsNoDupsCount; i++)
    {
        for (int j = i + 1; j < codepointsNoDupsCount; j++)
        {
            if (codepointsNoDups[i] == codepointsNoDups[j])
            {
                for (int k = j; k < codepointsNoDupsCount; k++) codepointsNoDups[k] = codepointsNoDups[k + 1];

                codepointsNoDupsCount--;
                j--;
            }
        }
    }

    // NOTE: The size of codepointsNoDups is the same as original array but
    // only required positions are filled (codepointsNoDupsCount)

    *codepointsResultCount = codepointsNoDupsCount;
    return codepointsNoDups;
}

int main3(void)
{
    setlocale(LC_ALL, "");

    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "raylib [text] example - font loading");

    int codepoint_count = 0;
    int *codepoints = LoadCodepoints("ПРИВЕТ", &codepoint_count);
    Font fontTtf = LoadFontEx("JetBrainsMono-Regular.ttf", 48, codepoints, codepoint_count);

    SetTextLineSpacing(16);
    SetTargetFPS(60);

    while (!WindowShouldClose()) {
        BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawTextEx(fontTtf, "ПРИВЕТ ФЫВА", (Vector2){0}, 32, 2, RED);
            /*DrawTextCodepoints(fontTtf, codepoints, codepoint_count, (Vector2){0}, 32, 2, RED);*/
        EndDrawing();
    }

    UnloadCodepoints(codepoints);
    UnloadFont(fontTtf);
    CloseWindow();

    return 0;
}
