// Bottom screen: filled boxes and anti-aliased text on a 16-bit bitmap.

#ifndef UI_H__
#define UI_H__

#include <nds.h>

#define UI_RGB(r, g, b)  (RGB15(r, g, b) | BIT(15))

#define UI_WHITE    UI_RGB(31, 31, 31)
#define UI_TEXT     UI_RGB(4, 4, 5)
#define UI_DIM      UI_RGB(17, 17, 18)
#define UI_BORDER   UI_RGB(8, 8, 9)
#define UI_REC      UI_RGB(26, 4, 4)

#define UI_RED      UI_RGB(31, 21, 21)
#define UI_ORANGE   UI_RGB(31, 25, 18)
#define UI_YELLOW   UI_RGB(31, 29, 17)
#define UI_GREEN    UI_RGB(21, 29, 21)
#define UI_BLUE     UI_RGB(20, 25, 31)
#define UI_PURPLE   UI_RGB(26, 21, 31)

void uiInit(void);
int uiLineHeight(void);
void uiFill(int x, int y, int w, int h, u16 color);
void uiBox(int x, int y, int w, int h, u16 fill, u16 border);
int uiTextWidth(const char *s);

// Clipped at right. Returns the end x.
int uiText(int x, int y, int right, const char *s, u16 color);

#endif // UI_H__
