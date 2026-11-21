// Hook sites of Emperor.exe 1.0.1.0 (image base 0x400000, no relocations). The mechanism -- Var<T>, Site, Matches,
// Install, Function, Original, GameAddress, PatchCode, Detour and the offline tests' g_imageShift -- is the core's
// (core/hook/site.h), shared with every game layer; this header takes those names into namespace emperor, so the
// modules write Var<int>, Matches(...) and emperor::g_imageShift as before. The addresses are in each module.
#pragma once

#include "core/hook/site.h"

namespace emperor {

using core::site::CallTo;
using core::site::Detour;
using core::site::Function;
using core::site::g_imageShift;
using core::site::GameAddress;
using core::site::Install;
using core::site::Matches;
using core::site::Original;
using core::site::PatchCode;
using core::site::Site;
using core::site::Var;

} // namespace emperor
