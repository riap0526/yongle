#include "games/emperor/smoke.h"

#include "core/log.h"
#include "games/emperor/site.h"

#include <cstdio>
#include <cstring>

// Evidence (Emperor.exe SHA256 6373328b...3c0e; Ghidra and the exe's
// bytes).
namespace emperor::smoke {
namespace {

// cheat_run 0x43ce20: push esi; mov esi, [esp+8] (the table index); ...; pop esi; plain ret; returns al: __cdecl.
// 0..99 and not multiplayer: the cheat count 0x13124e4 + 1, then the handler of entry n of the table chosen by
// 0x82ab50 (1 in the exe: cheat_table 0x7abe00, names XOR 3; entry 1 "I win again" = 0x43cdf0), called with no
// arguments; returns 1. Checked before the call: its bytes after the first 8, the table choice, entry 1's handler and
// the handler's bytes (mov al, 1; mov [0x10de043], al; mov [0x10de042], al). Read through At (an offline test's copy).
const Site kCheatRun{"cheat_run", 0x43ce20, {}};
constexpr BYTE kCheatRunBody[8] = {0x53, 0x83, 0xfe, 0x64, 0x7d, 0x4e, 0xe8, 0x8d}; // at 0x43ce28
constexpr DWORD kCheatRunBodyAt = 0x43ce28;
using CheatRun_fn = char(__cdecl *)(int);
constexpr int kIWinAgain = 1;
const Var<char> kCheatTableChoice{0x82ab50};
constexpr DWORD kCheatTableHandler1 = 0x7abe10;
constexpr DWORD kCheatIWinAgain = 0x43cdf0;
constexpr BYTE kIWinAgainCode[8] = {0xb0, 0x01, 0xa2, 0x43, 0xe0, 0x0d, 0x01, 0xa2};

bool Same(DWORD address, const BYTE (&expect)[8])
{
    return std::memcmp(core::site::At<BYTE>(address), expect, sizeof(expect)) == 0;
}

// mission_check 0x55cee0: lose_reason < 0 and 0x13125e8 < 0: the objectives check (0x55b6a0) decides; lose_reason
// >= 0: lost; cheat_win: won; continue_governing (0x13124f8) set: neither. Won: only once 0x13125e4 is 0.
const Var<unsigned char> kGameScreen{0x10de045};
const Var<int> kContinueGoverning{0x13124f8}; // dword (89 3d / a1)
const Var<int> kWonAlready{0x13125e4};
const Var<char> kCheatWin{0x10de043};
const Var<int> kLoseReason{0x13125dc};   // -1: not lost (4 bytes: a3)
const Var<short> kLoseMessage{0x1312534};  // the objectives check: 0x573 (2 bytes: 66 a3)
const Var<short> kLoseMessage2{0x1312536}; // the objectives check: 0x572 (2 bytes: 66 c7 05)
const Var<int> kDialogX{0x10de014}, kDialogY{0x10de010};
constexpr int kCityScreen = 0x01;

bool InCity(const char *name, char *why, int size)
{
    if (*kGameScreen != kCityScreen) {
        std::snprintf(why, size, "%s: not on the city screen (screen 0x%02x)", name, *kGameScreen);
        return false;
    }
    if (*kContinueGoverning != 0) {
        std::snprintf(why, size, "%s: the city is governed on after a won mission (0x13124f8 = %d): mission_check "
                                 "ignores wins and losses", name, *kContinueGoverning);
        return false;
    }
    return true;
}

bool Win(char *why, int size)
{
    if (!InCity("win", why, size)) {
        return false;
    }
    if (*kWonAlready != 0) {
        std::snprintf(why, size, "win: this mission was already won (0x13125e4 = %d)", *kWonAlready);
        return false;
    }
    if (!Same(kCheatRunBodyAt, kCheatRunBody) || *kCheatTableChoice == 0 ||
        *core::site::At<DWORD>(kCheatTableHandler1) != kCheatIWinAgain || !Same(kCheatIWinAgain, kIWinAgainCode)) {
        std::snprintf(why, size, "win: cheat_run or the cheat table is not as expected (not the tested exe?)");
        return false;
    }
    const char ok = Function<CheatRun_fn>(kCheatRun)(kIWinAgain);
    core_log("emperor: smoke: win: cheat_run(1) (I win again) returned %d, cheat_win %d; mission_check takes it on its "
             "next check", static_cast<int>(ok), static_cast<int>(*kCheatWin));
    if (!ok || !*kCheatWin) {
        std::snprintf(why, size, "win: cheat_run(1) did not set cheat_win");
        return false;
    }
    return true;
}

bool Lose(char *why, int size)
{
    if (!InCity("lose", why, size)) {
        return false;
    }
    if (*kLoseReason >= 0) {
        std::snprintf(why, size, "lose: already lost (lose_reason 0x%x)", *kLoseReason);
        return false;
    }
    *kLoseReason = 0x573;
    *kLoseMessage = 0x573;
    *kLoseMessage2 = 0x572;
    core_log("emperor: smoke: lose: lose_reason = 0x573 (in debt too long), as the objectives check writes "
             "it; mission_check takes it on its next two checks");
    return true;
}

} // namespace

const char kActions[] = "win lose";

bool Action(const char *name, char *why, int size)
{
    if (std::strcmp(name, "win") == 0) {
        return Win(why, size);
    }
    if (std::strcmp(name, "lose") == 0) {
        return Lose(why, size);
    }
    std::snprintf(why, size, "no such action");
    return false;
}

void DialogOffset(int *x, int *y)
{
    *x = *kDialogX;
    *y = *kDialogY;
}

} // namespace emperor::smoke
