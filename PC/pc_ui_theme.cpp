#include "pc_ui_theme.h"

namespace PcUi {

const Theme& DefaultTheme() {
    static Theme t;
    return t;
}

COLORREF Rgb(Color c) {
    return RGB(c.r, c.g, c.b);
}

HBRUSH CreateBrush(Color c) {
    return CreateSolidBrush(Rgb(c));
}

HFONT CreateUiFont(int pt, bool bold) {
    HDC dc = GetDC(nullptr);
    const int dpiY = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    const int height = -MulDiv(pt, dpiY > 0 ? dpiY : 96, 72);
    return CreateFontW(
        height,
        0,
        0,
        0,
        bold ? FW_SEMIBOLD : FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Microsoft YaHei UI"
    );
}

void DrawRoundFill(HDC dc, const RECT& rc, int radius, HBRUSH brush) {
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

void DrawRoundBorder(HDC dc, const RECT& rc, int radius, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawTextCenter(HDC dc, const RECT& rc, const wchar_t* text, HFONT font, COLORREF color, UINT format) {
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT r = rc;
    DrawTextW(dc, text ? text : L"", -1, &r, format);
    if (oldFont) SelectObject(dc, oldFont);
}

void DrawTextLeft(HDC dc, const RECT& rc, const wchar_t* text, HFONT font, COLORREF color, UINT format) {
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT r = rc;
    DrawTextW(dc, text ? text : L"", -1, &r, format);
    if (oldFont) SelectObject(dc, oldFont);
}

} // namespace PcUi
