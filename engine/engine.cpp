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

// VCL key codes (vcl/keycodes.hxx, == css::awt::Key), which is not installed
// with the SDK. Values checked against offapi.rdb for LO 26.8.
const std::map<std::string, int> kKeyNames = {
    {"Down", 1024},     {"Up", 1025},     {"Left", 1026},   {"Right", 1027},
    {"Home", 1028},     {"End", 1029},    {"PageUp", 1030}, {"PageDown", 1031},
    {"Return", 1280},   {"Enter", 1280},  {"Escape", 1281}, {"Tab", 1282},
    {"Backspace", 1283}, {"Space", 1284}, {"Insert", 1285}, {"Delete", 1286},
};

// ---------------------------------------------------------------------------
// Command dispatch.

constexpr long long kMaxTilePx = 8192;

class Engine {
public:
    explicit Engine(lok::Office *office) : m_office(office)
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
        if (cmd->s == "new_md")
            return newMd(req, id);
        if (cmd->s == "export_md")
            return exportMd(req, id);
        if (cmd->s == "ai")
            return ai(req, id);
        if (cmd->s == "quit") {
            m_quit = true;
            return Reply(id).ok(true).line();
        }
        return errorReply(id, "unknown cmd: " + cmd->s);
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
    };

    lok::Office *m_office; // never destroyed; see main()
    AiManager m_ai;
    std::map<long long, DocState> m_docs;
    long long m_nextDocId = 0;
    bool m_quit = false;

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
        }
        return {};
    }

    lok::Document *findDoc(const Json &req)
    {
        long long docId;
        if (!getInt(req, "doc_id", docId))
            return nullptr;
        auto it = m_docs.find(docId);
        return it == m_docs.end() ? nullptr : it->second.doc.get();
    }

    std::string open(const Json &req, const Json *id)
    {
        const Json *path = req.get("path");
        if (!path || path->type != Json::String || path->s.empty())
            return errorReply(id, "missing \"path\"");

        char *abs = realpath(path->s.c_str(), nullptr);
        if (!abs)
            return errorReply(id, path->s + ": " + std::strerror(errno));
        const std::string url = fileUrl(abs);
        std::free(abs);

        std::unique_ptr<lok::Document> doc(m_office->documentLoad(url.c_str()));
        if (!doc)
            return errorReply(id, "documentLoad failed: " + lokError());
        doc->initializeForRendering();
        return addDoc(std::move(doc), id, path->s);
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
            if (!doc->paste("text/html", html.data(), html.size()))
                return errorReply(id, "paste failed for text/html");
        }
        return addDoc(std::move(doc), id, "(new markdown document)");
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
    // rendering, registers it and replies with its layout.
    std::string addDoc(std::unique_ptr<lok::Document> doc, const Json *id, const std::string &what)
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
        m_docs[docId] = DocState{std::move(doc), std::move(ctx), {}, {}, std::move(pages)};
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
        lok::Document *doc = findDoc(req);
        if (!doc)
            return errorReply(id, "unknown doc_id");

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
        return Reply(id).ok(true).line();
    }

    std::string paste(const Json &req, const Json *id)
    {
        lok::Document *doc = findDoc(req);
        if (!doc)
            return errorReply(id, "unknown doc_id");
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

        if (poll(fds.data(), fds.size(), -1) < 0) {
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
        if (fds[1].revents & POLLIN) {
            drop.resize(clients.size(), false);
            for (const std::string &ev : engine.takeEvents()) {
                for (size_t i = 0; i < clients.size(); ++i) {
                    if (!drop[i] && !sendAll(clients[i].fd, ev))
                        drop[i] = true;
                }
            }
        }

        for (size_t i = drop.size(); i-- > 0;) {
            if (drop[i]) {
                logf("client disconnected (fd %d)", clients[i].fd);
                ::close(clients[i].fd);
                clients.erase(clients.begin() + long(i));
            }
        }
    }

    logf(engine.quitRequested() ? "quit requested; shutting down" : "signal received; shutting down");
    for (const Client &c : clients)
        ::close(c.fd);
    ::close(listenFd);
    unlink(socketPath.c_str());
    hardExit(0);
}
