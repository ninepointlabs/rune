#include "AiManager.h"

#include "AiStorage.h"
#include "ChatGptProvider.h"
#include "OpenRouterProvider.h"

#include <QDesktopServices>
#include <QRegularExpression>

namespace {

const QString kActiveKey = QStringLiteral("ai/activeProvider");
const QString kModelKeyPrefix = QStringLiteral("ai/model/");
const QString kPlanNoticeShownKey = QStringLiteral("chatgpt/planNoticeShown");

} // namespace

namespace {
bool s_testDefaults = false;
}

void AiManager::useTestDefaults()
{
    s_testDefaults = true;
}

AiManager::AiManager(QObject *parent) : QObject(parent)
{
    if (s_testDefaults) {
        m_storage = std::make_unique<MemoryAiStorage>();
        createProviders(AiEndpoints(), [](const QUrl &) { return false; });
    } else {
        m_storage = std::make_unique<SystemAiStorage>();
        createProviders(AiEndpoints(), [](const QUrl &url) { return QDesktopServices::openUrl(url); });
    }
}

AiManager::~AiManager()
{
    endStream();
    qDeleteAll(m_providers); // before m_storage, which they use
}

void AiManager::configure(std::unique_ptr<AiStorage> storage, const AiEndpoints &endpoints,
                          AiProvider::BrowserOpener openBrowser)
{
    endStream();
    // The old providers (and the storage they use) outlive the switch, so
    // QML bindings never see a null provider in between.
    const QList<AiProvider *> old = std::exchange(m_providers, {});
    std::unique_ptr<AiStorage> oldStorage = std::exchange(m_storage, std::move(storage));
    m_transcript.clear();
    m_showPlanNotice = false;
    createProviders(endpoints, std::move(openBrowser));
    for (AiProvider *p : old)
        p->disconnect(this);
    qDeleteAll(old);
    oldStorage.reset();
    emit transcriptChanged();
    emit planNoticeChanged();
    setError(QString(), QString());
}

void AiManager::createProviders(const AiEndpoints &endpoints, AiProvider::BrowserOpener openBrowser)
{
    m_providers = {new ChatGptProvider(m_storage.get(), endpoints, openBrowser),
                   new OpenRouterProvider(m_storage.get(), endpoints, openBrowser)};
    for (AiProvider *p : std::as_const(m_providers)) {
        // QML must not take ownership of these (they're returned from properties).
        QQmlEngine::setObjectOwnership(p, QQmlEngine::CppOwnership);
        auto previous = std::make_shared<AiProvider::State>(p->state());
        connect(p, &AiProvider::stateChanged, this, [this, p, previous] {
            const AiProvider::State was = std::exchange(*previous, p->state());
            onProviderStateChanged(p, was);
        });
        connect(p, &AiProvider::modelsChanged, this, [this, p] {
            if (p == activeProvider())
                emit activeChanged();
        });
    }
    m_activeId = m_storage->value(kActiveKey).toString();
    pickActiveProvider();
    emit providersChanged();
    emit activeChanged();
}

AiProvider *AiManager::chatgpt() const
{
    return provider(QStringLiteral("chatgpt"));
}

AiProvider *AiManager::openrouter() const
{
    return provider(QStringLiteral("openrouter"));
}

AiProvider *AiManager::provider(const QString &id) const
{
    for (AiProvider *p : m_providers)
        if (p->id() == id)
            return p;
    return nullptr;
}

void AiManager::onProviderStateChanged(AiProvider *provider, AiProvider::State previous)
{
    if (provider->state() == AiProvider::State::SignedIn && previous == AiProvider::State::SigningIn) {
        // Just signed in: use it, unless the active one is still usable.
        if (!isReady())
            setActiveProviderId(provider->id());
        if (provider == chatgpt() && !m_storage->value(kPlanNoticeShownKey).toBool()) {
            m_showPlanNotice = true;
            emit planNoticeChanged();
        }
    }
    if (provider->state() == AiProvider::State::SignedOut && provider == activeProvider()) {
        pickActiveProvider();
        if (provider == activeProvider()) // nothing else to switch to
            emit activeChanged();
    }
    if (provider == activeProvider())
        emit activeChanged();
}

// Keeps the remembered provider while it's signed in; otherwise any
// signed-in one; otherwise "".
void AiManager::pickActiveProvider()
{
    AiProvider *current = provider(m_activeId);
    if (current && current->state() == AiProvider::State::SignedIn)
        return;
    for (AiProvider *p : std::as_const(m_providers)) {
        if (p->state() == AiProvider::State::SignedIn) {
            setActiveProviderId(p->id());
            return;
        }
    }
    if (!current)
        m_activeId.clear();
}

void AiManager::setActiveProviderId(const QString &id)
{
    if (id == m_activeId || (!id.isEmpty() && !provider(id)))
        return;
    m_activeId = id;
    m_storage->setValue(kActiveKey, id);
    emit activeChanged();
}

QVariantList AiManager::models() const
{
    AiProvider *p = activeProvider();
    return p ? p->models() : QVariantList();
}

QString AiManager::model() const
{
    AiProvider *p = activeProvider();
    if (!p)
        return {};
    const QString chosen = m_storage->value(kModelKeyPrefix + p->id()).toString();
    const QVariantList list = p->models();
    // Until the list arrives, trust the saved choice; after, it must be on it.
    if (!chosen.isEmpty()
        && (list.isEmpty() || std::any_of(list.begin(), list.end(), [&](const QVariant &m) {
               return m.toMap().value(QStringLiteral("id")).toString() == chosen;
           })))
        return chosen;
    return p->defaultModel();
}

void AiManager::setModel(const QString &model)
{
    AiProvider *p = activeProvider();
    if (!p || model == this->model())
        return;
    m_storage->setValue(kModelKeyPrefix + p->id(), model);
    emit activeChanged();
}

bool AiManager::isReady() const
{
    AiProvider *p = activeProvider();
    return p && p->state() == AiProvider::State::SignedIn;
}

void AiManager::signIn(const QString &providerId)
{
    if (AiProvider *p = provider(providerId))
        p->signIn();
}

void AiManager::cancelSignIn(const QString &providerId)
{
    if (AiProvider *p = provider(providerId))
        p->cancelSignIn();
}

void AiManager::signOut(const QString &providerId)
{
    AiProvider *p = provider(providerId);
    if (!p)
        return;
    if (p == activeProvider())
        endStream();
    p->signOut();
}

void AiManager::refreshModels()
{
    if (AiProvider *p = activeProvider(); p && p->state() == AiProvider::State::SignedIn)
        p->refreshModels();
}

QString AiManager::instructions()
{
    return QStringLiteral("You are the writing assistant built into Rune, a word processor. "
                          "Be concise and write in plain prose unless asked otherwise. "
                          "When the user's document is provided, use it to answer questions about it.");
}

QString AiManager::editInstructions()
{
    return QStringLiteral(
        "You edit documents inside Rune, a word processor. The user gives an instruction and the document "
        "around their selection or cursor. Answer in exactly ONE of two forms, with nothing else at all: "
        "no preamble, no explanation, no notes about what you did.\n\n"
        "1. CHANGES to text that is already there (spelling, grammar, wording, punctuation, spacing, "
        "renaming, removing): one block per change, in document order:\n"
        "<<<<<<< FIND\n"
        "the exact existing text, copied character for character, without Markdown markup\n"
        "=======\n"
        "the replacement text\n"
        ">>>>>>> REPLACE\n"
        "Keep each FIND short but long enough to be unique (a few words around the change). Changes apply "
        "only inside <selection> if there is one, otherwise anywhere in the document. Replacement text is "
        "plain text: the existing formatting is kept, and a line break starts a new paragraph, so an empty "
        "line between two paragraphs means an empty paragraph there. To delete text, leave the replacement "
        "empty.\n\n"
        "2. NEW TEXT to write (drafting, adding, continuing, or rewriting the selection wholesale): just the "
        "text, in Markdown (headings, lists, **bold**, *italic*, tables). It replaces <selection> if there "
        "is one, otherwise it goes in at <cursor/>. Markdown has no empty paragraphs: for an empty line, "
        "write a line containing only &nbsp;. Only use a horizontal rule (---) if the user asks for a "
        "dividing line. Don't wrap the answer in a code block or repeat text that is already before or "
        "after the insertion point.\n\n"
        "Choose CHANGES whenever the instruction is about correcting or adjusting what is already written, "
        "and NEW TEXT when it asks for new content. Match the document's language, tone and conventions "
        "unless told otherwise.");
}

void AiManager::setDocument(DocumentController *document)
{
    if (m_document == document)
        return;
    if (m_document)
        m_document->textDocument()->disconnect(this);
    m_document = document;
    if (m_document) {
        // Anything that changes the document after an edit (typing, undo)
        // means undoLastEdit() would undo something else.
        connect(m_document->textDocument(), &QTextDocument::contentsChanged, this, [this] {
            if (!m_editing)
                setCanUndoEdit(false);
        });
    }
    emit documentChanged();
}

void AiManager::setCanUndoEdit(bool can)
{
    if (m_canUndoEdit == can)
        return;
    m_canUndoEdit = can;
    emit canUndoEditChanged();
}

namespace {

QString tail(const QString &text, int chars)
{
    return text.size() <= chars ? text : QStringLiteral("[…earlier text omitted…]\n") + text.right(chars);
}

QString head(const QString &text, int chars)
{
    return text.size() <= chars ? text : text.left(chars) + QStringLiteral("\n[…later text omitted…]");
}

} // namespace

// The document around the selection or cursor, marked up for the model.
QString AiManager::documentContext() const
{
    if (!m_document)
        return {};
    const DocumentController::MarkdownContext context = m_document->markdownContext();
    QString out = QStringLiteral("<document_before>\n") + tail(context.before, kContextChars) + QStringLiteral("\n</document_before>\n");
    if (context.selection.isEmpty())
        out += QStringLiteral("<cursor/>\n");
    else
        out += QStringLiteral("<selection>\n") + context.selection + QStringLiteral("\n</selection>\n");
    out += QStringLiteral("<document_after>\n") + head(context.after, kContextChars) + QStringLiteral("\n</document_after>");
    return out;
}

bool AiManager::send(const QString &prompt)
{
    if (isBusy()) {
        setError(QStringLiteral("busy"), tr("Wait for the current reply to finish, or stop it."));
        return false;
    }
    if (prompt.trimmed().isEmpty())
        return false;
    AiRequest request{QString(), instructions(), {}};
    if (m_document)
        request.instructions += QStringLiteral("\n\nThe user's document, as Markdown, with their selection or cursor "
                                               "position marked:\n") + documentContext();
    for (const QVariant &entry : std::as_const(m_transcript)) {
        const QVariantMap m = entry.toMap();
        if (m.value(QStringLiteral("kind")) == QLatin1String("edit"))
            continue; // edits are applied to the document, not part of the conversation
        request.messages.append({m.value(QStringLiteral("role")).toString(), m.value(QStringLiteral("text")).toString()});
    }
    request.messages.append({QStringLiteral("user"), prompt});
    if (!startRequest(request))
        return false;
    m_transcript.append(QVariantMap{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("text"), prompt}});
    m_transcript.append(QVariantMap{{QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("text"), QString()}});
    emit transcriptChanged();
    return true;
}

bool AiManager::edit(const QString &instruction)
{
    if (isBusy()) {
        setError(QStringLiteral("busy"), tr("Wait for the current reply to finish, or stop it."));
        return false;
    }
    if (instruction.trimmed().isEmpty())
        return false;
    if (!m_document) {
        setError(QStringLiteral("no_document"), tr("There's no document to edit."));
        return false;
    }
    const bool replacing = m_document->hasSelection();
    AiRequest request{QString(), editInstructions(),
                      {{QStringLiteral("user"), documentContext() + QStringLiteral("\n\nInstruction: ") + instruction}}};
    if (!startRequest(request))
        return false;
    if (!m_document->beginStreamedEdit()) {
        cancel();
        setError(QStringLiteral("document_busy"), tr("The document is busy (opening or saving). Try again in a moment."));
        return false;
    }
    m_editing = true;
    m_editText.clear();
    m_editKind = EditKind::Undecided;
    m_editParsed = 0;
    m_editApplied = m_editMissed = 0;
    setCanUndoEdit(false);
    m_transcript.append(QVariantMap{{QStringLiteral("role"), QStringLiteral("user")},
                                    {QStringLiteral("kind"), QStringLiteral("edit")},
                                    {QStringLiteral("text"), instruction}});
    m_transcript.append(QVariantMap{{QStringLiteral("role"), QStringLiteral("assistant")},
                                    {QStringLiteral("kind"), QStringLiteral("edit")},
                                    {QStringLiteral("replacing"), replacing},
                                    {QStringLiteral("text"), tr("Working on the document…")}});
    emit transcriptChanged();
    return true;
}

// Checks readiness and starts streaming `request` (model filled in). The
// stream's text goes to the transcript (send()) or the document (edit()).
bool AiManager::startRequest(const AiRequest &base)
{
    AiProvider *p = activeProvider();
    if (!isReady()) {
        setError(QStringLiteral("signed_out"), tr("Sign in to ChatGPT or OpenRouter first."));
        return false;
    }
    AiRequest request = base;
    request.model = model();
    if (request.model.isEmpty()) {
        setError(QStringLiteral("no_model"), tr("No model is available yet. Wait for the model list, then try again."));
        p->refreshModels();
        return false;
    }
    setError(QString(), QString());

    m_stream = p->stream(request);
    m_stream->setParent(this);
    connect(m_stream, &AiStream::delta, this, [this](const QString &text) {
        if (m_editing) {
            onEditDelta(text);
            return;
        }
        QVariantMap last = m_transcript.last().toMap();
        last.insert(QStringLiteral("text"), last.value(QStringLiteral("text")).toString() + text);
        m_transcript.last() = last;
        emit transcriptChanged();
    });
    connect(m_stream, &AiStream::finished, this, [this] {
        if (m_editing)
            finishEdit();
        endStream();
    });
    connect(m_stream, &AiStream::failed, this, [this](const QString &code, const QString &message) {
        setError(code, message);
        endStream();
    });
    emit busyChanged();
    return true;
}

namespace {

const QString kChangesMarker = QStringLiteral("<<<<<<<");

QString wordCount(qsizetype words)
{
    return words == 1 ? AiManager::tr("1 word") : AiManager::tr("%1 words").arg(words);
}

} // namespace

// A piece of an edit()'s reply: decide what kind of answer it is from its
// first characters, then write it in (new text) or apply each change block
// as soon as it is complete.
void AiManager::onEditDelta(const QString &text)
{
    QString piece = text;
    piece.remove(QLatin1Char('\r'));
    m_editText += piece;
    if (m_editKind == EditKind::Undecided) {
        QString lead = m_editText;
        while (!lead.isEmpty() && lead.front().isSpace())
            lead.remove(0, 1);
        if (lead.isEmpty() || (lead.size() < kChangesMarker.size() && kChangesMarker.startsWith(lead)))
            return; // can't tell yet
        m_editKind = lead.startsWith(kChangesMarker) ? EditKind::Changes : EditKind::Text;
        piece = m_editText; // everything so far
    }
    if (!m_document)
        return;
    if (m_editKind == EditKind::Text)
        m_document->appendStreamedText(piece);
    else
        applyCompleteChanges();
}

void AiManager::applyCompleteChanges()
{
    static const QRegularExpression block(
        QStringLiteral(R"(<<<<<<< ?FIND[^\n]*\n(.*?)\n?=======[^\n]*\n(.*?)\n?>>>>>>> ?REPLACE)"),
        QRegularExpression::DotMatchesEverythingOption);
    for (;;) {
        const QRegularExpressionMatch m = block.match(m_editText, m_editParsed);
        if (!m.hasMatch())
            return;
        m_editParsed = m.capturedEnd();
        const QString find = m.captured(1);
        if (find.trimmed().isEmpty())
            continue;
        if (m_document && m_document->applyStreamedReplacement(find, m.captured(2)))
            ++m_editApplied;
        else
            ++m_editMissed;
    }
}

// The reply is complete: finish the document edit and say what happened.
void AiManager::finishEdit()
{
    m_editing = false;
    if (m_editKind == EditKind::Undecided && !m_editText.trimmed().isEmpty()) {
        m_editKind = EditKind::Text; // a reply too short to tell: it's text
        if (m_document)
            m_document->appendStreamedText(m_editText);
    }
    QString summary;
    bool changed = false;
    if (m_editKind == EditKind::Changes) {
        applyCompleteChanges();
        changed = m_editApplied > 0;
        if (m_document)
            m_document->abortStreamedEdit(changed); // keeps the changes, or reverts nothing
        const QString missed = m_editMissed == 0 ? QString()
            : m_editMissed == 1 ? tr(" 1 change was skipped: its text wasn't found.")
                                : tr(" %1 changes were skipped: their text wasn't found.").arg(m_editMissed);
        summary = changed ? (m_editApplied == 1 ? tr("Made 1 change.") : tr("Made %1 changes.").arg(m_editApplied)) + missed
            : m_editMissed > 0 ? tr("Couldn't find the text the AI wanted to change, so the document wasn't changed.")
                               : tr("The AI suggested no changes.");
    } else {
        const QString text = m_editText.trimmed();
        changed = !text.isEmpty();
        if (m_document) {
            if (changed)
                m_document->finishStreamedEdit(m_editText);
            else
                m_document->abortStreamedEdit(false);
        }
        const bool replacing = m_transcript.last().toMap().value(QStringLiteral("replacing")).toBool();
        const qsizetype words = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
        summary = !changed ? tr("The AI returned nothing, so the document wasn't changed.")
            : replacing ? tr("Replaced the selection (%1).").arg(wordCount(words))
                        : tr("Inserted %1 at the cursor.").arg(wordCount(words));
    }
    QVariantMap last = m_transcript.last().toMap();
    last.insert(QStringLiteral("text"), summary);
    m_transcript.last() = last;
    emit transcriptChanged();
    setCanUndoEdit(changed);
}

void AiManager::undoLastEdit()
{
    if (!m_canUndoEdit || !m_document)
        return;
    setCanUndoEdit(false);
    m_document->undo();
}

void AiManager::cancel()
{
    if (m_stream)
        m_stream->cancel();
    endStream();
}

// Drops the stream and an assistant entry it left empty. An edit still
// open here was stopped or failed: a stop (no error) keeps what arrived, a
// failure reverts the document.
void AiManager::endStream()
{
    if (!m_stream)
        return;
    AiStream *stream = m_stream;
    m_stream = nullptr;
    stream->disconnect(this);
    stream->deleteLater();
    if (m_editing) {
        m_editing = false;
        const bool keep = m_lastError.isEmpty()
            && (m_editKind == EditKind::Changes ? m_editApplied > 0 : m_editKind == EditKind::Text);
        if (m_document)
            m_document->abortStreamedEdit(keep);
        QVariantMap last = m_transcript.last().toMap();
        last.insert(QStringLiteral("text"), keep ? tr("Stopped. Kept what was written so far.")
                                                 : m_lastError.isEmpty() ? tr("Stopped before anything was written.")
                                                                         : tr("Failed, so the document wasn't changed."));
        m_transcript.last() = last;
        emit transcriptChanged();
        setCanUndoEdit(keep);
    }
    if (!m_transcript.isEmpty()) {
        const QVariantMap last = m_transcript.last().toMap();
        if (last.value(QStringLiteral("role")) == QLatin1String("assistant")
            && last.value(QStringLiteral("text")).toString().isEmpty()) {
            m_transcript.removeLast();
            emit transcriptChanged();
        }
    }
    emit busyChanged();
}

void AiManager::clearTranscript()
{
    cancel();
    m_transcript.clear();
    emit transcriptChanged();
    setError(QString(), QString());
}

void AiManager::dismissPlanNotice()
{
    m_storage->setValue(kPlanNoticeShownKey, true);
    if (m_showPlanNotice) {
        m_showPlanNotice = false;
        emit planNoticeChanged();
    }
}

QString AiManager::manageUsageUrl() const
{
    return ChatGptProvider::manageUsageUrl();
}

QString AiManager::creditsUrl() const
{
    return OpenRouterProvider::creditsUrl();
}

void AiManager::setError(const QString &code, const QString &message)
{
    if (m_lastErrorCode == code && m_lastError == message)
        return;
    m_lastErrorCode = code;
    m_lastError = message;
    emit errorChanged();
}
