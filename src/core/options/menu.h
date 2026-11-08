// A menu window in the game's own style: a title and a column of text buttons (the pause menu, docs/archive/key_bindings.md
// section 8; laid out as Augustus's pause menu: buttons 192 x 25, 40 apart). Drawn through the options window's
// Painter in window coordinates (core/options/window.h: 640 x 480), centred there, so nothing here knows a game.
//
// Threads: one thread only (the game thread in the DLL).
#pragma once

#include <string>
#include <vector>

#include "core/options/window.h"

namespace core::options {

class Menu {
public:
    static constexpr int kClose = -2; // MouseUp / Key / RightClick: the menu wants to close
    static constexpr int kNone = -1;

    void Set(std::string title, std::vector<std::string> items);
    void Open(); // nothing pressed, nothing under the mouse

    void MouseMove(int x, int y);
    void MouseDown(int x, int y);
    int MouseUp(int x, int y); // the item clicked (pressed and released on it), or kNone
    int RightClick() { return kClose; }
    int Key(int vk);           // Escape: kClose; others: kNone
    void Draw(Painter &p);

    // Layout (window coordinates), as Augustus's: a panel 16 x 24 tiles, buttons 192 x 25, 40 apart.
    static constexpr int kPanelW = 256, kButtonW = 192, kButtonH = 25, kStep = 40, kTitleY = 14, kFirstY = 56;
    int PanelX() const { return (kWidth - kPanelW) / 2; }
    int PanelY() const { return (kHeight - PanelH()) / 2; }
    int PanelH() const;
    int ButtonX() const { return PanelX() + (kPanelW - kButtonW) / 2; }
    int ButtonY(int i) const { return PanelY() + kFirstY + i * kStep; }
    int ItemAt(int x, int y) const; // kNone: no button there
    int Hover() const { return hover_; }
    size_t Count() const { return items_.size(); }

private:
    std::string title_;
    std::vector<std::string> items_;
    int hover_ = kNone, pressed_ = kNone;
};

} // namespace core::options
