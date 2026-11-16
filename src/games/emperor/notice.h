// Our messages in the game's own notice boxes (0.2.0 item 6): the boxes the game shows at the
// top of the city ("Game saved"), with their look, place and time on screen (15 s), through the game's
// city_warning_show_text 0x547d30. The game's own "Warnings" switch (Help menu) does not hide ours: they answer
// something the player just did. Shown on the game thread, from the game's notice drawing (city_warnings_draw
// 0x547a20, hooked). The queue (Post and Zoom, any thread) is the core's: core/ui/notice.
#pragma once

namespace emperor::notice {

// After the game's hooks: hooks city_warnings_draw and installs core/ui/notice with Emperor's boxes: a red text in
// the game's red warning font; the zoom box made once and then rewritten in place while the zoom changes (the number
// counts along as the zoom moves), staying 15 s after the last change, as every notice does. False (logged) when a
// site does not match; core::ui::notice::Post then shows the text on our own caption line (core/output SetNotice).
bool Attach();

// For the offline test: installs core/ui/notice with Emperor's boxes without the hook (core::ui::notice::OnFrame then
// stands for the detour's first half).
void InstallForTest();

} // namespace emperor::notice
