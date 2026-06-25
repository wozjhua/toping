#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace PcUi {

struct Color {
    BYTE r;
    BYTE g;
    BYTE b;
};

struct Theme {
    Color bg{14, 18, 25};
    Color panel{22, 30, 40};
    Color panel2{28, 38, 52};
    Color stroke{64, 91, 115};
    Color accent{61, 165, 255};
    Color accent2{73, 209, 125};
    Color text{234, 244, 255};
    Color textMuted{158, 179, 199};
    Color danger{244, 107, 107};
    Color scrim{0, 0, 0};
};

const Theme& DefaultTheme();
COLORREF Rgb(Color c);
HBRUSH CreateBrush(Color c);
HFONT CreateUiFont(int pt, bool bold = false);
void DrawRoundFill(HDC dc, const RECT& rc, int radius, HBRUSH brush);
void DrawRoundBorder(HDC dc, const RECT& rc, int radius, COLORREF color, int width = 1);
void DrawTextCenter(HDC dc, const RECT& rc, const wchar_t* text, HFONT font, COLORREF color, UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
void DrawTextLeft(HDC dc, const RECT& rc, const wchar_t* text, HFONT font, COLORREF color, UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

} // namespace PcUi
