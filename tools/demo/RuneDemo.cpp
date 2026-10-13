// Rune demo director: a Qt generic plugin that loads into the unmodified
// rune binary, plays a scene script against the real window, and records
// it. Nothing in the app knows it is there; it drives the window the way
// the --auto-test checks do (real key and mouse events, the QML API).
//
//   QT_QPA_PLATFORM=offscreen QT_PLUGIN_PATH=<build>/plugins \
//   RUNE_DEMO_SCRIPT=scene.json RUNE_DEMO_OUT=out/ rune -plugin runedemo
//
// A script is {"size": [w, h], "fps": 30, "steps": [...]}; each step is an
// object with one of these keys:
//
//   eval: "js"              run JavaScript in Main.qml's scope (its ids:
//                           controller, ai, sidebar, aiPanel, root, ...);
//                           $OUT is the output directory
//   log: "js"               print the expression's value
//   compose: "file.md"      build the document from Markdown-ish text:
//                           "# " chapter, "## " scene, other lines are
//                           paragraphs (*italic*), typed as real keys;
//                           spaced: true puts an empty one after each
//   type: "text", cps: n    type at n characters a second (default 16);
//                           "\n" presses Return
//   key: "Ctrl+K"           a key press through the shortcut system
//   wait: seconds           keep recording
//   until: "js", timeout: s keep recording until the expression is true
//   expect: "js"            exit with status 4 unless true (a bad take)
//   scroll: "text", at: f   scroll the page so the text is f of the way
//                           down the view ("" for the top), over secs
//   select: "text"          select that text in the body (nth: which)
//   cursor: "text"          put the cursor after that text (before: true
//                           for before it)
//   point: target, secs     glide the pointer to an item ("objectName",
//                           "objectName#index"), a visible item showing
//                           that text ("label:Text", "label:Text#index"),
//                           [x, y], text in the body ("text:..."), or
//                           {"at": target, "dx": x, "dy": y}
//   click: target           glide there and click; drag: [from, to] drags
//   pointer: false          hide the pointer
//   shot: "name"            save the frame as <out>/<name>.png
//   record: "name"          start encoding frames to <out>/<name>.mp4;
//   record: false           stop (also on quit)
//   mark: "name"            note the current video time in <out>/marks.json
//   quit: true              exit (also after the last step)
//
// Every step also takes "after": seconds to keep recording once it's done.

#include <functional>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGenericPlugin>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>

namespace {

// Set by the director, so a failure still closes the video file.
std::function<void()> g_abort;

void fail(const QString &message)
{
    qCritical().noquote() << "runedemo:" << message;
    if (g_abort)
        g_abort();
    QCoreApplication::exit(3);
}

class Director : public QObject
{
public:
    explicit Director(QObject *parent) : QObject(parent)
    {
        g_abort = [this] {
            m_tick.stop();
            m_work = nullptr;
            stopRecording();
        };
        m_outDir = qEnvironmentVariable("RUNE_DEMO_OUT", QStringLiteral("."));
        QFile file(qEnvironmentVariable("RUNE_DEMO_SCRIPT"));
        if (!file.open(QIODevice::ReadOnly)) {
            QTimer::singleShot(0, this, [] { fail(QStringLiteral("set RUNE_DEMO_SCRIPT to a scene file")); });
            return;
        }
        m_scriptDir = QFileInfo(file).absolutePath();
        const QJsonObject script = QJsonDocument::fromJson(file.readAll()).object();
        m_steps = script.value(QLatin1String("steps")).toArray();
        m_fps = script.value(QLatin1String("fps")).toInt(30);
        const QJsonArray size = script.value(QLatin1String("size")).toArray({1280, 720});
        m_size = QSize(size.at(0).toInt(), size.at(1).toInt());
        // Let the window load and settle before the first step.
        QTimer::singleShot(800, this, [this] { start(); });
    }

private:
    QQuickWindow *m_window = nullptr;
    QQmlContext *m_context = nullptr;
    QObject *m_root = nullptr;
    QJsonArray m_steps;
    int m_index = -1;
    int m_fps = 30;
    QSize m_size;
    QString m_outDir, m_scriptDir;
    QTimer m_tick;
    QProcess *m_encoder = nullptr;
    qint64 m_frames = 0;       // frames written to the current clip
    QJsonObject m_marks;
    QString m_clip;

    // The current step's progress, advanced once a frame.
    std::function<bool()> m_work; // returns true when the step is done
    int m_afterFrames = 0;

    // The drawn pointer, in window (logical) coordinates.
    bool m_pointerShown = false;
    QPointF m_pointer{-1, -1};
    bool m_pressed = false;

    void start()
    {
        for (QWindow *w : QGuiApplication::topLevelWindows())
            if ((m_window = qobject_cast<QQuickWindow *>(w)))
                break;
        if (!m_window)
            return fail(QStringLiteral("no Quick window"));
        m_root = m_window;
        m_context = qmlContext(m_root);
        if (!m_context)
            return fail(QStringLiteral("no QML context on the window"));
        m_window->resize(m_size);
        m_pointer = QPointF(m_size.width() * 0.6, m_size.height() * 0.6);
        QCoreApplication::processEvents();
        m_tick.setTimerType(Qt::PreciseTimer);
        m_tick.setInterval(1000 / m_fps);
        connect(&m_tick, &QTimer::timeout, this, [this] { frame(); });
        m_tick.start();
    }

    // One video frame: advance the current step, then capture.
    void frame()
    {
        if (m_afterFrames > 0) {
            --m_afterFrames;
        } else if (!m_work || m_work()) {
            if (!m_tick.isActive())
                return; // failed
            m_work = nullptr;
            if (m_index >= 0)
                m_afterFrames = int(m_steps.at(m_index).toObject().value(QLatin1String("after")).toDouble() * m_fps);
            if (m_afterFrames == 0 && !next())
                return;
        }
        capture();
    }

    // Starts the next step; false when the script is over.
    bool next()
    {
        if (++m_index >= m_steps.size()) {
            finish();
            return false;
        }
        const QJsonObject step = m_steps.at(m_index).toObject();
        qInfo().noquote() << "runedemo: step" << m_index << QJsonDocument(step).toJson(QJsonDocument::Compact).left(160);
        if (step.contains(QLatin1String("eval"))) {
            evaluate(step.value(QLatin1String("eval")).toString());
        } else if (step.contains(QLatin1String("log"))) {
            const QVariant v = evaluate(step.value(QLatin1String("log")).toString());
            qInfo().noquote() << "runedemo: log" << QJsonDocument::fromVariant(QVariantList{v}).toJson(QJsonDocument::Compact);
        } else if (step.contains(QLatin1String("expect"))) {
            const QString expr = step.value(QLatin1String("expect")).toString();
            if (!evaluate(expr).toBool()) {
                qCritical().noquote() << "runedemo: expectation failed:" << expr;
                m_tick.stop();
                stopRecording();
                QCoreApplication::exit(4);
                return false;
            }
        } else if (step.contains(QLatin1String("scroll"))) {
            scrollTo(step.value(QLatin1String("scroll")).toString(), step.value(QLatin1String("at")).toDouble(0.25),
                     step.value(QLatin1String("secs")).toDouble(0.8));
        } else if (step.contains(QLatin1String("compose"))) {
            compose(step.value(QLatin1String("compose")).toString(), step.value(QLatin1String("spaced")).toBool());
        } else if (step.contains(QLatin1String("type"))) {
            startTyping(step.value(QLatin1String("type")).toString(), step.value(QLatin1String("cps")).toDouble(16));
        } else if (step.contains(QLatin1String("key"))) {
            pressKey(step.value(QLatin1String("key")).toString());
        } else if (step.contains(QLatin1String("wait"))) {
            int frames = int(step.value(QLatin1String("wait")).toDouble() * m_fps);
            m_work = [frames]() mutable { return --frames <= 0; };
        } else if (step.contains(QLatin1String("until"))) {
            const QString expr = step.value(QLatin1String("until")).toString();
            int frames = int(step.value(QLatin1String("timeout")).toDouble(60) * m_fps);
            m_work = [this, expr, frames]() mutable {
                if (evaluate(expr).toBool())
                    return true;
                if (--frames <= 0) {
                    fail(QStringLiteral("timed out waiting for: ") + expr);
                    return true;
                }
                return false;
            };
        } else if (step.contains(QLatin1String("select"))) {
            selectText(step.value(QLatin1String("select")).toString(), step.value(QLatin1String("nth")).toInt());
        } else if (step.contains(QLatin1String("cursor"))) {
            placeCursor(step.value(QLatin1String("cursor")).toString(), step.value(QLatin1String("before")).toBool());
        } else if (step.contains(QLatin1String("point"))) {
            glide(step.value(QLatin1String("point")), step.value(QLatin1String("secs")).toDouble(0.6), nullptr);
        } else if (step.contains(QLatin1String("click"))) {
            glide(step.value(QLatin1String("click")), step.value(QLatin1String("secs")).toDouble(0.6), [this] {
                QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, m_pointer.toPoint());
            });
        } else if (step.contains(QLatin1String("drag"))) {
            drag(step.value(QLatin1String("drag")).toArray(), step.value(QLatin1String("secs")).toDouble(1.2));
        } else if (step.contains(QLatin1String("pointer"))) {
            m_pointerShown = step.value(QLatin1String("pointer")).toBool();
        } else if (step.contains(QLatin1String("shot"))) {
            const QString path = m_outDir + '/' + step.value(QLatin1String("shot")).toString() + QStringLiteral(".png");
            if (!grab().save(path))
                fail(QStringLiteral("could not write ") + path);
        } else if (step.contains(QLatin1String("record"))) {
            const QJsonValue v = step.value(QLatin1String("record"));
            if (v.isString())
                startRecording(v.toString());
            else
                stopRecording();
        } else if (step.contains(QLatin1String("mark"))) {
            m_marks.insert(step.value(QLatin1String("mark")).toString(),
                           QJsonObject{{"clip", m_clip}, {"seconds", double(m_frames) / m_fps}});
        } else if (step.contains(QLatin1String("quit"))) {
            finish();
            return false;
        } else {
            fail(QStringLiteral("unknown step ") + QString::number(m_index));
            return false;
        }
        return true;
    }

    QVariant evaluate(const QString &js)
    {
        QString code = js;
        code.replace(QLatin1String("$OUT"), m_outDir);
        QQmlExpression expr(m_context, m_root, code);
        const QVariant result = expr.evaluate();
        if (expr.hasError())
            fail(expr.error().toString() + QStringLiteral(" in: ") + js);
        // QJSValue results (e.g. from functions) become plain variants.
        return result.canConvert<QJSValue>() ? result.value<QJSValue>().toVariant() : result;
    }

    QQuickItem *item(const QString &spec)
    {
        const int hash = spec.indexOf('#');
        const QString name = hash < 0 ? spec : spec.left(hash);
        int wanted = hash < 0 ? 0 : spec.mid(hash + 1).toInt();
        QQuickItem *found = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            if (found || !it)
                return;
            if (it->objectName() == name && it->isVisible() && wanted-- == 0) {
                found = it;
                return;
            }
            for (QQuickItem *child : it->childItems())
                walk(child);
        };
        walk(m_window->contentItem());
        return found;
    }

    QTextDocument *body()
    {
        auto *doc = evaluate(QStringLiteral("pageView.editor.textDocument")).value<QQuickTextDocument *>();
        return doc ? doc->textDocument() : nullptr;
    }

    // The window position of document position `pos` in the body editor.
    QPointF textPoint(int pos)
    {
        QQuickItem *editor = evaluate(QStringLiteral("pageView.editor")).value<QQuickItem *>();
        if (!editor)
            return {};
        QRectF r;
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, r), Q_ARG(int, pos));
        return editor->mapToScene(r.center());
    }

    int find(const QString &text, int nth = 0)
    {
        QTextDocument *doc = body();
        if (!doc)
            return -1;
        QTextCursor c(doc);
        for (int i = 0; i <= nth; ++i) {
            c = doc->find(text, c);
            if (c.isNull())
                return -1;
        }
        return c.selectionStart();
    }

    void selectText(const QString &text, int nth)
    {
        const int start = find(text, nth);
        if (start < 0)
            return fail(QStringLiteral("text not found: ") + text);
        evaluate(QStringLiteral("root.focusEditor(); controller.setSelection(%1, %2, %1)")
                     .arg(start + text.size()).arg(start));
        evaluate(QStringLiteral("pageView.editor.select(%1, %2)").arg(start).arg(start + text.size()));
    }

    void placeCursor(const QString &text, bool before)
    {
        const int start = find(text);
        if (start < 0)
            return fail(QStringLiteral("text not found: ") + text);
        const int pos = before ? start : start + text.size();
        evaluate(QStringLiteral("root.focusEditor(); pageView.editor.cursorPosition = %1").arg(pos));
    }

    // Scrolls the page view so `text` sits `at` of the way down the view.
    void scrollTo(const QString &text, double at, double secs)
    {
        auto *flick = evaluate(QStringLiteral("pageView")).value<QQuickItem *>();
        auto *editor = evaluate(QStringLiteral("pageView.editor")).value<QQuickItem *>();
        auto *content = flick ? flick->property("contentItem").value<QQuickItem *>() : nullptr;
        const int pos = text.isEmpty() ? 0 : find(text);
        if (!flick || !editor || !content || pos < 0)
            return fail(QStringLiteral("cannot scroll to ") + text);
        QRectF r;
        QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, r), Q_ARG(int, pos));
        const qreal maxY = std::max(0.0, flick->property("contentHeight").toReal() - flick->height());
        const qreal to = text.isEmpty() ? 0
            : std::clamp(editor->mapToItem(content, r.topLeft()).y() - at * flick->height(), 0.0, maxY);
        const qreal from = flick->property("contentY").toReal();
        const int total = std::max(1, int(secs * m_fps));
        int frame = 0;
        m_work = [=]() mutable {
            const double t = double(++frame) / total;
            const double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
            flick->setProperty("contentY", from + (to - from) * e);
            return frame >= total;
        };
    }

    // The nth visible item whose `text` is `text`.
    QQuickItem *labelled(const QString &text, int nth)
    {
        QQuickItem *found = nullptr;
        std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
            if (found || !it || !it->isVisible())
                return;
            if (it->property("text").toString() == text && nth-- == 0) {
                found = it;
                return;
            }
            for (QQuickItem *child : it->childItems())
                walk(child);
        };
        walk(m_window->contentItem());
        return found;
    }

    QPointF target(const QJsonValue &spec)
    {
        if (spec.isObject()) {
            const QJsonObject o = spec.toObject();
            return target(o.value(QLatin1String("at")))
                + QPointF(o.value(QLatin1String("dx")).toDouble(), o.value(QLatin1String("dy")).toDouble());
        }
        if (spec.isArray())
            return QPointF(spec.toArray().at(0).toDouble(), spec.toArray().at(1).toDouble());
        const QString s = spec.toString();
        if (s.startsWith(QLatin1String("text:"))) {
            const int pos = find(s.mid(5));
            return pos < 0 ? QPointF() : textPoint(pos + 1);
        }
        QQuickItem *it = nullptr;
        if (s.startsWith(QLatin1String("label:"))) {
            const int hash = s.lastIndexOf('#');
            it = hash < 0 ? labelled(s.mid(6), 0) : labelled(s.mid(6, hash - 6), s.mid(hash + 1).toInt());
        } else {
            it = item(s);
        }
        if (!it) {
            fail(QStringLiteral("no visible item ") + s);
            return {};
        }
        return it->mapToScene(QPointF(it->width() / 2, it->height() / 2));
    }

    // Moves the pointer to `spec` over `secs`, then runs `done`.
    void glide(const QJsonValue &spec, double secs, std::function<void()> done)
    {
        const QPointF to = target(spec);
        const QPointF from = m_pointer;
        m_pointerShown = true;
        const int total = std::max(1, int(secs * m_fps));
        int frame = 0;
        m_work = [=]() mutable {
            const double t = double(++frame) / total;
            const double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; // ease in-out
            m_pointer = from + (to - from) * e;
            QTest::mouseMove(m_window, m_pointer.toPoint());
            if (frame < total)
                return false;
            if (done)
                done();
            return true;
        };
    }

    void drag(const QJsonArray &ends, double secs)
    {
        const QPointF from = target(ends.at(0));
        const QPointF to = target(ends.at(1));
        const QPointF start = m_pointer;
        m_pointerShown = true;
        const int approach = int(0.5 * m_fps), carry = std::max(1, int(secs * m_fps));
        int frame = 0;
        m_work = [=]() mutable {
            ++frame;
            if (frame <= approach) {
                const double t = double(frame) / approach;
                m_pointer = start + (from - start) * (1 - std::pow(1 - t, 3));
                QTest::mouseMove(m_window, m_pointer.toPoint());
                if (frame == approach) {
                    QTest::mousePress(m_window, Qt::LeftButton, Qt::NoModifier, m_pointer.toPoint());
                    m_pressed = true;
                }
                return false;
            }
            const double t = double(frame - approach) / carry;
            const double e = t < 0.5 ? 2 * t * t : 1 - std::pow(-2 * t + 2, 2) / 2;
            m_pointer = from + (to - from) * std::min(1.0, e);
            // A real mouse move, buttons held, so MouseArea sees a drag.
            QMouseEvent move(QEvent::MouseMove, m_pointer, m_window->mapToGlobal(m_pointer),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(m_window, &move);
            if (t < 1)
                return false;
            QTest::mouseRelease(m_window, Qt::LeftButton, Qt::NoModifier, m_pointer.toPoint());
            m_pressed = false;
            return true;
        };
    }

    void sendChar(QChar ch)
    {
        if (ch == '\n') {
            QTest::keyClick(m_window, Qt::Key_Return);
            return;
        }
        if (ch == '\t') {
            pressKey(QStringLiteral("Tab"));
            return;
        }
        const int key = ch == ' ' ? Qt::Key_Space : ch.toUpper().unicode();
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(ch));
        QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString(ch));
        QCoreApplication::sendEvent(m_window, &press);
        QCoreApplication::sendEvent(m_window, &release);
    }

    // Types `text` at about `cps` characters a second, a little unevenly,
    // with a beat after punctuation, the way a person types.
    void startTyping(const QString &text, double cps)
    {
        int i = 0;
        double budget = 0;
        m_work = [=]() mutable {
            if (cps <= 0) {
                for (const QChar ch : text)
                    sendChar(ch);
                return true;
            }
            budget += cps / m_fps * (0.6 + QRandomGenerator::global()->generateDouble() * 0.8);
            while (budget >= 1 && i < text.size()) {
                const QChar ch = text.at(i++);
                sendChar(ch);
                budget -= 1;
                if (QStringLiteral(".,;:!?").contains(ch))
                    budget -= 2;
            }
            return i >= text.size();
        };
    }

    void pressKey(const QString &spec)
    {
        const QKeyCombination combo = QKeySequence::fromString(spec)[0];
        QTest::keyClick(m_window, combo.key(), combo.keyboardModifiers());
    }

    // Builds the body from a Markdown-ish file, as real typing, all at once.
    void compose(const QString &file, bool spaced)
    {
        QFile f(QDir(m_scriptDir).filePath(file));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return fail(QStringLiteral("cannot read ") + file);
        QStringList paragraphs;
        QString current;
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        for (const QString &line : lines) {
            if (line.trimmed().isEmpty()) {
                if (!current.isEmpty())
                    paragraphs << current;
                current.clear();
            } else if (line.startsWith('#')) {
                if (!current.isEmpty())
                    paragraphs << current;
                paragraphs << line;
                current.clear();
            } else {
                current += (current.isEmpty() ? QString() : QStringLiteral(" ")) + line.trimmed();
            }
        }
        if (!current.isEmpty())
            paragraphs << current;
        evaluate(QStringLiteral("root.focusEditor()"));
        int n = 0;
        m_work = [this, paragraphs, n, spaced]() mutable {
            // A few paragraphs a frame, so the queued heading fix-ups run.
            for (int k = 0; k < 4 && n < paragraphs.size(); ++k, ++n) {
                QString p = paragraphs.at(n);
                int level = 0;
                while (p.startsWith('#')) {
                    ++level;
                    p.remove(0, 1);
                }
                evaluate(QStringLiteral("controller.setHeadingLevel(%1)").arg(level));

                // *text* is italic, as in Markdown.
                for (const QChar ch : p.trimmed()) {
                    if (ch == '*')
                        evaluate(QStringLiteral("controller.toggleItalic()"));
                    else
                        sendChar(ch);
                }
                if (n + 1 < paragraphs.size())
                    QTest::keyClick(m_window, Qt::Key_Return);
                // spaced: an empty paragraph after each body paragraph.
                if (spaced && level == 0)
                    QTest::keyClick(m_window, Qt::Key_Return);
                QCoreApplication::processEvents();
            }
            return n >= paragraphs.size();
        };
    }

    QImage grab()
    {
        QImage image = m_window->grabWindow();
        if (m_pointerShown)
            drawPointer(image);
        return image;
    }

    // An arrow pointer, since the offscreen platform draws none.
    void drawPointer(QImage &image)
    {
        const qreal dpr = image.devicePixelRatio();
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(m_pointer);
        p.scale(m_pressed ? 0.92 : 1.0, m_pressed ? 0.92 : 1.0);
        Q_UNUSED(dpr);
        QPainterPath arrow;
        arrow.moveTo(0, 0);
        arrow.lineTo(0, 17);
        arrow.lineTo(4.2, 13.2);
        arrow.lineTo(7.2, 20);
        arrow.lineTo(10, 18.8);
        arrow.lineTo(7.1, 12.2);
        arrow.lineTo(12.5, 12.2);
        arrow.closeSubpath();
        p.setPen(QPen(QColor(0, 0, 0, 120), 3));
        p.drawPath(arrow.translated(0.6, 1));
        p.setPen(QPen(Qt::black, 1.3));
        p.setBrush(Qt::white);
        p.drawPath(arrow);
    }

    void capture()
    {
        if (!m_encoder)
            return;
        QImage image = grab().convertToFormat(QImage::Format_RGB32);
        m_encoder->write(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
        m_encoder->waitForBytesWritten(-1);
        ++m_frames;
    }

    void startRecording(const QString &name)
    {
        stopRecording();
        const QImage probe = m_window->grabWindow();
        m_clip = name;
        m_frames = 0;
        m_encoder = new QProcess(this);
        m_encoder->setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_encoder->start(QStringLiteral("ffmpeg"),
                         {"-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgra",
                          "-s", QStringLiteral("%1x%2").arg(probe.width()).arg(probe.height()),
                          "-r", QString::number(m_fps), "-i", "-",
                          "-c:v", "libx264", "-preset", "slow", "-crf", "14", "-pix_fmt", "yuv444p",
                          m_outDir + '/' + name + QStringLiteral(".mp4")});
        if (!m_encoder->waitForStarted())
            fail(QStringLiteral("cannot start ffmpeg"));
    }

    void stopRecording()
    {
        if (!m_encoder)
            return;
        m_encoder->closeWriteChannel();
        m_encoder->waitForFinished(-1);
        delete m_encoder;
        m_encoder = nullptr;
    }

    void finish()
    {
        m_tick.stop();
        stopRecording();
        if (!m_marks.isEmpty()) {
            QFile f(m_outDir + QStringLiteral("/marks.json"));
            QJsonObject all;
            if (f.open(QIODevice::ReadOnly))
                all = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            for (auto it = m_marks.begin(); it != m_marks.end(); ++it)
                all.insert(it.key(), it.value());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(QJsonDocument(all).toJson());
        }
        // The app may ask about unsaved changes on close; just leave.
        QCoreApplication::exit(0);
    }
};

class RuneDemoPlugin : public QGenericPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QGenericPluginFactoryInterface_iid FILE "runedemo.json")
public:
    // QGuiApplication owns and deletes what this returns: no parent.
    QObject *create(const QString &, const QString &) override { return new Director(nullptr); }
};

} // namespace

#include "RuneDemo.moc"
