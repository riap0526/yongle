#include "core/options/menu.h"

#include <windows.h>

namespace core::options {

void Menu::Set(std::string title, std::vector<std::string> items)
{
    title_ = std::move(title);
    items_ = std::move(items);
    Open();
}

void Menu::Open()
{
    hover_ = pressed_ = kNone;
}

int Menu::PanelH() const
{
    // The title, the buttons, the bottom margin; whole 16 px tiles.
    const int n = static_cast<int>(items_.size());
    const int h = look_.firstY + (n > 0 ? (n - 1) * look_.step + look_.buttonH : 0) + look_.bottom;
    return (h + 15) / 16 * 16;
}

int Menu::ItemAt(int x, int y) const
{
    if (x < ButtonX() || x >= ButtonX() + look_.buttonW) {
        return kNone;
    }
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (y >= ButtonY(i) && y < ButtonY(i) + look_.buttonH) {
            return i;
        }
    }
    return kNone;
}

void Menu::MouseMove(int x, int y)
{
    hover_ = ItemAt(x, y);
}

void Menu::MouseDown(int x, int y)
{
    hover_ = pressed_ = ItemAt(x, y);
}

int Menu::MouseUp(int x, int y)
{
    const int at = ItemAt(x, y);
    const int clicked = at != kNone && at == pressed_ ? at : kNone;
    pressed_ = kNone;
    hover_ = at;
    return clicked;
}

int Menu::Key(int vk)
{
    return vk == VK_ESCAPE ? kClose : kNone;
}

void Menu::Draw(Painter &p)
{
    const MenuLook &l = look_;
    const int px = PanelX(), py = PanelY();
    p.Panel(px, py, l.panelW, PanelH());
    p.Text(title_.c_str(), px + (l.panelW - p.TextWidth(title_.c_str(), Font::Title)) / 2, py + l.titleY, Font::Title);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const int x = ButtonX(), y = ButtonY(i);
        const bool hover = i == hover_;
        p.ButtonBar(x, y, l.buttonW, hover ? l.hoverBarStyle : l.barStyle);
        const char *label = items_[i].c_str();
        const Font f = hover ? l.hoverLabel : l.label;
        p.Text(label, x + (l.buttonW - p.TextWidth(label, f)) / 2, y + l.labelDown, f);
        if (hover && l.hoverFrame) {
            p.Frame(x, y, l.buttonW, l.buttonH);
        }
    }
}

} // namespace core::options
