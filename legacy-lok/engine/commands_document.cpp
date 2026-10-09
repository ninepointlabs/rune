// Document lifecycle and rendering: open/new_md/close, tile, save,
// autosave and Markdown export.

#include "engine_internal.h"

#include "md_to_html.h"

#include <png.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace rune {

namespace {

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

constexpr long long kMaxTilePx = 8192;

} // namespace

std::string Engine::autosavePath(long long docId, const DocState &st) const
{
    const std::string name = st.path.empty() ? "untitled.docx" : baseName(st.path);
    return m_autosaveDir + "/" + std::to_string(docId) + "_" + name;
}

// Writes the autosave copy (format from its extension); "" on failure.
std::string Engine::writeAutosave(long long docId, DocState &st)
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

std::string Engine::save(const Json &req, const Json *id)
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

std::string Engine::autosave(const Json &req, const Json *id)
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

std::string Engine::open(const Json &req, const Json *id)
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

std::string Engine::lokError()
{
    char *err = m_office->getError();
    std::string msg = err && *err ? err : "unknown error";
    m_office->freeError(err);
    return msg;
}

// A new Writer document seeded with Markdown (converted to HTML and pasted).
std::string Engine::newMd(const Json &req, const Json *id)
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
std::string Engine::exportMd(const Json &req, const Json *id)
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
std::string Engine::addDoc(std::unique_ptr<lok::Document> doc, const Json *id, const std::string &what,
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

std::string Engine::tile(const Json &req, const Json *id)
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

std::string Engine::close(const Json &req, const Json *id)
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

} // namespace rune
