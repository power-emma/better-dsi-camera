#include "font.h"
#include "ui.h"

static u16 *fb; // 256x256, 192 rows shown

void uiInit(void)
{
    videoSetModeSub(MODE_5_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    int bg = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    fb = bgGetGfxPtr(bg);
    uiFill(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, UI_WHITE);
}

int uiLineHeight(void)
{
    return fontHeight;
}

// 16-bit writes only: VRAM ignores byte writes.
void uiFill(int x, int y, int w, int h, u16 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_WIDTH) w = SCREEN_WIDTH - x;
    if (y + h > SCREEN_HEIGHT) h = SCREEN_HEIGHT - y;

    for (int r = 0; r < h; r++)
    {
        u16 *p = fb + (y + r) * 256 + x;
        for (int i = 0; i < w; i++)
            p[i] = color;
    }
}

void uiBox(int x, int y, int w, int h, u16 fill, u16 border)
{
    uiFill(x, y, w, h, border);
    uiFill(x + 1, y + 1, w - 2, h - 2, fill);
}

int uiTextWidth(const char *s)
{
    int w = 0;
    for (; *s; s++)
        if (*s >= 32 && *s < 127)
            w += fontWidth[*s - 32];
    return w;
}

// a: 0-32
static u16 blend(u16 bg, u16 fg, int a)
{
    int r = ((bg & 31) * (32 - a) + (fg & 31) * a) >> 5;
    int g = (((bg >> 5) & 31) * (32 - a) + ((fg >> 5) & 31) * a) >> 5;
    int b = (((bg >> 10) & 31) * (32 - a) + ((fg >> 10) & 31) * a) >> 5;
    return r | (g << 5) | (b << 10) | BIT(15);
}

int uiText(int x, int y, int right, const char *s, u16 color)
{
    if (right > SCREEN_WIDTH)
        right = SCREEN_WIDTH;

    for (; *s; s++)
    {
        if (*s < 32 || *s >= 127)
            continue;
        int c = *s - 32;
        int w = fontWidth[c];
        const unsigned char *g = fontData + fontOffset[c];
        for (int r = 0; r < fontHeight; r++)
        {
            if (y + r < 0 || y + r >= SCREEN_HEIGHT)
                continue;
            u16 *p = fb + (y + r) * 256;
            for (int i = 0; i < w; i++)
            {
                int px = x + i;
                int a = (g[r * w + i] + 4) >> 3;
                if (a > 0 && px >= 0 && px < right)
                    p[px] = blend(p[px], color, a);
            }
        }
        x += w;
    }
    return x;
}
