#include "pc_menu_create_dialog.h"

const wchar_t* PcMenuCategoryLabel(PcMenuCategory cat) {
    switch (cat) {
        case PcMenuCategory::NormalKey: return L"普通按键";
        case PcMenuCategory::MenuRadial: return L"轮盘菜单";
        case PcMenuCategory::MenuItemOperation:
        case PcMenuCategory::MenuHorizontal:
        case PcMenuCategory::MenuVertical: return L"道具操作";
        case PcMenuCategory::MenuBagOperation: return L"背包操作";
        default: return L"菜单";
    }
}

bool ShowPcMenuCreatePopup(HWND ownerHwnd, PcMenuCategory& outCategory) {
    if (!ownerHwnd) return false;
    HMENU menu = CreatePopupMenu();
    if (!menu) return false;
    AppendMenuW(menu, MF_STRING, 1, L"轮盘菜单");
    AppendMenuW(menu, MF_STRING, 2, L"道具操作");
    AppendMenuW(menu, MF_STRING, 3, L"背包操作");
    POINT pt{};
    GetCursorPos(&pt);
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, ownerHwnd, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case 1: outCategory = PcMenuCategory::MenuRadial; return true;
        case 2: outCategory = PcMenuCategory::MenuItemOperation; return true;
        case 3: outCategory = PcMenuCategory::MenuBagOperation; return true;
        default: return false;
    }
}
