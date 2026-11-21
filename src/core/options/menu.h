// A menu window in the game's own style: a title and a column of text buttons (the pause menu). Drawn through the
// options window's Painter in window coordinates (core/options/window.h: 640 x 480), centred there, so nothing here
// knows a game. The layout is the game layer's (MenuLook): both games use Augustus's pause menu layout (buttons 192 x
// 25, 40 apart) with the one under the mouse drawn with the game's darker button bar; the default (a frame around the
// one under the mouse) is what the options window's buttons still use.
//
// Threads: one thread only (the game thread in the DLL).
#pragma once

#include <string>
#include <vector>

#include "core/options/window.h"

namespace core::options {

struct MenuLook {
    int panelW = 256;      // the panel's width (whole 16 px tiles)
    int buttonW = 192;     // a button's width (centred in the panel)
    int buttonH = 25;      // a button's height (where it takes the mouse; the frame's height)
    int step = 40;         // from one button's top to the next's
    int titleY = 14;       // the title's top below the panel's
    int firstY = 56;       // the first button's top below the panel's
    int bottom = 23;       // below the last button to the panel's bottom (rounded up to whole tiles)
    int labelDown = 5;     // a label's top below its button's
    int barStyle = 0;      // Painter::ButtonBar's style for a button
    int hoverBarStyle = 0; // ... for the button under the mouse
    bool hoverFrame = true; // the frame around the button under the mouse
    Font label = Font::Normal, hoverLabel = Font::Normal; // the buttons' text
};

class Menu {
public:
    static constexpr int kClose = -2; // MouseUp / Key / RightClick: the menu wants to close
    static constexpr int kNone = -1;

    void SetLook(const MenuLook &look) { look_ = look; }
    const MenuLook &Look() const { return look_; }
    void Set(std::string title, std::vector<std::string> items);
    void Open(); // nothing pressed, nothing under the mouse

    void MouseMove(int x, int y);
    void MouseDown(int x, int y);
    int MouseUp(int x, int y); // the item clicked (pressed and released on it), or kNone
    int RightClick() { return kClose; }
    int Key(int vk);           // Escape: kClose; others: kNone
    void Draw(Painter &p);

    // The default look's numbers (Yongle's; window coordinates).
    static constexpr int kPanelW = 256, kButtonW = 192, kButtonH = 25, kStep = 40, kTitleY = 14, kFirstY = 56;
    int PanelX() const { return (kWidth - look_.panelW) / 2; }
    int PanelY() const { return (kHeight - PanelH()) / 2; }
    int PanelH() const;
    int ButtonX() const { return PanelX() + (look_.panelW - look_.buttonW) / 2; }
    int ButtonY(int i) const { return PanelY() + look_.firstY + i * look_.step; }
    int ItemAt(int x, int y) const; // kNone: no button there
    int Hover() const { return hover_; }
    size_t Count() const { return items_.size(); }

private:
    MenuLook look_;
    std::string title_;
    std::vector<std::string> items_;
    int hover_ = kNone, pressed_ = kNone;
};

} // namespace core::options
