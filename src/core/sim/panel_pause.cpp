#include "core/sim/panel_pause.h"

#include "core/diag/guard.h"
#include "core/log.h"
#include "core/ui/panel.h"
#include "core/ui/settings_view.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

namespace core::sim::panel_pause {

namespace {

core::diag::Feature g_drawFrames{"draw-only frames while the panel asks (core/sim/panel_pause)"};

bool g_paused = false;
bool g_drawOnly = false;
DWORD g_lastDrawOnly = 0;

struct DrawOnlyCall {
    const PauseGame *game;
    int result;
    core::diag::Restore r;
};

// g_drawOnly is in c.r: false again if the frame is abandoned.
void DrawOnlyFrame(DrawOnlyCall &c)
{
    c.r.Set(&g_drawOnly, true);
    c.result = c.game->drawOnlyFrame();
    c.r.Undo();
}

} // namespace

bool Loop(const PauseGame &game, int *result)
{
    *result = 0;
    const bool own = game.ownWindowOpen();
    if (core::ui::IsOpen() || own) {
        if (!g_paused) {
            g_paused = true;
            game.paused(own);
        }
        const DWORD now = timeGetTime();
        if ((core::ui::WantsGameFrames() || own) && now - g_lastDrawOnly >= 16 && core::diag::Enabled(g_drawFrames)) {
            g_lastDrawOnly = now;
            DrawOnlyCall c{&game, 0, {}};
            if (core::diag::Guarded(g_drawFrames, [](void *p) { DrawOnlyFrame(*static_cast<DrawOnlyCall *>(p)); },
                                    &c, &c.r)) {
                *result = c.result;
                return true;
            }
            core_log("%s: no more draw-only frames after an error: the game stays paused while the panel asks",
                     game.logPrefix);
            return true;
        }
        Sleep(5);
        return true;
    }
    if (g_paused) {
        g_paused = false;
        game.resumed();
    }
    return false;
}

bool DrawOnly()
{
    return g_drawOnly;
}

} // namespace core::sim::panel_pause
