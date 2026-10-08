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

#include "engine_internal.h"

#include <LibreOfficeKit/LibreOfficeKitInit.h>

#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace rune {

void logf(const char *fmt, ...)
{
    std::fputs("rune-engine: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

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

namespace {

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

} // namespace

Engine::Engine(lok::Office *office)
    : m_office(office), m_autosaveDir(autosaveDir()), m_autosaveInterval(autosaveInterval())
{
    m_wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (m_wakeFd < 0)
        logf("eventfd: %s", std::strerror(errno));
}

bool Engine::quitRequested() const { return m_quit; }

// Readable when LOK callbacks have queued events; see takeEvents().
int Engine::eventFd() const { return m_wakeFd; }

// Drains queued LOK callbacks into push-event lines for all clients.
std::vector<std::string> Engine::takeEvents()
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

std::string Engine::handle(const std::string &line)
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
    if (cmd->s == "table")
        return table(req, id);
    if (cmd->s == "ai")
        return ai(req, id);
    if (cmd->s == "quit") {
        m_quit = true;
        return Reply(id).ok(true).line();
    }
    return errorReply(id, "unknown cmd: " + cmd->s);
}

// poll() timeout until the next autosave round; -1 when none is enabled.
int Engine::autosaveTimeoutMs() const
{
    if (!autosaveActive())
        return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        m_nextAutosave - std::chrono::steady_clock::now());
    return int(std::max<long long>(0, left.count()));
}

// Runs an autosave round if one is due; returns push events for clients.
std::vector<std::string> Engine::autosaveIfDue()
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
void Engine::shutdown()
{
    for (auto &[docId, st] : m_docs) {
        if (st.autosave && st.dirty)
            writeAutosave(docId, st);
    }
}

// Runs on LibreOffice's main-loop thread: queue and wake, nothing else.
void Engine::onLokCallback(int type, const char *payload, void *data)
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
std::vector<Rect> Engine::pageRects(lok::Document *doc, long docW, long docH)
{
    char *rectStr = doc->getPartPageRectangles();
    std::vector<Rect> pages = parseRects(rectStr);
    std::free(rectStr);
    if (pages.empty())
        pages.push_back(Rect{0, 0, docW, docH});
    return pages;
}

std::string Engine::formatEvent(const Pending &p)
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

Engine::DocState *Engine::findState(const Json &req, long long &docId)
{
    if (!getInt(req, "doc_id", docId))
        return nullptr;
    auto it = m_docs.find(docId);
    return it == m_docs.end() ? nullptr : &it->second;
}

lok::Document *Engine::findDoc(const Json &req)
{
    long long docId;
    DocState *st = findState(req, docId);
    return st ? st->doc.get() : nullptr;
}

bool Engine::autosaveActive() const
{
    return std::any_of(m_docs.begin(), m_docs.end(), [](const auto &d) { return d.second.autosave; });
}

std::string Engine::ai(const Json &req, const Json *id)
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

} // namespace rune

using namespace rune;

namespace {

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
