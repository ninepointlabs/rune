#pragma once

// AiManager: the AI layer's QML-facing API. Owns the providers (ChatGPT,
// OpenRouter), which one is active and which model it uses (both
// remembered), and a conversation: send() streams a reply into
// `transcript`.
//
//   AiManager { id: ai }
//   ai.signIn("chatgpt"); ai.send("Hello")  // transcript grows as it streams
//
// Built with SystemAiStorage (keyring + QSettings), the production
// endpoints and the desktop's browser; configure() swaps all three (tests).

#include "AiProvider.h"
#include "DocumentController.h"

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>

#include <memory>

class AiStorage;
class ChatGptProvider;
class OpenRouterProvider;

class AiManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(AiProvider *chatgpt READ chatgpt NOTIFY providersChanged)
    Q_PROPERTY(AiProvider *openrouter READ openrouter NOTIFY providersChanged)
    // "chatgpt", "openrouter", or "" when neither is signed in.
    Q_PROPERTY(QString activeProviderId READ activeProviderId WRITE setActiveProviderId NOTIFY activeChanged)
    // The active provider's models ([{id, name}]) and the one in use.
    Q_PROPERTY(QVariantList models READ models NOTIFY activeChanged)
    Q_PROPERTY(QString model READ model WRITE setModel NOTIFY activeChanged)
    // The active provider is signed in and send() can go.
    Q_PROPERTY(bool ready READ isReady NOTIFY activeChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    // [{role: "user"|"assistant", text}], the last assistant entry growing
    // while busy.
    Q_PROPERTY(QVariantList transcript READ transcript NOTIFY transcriptChanged)
    // Why the last send() failed ("" if it didn't), and a stable code for
    // the UI ("usage_limit", "credits", "signed_out", ...; see AiStream).
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)
    Q_PROPERTY(QString lastErrorCode READ lastErrorCode NOTIFY errorChanged)
    // OpenAI's one-time "You're using your ChatGPT plan" notice is due.
    Q_PROPERTY(bool showPlanNotice READ showPlanNotice NOTIFY planNoticeChanged)
    // The document the AI sees (both modes) and edits (edit()). Optional:
    // without one, send() has no document context and edit() fails.
    Q_PROPERTY(DocumentController *document READ document WRITE setDocument NOTIFY documentChanged)
    // The last edit() finished and nothing has changed the document since:
    // undoLastEdit() would take back exactly that edit.
    Q_PROPERTY(bool canUndoEdit READ canUndoEdit NOTIFY canUndoEditChanged)

public:
    explicit AiManager(QObject *parent = nullptr);
    ~AiManager() override;

    // For --auto-test, before any AiManager exists: start with
    // MemoryAiStorage and a browser opener that opens nothing, so a test
    // run can't read or change the user's keyring, settings or browser.
    static void useTestDefaults();

    // Replaces storage, endpoints and the browser opener, recreating the
    // providers (any conversation is cleared).
    void configure(std::unique_ptr<AiStorage> storage, const AiEndpoints &endpoints,
                   AiProvider::BrowserOpener openBrowser);
    AiStorage *storage() const { return m_storage.get(); }

    AiProvider *chatgpt() const;
    AiProvider *openrouter() const;
    AiProvider *provider(const QString &id) const;
    AiProvider *activeProvider() const { return provider(m_activeId); }

    QString activeProviderId() const { return m_activeId; }
    void setActiveProviderId(const QString &id);
    QVariantList models() const;
    QString model() const;
    void setModel(const QString &model);
    bool isReady() const;
    bool isBusy() const { return m_stream != nullptr; }
    QVariantList transcript() const { return m_transcript; }
    QString lastError() const { return m_lastError; }
    QString lastErrorCode() const { return m_lastErrorCode; }
    bool showPlanNotice() const { return m_showPlanNotice; }
    DocumentController *document() const { return m_document; }
    void setDocument(DocumentController *document);
    bool canUndoEdit() const { return m_canUndoEdit; }

    Q_INVOKABLE void signIn(const QString &providerId);
    Q_INVOKABLE void cancelSignIn(const QString &providerId);
    Q_INVOKABLE void signOut(const QString &providerId);
    // Fetches the active provider's models (cheap; call when the UI shows them).
    Q_INVOKABLE void refreshModels();
    // Ask: sends `prompt` with the conversation so far and the document
    // (Markdown, selection marked) as context; the reply streams into the
    // transcript. False (and lastError set) if it can't: not ready, busy,
    // or an empty prompt.
    Q_INVOKABLE bool send(const QString &prompt);
    // Edit: applies `instruction` to the document, as one undo step. The
    // model answers in one of two ways (editInstructions()):
    //  - new text (Markdown): replaces the selection, or is written at the
    //    cursor; streamed in as plain text, formatted when complete;
    //  - changes to existing text: find/replace blocks, each applied as it
    //    arrives (DocumentController::applyStreamedReplacement()), within
    //    the selection if any. Ones whose text isn't found are skipped and
    //    counted.
    // A failure reverts the document; cancel() keeps what was done. The
    // transcript records the instruction and what happened, not the text.
    // Each edit is independent of the conversation.
    Q_INVOKABLE bool edit(const QString &instruction);
    Q_INVOKABLE void undoLastEdit();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void clearTranscript();
    Q_INVOKABLE void dismissPlanNotice();

    Q_INVOKABLE QString manageUsageUrl() const;
    Q_INVOKABLE QString creditsUrl() const;

    // The system prompts: Ask (document context appended per request) and Edit.
    static QString instructions();
    static QString editInstructions();
    // Each side of the document context is cut to this many characters
    // (the end of `before`, the start of `after`) around the selection.
    static constexpr int kContextChars = 60000;

signals:
    void providersChanged();
    void activeChanged();
    void busyChanged();
    void transcriptChanged();
    void errorChanged();
    void planNoticeChanged();
    void documentChanged();
    void canUndoEditChanged();

private:
    void createProviders(const AiEndpoints &endpoints, AiProvider::BrowserOpener openBrowser);
    void onProviderStateChanged(AiProvider *provider, AiProvider::State previous);
    void pickActiveProvider();
    void setError(const QString &code, const QString &message);
    void endStream();
    bool startRequest(const AiRequest &request);
    QString documentContext() const;
    void setCanUndoEdit(bool can);

    std::unique_ptr<AiStorage> m_storage;
    QList<AiProvider *> m_providers;
    QString m_activeId;
    QVariantList m_transcript;
    QPointer<AiStream> m_stream;
    QString m_lastError;
    QString m_lastErrorCode;
    bool m_showPlanNotice = false;
    QPointer<DocumentController> m_document;
    // The request in flight is an edit(): the reply so far, which kind of
    // answer it turned out to be, how much of it has been parsed into
    // changes, and how those went.
    enum class EditKind { Undecided, Text, Changes };
    bool m_editing = false;
    QString m_editText;
    EditKind m_editKind = EditKind::Undecided;
    qsizetype m_editParsed = 0;
    int m_editApplied = 0;
    int m_editMissed = 0;
    void onEditDelta(const QString &text);
    void applyCompleteChanges();
    void finishEdit();
    bool m_canUndoEdit = false;
};
