// Rune spike: LibreOfficeKit tiled rendering -> QML display.
//
// Usage: rune_spike [file.docx] [--png out.png] [--scale N]
//   --png   render page 0 to a PNG and exit (no window; headless check)
//   --scale pixels-per-CSS-pixel multiplier for the tile (default 1.5)

#include <LibreOfficeKit/LibreOfficeKitInit.h>
#include <LibreOfficeKit/LibreOfficeKit.hxx>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QRegularExpression>
#include <QTextStream>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

struct RenderResult {
    QImage image;
    QString error;
    int parts = 0;
    long docWidthTwips = 0, docHeightTwips = 0;
    QRect pageTwips;
    qint64 loadMs = 0, paintMs = 0;
};

// Writer reports page rectangles as "x, y, w, h; x, y, w, h; ..." in twips.
QRect firstPageRect(const char *rects)
{
    if (!rects)
        return {};
    const QStringList nums = QString::fromUtf8(rects).section(';', 0, 0).split(',');
    if (nums.size() != 4)
        return {};
    return QRect(nums[0].trimmed().toInt(), nums[1].trimmed().toInt(),
                 nums[2].trimmed().toInt(), nums[3].trimmed().toInt());
}

RenderResult renderPage0(const QString &docPath, double scale)
{
    RenderResult r;
    QElapsedTimer t;
    t.start();

    // Keep LO's VCL headless so it never tries to load its own Qt/GTK plugin
    // inside our Qt process.
    qputenv("SAL_USE_VCLPLUGIN", "svp");

    std::unique_ptr<lok::Office> office(lok::lok_cpp_init(LO_PROGRAM_DIR));
    if (!office) {
        r.error = QStringLiteral("lok_cpp_init failed for " LO_PROGRAM_DIR);
        return r;
    }

    const QByteArray url = QUrl::fromLocalFile(QFileInfo(docPath).absoluteFilePath()).toEncoded();
    std::unique_ptr<lok::Document> doc(office->documentLoad(url.constData()));
    if (!doc) {
        char *err = office->getError();
        r.error = QStringLiteral("documentLoad failed: %1").arg(QString::fromUtf8(err ? err : "?"));
        office->freeError(err);
        return r;
    }
    doc->initializeForRendering();
    r.loadMs = t.restart();

    r.parts = doc->getParts();
    doc->getDocumentSize(&r.docWidthTwips, &r.docHeightTwips);

    char *rects = doc->getPartPageRectangles();
    r.pageTwips = firstPageRect(rects);
    std::free(rects);
    if (r.pageTwips.isEmpty()) {
        // Fallback: US Letter at the document origin.
        r.pageTwips = QRect(0, 0, 12240, 15840);
    }

    // twips -> pixels: 1440 twips/inch, 96 px/inch.
    const int pxW = qRound(r.pageTwips.width() / 15.0 * scale);
    const int pxH = qRound(r.pageTwips.height() / 15.0 * scale);
    std::vector<unsigned char> buf(size_t(pxW) * pxH * 4);

    doc->paintTile(buf.data(), pxW, pxH,
                   r.pageTwips.x(), r.pageTwips.y(),
                   r.pageTwips.width(), r.pageTwips.height());
    r.paintMs = t.elapsed();

    // BGRA bytes == QImage ARGB32 on little-endian.
    const QImage::Format fmt = doc->getTileMode() == LOK_TILEMODE_BGRA
                                   ? QImage::Format_ARGB32_Premultiplied
                                   : QImage::Format_RGBA8888_Premultiplied;
    r.image = QImage(buf.data(), pxW, pxH, pxW * 4, fmt).copy();
    return r;
}

// Pull the active Omarchy palette; fall back to a neutral dark palette.
QVariantMap omarchyTheme()
{
    QVariantMap m{
        {"background", "#1a1b26"}, {"foreground", "#c0caf5"},
        {"accent", "#7aa2f7"},     {"muted", "#414868"},
        {"lighter_background", "#24283b"}, {"red", "#f7768e"},
    };
    QFile f(QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return m;
    static const QRegularExpression re(R"(^\s*(\w+)\s*=\s*"(#[0-9A-Fa-f]{6,8})\")");
    QTextStream in(&f);
    while (!in.atEnd()) {
        const auto match = re.match(in.readLine());
        if (match.hasMatch())
            m.insert(match.captured(1), match.captured(2));
    }
    return m;
}

class TileProvider : public QQuickImageProvider {
public:
    explicit TileProvider(QImage img) : QQuickImageProvider(Image), m_img(std::move(img)) {}
    QImage requestImage(const QString &, QSize *size, const QSize &) override
    {
        if (size)
            *size = m_img.size();
        return m_img;
    }

private:
    QImage m_img;
};

} // namespace

int main(int argc, char *argv[])
{
    QString docPath = QStringLiteral(SPIKE_DEFAULT_DOC);
    QString pngOut;
    double scale = 1.5;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--png" && i + 1 < argc)
            pngOut = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--scale" && i + 1 < argc)
            scale = QString::fromLocal8Bit(argv[++i]).toDouble();
        else
            docPath = a;
    }

    // Render before Qt spins up so LOK owns its VCL init cleanly.
    const RenderResult r = renderPage0(docPath, scale);
    const QString status = r.error.isEmpty()
        ? QStringLiteral("%1 · %2 page(s) · doc %3×%4 twips · page0 %5×%6 twips → %7×%8 px · load %9 ms · paint %10 ms")
              .arg(QFileInfo(docPath).fileName()).arg(r.parts)
              .arg(r.docWidthTwips).arg(r.docHeightTwips)
              .arg(r.pageTwips.width()).arg(r.pageTwips.height())
              .arg(r.image.width()).arg(r.image.height())
              .arg(r.loadMs).arg(r.paintMs)
        : r.error;
    std::fprintf(r.error.isEmpty() ? stdout : stderr, "%s\n", qPrintable(status));
    std::fflush(stdout);

    // LO 26.8 segfaults in libswlo static destructors during exit() even after
    // a clean Office::destroy (SystemClipboard -> LOKClipboard after teardown),
    // so skip atexit handlers once we are done.
    const auto quit = [](int rc) {
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(rc);
    };

    if (!pngOut.isEmpty()) {
        if (!r.error.isEmpty() || !r.image.save(pngOut))
            quit(1);
        std::printf("wrote %s\n", qPrintable(pngOut));
        quit(0);
    }

    int rc;
    {
        QGuiApplication app(argc, argv);
        QQmlApplicationEngine engine;
        engine.addImageProvider("lok", new TileProvider(r.image));
        engine.rootContext()->setContextProperty("theme", omarchyTheme());
        engine.rootContext()->setContextProperty("statusText", status);
        engine.rootContext()->setContextProperty("renderOk", r.error.isEmpty());
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
        engine.loadFromModule("RuneSpike", "Main");
        rc = app.exec();
    }
    quit(rc);
}
