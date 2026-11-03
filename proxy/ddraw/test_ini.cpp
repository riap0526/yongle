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

std::string ReadFile(const char *path)
{
    std::string s;
    if (FILE *f = std::fopen(path, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

int main(int argc, char **argv)
{
    // test_ini <template> <older file> <output>: writes what FillTemplate makes of a real older file (for checking
    // the upgrade from earlier releases by eye; e.g. their emperor_engine.ini from git show v0.1.0:...).
    if (argc == 4) {
        const core::settings::FilledIni f = core::settings::FillTemplate(ReadFile(argv[1]), ReadFile(argv[2]));
        if (FILE *o = std::fopen(argv[3], "wb")) {
            std::fwrite(f.text.data(), 1, f.text.size(), o);
            std::fclose(o);
        }
        std::printf("%d kept, %d added at their defaults, %d kept as written\n", f.kept, f.added, f.extra);
        return 0;
    }

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

    // FillTemplate: an older file brought up to this version's template.
    const std::string tmpl = "; Settings. Header.\n"
                             "\n"
                             "[display]\n"
                             "; mode:\n"
                             "; a = first (default)\n"
                             "; b = second\n"
                             "mode = a\n"
                             "; Size. Default: 100.\n"
                             "size = 100\n"
                             "\n"
                             "[sound]\n"
                             "; on = sounds (default)\n"
                             "sound = on\n";
    Check(core::settings::FillTemplate(tmpl, tmpl).text == tmpl, "FillTemplate: the template itself is unchanged");
    const std::string older = "\xEF\xBB\xBF; Settings. An older header.\r\n"
                              "\r\n"
                              "[Display]\r\n"
                              "; mode: framed = old description\r\n"
                              "MODE = b ; my note\r\n"
                              "size=150\r\n"
                              "size = 125\r\n"
                              "typo_key = 1\r\n"
                              "not a setting\r\n"
                              "\r\n"
                              "[mine]\r\n"
                              "x = 2\r\n";
    const core::settings::FilledIni f = core::settings::FillTemplate(tmpl, older);
    const std::string want = "\xEF\xBB\xBF; Settings. Header.\r\n"
                             "\r\n"
                             "[display]\r\n"
                             "; mode:\r\n"
                             "; a = first (default)\r\n"
                             "; b = second\r\n"
                             "mode = b\r\n"
                             "; Size. Default: 100.\r\n"
                             "size = 125\r\n"
                             "\r\n"
                             "; Not described above (kept as written):\r\n"
                             "typo_key = 1\r\n"
                             "not a setting\r\n"
                             "\r\n"
                             "[sound]\r\n"
                             "; on = sounds (default)\r\n"
                             "sound = on\r\n"
                             "\r\n"
                             "; Sections not described above (kept as written):\r\n"
                             "[mine]\r\n"
                             "x = 2\r\n";
    Check(f.text == want, "FillTemplate: values, defaults, kept lines, CRLF and BOM");
    if (f.text != want) std::printf("  got:\n    %s\n", Show(f.text).c_str());
    Check(f.kept == 2 && f.added == 1 && f.extra == 3, "FillTemplate: counts (2 kept, 1 added, 3 as written)");
    Check(core::settings::FillTemplate(tmpl, f.text).text == f.text, "FillTemplate: filling again changes nothing");
    const std::string lf = "[display]\nsize = 200\n";
    const std::string lfGot = core::settings::FillTemplate(tmpl, lf).text;
    Check(lfGot.find('\r') == std::string::npos && lfGot.find("size = 200\n") != std::string::npos &&
              lfGot.find("mode = a\n") != std::string::npos && lfGot.find("sound = on\n") != std::string::npos,
          "FillTemplate: LF file, missing settings at their defaults");
    std::string crTmpl; // a template checked out with CRLF line ends gives the same result
    for (char c : tmpl) crTmpl += c == '\n' ? std::string("\r\n") : std::string(1, c);
    Check(core::settings::FillTemplate(crTmpl, older).text == want, "FillTemplate: a CRLF template is the same");
    const std::string top = "stray = 1\n[display]\nmode = b\n";
    const std::string topGot = core::settings::FillTemplate(tmpl, top).text;
    Check(topGot.find("; Settings. Header.\n\n; Not described above (kept as written):\nstray = 1\n\n[display]") == 0 &&
              core::settings::FillTemplate(tmpl, topGot).text == topGot,
          "FillTemplate: a key before the first section stays before it");

    std::printf("RESULT: %s (%d failure(s))\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
