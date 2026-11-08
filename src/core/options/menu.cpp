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
    // The title, the buttons, a bottom margin as wide as the side ones; whole 16 px tiles.
    const int n = static_cast<int>(items_.size());
    const int h = kFirstY + (n > 0 ? (n - 1) * kStep + kButtonH : 0) + (kPanelW - kButtonW) / 2 - 9;
    return (h + 15) / 16 * 16;
}

int Menu::ItemAt(int x, int y) const
{
    if (x < ButtonX() || x >= ButtonX() + kButtonW) {
        return kNone;
    }
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (y >= ButtonY(i) && y < ButtonY(i) + kButtonH) {
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
    const int px = PanelX(), py = PanelY();
    p.Panel(px, py, kPanelW, PanelH());
    p.Text(title_.c_str(), px + (kPanelW - p.TextWidth(title_.c_str(), Font::Title)) / 2, py + kTitleY, Font::Title);
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const int x = ButtonX(), y = ButtonY(i);
        p.ButtonBar(x, y, kButtonW, 0);
        const char *label = items_[i].c_str();
        p.Text(label, x + (kButtonW - p.TextWidth(label, Font::Normal)) / 2, y + 5, Font::Normal);
        if (i == hover_) {
            p.Frame(x, y, kButtonW, kButtonH);
        }
    }
}

} // namespace core::options
