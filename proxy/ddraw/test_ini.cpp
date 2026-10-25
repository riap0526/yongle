// Offline test of core/settings/ini_text (no DLL): changing values in emperor_engine.ini keeps everything else as
// the user wrote it (comments, blank lines, unknown settings, spacing, line ends, BOM), and adds missing keys and
// sections in the right place.
#include "core/settings/ini_text.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using core::settings::IniChange;
using core::settings::IniLine;
using core::settings::ReadIni;
using core::settings::UpdateIni;

int g_failures = 0;

std::string Show(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n\n    ";
        else out += c;
    }
    return out;
}

void Expect(const char *what, const std::string &in, const std::vector<IniChange> &changes, const std::string &want)
{
    std::string got = UpdateIni(in, changes);
    bool ok = got == want;
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        std::printf("  want:\n    %s\n  got:\n    %s\n", Show(want).c_str(), Show(got).c_str());
        ++g_failures;
    }
}

void Check(bool ok, const char *what)
{
    std::printf("%-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

} // namespace

int main()
{
    const std::string base = "; Emperor engine settings\r\n"
                             "\r\n"
                             "[display]\r\n"
                             "; the output\r\n"
                             "present = display\r\n"
                             "\r\n"
                             "; size\r\n"
                             "resolution = 1024x768\r\n"
                             "my_own_key = 42\r\n"
                             "\r\n"
                             "[gameplay]\r\n"
                             "smooth_scroll = on   ; my note\r\n";

    Expect("value replaced, everything else kept", base, {{"display", "present", "cap60"}},
           "; Emperor engine settings\r\n\r\n[display]\r\n; the output\r\npresent = cap60\r\n\r\n; size\r\n"
           "resolution = 1024x768\r\nmy_own_key = 42\r\n\r\n[gameplay]\r\nsmooth_scroll = on   ; my note\r\n");
    Expect("inline comment and spacing after the value kept", base, {{"gameplay", "smooth_scroll", "off"}},
           "; Emperor engine settings\r\n\r\n[display]\r\n; the output\r\npresent = display\r\n\r\n; size\r\n"
           "resolution = 1024x768\r\nmy_own_key = 42\r\n\r\n[gameplay]\r\nsmooth_scroll = off   ; my note\r\n");
    Expect("section and key names case-insensitive", base, {{"DISPLAY", "Resolution", "scaled"}},
           "; Emperor engine settings\r\n\r\n[display]\r\n; the output\r\npresent = display\r\n\r\n; size\r\n"
           "resolution = scaled\r\nmy_own_key = 42\r\n\r\n[gameplay]\r\nsmooth_scroll = on   ; my note\r\n");
    Expect("missing key added after the section's last line", base, {{"display", "scale", "150"}},
           "; Emperor engine settings\r\n\r\n[display]\r\n; the output\r\npresent = display\r\n\r\n; size\r\n"
           "resolution = 1024x768\r\nmy_own_key = 42\r\nscale = 150\r\n\r\n[gameplay]\r\n"
           "smooth_scroll = on   ; my note\r\n");
    Expect("missing section added at the end", base, {{"ui", "panel_scale", "120"}},
           base + "\r\n[ui]\r\npanel_scale = 120\r\n");
    Expect("two keys added to one section keep their order", base,
           {{"timing", "high_resolution_timer", "off"}, {"timing", "other", "1"}},
           base + "\r\n[timing]\r\nhigh_resolution_timer = off\r\nother = 1\r\n");
    Expect("added to the last section without a final line end",
           "[gameplay]\nsmooth_scroll = on", {{"gameplay", "x", "1"}}, "[gameplay]\nsmooth_scroll = on\nx = 1\n");
    Expect("LF file stays LF", "[display]\npresent = display\n", {{"display", "present", "cap60"}},
           "[display]\npresent = cap60\n");
    Expect("BOM kept", "\xEF\xBB\xBF[display]\r\npresent = display\r\n", {{"display", "present", "cap60"}},
           "\xEF\xBB\xBF[display]\r\npresent = cap60\r\n");
    Expect("every line of a repeated key changed",
           "[display]\npresent = original\npresent = display\n", {{"display", "present", "cap60"}},
           "[display]\npresent = cap60\npresent = cap60\n");
    Expect("a repeated section: the key where it is",
           "[display]\npresent = display\n[gameplay]\nsmooth_scroll = on\n[display]\nscale = auto\n",
           {{"display", "scale", "125"}},
           "[display]\npresent = display\n[gameplay]\nsmooth_scroll = on\n[display]\nscale = 125\n");
    Expect("empty value filled in", "[ui]\npanel_key =\n", {{"ui", "panel_key", "F12"}}, "[ui]\npanel_key = F12\n");
    Expect("empty value before a comment", "[ui]\npanel_key =; note\n", {{"ui", "panel_key", "F12"}},
           "[ui]\npanel_key = F12 ; note\n");
    Expect("no spaces around = kept", "[ui]\npanel_scale=auto\n", {{"ui", "panel_scale", "150"}},
           "[ui]\npanel_scale=150\n");
    Expect("same key name in another section untouched", "[a]\nk = 1\n[b]\nk = 2\n", {{"b", "k", "3"}},
           "[a]\nk = 1\n[b]\nk = 3\n");
    Expect("new file", "", {{"display", "present", "cap60"}}, "[display]\r\npresent = cap60\r\n");
    Expect("no changes: same text", base, {}, base);

    std::vector<IniLine> keys;
    std::vector<int> invalid;
    ReadIni("\xEF\xBB\xBF; c\n[display]\npresent = cap60 ; x\nnot a setting\n[ui]\npanel_key=Ctrl+F12\n", &keys,
            &invalid);
    Check(keys.size() == 2 && keys[0].section == "display" && keys[0].key == "present" && keys[0].value == "cap60" &&
              keys[0].line == 3 && keys[1].value == "Ctrl+F12",
          "ReadIni: keys, values without comments, line numbers");
    Check(invalid.size() == 1 && invalid[0] == 4, "ReadIni: a line that is not a setting");

    const std::string old = "[display]\r\n; How the menus are shown:\r\n; framed = ...\r\nfullscreen_pictures = framed\r\n"
                            "\r\n; The map of China:\r\nchina_map = native\r\n\r\n[debug]\r\nframe_labels = off ; x\r\n";
    std::string got = core::settings::RemoveIniKeys(old, {{"Display", "Fullscreen_Pictures"}});
    Check(got == "[display]\r\n; The map of China:\r\nchina_map = native\r\n\r\n[debug]\r\nframe_labels = off ; x\r\n",
          "RemoveIniKeys: the key, its description and one blank line; the rest kept");
    Check(core::settings::RemoveIniKeys(old, {{"debug", "fullscreen_pictures"}}) == old,
          "RemoveIniKeys: the key in another section is not touched");
    Check(core::settings::RemoveIniKeys(old, {{"debug", "frame_labels"}}) ==
              "[display]\r\n; How the menus are shown:\r\n; framed = ...\r\nfullscreen_pictures = framed\r\n\r\n"
              "; The map of China:\r\nchina_map = native\r\n\r\n[debug]\r\n",
          "RemoveIniKeys: a key right after its section header");

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
