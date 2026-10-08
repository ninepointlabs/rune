#include "md_to_html.h"

#include <cctype>
#include <cstring>
#include <vector>

namespace {

void appendEscaped(std::string &out, char c)
{
    switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    default: out += c;
    }
}

std::string escape(const std::string &s)
{
    std::string out;
    for (const char c : s)
        appendEscaped(out, c);
    return out;
}

bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

// Closing delimiter for an emphasis run starting at `from`; npos if none.
// The closer must follow non-space text, and for '_' must not be intraword.
size_t findCloser(const std::string &s, size_t from, const std::string &delim)
{
    for (size_t j = from; (j = s.find(delim, j)) != std::string::npos; ++j) {
        if (j == from || s[j - 1] == ' ')
            continue;
        if (delim[0] == '_' && j + delim.size() < s.size() && isAlnum(s[j + delim.size()]))
            continue;
        if (delim.size() == 1 && j + 1 < s.size() && s[j + 1] == delim[0])
            continue; // part of a "**" run, not a single-char closer
        return j;
    }
    return std::string::npos;
}

std::string inlineHtml(const std::string &s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::ispunct(static_cast<unsigned char>(s[i + 1]))) {
            appendEscaped(out, s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`') {
            const size_t close = s.find('`', i + 1);
            if (close != std::string::npos) {
                out += "<code>" + escape(s.substr(i + 1, close - i - 1)) + "</code>";
                i = close + 1;
                continue;
            }
        }
        if (c == '[') {
            const size_t mid = s.find("](", i + 1);
            const size_t close = mid == std::string::npos ? mid : s.find(')', mid + 2);
            if (close != std::string::npos) {
                out += "<a href=\"" + escape(s.substr(mid + 2, close - mid - 2)) + "\">"
                    + inlineHtml(s.substr(i + 1, mid - i - 1)) + "</a>";
                i = close + 1;
                continue;
            }
        }
        if (c == '*' || c == '_') {
            const bool intraword = c == '_' && i > 0 && isAlnum(s[i - 1]);
            const bool strong = i + 1 < s.size() && s[i + 1] == c;
            const std::string delim(strong ? 2 : 1, c);
            const size_t start = i + delim.size();
            if (!intraword && start < s.size() && s[start] != ' ') {
                const size_t close = findCloser(s, start, delim);
                if (close != std::string::npos) {
                    const char *tag = strong ? "strong" : "em";
                    out += std::string("<") + tag + ">" + inlineHtml(s.substr(start, close - start))
                        + "</" + tag + ">";
                    i = close + delim.size();
                    continue;
                }
            }
        }
        appendEscaped(out, c);
        ++i;
    }
    return out;
}

std::string trim(const std::string &s)
{
    const size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

// "---", "***", "___" (3+ of one char, spaces allowed).
bool isRule(const std::string &t)
{
    if (t.empty() || (t[0] != '-' && t[0] != '*' && t[0] != '_'))
        return false;
    int n = 0;
    for (const char c : t) {
        if (c == t[0])
            ++n;
        else if (c != ' ')
            return false;
    }
    return n >= 3;
}

// Heading level 1..6, or 0. On success `text` is the heading content.
int headingLevel(const std::string &t, std::string &text)
{
    int level = 0;
    while (level < int(t.size()) && t[level] == '#')
        ++level;
    if (level == 0 || level > 6 || (level < int(t.size()) && t[level] != ' '))
        return 0;
    text = trim(t.substr(level));
    while (!text.empty() && text.back() == '#') // optional closing hashes
        text.pop_back();
    text = trim(text);
    return level;
}

// List item marker: "ul" for -, *, +; "ol" for 1. / 1); nullptr otherwise.
const char *listItem(const std::string &t, std::string &text)
{
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        text = trim(t.substr(2));
        return "ul";
    }
    size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d])))
        ++d;
    if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
        text = trim(t.substr(d + 2));
        return "ol";
    }
    return nullptr;
}

} // namespace

std::string mdToHtml(const std::string &md)
{
    std::vector<std::string> lines;
    for (size_t pos = 0; pos <= md.size();) {
        size_t nl = md.find('\n', pos);
        if (nl == std::string::npos)
            nl = md.size();
        std::string line = md.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        pos = nl + 1;
    }

    std::string body;
    std::string para;           // pending paragraph text, lines joined by spaces
    const char *list = nullptr; // open list tag, if any

    auto flushPara = [&] {
        if (!para.empty())
            body += "<p>" + inlineHtml(para) + "</p>\n";
        para.clear();
    };
    auto closeList = [&] {
        if (list)
            body += std::string("</") + list + ">\n";
        list = nullptr;
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i]);
        std::string text;

        if (t.rfind("```", 0) == 0 || t.rfind("~~~", 0) == 0) {
            flushPara();
            closeList();
            const std::string fence = t.substr(0, 3);
            body += "<pre><code>";
            bool first = true;
            for (++i; i < lines.size() && trim(lines[i]).rfind(fence, 0) != 0; ++i) {
                if (!first)
                    body += '\n';
                body += escape(lines[i]);
                first = false;
            }
            body += "</code></pre>\n";
            continue;
        }
        if (t.empty()) {
            flushPara();
            closeList();
            continue;
        }
        if (isRule(t)) {
            flushPara();
            closeList();
            body += "<hr>\n";
            continue;
        }
        if (const int level = headingLevel(t, text)) {
            flushPara();
            closeList();
            const std::string h = "h" + std::to_string(level);
            body += "<" + h + ">" + inlineHtml(text) + "</" + h + ">\n";
            continue;
        }
        if (const char *tag = listItem(t, text)) {
            flushPara();
            if (!list || std::strcmp(list, tag) != 0) {
                closeList();
                list = tag;
                body += std::string("<") + tag + ">\n";
            }
            body += "<li>" + inlineHtml(text) + "</li>\n";
            continue;
        }
        closeList();
        if (!para.empty())
            para += ' ';
        para += t;
    }
    flushPara();
    closeList();

    return "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
           "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\n"
           "</head>\n<body>\n"
        + body + "</body>\n</html>\n";
}
