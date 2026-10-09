// Shared declarations for the rune-engine translation units: the minimal
// JSON types, reply helpers and the Engine class. Engine's members are
// defined across engine.cpp (core, dispatch, callbacks) and the
// commands_*.cpp files (one per feature area).

#pragma once

#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include "ai_manager.h"

#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rune {

void logf(const char *fmt, ...);

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

std::string jsonQuote(const std::string &s);
std::string jsonScalar(const Json *v);

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

std::string errorReply(const Json *id, const std::string &message);
bool getInt(const Json &req, const char *key, long long &out);

struct Rect {
    long x = 0, y = 0, w = 0, h = 0;
};

std::string rectJson(const Rect &r);
std::string rectsJson(const std::vector<Rect> &rects);

// ---------------------------------------------------------------------------
// Command dispatch.

class Engine {
public:
    explicit Engine(lok::Office *office);

    bool quitRequested() const;
    int eventFd() const;
    std::vector<std::string> takeEvents();
    std::string handle(const std::string &line);
    int autosaveTimeoutMs() const;
    std::vector<std::string> autosaveIfDue();
    void shutdown();

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

    // engine.cpp: callbacks, lookup, autosave scheduling, ai.
    static void onLokCallback(int type, const char *payload, void *data);
    static std::vector<Rect> pageRects(lok::Document *doc, long docW, long docH);
    std::string formatEvent(const Pending &p);
    DocState *findState(const Json &req, long long &docId);
    lok::Document *findDoc(const Json &req);
    bool autosaveActive() const;
    std::string ai(const Json &req, const Json *id);

    // commands_document.cpp: lifecycle, rendering, save/autosave.
    std::string autosavePath(long long docId, const DocState &st) const;
    std::string writeAutosave(long long docId, DocState &st);
    std::string save(const Json &req, const Json *id);
    std::string autosave(const Json &req, const Json *id);
    std::string open(const Json &req, const Json *id);
    std::string lokError();
    std::string newMd(const Json &req, const Json *id);
    std::string exportMd(const Json &req, const Json *id);
    std::string addDoc(std::unique_ptr<lok::Document> doc, const Json *id, const std::string &what,
                       const std::string &path);
    std::string tile(const Json &req, const Json *id);
    std::string close(const Json &req, const Json *id);

    // commands_editing.cpp: live text editing.
    std::string key(const Json &req, const Json *id);
    std::string paste(const Json &req, const Json *id);
    std::string mouse(const Json &req, const Json *id);
    std::string copy(const Json &req, const Json *id, bool cut);

    // commands_formatting.cpp: formatting commands and state.
    std::string format(const Json &req, const Json *id);
    std::string color(const Json &req, const Json *id);
    static void setColor(DocState &st, long long value);
    std::string style(const Json &req, const Json *id);
    std::string para(const Json &req, const Json *id);
    std::string getState(const Json &req, const Json *id);

    // Format painter. Toggle commands flip the state, so they are only sent
    // when the cached value differs from the target; the rest set a value.
    static constexpr const char *kToggleKeys[] = {"Bold", "Italic", "Underline", "Strikeout"};
    static constexpr const char *kAlignKeys[] = {"LeftPara", "CenterPara", "RightPara", "JustifyPara"};

    std::string getCharStyle(const Json &req, const Json *id);
    static bool parseStateColor(const std::string &json, long long &out);
    std::string applyCharStyle(const Json &req, const Json *id);

    // commands_tables.cpp: table insert, AutoFit, row operations.
    std::string table(const Json &req, const Json *id);
};

} // namespace rune
