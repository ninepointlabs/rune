// rune-engine: LibreOfficeKit behind a JSON-lines Unix socket.
//
// Usage: rune-engine [--socket-path PATH]
//
// One JSON object per line in both directions; see README.md for the
// protocol. Single-threaded: LOK is not thread-safe, so every command runs on
// the main thread inside a poll() loop. Logs go to stderr; stdout is unused.
//
// LOK callbacks arrive on LibreOffice's own main-loop thread. They only queue
// the raw payload and wake the poll loop through an eventfd; parsing and
// broadcasting to clients happens back on our thread.

#include <LibreOfficeKit/LibreOfficeKitInit.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include "ai_manager.h"
#include "md_to_html.h"

#include <png.h>

#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

void logf(const char *fmt, ...)
{
    std::fputs("rune-engine: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

// ---------------------------------------------------------------------------
// Minimal JSON: enough to parse command objects and emit flat replies.

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    const Json *get(const std::string &key) const
    {
        if (type != Object)
            return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : p(text.data()), end(text.data() + text.size()) {}

    bool parse(Json &out)
    {
        ws();
        if (!value(out, 0))
            return false;
        ws();
        return p == end;
    }

private:
    const char *p;
    const char *end;

    void ws()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }

    bool literal(const char *word)
    {
        const size_t len = std::strlen(word);
        if (size_t(end - p) < len || std::memcmp(p, word, len) != 0)
            return false;
        p += len;
        return true;
    }

    bool value(Json &v, int depth)
    {
        if (depth > 32 || p >= end)
            return false;
        switch (*p) {
        case '{': return object(v, depth);
        case '[': return array(v, depth);
        case '"': v.type = Json::String; return string(v.s);
        case 't': v.type = Json::Bool; v.b = true; return literal("true");
        case 'f': v.type = Json::Bool; v.b = false; return literal("false");
        case 'n': v.type = Json::Null; return literal("null");
        default: return number(v);
        }
    }

    bool number(Json &v)
    {
        // from_chars is locale-independent, unlike strtod (LOK may setlocale).
        auto [next, ec] = std::from_chars(p, end, v.n);
        if (ec != std::errc() || next == p)
            return false;
        v.type = Json::Number;
        p = next;
        return true;
    }

    static void appendUtf8(std::string &out, unsigned cp)
    {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned &cp)
    {
        if (end - p < 4)
            return false;
        auto [next, ec] = std::from_chars(p, p + 4, cp, 16);
        if (ec != std::errc() || next != p + 4)
            return false;
        p = next;
        return true;
    }

    bool string(std::string &out)
    {
        ++p; // opening quote
        out.clear();
        while (p < end) {
            const char c = *p++;
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end)
                return false;
            switch (*p++) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00) {
                    unsigned lo;
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                        return false;
                    p += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return false;
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool array(Json &v, int depth)
    {
        v.type = Json::Array;
        ++p;
        ws();
        if (p < end && *p == ']') {
            ++p;
            return true;
        }
        while (true) {
            v.arr.emplace_back();
            ws();
            if (!value(v.arr.back(), depth + 1))
                return false;
            ws();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == ']') {
                ++p;
                return true;
            }
            return false;
        }
    }

    bool object(Json &v, int depth)
    {
        v.type = Json::Object;
        ++p;
        ws();
        if (p < end && *p == '}') {
            ++p;
            return true;
        }
        while (true) {
            ws();
            std::string key;
            if (p >= end || *p != '"' || !string(key))
                return false;
            ws();
            if (p >= end || *p++ != ':')
                return false;
            ws();
            if (!value(v.obj[key], depth + 1))
                return false;
            ws();
            if (p < end && *p == ',') {
                ++p;
                continue;
            }
            if (p < end && *p == '}') {
                ++p;
                return true;
            }
            return false;
        }
    }
};

std::string jsonQuote(const std::string &s)
{
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + '"';
}

// Echo the request id back verbatim (numbers and strings; anything else -> null).
std::string jsonScalar(const Json *v)
{
    if (!v)
        return "null";
    switch (v->type) {
    case Json::String: return jsonQuote(v->s);
    case Json::Bool: return v->b ? "true" : "false";
    case Json::Number: {
        char buf[32];
        if (std::trunc(v->n) == v->n && std::fabs(v->n) < 1e15)
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v->n));
        else
            std::snprintf(buf, sizeof buf, "%.17g", v->n);
        return buf;
    }
    default: return "null";
    }
}

// Serializes any parsed value back to JSON.
std::string jsonDump(const Json &v)
{
    std::string out;
    switch (v.type) {
    case Json::Array:
        out = "[";
        for (const Json &e : v.arr) {
            if (out.size() > 1)
                out += ',';
            out += jsonDump(e);
        }
        return out + ']';
    case Json::Object:
        out = "{";
        for (const auto &[k, e] : v.obj) {
            if (out.size() > 1)
                out += ',';
            out += jsonQuote(k) + ':' + jsonDump(e);
        }
        return out + '}';
    default:
        return jsonScalar(&v);
    }
}

// Builds one reply line. The default constructor omits "id": that form is
// for push events, which are not replies to anything.
class Reply {
public:
    Reply() = default;
    explicit Reply(const Json *id) { raw("id", jsonScalar(id)); }

    Reply &raw(const char *key, const std::string &json)
    {
        if (m_s.size() > 1)
            m_s += ',';
        m_s += jsonQuote(key);
        m_s += ':';
        m_s += json;
        return *this;
    }
    Reply &num(const char *key, long long v) { return raw(key, std::to_string(v)); }
    Reply &str(const char *key, const std::string &v) { return raw(key, jsonQuote(v)); }
    Reply &ok(bool v) { return raw("ok", v ? "true" : "false"); }

    std::string line() const { return m_s + "}\n"; }

private:
    std::string m_s = "{";
};

std::string errorReply(const Json *id, const std::string &message)
{
    return Reply(id).ok(false).str("error", message).line();
}

// Reads an integer field; false if missing/non-integral/out of range.
bool getInt(const Json &req, const char *key, long long &out)
{
    const Json *v = req.get(key);
    if (!v || v->type != Json::Number || std::trunc(v->n) != v->n || std::fabs(v->n) > 1e12)
        return false;
    out = static_cast<long long>(v->n);
    return true;
}

// ---------------------------------------------------------------------------
// Tile encoding: LOK premultiplied BGRA/RGBA -> straight RGBA PNG -> base64.

std::string base64(const unsigned char *data, size_t len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < len) {
        unsigned v = data[i] << 16;
        if (i + 1 < len)
            v |= data[i + 1] << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

void toStraightRgba(std::vector<unsigned char> &px, bool bgra)
{
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        unsigned char *q = &px[i];
        if (bgra)
            std::swap(q[0], q[2]);
        const unsigned a = q[3];
        if (a == 0) {
            q[0] = q[1] = q[2] = 0;
        } else if (a < 255) {
            for (int c = 0; c < 3; ++c)
                q[c] = static_cast<unsigned char>(std::min(255u, (q[c] * 255u + a / 2) / a));
        }
    }
}

bool encodePng(const std::vector<unsigned char> &rgba, int w, int h, std::vector<unsigned char> &out)
{
    png_image img;
    std::memset(&img, 0, sizeof img);
    img.version = PNG_IMAGE_VERSION;
    img.width = w;
    img.height = h;
    img.format = PNG_FORMAT_RGBA;

    png_alloc_size_t size = 0;
    if (!png_image_write_get_memory_size(img, size, 0, rgba.data(), 0, nullptr))
        return false;
    out.resize(size);
    if (!png_image_write_to_memory(&img, out.data(), &size, 0, rgba.data(), 0, nullptr)) {
        logf("png encode failed: %s", img.message);
        return false;
    }
    out.resize(size);
    return true;
}

// ---------------------------------------------------------------------------
// Document helpers.

// Percent-encode an absolute path into a file:// URL.
std::string fileUrl(const std::string &absPath)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string url = "file://";
    for (const unsigned char c : absPath) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            url += char(c);
        } else {
            url += '%';
            url += hex[c >> 4];
            url += hex[c & 15];
        }
    }
    return url;
}

struct Rect {
    long x = 0, y = 0, w = 0, h = 0;
};

// LOK rectangle lists: "x, y, w, h; x, y, w, h; ..." in twips (page rects,
// text selections). Trailing per-rect fields (e.g. part) are ignored.
std::vector<Rect> parseRects(const char *s)
{
    std::vector<Rect> rects;
    if (!s)
        return rects;
    const char *p = s;
    while (*p) {
        Rect r;
        if (std::sscanf(p, " %ld , %ld , %ld , %ld", &r.x, &r.y, &r.w, &r.h) == 4)
            rects.push_back(r);
        p = std::strchr(p, ';');
        if (!p)
            break;
        ++p;
    }
    return rects;
}

std::string rectJson(const Rect &r)
{
    return "[" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + ","
        + std::to_string(r.h) + "]";
}

std::string rectsJson(const std::vector<Rect> &rects)
{
    std::string out = "[";
    for (const Rect &r : rects) {
        if (out.size() > 1)
            out += ',';
        out += rectJson(r);
    }
    return out + ']';
}

// A single LOK rectangle. "EMPTY" (invalidate everything) maps to the
// largest possible area, matching LOK's own convention.
bool parseRect(const std::string &s, Rect &r)
{
    if (s.rfind("EMPTY", 0) == 0) {
        r = {0, 0, INT_MAX, INT_MAX};
        return true;
    }
    return std::sscanf(s.c_str(), " %ld , %ld , %ld , %ld", &r.x, &r.y, &r.w, &r.h) == 4;
}

// The visible-cursor payload is either a bare rectangle or, with
// LOK_FEATURE_VIEWID_IN_VISCURSOR_INVALIDATION_CALLBACK, JSON carrying one
// under "rectangle".
bool parseCursorRect(const std::string &s, Rect &r)
{
    if (s.empty() || s[0] != '{')
        return parseRect(s, r);
    Json j;
    if (!JsonParser(s).parse(j))
        return false;
    const Json *rect = j.get("rectangle");
    return rect && rect->type == Json::String && parseRect(rect->s, r);
}

// A plain STATE_CHANGED value as JSON: true/false stay booleans, anything
// else ("12", "Liberation Serif", "disabled") is a string.
std::string stateValueJson(const std::string &v)
{
    return v == "true" || v == "false" ? v : jsonQuote(v);
}

// STATE_CHANGED payload -> (key without ".uno:", JSON value) pairs. Usually
// ".uno:Bold=true", possibly several joined by ';' (split only before
// ".uno:", so values may contain ';'), or JSON {"commandName", "state"}.
std::vector<std::pair<std::string, std::string>> parseStateChange(const std::string &s)
{
    static const std::string prefix = ".uno:";
    std::vector<std::pair<std::string, std::string>> out;
    if (!s.empty() && s[0] == '{') {
        Json j;
        const Json *name = nullptr, *state = nullptr;
        if (JsonParser(s).parse(j) && (name = j.get("commandName")) && name->type == Json::String
            && name->s.rfind(prefix, 0) == 0 && (state = j.get("state"))) {
            out.emplace_back(name->s.substr(prefix.size()),
                             state->type == Json::String ? stateValueJson(state->s) : jsonDump(*state));
        }
        return out;
    }
    for (size_t pos = s.find(prefix); pos != std::string::npos;) {
        const size_t next = s.find(";" + prefix, pos);
        const std::string item = s.substr(pos + prefix.size(),
                                          next == std::string::npos ? std::string::npos : next - pos - prefix.size());
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0)
            out.emplace_back(item.substr(0, eq), stateValueJson(item.substr(eq + 1)));
        pos = next == std::string::npos ? next : next + 1;
    }
    return out;
}

// `style` names -> Writer programmatic paragraph style names for
// .uno:StyleApply (UI names like "Body Text" are not found). STATE_CHANGED
// reports StyleApply with UI names: "Default Paragraph Style", "Body Text".
const std::map<std::string, const char *> kParaStyles = {
    {"Normal", "Standard"}, {"Text Body", "Text body"},
    {"Heading 1", "Heading 1"}, {"Heading 2", "Heading 2"}, {"Heading 3", "Heading 3"},
};

// `format` args -> the LOK typed-argument JSON postUnoCommand expects. A
// JSON object passes through; a plain value is wrapped for the commands
// that take one (font name, size in points).
bool unoArgs(const std::string &command, const std::string &value, std::string &out, std::string &error)
{
    if (!value.empty() && value[0] == '{') {
        Json j;
        if (!JsonParser(value).parse(j) || j.type != Json::Object) {
            error = "\"args\" is not a JSON object";
            return false;
        }
        out = value;
        return true;
    }
    if (command == ".uno:CharFontName") {
        if (value.empty()) {
            error = "empty font name";
            return false;
        }
        out = "{\"CharFontName.FamilyName\":{\"type\":\"string\",\"value\":" + jsonQuote(value) + "}}";
        return true;
    }
    if (command == ".uno:FontHeight") {
        char *end = nullptr;
        const double pt = std::strtod(value.c_str(), &end);
        if (value.empty() || *end != '\0' || !(pt >= 1 && pt <= 999)) {
            error = "invalid font size: " + value;
            return false;
        }
        out = "{\"FontHeight.Height\":{\"type\":\"float\",\"value\":" + jsonQuote(value) + "}}";
        return true;
    }
    error = "plain \"args\" not supported for " + command + "; pass a JSON object";
    return false;
}

// "#RRGGBB" -> R*65536 + G*256 + B; "auto" -> -1 (COL_AUTO as a UNO long).
bool parseColor(const std::string &hex, long long &value)
{
    if (hex == "auto") {
        value = -1;
        return true;
    }
    if (hex.size() != 7 || hex[0] != '#'
        || hex.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
        return false;
    value = std::stoll(hex.substr(1), nullptr, 16);
    return true;
}

// VCL key codes (vcl/keycodes.hxx, == css::awt::Key), which is not installed
// with the SDK. Values checked against offapi.rdb for LO 26.8.
const std::map<std::string, int> kKeyNames = {
    {"Down", 1024},     {"Up", 1025},     {"Left", 1026},   {"Right", 1027},
    {"Home", 1028},     {"End", 1029},    {"PageUp", 1030}, {"PageDown", 1031},
    {"Return", 1280},   {"Enter", 1280},  {"Escape", 1281}, {"Tab", 1282},
    {"Backspace", 1283}, {"Space", 1284}, {"Insert", 1285}, {"Delete", 1286},
};

// `save` formats -> LOK saveAs format strings.
const std::map<std::string, const char *> kSaveFormats = {
    {"docx", "docx"}, {"odt", "odt"}, {"pdf", "pdf"}, {"txt", "txt"}, {"doc", "doc"},
};

std::string baseName(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Absolute form of a path whose directory must exist (the file need not).
bool resolveTargetPath(const std::string &path, std::string &out, std::string &error)
{
    const size_t slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    const std::string name = baseName(path);
    if (name.empty() || name == "." || name == "..") {
        error = path + ": not a file path";
        return false;
    }
    char *abs = realpath(dir.c_str(), nullptr);
    if (!abs) {
        error = dir + ": " + std::strerror(errno);
        return false;
    }
    out = abs;
    std::free(abs);
    if (out != "/")
        out += '/';
    out += name;
    return true;
}

// mkdir -p, owner-only for anything it creates.
bool makeDirs(const std::string &dir)
{
    for (size_t pos = 1; pos <= dir.size(); ++pos) {
        if (pos != dir.size() && dir[pos] != '/')
            continue;
        const std::string prefix = dir.substr(0, pos);
        if (mkdir(prefix.c_str(), 0700) < 0 && errno != EEXIST)
            return false;
    }
    return true;
}

// $XDG_STATE_HOME/rune/autosave, else ~/.local/state/rune/autosave.
std::string autosaveDir()
{
    const char *state = std::getenv("XDG_STATE_HOME");
    if (state && *state == '/')
        return std::string(state) + "/rune/autosave";
    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/.local/state/rune/autosave";
}

// Seconds between autosaves; RUNE_AUTOSAVE_INTERVAL overrides (for tests).
std::chrono::milliseconds autosaveInterval()
{
    const char *env = std::getenv("RUNE_AUTOSAVE_INTERVAL");
    double secs = 30;
    if (env && *env) {
        double v = 0;
        auto [next, ec] = std::from_chars(env, env + std::strlen(env), v);
        if (ec == std::errc() && *next == '\0' && v > 0)
            secs = v;
    }
    return std::chrono::milliseconds(std::llround(secs * 1000));
}

// ---------------------------------------------------------------------------
// Command dispatch.

constexpr long long kMaxTilePx = 8192;

class Engine {
public:
    explicit Engine(lok::Office *office)
        : m_office(office), m_autosaveDir(autosaveDir()), m_autosaveInterval(autosaveInterval())
    {
        m_wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (m_wakeFd < 0)
            logf("eventfd: %s", std::strerror(errno));
    }

    bool quitRequested() const { return m_quit; }

    // Readable when LOK callbacks have queued events; see takeEvents().
    int eventFd() const { return m_wakeFd; }

    // Drains queued LOK callbacks into push-event lines for all clients.
    std::vector<std::string> takeEvents()
    {
        uint64_t count;
        while (read(m_wakeFd, &count, sizeof count) > 0) {
        }
        std::vector<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            pending.swap(m_queue);
        }
        std::vector<std::string> lines;
        for (const Pending &p : pending) {
            std::string line = formatEvent(p);
            if (!line.empty())
                lines.push_back(std::move(line));
        }
        return lines;
    }

    std::string handle(const std::string &line)
    {
        Json req;
        if (!JsonParser(line).parse(req) || req.type != Json::Object)
            return errorReply(nullptr, "invalid JSON");
        const Json *id = req.get("id");
        const Json *cmd = req.get("cmd");
        if (!cmd || cmd->type != Json::String)
            return errorReply(id, "missing \"cmd\"");

        if (cmd->s == "ping")
            return Reply(id).ok(true).line();
        if (cmd->s == "open")
            return open(req, id);
        if (cmd->s == "tile")
            return tile(req, id);
        if (cmd->s == "close")
            return close(req, id);
        if (cmd->s == "key")
            return key(req, id);
        if (cmd->s == "paste")
            return paste(req, id);
        if (cmd->s == "mouse")
            return mouse(req, id);
        if (cmd->s == "copy")
            return copy(req, id, false);
        if (cmd->s == "cut")
            return copy(req, id, true);
        if (cmd->s == "new_md")
            return newMd(req, id);
        if (cmd->s == "export_md")
            return exportMd(req, id);
        if (cmd->s == "save")
            return save(req, id);
        if (cmd->s == "autosave")
            return autosave(req, id);
        if (cmd->s == "format")
            return format(req, id);
        if (cmd->s == "style")
            return style(req, id);
        if (cmd->s == "color")
            return color(req, id);
        if (cmd->s == "para")
            return para(req, id);
        if (cmd->s == "get_state")
            return getState(req, id);
        if (cmd->s == "get_char_style")
            return getCharStyle(req, id);
        if (cmd->s == "apply_char_style")
            return applyCharStyle(req, id);
        if (cmd->s == "ai")
            return ai(req, id);
        if (cmd->s == "quit") {
            m_quit = true;
            return Reply(id).ok(true).line();
        }
        return errorReply(id, "unknown cmd: " + cmd->s);
    }

    // poll() timeout until the next autosave round; -1 when none is enabled.
    int autosaveTimeoutMs() const
    {
        if (!autosaveActive())
            return -1;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            m_nextAutosave - std::chrono::steady_clock::now());
        return int(std::max<long long>(0, left.count()));
    }

    // Runs an autosave round if one is due; returns push events for clients.
    std::vector<std::string> autosaveIfDue()
    {
        std::vector<std::string> lines;
        if (!autosaveActive() || std::chrono::steady_clock::now() < m_nextAutosave)
            return lines;
        m_nextAutosave = std::chrono::steady_clock::now() + m_autosaveInterval;
        for (auto &[docId, st] : m_docs) {
            if (!st.autosave || !st.dirty)
                continue;
            const std::string path = writeAutosave(docId, st);
            if (!path.empty())
                lines.push_back(Reply().str("event", "autosaved").num("doc_id", docId).str("path", path).line());
        }
        return lines;
    }

    // Engine shutdown: autosave every enabled document with unsaved edits.
    // The files are kept for recovery.
    void shutdown()
    {
        for (auto &[docId, st] : m_docs) {
            if (st.autosave && st.dirty)
                writeAutosave(docId, st);
        }
    }

private:
    // registerCallback() context: which engine and document a callback is for.
    struct CallbackCtx {
        Engine *engine;
        long long docId;
    };
    struct Pending {
        long long docId;
        int type;
        std::string payload;
    };
    // Main-thread-only per-document state.
    struct DocState {
        std::unique_ptr<lok::Document> doc;
        std::unique_ptr<CallbackCtx> ctx;
        std::string selStart, selEnd; // last TEXT_SELECTION_START/END rects
        std::vector<Rect> pages;      // page rects in twips; never empty
        std::string path;             // absolute save target; empty for new_md
        bool autosave = false;
        bool dirty = false;           // edited since the last save/autosave
        // Latest STATE_CHANGED value per command (key without ".uno:") as JSON.
        std::map<std::string, std::string> state;
    };

    lok::Office *m_office; // never destroyed; see main()
    AiManager m_ai;
    std::map<long long, DocState> m_docs;
    long long m_nextDocId = 0;
    bool m_quit = false;

    const std::string m_autosaveDir;
    const std::chrono::milliseconds m_autosaveInterval;
    std::chrono::steady_clock::time_point m_nextAutosave;

    int m_wakeFd = -1;
    std::mutex m_queueMutex; // guards m_queue; LOK calls back on its own thread
    std::vector<Pending> m_queue;

    // Runs on LibreOffice's main-loop thread: queue and wake, nothing else.
    static void onLokCallback(int type, const char *payload, void *data)
    {
        if (getenv("RUNE_DEBUG_CB")) logf("cb %d: %.200s", type, payload ? payload : "(null)");
        switch (type) {
        case LOK_CALLBACK_INVALIDATE_TILES:
        case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR:
        case LOK_CALLBACK_TEXT_SELECTION:
        case LOK_CALLBACK_TEXT_SELECTION_START:
        case LOK_CALLBACK_TEXT_SELECTION_END:
        case LOK_CALLBACK_CURSOR_VISIBLE:
        case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED:
        case LOK_CALLBACK_STATE_CHANGED:
            break;
        default:
            return;
        }
        auto *ctx = static_cast<CallbackCtx *>(data);
        Engine *self = ctx->engine;
        {
            std::lock_guard<std::mutex> lock(self->m_queueMutex);
            self->m_queue.push_back({ctx->docId, type, payload ? payload : ""});
        }
        const uint64_t one = 1;
        if (write(self->m_wakeFd, &one, sizeof one) < 0 && errno != EAGAIN)
            logf("eventfd write: %s", std::strerror(errno));
    }

    // One queued callback -> one push-event line ("" to drop it).
    // Page rects in twips. Non-Writer documents report none; use the whole
    // part as a single page so callers always get at least one rect.
    static std::vector<Rect> pageRects(lok::Document *doc, long docW, long docH)
    {
        char *rectStr = doc->getPartPageRectangles();
        std::vector<Rect> pages = parseRects(rectStr);
        std::free(rectStr);
        if (pages.empty())
            pages.push_back(Rect{0, 0, docW, docH});
        return pages;
    }

    std::string formatEvent(const Pending &p)
    {
        auto it = m_docs.find(p.docId);
        if (it == m_docs.end())
            return {}; // closed since the callback fired
        DocState &st = it->second;
        Rect r;

        switch (p.type) {
        case LOK_CALLBACK_INVALIDATE_TILES:
        case LOK_CALLBACK_INVALIDATE_VISIBLE_CURSOR: {
            const bool tiles = p.type == LOK_CALLBACK_INVALIDATE_TILES;
            if (!(tiles ? parseRect(p.payload, r) : parseCursorRect(p.payload, r))) {
                logf("doc %lld: unparsed %s payload: %s", p.docId,
                     tiles ? "INVALIDATE_TILES" : "INVALIDATE_VISIBLE_CURSOR", p.payload.c_str());
                return {};
            }
            return Reply()
                .str("event", tiles ? "tiles_changed" : "cursor_changed")
                .num("doc_id", p.docId)
                .num("x", r.x)
                .num("y", r.y)
                .num("width", r.w)
                .num("height", r.h)
                .line();
        }
        case LOK_CALLBACK_TEXT_SELECTION_START:
            st.selStart = p.payload;
            return {};
        case LOK_CALLBACK_TEXT_SELECTION_END:
            st.selEnd = p.payload;
            return {};
        case LOK_CALLBACK_TEXT_SELECTION: {
            // LOK sends START/END before every TEXT_SELECTION, so they are
            // current here. Empty payload means the selection was cleared.
            const std::vector<Rect> rects = parseRects(p.payload.c_str());
            Reply ev;
            ev.str("event", "selection_changed").num("doc_id", p.docId).raw("rects", rectsJson(rects));
            if (!rects.empty()) {
                if (parseRect(st.selStart, r))
                    ev.raw("start", rectJson(r));
                if (parseRect(st.selEnd, r))
                    ev.raw("end", rectJson(r));
            }
            return ev.line();
        }
        case LOK_CALLBACK_CURSOR_VISIBLE:
            return Reply()
                .str("event", "cursor_visible")
                .num("doc_id", p.docId)
                .raw("visible", p.payload == "true" ? "true" : "false")
                .line();
        case LOK_CALLBACK_DOCUMENT_SIZE_CHANGED: {
            long w = 0, h = 0;
            if (std::sscanf(p.payload.c_str(), " %ld , %ld", &w, &h) != 2)
                st.doc->getDocumentSize(&w, &h);
            // Pages come and go as the document grows or shrinks; keep the
            // whole document "visible" so LOK reports cursor/tiles everywhere.
            st.pages = pageRects(st.doc.get(), w, h);
            long curW = 0, curH = 0;
            st.doc->getDocumentSize(&curW, &curH);
            st.doc->setClientVisibleArea(0, 0, int(std::max(w, curW)), int(std::max(h, curH)));
            return Reply()
                .str("event", "size_changed")
                .num("doc_id", p.docId)
                .raw("doc_size", "[" + std::to_string(w) + "," + std::to_string(h) + "]")
                .num("pages", static_cast<long long>(st.pages.size()))
                .raw("page_rects", rectsJson(st.pages))
                .line();
        }
        case LOK_CALLBACK_STATE_CHANGED: {
                    // Accumulate into cached state; don't push (LO fires ~140
                    // individual callbacks per document open).
                    for (const auto &[key, value] : parseStateChange(p.payload))
                        st.state[key] = value;
                    return {};
                }
        }
        return {};
    }

    DocState *findState(const Json &req, long long &docId)
    {
        if (!getInt(req, "doc_id", docId))
            return nullptr;
        auto it = m_docs.find(docId);
        return it == m_docs.end() ? nullptr : &it->second;
    }

    lok::Document *findDoc(const Json &req)
    {
        long long docId;
        DocState *st = findState(req, docId);
        return st ? st->doc.get() : nullptr;
    }

    bool autosaveActive() const
    {
        return std::any_of(m_docs.begin(), m_docs.end(), [](const auto &d) { return d.second.autosave; });
    }

    std::string autosavePath(long long docId, const DocState &st) const
    {
        const std::string name = st.path.empty() ? "untitled.docx" : baseName(st.path);
        return m_autosaveDir + "/" + std::to_string(docId) + "_" + name;
    }

    // Writes the autosave copy (format from its extension); "" on failure.
    std::string writeAutosave(long long docId, DocState &st)
    {
        if (!makeDirs(m_autosaveDir)) {
            logf("autosave: mkdir %s: %s", m_autosaveDir.c_str(), std::strerror(errno));
            return {};
        }
        const std::string path = autosavePath(docId, st);
        if (!st.doc->saveAs(fileUrl(path).c_str(), nullptr, nullptr)) {
            logf("autosave doc %lld to %s failed: %s", docId, path.c_str(), lokError().c_str());
            return {};
        }
        st.dirty = false;
        logf("autosaved doc %lld to %s", docId, path.c_str());
        return path;
    }

    std::string save(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");

        const Json *pathArg = req.get("path");
        const Json *formatArg = req.get("format");
        if (pathArg && (pathArg->type != Json::String || pathArg->s.empty()))
            return errorReply(id, "invalid \"path\"");
        if (formatArg && formatArg->type != Json::String)
            return errorReply(id, "invalid \"format\"");

        // No format: LOK picks the filter from the target's extension, which
        // for the stored path is the format the document was opened in.
        const char *format = nullptr;
        const char *filterOptions = nullptr;
        if (formatArg) {
            auto it = kSaveFormats.find(formatArg->s);
            if (it == kSaveFormats.end())
                return errorReply(id, "unknown format: " + formatArg->s + " (docx, odt, pdf, txt, doc)");
            format = it->second;
            if (formatArg->s == "txt")
                filterOptions = "UTF8";
        }

        std::string target;
        if (pathArg) {
            std::string error;
            if (!resolveTargetPath(pathArg->s, target, error))
                return errorReply(id, error);
        } else if (st->path.empty()) {
            return errorReply(id, "document has no path yet; pass \"path\"");
        } else {
            target = st->path;
        }

        if (!st->doc->saveAs(fileUrl(target).c_str(), format, filterOptions))
            return errorReply(id, "saveAs " + target + " failed: " + lokError());
        logf("saved doc %lld to %s", docId, target.c_str());

        // PDF is an export: the document keeps saving to its editable path.
        if (!(formatArg && formatArg->s == "pdf")) {
            unlink(autosavePath(docId, *st).c_str());
            st->path = target;
            st->dirty = false;
        }
        return Reply(id).ok(true).str("path", target).line();
    }

    std::string autosave(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        const Json *enabled = req.get("enabled");
        if (!enabled || enabled->type != Json::Bool)
            return errorReply(id, "\"enabled\" must be true or false");
        // The first enabled document starts the clock.
        if (enabled->b && !autosaveActive())
            m_nextAutosave = std::chrono::steady_clock::now() + m_autosaveInterval;
        st->autosave = enabled->b;
        return Reply(id).ok(true).line();
    }

    std::string open(const Json &req, const Json *id)
    {
        const Json *path = req.get("path");
        if (!path || path->type != Json::String || path->s.empty())
            return errorReply(id, "missing \"path\"");

        char *abs = realpath(path->s.c_str(), nullptr);
        if (!abs)
            return errorReply(id, path->s + ": " + std::strerror(errno));
        const std::string absPath = abs;
        std::free(abs);

        std::unique_ptr<lok::Document> doc(m_office->documentLoad(fileUrl(absPath).c_str()));
        if (!doc)
            return errorReply(id, "documentLoad failed: " + lokError());
        doc->initializeForRendering();
        return addDoc(std::move(doc), id, absPath, absPath);
    }

    std::string lokError()
    {
        char *err = m_office->getError();
        std::string msg = err && *err ? err : "unknown error";
        m_office->freeError(err);
        return msg;
    }

    // A new Writer document seeded with Markdown (converted to HTML and pasted).
    std::string newMd(const Json &req, const Json *id)
    {
        const Json *md = req.get("markdown");
        if (!md || md->type != Json::String)
            return errorReply(id, "missing \"markdown\"");

        std::unique_ptr<lok::Document> doc(m_office->documentLoad("private:factory/swriter"));
        if (!doc)
            return errorReply(id, "documentLoad failed: " + lokError());
        doc->initializeForRendering();

        if (!md->s.empty()) {
            const std::string html = mdToHtml(md->s);
            // A fresh factory document's edit view can still be settling when
            // many documents have cycled through this process; paste() can
            // transiently fail right after documentLoad(). Not seen on a
            // freshly-started engine, only after several prior open/closes,
            // so retry briefly rather than failing a document that is
            // otherwise perfectly fine.
            bool pasted = false;
            for (int attempt = 0; attempt < 5 && !pasted; ++attempt) {
                if (attempt > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                pasted = doc->paste("text/html", html.data(), html.size());
            }
            if (!pasted)
                return errorReply(id, "paste failed for text/html");
        }
        return addDoc(std::move(doc), id, "(new markdown document)", {});
    }

    // Plain-text export reshaped as Markdown paragraphs. Lossy: formatting
    // (headings, emphasis, lists) is not preserved, only the text.
    std::string exportMd(const Json &req, const Json *id)
    {
        lok::Document *doc = findDoc(req);
        if (!doc)
            return errorReply(id, "unknown doc_id");

        const char *tmpRoot = std::getenv("TMPDIR");
        std::string dir = std::string(tmpRoot && *tmpRoot ? tmpRoot : "/tmp") + "/rune-export-XXXXXX";
        if (!mkdtemp(dir.data()))
            return errorReply(id, "mkdtemp: " + std::string(std::strerror(errno)));
        const std::string file = dir + "/export.txt";

        std::string text;
        std::string error;
        if (!doc->saveAs(fileUrl(file).c_str(), "txt", "UTF8")) {
            error = "saveAs failed: " + lokError();
        } else if (FILE *f = std::fopen(file.c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
                text.append(buf, n);
            std::fclose(f);
        } else {
            error = file + ": " + std::strerror(errno);
        }
        unlink(file.c_str());
        rmdir(dir.c_str());
        if (!error.empty())
            return errorReply(id, error);

        if (text.rfind("\xEF\xBB\xBF", 0) == 0) // UTF-8 BOM
            text.erase(0, 3);
        std::string markdown;
        for (size_t pos = 0; pos < text.size();) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos)
                nl = text.size();
            const std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            // Trim: LO indents list items, which Markdown would read as code.
            const size_t b = line.find_first_not_of(" \t\r");
            if (b == std::string::npos)
                continue;
            if (!markdown.empty())
                markdown += "\n\n";
            markdown += line.substr(b, line.find_last_not_of(" \t\r") - b + 1);
        }
        if (!markdown.empty())
            markdown += '\n';
        return Reply(id).ok(true).str("markdown", markdown).line();
    }

    // Shared tail of open/new_md: takes a document already initialized for
    // rendering, registers it and replies with its layout. `path` is where
    // `save` writes by default ("" if the document has none yet).
    std::string addDoc(std::unique_ptr<lok::Document> doc, const Json *id, const std::string &what,
                       const std::string &path)
    {
        long docW = 0, docH = 0;
        doc->getDocumentSize(&docW, &docH);
        std::vector<Rect> pages = pageRects(doc.get(), docW, docH);
        const int parts = doc->getParts();

        const long long docId = m_nextDocId++;
        auto ctx = std::make_unique<CallbackCtx>(CallbackCtx{this, docId});
        doc->registerCallback(&Engine::onLokCallback, ctx.get());
        doc->setClientVisibleArea(0, 0, int(docW), int(docH));
        logf("opened doc %lld: %s (%d part(s), %zu page(s))", docId, what.c_str(), parts,
             pages.size());

        Reply reply(id);
        reply.ok(true)
            .num("doc_id", docId)
            .num("parts", parts)
            .num("pages", static_cast<long long>(pages.size()))
            .raw("page_rect", rectJson(pages.front()))
            .raw("page_rects", rectsJson(pages))
            .raw("doc_size", "[" + std::to_string(docW) + "," + std::to_string(docH) + "]");
        m_docs[docId] = DocState{std::move(doc), std::move(ctx), {}, {}, std::move(pages), path, false, false, {}};
        return reply.line();
    }

    std::string tile(const Json &req, const Json *id)
    {
        lok::Document *doc = findDoc(req);
        if (!doc)
            return errorReply(id, "unknown doc_id");

        long long part = 0, x, y, w, h;
        if (req.get("part") && !getInt(req, "part", part))
            return errorReply(id, "invalid \"part\"");
        if (!getInt(req, "x", x) || !getInt(req, "y", y) || !getInt(req, "width", w)
            || !getInt(req, "height", h) || w <= 0 || h <= 0 || std::llabs(x) > INT_MAX
            || std::llabs(y) > INT_MAX || w > INT_MAX || h > INT_MAX)
            return errorReply(id, "x, y, width, height (twips) are required; width/height > 0");
        if (part < 0 || part >= doc->getParts())
            return errorReply(id, "part out of range");

        // Output size in pixels. Default is 96 DPI (15 twips per pixel); if
        // only one dimension is given the other follows the tile's aspect.
        long long pxW = 0, pxH = 0;
        const bool hasW = req.get("px_width") != nullptr, hasH = req.get("px_height") != nullptr;
        if ((hasW && !getInt(req, "px_width", pxW)) || (hasH && !getInt(req, "px_height", pxH)))
            return errorReply(id, "invalid px_width/px_height");
        if (!hasW && !hasH) {
            pxW = std::llround(w / 15.0);
            pxH = std::llround(h / 15.0);
        } else if (!hasH) {
            pxH = std::llround(double(pxW) * h / w);
        } else if (!hasW) {
            pxW = std::llround(double(pxH) * w / h);
        }
        if (pxW <= 0 || pxH <= 0 || pxW > kMaxTilePx || pxH > kMaxTilePx)
            return errorReply(id, "tile pixel size must be 1.." + std::to_string(kMaxTilePx));

        if (doc->getPart() != part)
            doc->setPart(static_cast<int>(part));

        std::vector<unsigned char> buf(size_t(pxW) * size_t(pxH) * 4);
        doc->paintTile(buf.data(), int(pxW), int(pxH), int(x), int(y), int(w), int(h));
        toStraightRgba(buf, doc->getTileMode() == LOK_TILEMODE_BGRA);

        std::vector<unsigned char> png;
        if (!encodePng(buf, int(pxW), int(pxH), png))
            return errorReply(id, "PNG encoding failed");

        return Reply(id)
            .ok(true)
            .num("px_width", pxW)
            .num("px_height", pxH)
            .str("tile", base64(png.data(), png.size()))
            .line();
    }

    std::string key(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        lok::Document *doc = st->doc.get();

        const Json *type = req.get("type");
        int lokType;
        if (type && type->type == Json::String && type->s == "input")
            lokType = LOK_KEYEVENT_KEYINPUT;
        else if (type && type->type == Json::String && type->s == "up")
            lokType = LOK_KEYEVENT_KEYUP;
        else
            return errorReply(id, "\"type\" must be \"input\" or \"up\"");

        long long charCode = 0, keyCode = 0;
        if ((req.get("char_code") && !getInt(req, "char_code", charCode))
            || (req.get("key_code") && !getInt(req, "key_code", keyCode)) || charCode < 0
            || charCode > 0x10FFFF || keyCode < 0 || keyCode > 0xFFFF)
            return errorReply(id, "invalid char_code/key_code");
        // Convenience for hand testing: a key name instead of a VCL code.
        if (const Json *name = req.get("key")) {
            auto it = name->type == Json::String ? kKeyNames.find(name->s) : kKeyNames.end();
            if (it == kKeyNames.end())
                return errorReply(id, "unknown \"key\" name");
            keyCode |= it->second;
        }
        if (charCode == 0 && keyCode == 0)
            return errorReply(id, "char_code or key_code is required");

        doc->postKeyEvent(lokType, int(charCode), int(keyCode));
        // Conservative: navigation keys count too; at worst an extra autosave.
        if (lokType == LOK_KEYEVENT_KEYINPUT)
            st->dirty = true;
        return Reply(id).ok(true).line();
    }

    std::string paste(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        lok::Document *doc = st->doc.get();
        const Json *mime = req.get("mime_type");
        const Json *data = req.get("data");
        if (!data || data->type != Json::String)
            return errorReply(id, "missing \"data\"");
        std::string mimeType = mime && mime->type == Json::String ? mime->s : "";
        // LO only recognises plain text with an explicit charset.
        if (mimeType.empty() || mimeType == "text/plain")
            mimeType = "text/plain;charset=utf-8";
        if (!doc->paste(mimeType.c_str(), data->s.data(), data->s.size()))
            return errorReply(id, "paste failed for " + mimeType);
        st->dirty = true;
        return Reply(id).ok(true).line();
    }

    // x/y in document twips, like tile and cursor coordinates.
    std::string mouse(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        lok::Document *doc = st->doc.get();

        const Json *type = req.get("type");
        int lokType;
        if (type && type->type == Json::String && type->s == "down")
            lokType = LOK_MOUSEEVENT_MOUSEBUTTONDOWN;
        else if (type && type->type == Json::String && type->s == "move")
            lokType = LOK_MOUSEEVENT_MOUSEMOVE;
        else if (type && type->type == Json::String && type->s == "up")
            lokType = LOK_MOUSEEVENT_MOUSEBUTTONUP;
        else
            return errorReply(id, "\"type\" must be \"down\", \"move\" or \"up\"");

        long long x, y, count = 1, buttons = 1, modifiers = 0;
        if (!getInt(req, "x", x) || !getInt(req, "y", y) || x < INT_MIN || x > INT_MAX
            || y < INT_MIN || y > INT_MAX)
            return errorReply(id, "\"x\" and \"y\" must be integers");
        if ((req.get("count") && !getInt(req, "count", count))
            || (req.get("buttons") && !getInt(req, "buttons", buttons))
            || (req.get("modifiers") && !getInt(req, "modifiers", modifiers)) || count < 1
            || count > 3 || buttons < 0 || buttons > 0xFFFF || modifiers < 0 || modifiers > 0xFFFF)
            return errorReply(id, "invalid count/buttons/modifiers");

        doc->postMouseEvent(lokType, int(x), int(y), int(count), int(buttons), int(modifiers));
        return Reply(id).ok(true).line();
    }

    // Selected text as UTF-8; `cut` then deletes the selection.
    std::string copy(const Json &req, const Json *id, bool cut)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        lok::Document *doc = st->doc.get();

        std::string text;
        if (char *sel = doc->getTextSelection("text/plain;charset=utf-8", nullptr)) {
            text = sel;
            std::free(sel);
        }
        if (cut && !text.empty()) {
            const int del = kKeyNames.at("Delete");
            doc->postKeyEvent(LOK_KEYEVENT_KEYINPUT, 0, del);
            doc->postKeyEvent(LOK_KEYEVENT_KEYUP, 0, del);
            st->dirty = true;
        }
        return Reply(id).ok(true).str("text", text).line();
    }

    // Pass-through to LOK; it runs asynchronously, so an unknown or
    // inapplicable command still replies ok. Only .uno: commands, so a
    // client can't dispatch macro: or script URLs.
    std::string format(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        const Json *command = req.get("command");
        if (!command || command->type != Json::String || command->s.rfind(".uno:", 0) != 0
            || command->s.size() == 5)
            return errorReply(id, "\"command\" must be a .uno: command");
        const Json *argsField = req.get("args");
        if (!argsField) {
            st->doc->postUnoCommand(command->s.c_str());
        } else {
            if (argsField->type != Json::String)
                return errorReply(id, "\"args\" must be a string");
            std::string args, error;
            if (!unoArgs(command->s, argsField->s, args, error))
                return errorReply(id, error);
            st->doc->postUnoCommand(command->s.c_str(), args.c_str());
        }
        st->dirty = true;
        return Reply(id).ok(true).line();
    }

    // Sets the character color: "#RRGGBB", or "auto" for the automatic color.
    std::string color(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        const Json *hex = req.get("hex");
        if (!hex || hex->type != Json::String)
            return errorReply(id, "missing \"hex\"");
        long long value;
        if (!parseColor(hex->s, value))
            return errorReply(id, "invalid \"hex\": " + hex->s + " (#RRGGBB or auto)");
        setColor(*st, value);
        st->dirty = true;
        return Reply(id).ok(true).line();
    }

    // value: R*65536 + G*256 + B, or -1 for the automatic color.
    static void setColor(DocState &st, long long value)
    {
        const std::string args = "{\"Color\":{\"type\":\"long\",\"value\":" + std::to_string(value) + "}}";
        st.doc->postUnoCommand(".uno:Color", args.c_str());
    }

    // Applies a paragraph style to the paragraph(s) at the cursor.
    std::string style(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        const Json *name = req.get("name");
        if (!name || name->type != Json::String)
            return errorReply(id, "missing \"name\"");
        auto it = kParaStyles.find(name->s);
        if (it == kParaStyles.end())
            return errorReply(id, "unknown style: " + name->s
                                      + " (Normal, Heading 1, Heading 2, Heading 3, Text Body)");
        const std::string args = std::string("{\"Style\":{\"type\":\"string\",\"value\":")
            + jsonQuote(it->second)
            + "},\"FamilyName\":{\"type\":\"string\",\"value\":\"ParagraphStyles\"}}";
        st->doc->postUnoCommand(".uno:StyleApply", args.c_str());
        st->dirty = true;
        return Reply(id).ok(true).line();
    }

    // Paragraph spacing, directional only (space_before/space_after exact
    // point values and keep-together/keep-with-next/widow-orphan are NOT
    // implemented: LibreOfficeKit's postUnoCommand(".uno:ParagraphDialog",
    // args) does not apply ParaTopMargin/ParaBottomMargin/ParaSplit in
    // headless mode -- verified empirically by diffing rendered tiles
    // before/after the call, which showed no change for any of those
    // properties. ParagraphDialog is a dialog-driving command; without a
    // GUI event loop behind it, LOK accepts the call (postUnoCommand never
    // reports failure) but the properties are never actually applied. Only
    // the simple toggle dispatch commands ParaspaceIncrease/ParaspaceDecrease
    // were confirmed to work (visually, via tile diff). Exact paragraph
    // dialog settings would need the full UNO API (XPropertySet on a text
    // cursor), which is a different, heavier integration than LOK's
    // simple command dispatch.
    std::string para(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        const Json *dir = req.get("direction");
        if (!dir || dir->type != Json::String)
            return errorReply(id, "missing \"direction\" (\"increase\" or \"decrease\")");
        if (dir->s == "increase")
            st->doc->postUnoCommand(".uno:ParaspaceIncrease");
        else if (dir->s == "decrease")
            st->doc->postUnoCommand(".uno:ParaspaceDecrease");
        else
            return errorReply(id, "\"direction\" must be \"increase\" or \"decrease\"");
        st->dirty = true;
        return Reply(id).ok(true).line();
    }

    // Formatting state at the cursor, cached from STATE_CHANGED callbacks.
    std::string getState(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        std::string state = "{";
        for (const auto &[key, value] : st->state) {
            if (state.size() > 1)
                state += ',';
            state += jsonQuote(key) + ':' + value;
        }
        return Reply(id).ok(true).raw("state", state + "}").line();
    }

    // Format painter. Toggle commands flip the state, so they are only sent
    // when the cached value differs from the target; the rest set a value.
    static constexpr const char *kToggleKeys[] = {"Bold", "Italic", "Underline", "Strikeout"};
    static constexpr const char *kAlignKeys[] = {"LeftPara", "CenterPara", "RightPara", "JustifyPara"};

    // Character + paragraph formatting snapshot at the cursor, from the same
    // STATE_CHANGED cache as get_state. Color is a number (-1 = automatic).
    std::string getCharStyle(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        std::vector<const char *> keys(std::begin(kToggleKeys), std::end(kToggleKeys));
        keys.insert(keys.end(), {"CharFontName", "FontHeight", "Color"});
        keys.insert(keys.end(), std::begin(kAlignKeys), std::end(kAlignKeys));
        std::string style = "{";
        for (const char *key : keys) {
            auto it = st->state.find(key);
            if (it == st->state.end())
                continue;
            std::string value = it->second;
            long long color;
            if (std::strcmp(key, "Color") == 0) {
                if (!parseStateColor(value, color))
                    continue; // e.g. "disabled"
                value = std::to_string(color);
            }
            if (style.size() > 1)
                style += ',';
            style += jsonQuote(key) + ':' + value;
        }
        return Reply(id).ok(true).raw("style", style + "}").line();
    }

    // A cached Color value ("\"16711680\"") -> number; false if not one.
    static bool parseStateColor(const std::string &json, long long &out)
    {
        if (json.size() < 3 || json.front() != '"' || json.back() != '"')
            return false;
        const std::string s = json.substr(1, json.size() - 2);
        auto [next, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc() && next == s.data() + s.size();
    }

    // Applies a get_char_style snapshot to the selection. Unknown keys are
    // ignored and empty values (mixed selection) skipped; everything is
    // validated before any command is sent.
    std::string applyCharStyle(const Json &req, const Json *id)
    {
        long long docId;
        DocState *st = findState(req, docId);
        if (!st)
            return errorReply(id, "unknown doc_id");
        const Json *style = req.get("style");
        if (!style || style->type != Json::Object)
            return errorReply(id, "\"style\" must be an object");

        std::vector<std::pair<std::string, std::string>> commands; // (command, args or "")
        for (const char *key : kToggleKeys) {
            const Json *v = style->get(key);
            if (!v)
                continue;
            if (v->type != Json::Bool)
                return errorReply(id, std::string("\"") + key + "\" must be true or false");
            auto it = st->state.find(key);
            const bool current = it != st->state.end() && it->second == "true";
            if (current != v->b)
                commands.emplace_back(std::string(".uno:") + key, "");
        }
        if (const Json *v = style->get("CharFontName")) {
            if (v->type != Json::String)
                return errorReply(id, "\"CharFontName\" must be a string");
            std::string args, error;
            if (!v->s.empty()) {
                if (!unoArgs(".uno:CharFontName", v->s, args, error))
                    return errorReply(id, error);
                commands.emplace_back(".uno:CharFontName", args);
            }
        }
        if (const Json *v = style->get("FontHeight")) {
            std::string size;
            if (v->type == Json::String) {
                size = v->s;
            } else if (v->type == Json::Number) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%g", v->n);
                size = buf;
            } else {
                return errorReply(id, "\"FontHeight\" must be a string or number");
            }
            std::string args, error;
            if (!size.empty()) {
                if (!unoArgs(".uno:FontHeight", size, args, error))
                    return errorReply(id, error);
                commands.emplace_back(".uno:FontHeight", args);
            }
        }
        long long color = 0;
        bool hasColor = false;
        if (const Json *v = style->get("Color")) {
            if (v->type != Json::Number || std::trunc(v->n) != v->n || v->n < -1 || v->n > 0xFFFFFF)
                return errorReply(id, "\"Color\" must be -1 (automatic) or 0..16777215");
            color = static_cast<long long>(v->n);
            hasColor = true;
        }
        for (const char *key : kAlignKeys) {
            const Json *v = style->get(key);
            if (v && v->type != Json::Bool)
                return errorReply(id, std::string("\"") + key + "\" must be true or false");
            if (v && v->b) {
                commands.emplace_back(std::string(".uno:") + key, "");
                break;
            }
        }

        for (const auto &[command, args] : commands) {
            if (args.empty())
                st->doc->postUnoCommand(command.c_str());
            else
                st->doc->postUnoCommand(command.c_str(), args.c_str());
        }
        if (hasColor)
            setColor(*st, color);
        // Toggles are async: record the target now so a second apply before
        // STATE_CHANGED arrives does not flip them back.
        for (const char *key : kToggleKeys)
            if (const Json *v = style->get(key))
                st->state[key] = v->b ? "true" : "false";
        if (!commands.empty() || hasColor)
            st->dirty = true;
        return Reply(id).ok(true).line();
    }

    std::string close(const Json &req, const Json *id)
    {
        long long docId;
        auto it = getInt(req, "doc_id", docId) ? m_docs.find(docId) : m_docs.end();
        if (it == m_docs.end())
            return errorReply(id, "unknown doc_id");
        // Unregister first: LOK invokes callbacks under the SolarMutex, which
        // registerCallback also takes, so none is in flight once it returns.
        it->second.doc->registerCallback(nullptr, nullptr);
        // An explicit close discards the recovery copy; only crashes and
        // engine shutdown leave one behind.
        unlink(autosavePath(docId, it->second).c_str());
        m_docs.erase(it);
        logf("closed doc %lld", docId);
        return Reply(id).ok(true).line();
    }

    std::string ai(const Json &req, const Json *id)
    {
        auto field = [&req](const char *key) {
            const Json *v = req.get(key);
            return v && v->type == Json::String ? v->s : std::string();
        };
        const std::string action = field("action");
        const std::string provider = field("provider");

        if (action == "list_providers" || action == "status") {
            std::string out = action == "status" ? "{" : "[";
            for (const auto &p : m_ai.providers()) {
                if (out.size() > 1)
                    out += ',';
                out += jsonQuote(p.first);
                if (action == "status")
                    out += std::string(":{\"configured\":") + (m_ai.hasToken(p.first) ? "true" : "false") + "}";
            }
            out += action == "status" ? "}" : "]";
            return Reply(id).ok(true).raw("providers", out).line();
        }
        if (action == "set_token") {
            if (!m_ai.hasProvider(provider))
                return errorReply(id, "unknown provider: " + provider);
            if (!m_ai.setToken(provider, field("token")))
                return errorReply(id, "missing \"token\"");
            return Reply(id).ok(true).line();
        }
        if (action == "send") {
            AiManager::Result r = m_ai.sendMessage(provider, field("model"), field("system"), field("user"));
            if (!r.ok)
                return errorReply(id, r.error);
            return Reply(id).ok(true).str("content", r.content).str("model", r.model)
                .str("provider", provider).line();
        }
        return errorReply(id, action.empty() ? "missing \"action\"" : "unknown ai action: " + action);
    }
};

// ---------------------------------------------------------------------------
// Socket server.

volatile sig_atomic_t g_stop = 0;

void onSignal(int) { g_stop = 1; }

std::string defaultSocketPath()
{
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime)
        return std::string(runtime) + "/rune-engine.sock";
    return "/tmp/rune-engine.sock";
}

bool fillAddr(const std::string &path, sockaddr_un &addr)
{
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path)
        return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Returns a listening fd, or -1. Refuses to steal a socket from a live engine
// but removes a stale one left by a crash.
int listenOn(const std::string &path)
{
    sockaddr_un addr;
    if (!fillAddr(path, addr)) {
        logf("socket path too long: %s", path.c_str());
        return -1;
    }

    const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
        const bool live = connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
        ::close(probe);
        if (live) {
            logf("another engine is already listening on %s", path.c_str());
            return -1;
        }
    }
    unlink(path.c_str());

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        logf("socket: %s", std::strerror(errno));
        return -1;
    }
    const mode_t oldMask = umask(0077); // socket is owner-only
    const int rc = bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    umask(oldMask);
    if (rc < 0 || listen(fd, 16) < 0) {
        logf("bind/listen %s: %s", path.c_str(), std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

bool sendAll(int fd, const std::string &data)
{
    size_t off = 0;
    while (off < data.size()) {
        const ssize_t n = send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        off += size_t(n);
    }
    return true;
}

struct Client {
    int fd;
    std::string inbuf;
};

constexpr size_t kMaxLine = 16 * 1024 * 1024;

// LO 26.8 segfaults in libswlo static destructors during exit() even after a
// clean Office::destroy, so skip atexit handlers entirely.
[[noreturn]] void hardExit(int rc)
{
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}

void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "Usage: %s [--socket-path PATH]\n"
                 "  --socket-path  Unix socket to listen on (default: %s)\n",
                 argv0, defaultSocketPath().c_str());
}

} // namespace

int main(int argc, char *argv[])
{
    std::string socketPath = defaultSocketPath();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--socket-path" && i + 1 < argc) {
            socketPath = argv[++i];
        } else if (a.rfind("--socket-path=", 0) == 0) {
            socketPath = a.substr(std::strlen("--socket-path="));
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    // Keep LO's VCL headless; never load its GTK/Qt plugins.
    setenv("SAL_USE_VCLPLUGIN", "svp", 1);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSignal; // no SA_RESTART: poll() must return EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // Bind before the slow LOK init so a second instance fails fast and early
    // clients just wait in the backlog.
    const int listenFd = listenOn(socketPath);
    if (listenFd < 0)
        return 1;

    lok::Office *office = lok::lok_cpp_init(LO_PROGRAM_DIR);
    if (!office) {
        logf("lok_cpp_init failed for %s", LO_PROGRAM_DIR);
        unlink(socketPath.c_str());
        hardExit(1);
    }
    logf("listening on %s", socketPath.c_str());

    Engine engine(office);
    if (engine.eventFd() < 0) {
        unlink(socketPath.c_str());
        hardExit(1);
    }
    std::vector<Client> clients;

    while (!g_stop && !engine.quitRequested()) {
        // Layout: [listen, events, clients...].
        std::vector<pollfd> fds;
        fds.push_back({listenFd, POLLIN, 0});
        fds.push_back({engine.eventFd(), POLLIN, 0});
        for (const Client &c : clients)
            fds.push_back({c.fd, POLLIN, 0});
        constexpr size_t kFirstClient = 2;

        if (poll(fds.data(), fds.size(), engine.autosaveTimeoutMs()) < 0) {
            if (errno == EINTR)
                continue;
            logf("poll: %s", std::strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN) {
            const int cfd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
            if (cfd >= 0) {
                clients.push_back({cfd, {}});
                logf("client connected (fd %d)", cfd);
            }
        }

        // fds[kFirstClient + i] corresponds to clients[i] as they were before accept().
        std::vector<bool> drop(clients.size(), false);
        for (size_t i = 0; kFirstClient + i < fds.size() && !engine.quitRequested(); ++i) {
            if (!fds[kFirstClient + i].revents)
                continue;
            Client &c = clients[i];
            char buf[65536];
            const ssize_t n = read(c.fd, buf, sizeof buf);
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                drop[i] = true;
                continue;
            }
            c.inbuf.append(buf, size_t(n));

            size_t nl;
            while (!drop[i] && !engine.quitRequested() && (nl = c.inbuf.find('\n')) != std::string::npos) {
                std::string line = c.inbuf.substr(0, nl);
                c.inbuf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.find_first_not_of(" \t") == std::string::npos)
                    continue;
                if (!sendAll(c.fd, engine.handle(line)))
                    drop[i] = true;
            }
            if (c.inbuf.size() > kMaxLine) {
                logf("client fd %d sent an oversized line; dropping", c.fd);
                drop[i] = true;
            }
        }

        // Push events go to every client, including ones accepted this round.
        // Drained after commands so a reply precedes the events it caused.
        drop.resize(clients.size(), false);
        auto broadcast = [&](const std::vector<std::string> &events) {
            for (const std::string &ev : events) {
                for (size_t i = 0; i < clients.size(); ++i) {
                    if (!drop[i] && !sendAll(clients[i].fd, ev))
                        drop[i] = true;
                }
            }
        };
        if (fds[1].revents & POLLIN)
            broadcast(engine.takeEvents());
        if (!engine.quitRequested())
            broadcast(engine.autosaveIfDue());

        for (size_t i = drop.size(); i-- > 0;) {
            if (drop[i]) {
                logf("client disconnected (fd %d)", clients[i].fd);
                ::close(clients[i].fd);
                clients.erase(clients.begin() + long(i));
            }
        }
    }

    logf(engine.quitRequested() ? "quit requested; shutting down" : "signal received; shutting down");
    engine.shutdown();
    for (const Client &c : clients)
        ::close(c.fd);
    ::close(listenFd);
    unlink(socketPath.c_str());
    hardExit(0);
}
