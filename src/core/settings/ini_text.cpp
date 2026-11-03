#include "core/settings/ini_text.h"

#include <cstring>
#include <utility>

namespace core::settings {

namespace {

bool SameName(const std::string &a, const std::string &b)
{
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

bool Space(char c)
{
    return c == ' ' || c == '\t';
}

// One physical line: [begin, end) without its line end; next is where the following line starts.
struct Span {
    size_t begin, end, next;
};

size_t BomLength(const std::string &text)
{
    return text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                   static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF
               ? 3
               : 0;
}

std::vector<Span> Lines(const std::string &text)
{
    std::vector<Span> out;
    size_t p = BomLength(text);
    while (p < text.size()) {
        size_t nl = text.find('\n', p);
        size_t e = nl == std::string::npos ? text.size() : nl;
        size_t next = nl == std::string::npos ? text.size() : nl + 1;
        if (e > p && text[e - 1] == '\r') {
            --e;
        }
        out.push_back(Span{p, e, next});
        p = next;
    }
    return out;
}

// The parts of a line, as positions in the text.
struct Parsed {
    enum Kind { kEmpty, kComment, kSection, kKey, kInvalid } kind;
    size_t nameBegin, nameEnd;   // section name or key
    size_t valueBegin, valueEnd; // key lines: the value, trimmed, before any comment
};

Parsed Parse(const std::string &t, const Span &s)
{
    size_t b = s.begin, e = s.end;
    bool comment = false;
    for (size_t c = b; c < e; ++c) {
        if (t[c] == ';' || t[c] == '#') {
            e = c;
            comment = true;
            break;
        }
    }
    while (b < e && Space(t[b])) ++b;
    while (e > b && Space(t[e - 1])) --e;
    if (b == e) {
        return Parsed{comment ? Parsed::kComment : Parsed::kEmpty, 0, 0, 0, 0};
    }
    if (t[b] == '[' && t[e - 1] == ']') {
        return Parsed{Parsed::kSection, b + 1, e - 1, 0, 0};
    }
    size_t eq = t.find('=', b);
    if (eq == std::string::npos || eq >= e) {
        return Parsed{Parsed::kInvalid, 0, 0, 0, 0};
    }
    size_t kb = b, ke = eq, vb = eq + 1, ve = e;
    while (ke > kb && Space(t[ke - 1])) --ke;
    while (vb < ve && Space(t[vb])) ++vb;
    return Parsed{Parsed::kKey, kb, ke, vb, ve};
}

std::string Name(const std::string &t, const Parsed &p)
{
    return t.substr(p.nameBegin, p.nameEnd - p.nameBegin);
}

} // namespace

void ReadIni(const std::string &text, std::vector<IniLine> *keys, std::vector<int> *invalid)
{
    std::string section;
    int n = 0;
    for (const Span &s : Lines(text)) {
        ++n;
        Parsed p = Parse(text, s);
        if (p.kind == Parsed::kSection) {
            section = Name(text, p);
        }
        else if (p.kind == Parsed::kKey && keys) {
            keys->push_back(IniLine{section, Name(text, p), text.substr(p.valueBegin, p.valueEnd - p.valueBegin), n});
        }
        else if (p.kind == Parsed::kInvalid && invalid) {
            invalid->push_back(n);
        }
    }
}

std::string RemoveIniKeys(const std::string &text, const std::vector<IniKey> &keys)
{
    const std::vector<Span> lines = Lines(text);
    std::vector<Parsed> parsed;
    std::vector<bool> drop(lines.size(), false);
    std::string section;
    for (size_t i = 0; i < lines.size(); ++i) {
        parsed.push_back(Parse(text, lines[i]));
        const Parsed &p = parsed.back();
        if (p.kind == Parsed::kSection) {
            section = Name(text, p);
        }
        if (p.kind != Parsed::kKey) {
            continue;
        }
        for (const IniKey &k : keys) {
            if (SameName(section, k.section) && SameName(Name(text, p), k.key)) {
                drop[i] = true;
                for (size_t j = i; j > 0 && parsed[j - 1].kind == Parsed::kComment; --j) {
                    drop[j - 1] = true; // its description
                }
                break;
            }
        }
    }
    std::string out = text.substr(0, BomLength(text));
    bool lastBlank = true;    // at the start of the file a blank line is not kept after a removed block
    bool lastSection = false; // nor right under a section header
    bool removedBefore = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (drop[i]) {
            removedBefore = true;
            continue;
        }
        const bool blank = parsed[i].kind == Parsed::kEmpty;
        if (blank && removedBefore && (lastBlank || lastSection)) {
            continue; // the blank line that separated the removed block
        }
        out.append(text, lines[i].begin, lines[i].next - lines[i].begin);
        lastBlank = blank;
        lastSection = parsed[i].kind == Parsed::kSection;
        removedBefore = false;
    }
    return out;
}

std::string UpdateIni(const std::string &text, const std::vector<IniChange> &changes)
{
    const bool crlf = text.find("\r\n") != std::string::npos || text.find('\n') == std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";
    const std::vector<Span> lines = Lines(text);
    std::vector<Parsed> parsed;
    std::vector<std::string> sectionOf; // the section each line belongs to ("" before the first header)
    std::string section;
    for (const Span &s : lines) {
        parsed.push_back(Parse(text, s));
        if (parsed.back().kind == Parsed::kSection) {
            section = Name(text, parsed.back());
        }
        sectionOf.push_back(section);
    }

    struct Edit {
        size_t at, end; // replace [at, end) (an insertion when equal)
        std::string with;
    };
    std::vector<Edit> edits;
    std::vector<std::pair<std::string, std::string>> appended; // new sections at the end: name, their lines
    for (const IniChange &c : changes) {
        bool found = false;
        size_t last = std::string::npos; // the section's last line that is not empty (comments count)
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!SameName(sectionOf[i], c.section)) {
                continue;
            }
            const Parsed &p = parsed[i];
            if (p.kind != Parsed::kEmpty) {
                last = i;
            }
            if (p.kind != Parsed::kKey || !SameName(Name(text, p), c.key)) {
                continue;
            }
            found = true;
            if (p.valueBegin < p.valueEnd) {
                edits.push_back(Edit{p.valueBegin, p.valueEnd, c.value});
            }
            else { // "key =" or "key = ; comment": the value goes right after '='
                size_t at = text.find('=', p.nameEnd) + 1;
                bool more = at < lines[i].end && !Space(text[at]);
                edits.push_back(Edit{at, at, " " + c.value + (more ? " " : "")});
            }
        }
        if (found) {
            continue;
        }
        std::string line = c.key + " = " + c.value + eol;
        if (last != std::string::npos) {
            size_t at = lines[last].next;
            edits.push_back(Edit{at, at, (at > lines[last].end ? "" : eol) + line});
        }
        else if (c.section.empty()) {
            size_t at = BomLength(text);
            edits.push_back(Edit{at, at, line});
        }
        else {
            bool added = false;
            for (auto &a : appended) {
                if (SameName(a.first, c.section)) {
                    a.second += line;
                    added = true;
                }
            }
            if (!added) {
                appended.emplace_back(c.section, line);
            }
        }
    }

    // Apply from the end of the text so earlier positions stay valid. Insertions at the same place are applied
    // in reverse, so they end up in the order of the changes.
    std::vector<size_t> order;
    for (size_t i = 0; i < edits.size(); ++i) {
        size_t j = order.size();
        order.push_back(i);
        while (j > 0 && edits[order[j - 1]].at < edits[i].at) { // stable: descending position
            order[j] = order[j - 1];
            --j;
        }
        order[j] = i;
    }
    std::string out = text;
    for (size_t k = 0; k < order.size();) {
        size_t m = k;
        while (m < order.size() && edits[order[m]].at == edits[order[k]].at) ++m;
        for (size_t n = m; n-- > k;) {
            const Edit &e = edits[order[n]];
            out.replace(e.at, e.end - e.at, e.with);
        }
        k = m;
    }
    for (const auto &a : appended) {
        if (out.size() > BomLength(out)) { // a new file starts with the section, not a blank line
            if (out.back() != '\n') {
                out += eol;
            }
            out += eol;
        }
        out += "[" + a.first + "]" + eol + a.second;
    }
    return out;
}

FilledIni FillTemplate(const std::string &tmplIn, const std::string &text)
{
    // The template as plain LF text without a BOM (the build may have given it CRLF).
    std::string tmpl;
    for (size_t i = BomLength(tmplIn); i < tmplIn.size(); ++i) {
        if (tmplIn[i] != '\r') tmpl += tmplIn[i];
    }

    // The template: its lines, the section of each, its keys.
    const std::vector<Span> tl = Lines(tmpl);
    std::vector<Parsed> tp;
    std::vector<std::string> tsec;
    std::vector<std::string> sections{""}; // in order; "" = before the first header
    {
        std::string section;
        for (const Span &s : tl) {
            tp.push_back(Parse(tmpl, s));
            if (tp.back().kind == Parsed::kSection) {
                section = Name(tmpl, tp.back());
                sections.push_back(section);
            }
            tsec.push_back(section);
        }
    }
    auto inTemplate = [&](const std::string &section, const std::string &key) {
        for (size_t i = 0; i < tl.size(); ++i) {
            if (tp[i].kind == Parsed::kKey && SameName(tsec[i], section) && SameName(Name(tmpl, tp[i]), key)) {
                return true;
            }
        }
        return false;
    };
    auto templateSection = [&](const std::string &section) -> int {
        for (size_t k = 0; k < sections.size(); ++k) {
            if (SameName(sections[k], section)) return static_cast<int>(k);
        }
        return -1;
    };

    // The file: its values, and the lines kept as written (per template section, or per other section).
    struct Value {
        std::string section, key, value;
    };
    std::vector<Value> values;
    std::vector<std::vector<std::string>> extras(sections.size());
    std::vector<std::pair<std::string, std::vector<std::string>>> otherSections;
    FilledIni r;
    {
        std::string section;
        for (const Span &s : Lines(text)) {
            const Parsed p = Parse(text, s);
            if (p.kind == Parsed::kSection) {
                section = Name(text, p);
                continue;
            }
            if (p.kind != Parsed::kKey && p.kind != Parsed::kInvalid) {
                continue; // comments and blank lines: the template's own replace them
            }
            if (p.kind == Parsed::kKey && inTemplate(section, Name(text, p))) {
                values.push_back(Value{section, Name(text, p), text.substr(p.valueBegin, p.valueEnd - p.valueBegin)});
                continue;
            }
            std::string raw = text.substr(s.begin, s.end - s.begin);
            while (!raw.empty() && Space(raw.back())) raw.pop_back();
            ++r.extra;
            const int k = templateSection(section);
            if (k >= 0) {
                extras[k].push_back(raw);
                continue;
            }
            bool added = false;
            for (auto &o : otherSections) {
                if (SameName(o.first, section)) {
                    o.second.push_back(raw);
                    added = true;
                }
            }
            if (!added) {
                otherSections.emplace_back(section, std::vector<std::string>{raw});
            }
        }
    }

    // The template's lines with the values; each section's kept lines after its last line that is not blank.
    std::vector<int> lastOf(sections.size(), -1);
    for (size_t i = 0; i < tl.size(); ++i) {
        if (tp[i].kind != Parsed::kEmpty) {
            lastOf[templateSection(tsec[i])] = static_cast<int>(i);
        }
    }
    std::vector<std::string> out;
    auto keep = [&](const std::vector<std::string> &lines) {
        out.push_back("");
        out.push_back("; Not described above (kept as written):");
        out.insert(out.end(), lines.begin(), lines.end());
    };
    for (size_t k = 0; k < sections.size(); ++k) {
        if (lastOf[k] < 0 && !extras[k].empty()) { // a section of the template with nothing in it: at its header
            for (size_t i = 0; i < tl.size(); ++i) {
                if (tp[i].kind == Parsed::kSection && SameName(Name(tmpl, tp[i]), sections[k])) lastOf[k] = static_cast<int>(i);
            }
        }
    }
    if (lastOf[0] < 0 && !extras[0].empty()) { // no header comment in the template: first in the file
        out.insert(out.end(), extras[0].begin(), extras[0].end());
        out.push_back("");
    }
    for (size_t i = 0; i < tl.size(); ++i) {
        std::string line = tmpl.substr(tl[i].begin, tl[i].end - tl[i].begin);
        if (tp[i].kind == Parsed::kKey) {
            const std::string key = Name(tmpl, tp[i]);
            const Value *v = nullptr;
            for (const Value &x : values) {
                if (SameName(x.section, tsec[i]) && SameName(x.key, key)) v = &x;
            }
            if (v) {
                ++r.kept;
                line = line.substr(0, tp[i].valueBegin - tl[i].begin) + v->value + line.substr(tp[i].valueEnd - tl[i].begin);
                while (!line.empty() && Space(line.back())) line.pop_back();
            }
            else {
                ++r.added;
            }
        }
        out.push_back(line);
        for (size_t k = 0; k < sections.size(); ++k) {
            if (lastOf[k] == static_cast<int>(i) && !extras[k].empty()) keep(extras[k]);
        }
    }
    if (!otherSections.empty()) {
        while (!out.empty() && out.back().empty()) out.pop_back();
        out.push_back("");
        out.push_back("; Sections not described above (kept as written):");
        for (const auto &o : otherSections) {
            out.push_back("[" + o.first + "]");
            out.insert(out.end(), o.second.begin(), o.second.end());
        }
    }

    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::string eol = crlf ? "\r\n" : "\n";
    r.text = text.substr(0, BomLength(text));
    const bool endsWithEol = !tmpl.empty() && tmpl.back() == '\n';
    for (size_t i = 0; i < out.size(); ++i) {
        r.text += out[i];
        if (i + 1 < out.size() || endsWithEol || !otherSections.empty()) r.text += eol;
    }
    return r;
}

} // namespace core::settings
