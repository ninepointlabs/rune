import QtQuick
import QtQuick.Controls.Basic
import Rune

// The AI side panel: sign-in for each provider, then a model picker and a
// simple conversation with the active one. Not yet connected to the
// document (that's the next step); see AiManager.
//
// ChatGPT wording follows OpenAI's Sign in with ChatGPT UI/UX guidelines
// ("Continue with ChatGPT", the plan-usage notice, "Using ChatGPT plan",
// "Usage limit reached" with "Manage usage").
Rectangle {
    id: panel

    required property var appRoot   // the Main.qml Window, for its theme
    required property AiManager ai
    required property DocumentController controller
    // "edit": the prompt changes the document; "ask": it's a question.
    property string mode: "edit"
    readonly property var theme: appRoot.theme
    readonly property bool usingChatGpt: ai.ready && ai.activeProviderId === "chatgpt"

    signal closeRequested()

    // Ctrl+Shift+E: show the panel in Edit mode with the prompt focused.
    function startEdit() {
        mode = "edit"
        visible = true
        prompt.forceActiveFocus()
    }

    color: appRoot.barColor

    // Models are fetched when the panel is shown and when the provider
    // changes (a binding, so only on an actual change: activeChanged also
    // fires when the model list arrives).
    readonly property string providerId: ai.activeProviderId
    onProviderIdChanged: if (visible) ai.refreshModels()
    onVisibleChanged: if (visible) ai.refreshModels()

    component Link: Text {
        property string url
        color: panel.theme.accent
        font.pixelSize: 12
        font.underline: linkMouse.containsMouse
        MouseArea {
            id: linkMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.activated()
        }
        signal activated()
        onActivated: if (url !== "") Qt.openUrlExternally(url)
    }

    component PanelButton: Rectangle {
        id: button
        property string label
        property bool primary: false
        property bool enabledState: true
        signal clicked()
        implicitWidth: buttonLabel.implicitWidth + 24
        implicitHeight: 30
        radius: 4
        opacity: enabledState ? 1 : 0.5
        color: primary ? (buttonMouse.containsMouse ? Qt.lighter(panel.theme.accent, 1.1) : panel.theme.accent)
                       : (buttonMouse.containsMouse ? Qt.alpha(panel.theme.foreground, 0.12) : Qt.alpha(panel.theme.foreground, 0.06))
        border.color: primary ? "transparent" : Qt.alpha(panel.theme.foreground, 0.2)
        Text {
            id: buttonLabel
            anchors.centerIn: parent
            text: button.label
            color: button.primary ? panel.theme.background : panel.theme.foreground
            font.pixelSize: 13
            font.bold: button.primary
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: button.enabledState
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
    }

    // One provider's sign-in state and actions.
    component AccountRow: Column {
        id: row
        required property var provider     // AiProvider
        required property string signInLabel
        required property string blurb
        spacing: 6
        width: parent ? parent.width : 0

        readonly property bool signedIn: provider.state === AiProvider.SignedIn
        readonly property bool signingIn: provider.state === AiProvider.SigningIn

        Row {
            spacing: 8
            width: parent.width
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: row.provider.name
                color: panel.theme.foreground
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.signedIn
                text: "● Signed in" + (row.provider.account !== "" ? " as " + row.provider.account : "")
                color: panel.theme.green !== undefined ? panel.theme.green : panel.theme.accent
                font.pixelSize: 12
                elide: Text.ElideRight
                width: Math.min(implicitWidth, row.width - 140)
            }
            Link {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.signedIn
                text: "Sign out"
                objectName: row.provider.id + "SignOut"
                onActivated: panel.ai.signOut(row.provider.id)
            }
        }
        Text {
            visible: !row.signedIn
            width: parent.width
            wrapMode: Text.WordWrap
            text: row.blurb
            color: Qt.alpha(panel.theme.foreground, 0.7)
            font.pixelSize: 12
        }
        Row {
            visible: !row.signedIn
            spacing: 10
            PanelButton {
                objectName: row.provider.id + "SignIn"
                label: row.signingIn ? "Waiting for the browser…" : row.signInLabel
                primary: !row.signingIn
                enabledState: !row.signingIn
                onClicked: panel.ai.signIn(row.provider.id)
            }
            Link {
                anchors.verticalCenter: parent.verticalCenter
                visible: row.signingIn
                text: "Cancel"
                onActivated: panel.ai.cancelSignIn(row.provider.id)
            }
        }
        Text {
            visible: row.provider.error !== ""
            width: parent.width
            wrapMode: Text.WordWrap
            text: row.provider.error
            color: row.signedIn ? Qt.alpha(panel.theme.foreground, 0.7)
                                : (panel.theme.red !== undefined ? panel.theme.red : panel.theme.foreground)
            font.pixelSize: 12
        }
        Link {
            visible: row.provider.errorLink !== ""
            text: "Open the key's page on OpenRouter"
            url: row.provider.errorLink
        }
    }

    Rectangle { // divider against the document
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: 1
        color: Qt.alpha(panel.theme.muted, 0.6)
    }

    Item {
        anchors { fill: parent; margins: 14; leftMargin: 15 }

        Text {
            id: title
            text: "AI"
            color: panel.theme.foreground
            font.pixelSize: 15
            font.bold: true
        }
        Link {
            anchors { right: parent.right; verticalCenter: title.verticalCenter }
            text: "Close"
            onActivated: panel.closeRequested()
        }

        Column {
            id: accounts
            anchors { left: parent.left; right: parent.right; top: title.bottom; topMargin: 14 }
            spacing: 14

            AccountRow {
                provider: panel.ai.chatgpt
                signInLabel: "Continue with ChatGPT"
                blurb: "Complete eligible AI requests in this app with usage included in your ChatGPT plan or credits balance."
            }
            AccountRow {
                provider: panel.ai.openrouter
                signInLabel: "Sign in with OpenRouter"
                blurb: "Use Claude, GPT and Grok models, paid from your OpenRouter credits."
            }

            Rectangle { width: parent.width; height: 1; color: Qt.alpha(panel.theme.muted, 0.5) }
        }

        // --- Conversation (once a provider is signed in) ---------------------
        Item {
            id: conversation
            visible: panel.ai.ready
            anchors { left: parent.left; right: parent.right; top: accounts.bottom; topMargin: 12; bottom: parent.bottom }

            Column {
                id: settings
                width: parent.width
                spacing: 8

                // Both signed in: choose which one answers.
                Row {
                    visible: panel.ai.chatgpt.state === AiProvider.SignedIn
                             && panel.ai.openrouter.state === AiProvider.SignedIn
                    spacing: 6
                    Repeater {
                        model: [["chatgpt", "ChatGPT"], ["openrouter", "OpenRouter"]]
                        PanelButton {
                            required property var modelData
                            label: modelData[1]
                            primary: panel.ai.activeProviderId === modelData[0]
                            onClicked: panel.ai.activeProviderId = modelData[0]
                        }
                    }
                }

                ComboBox {
                    id: modelPicker
                    objectName: "modelPicker"
                    width: parent.width
                    model: panel.ai.models
                    textRole: "name"
                    valueRole: "id"
                    displayText: panel.ai.models.length === 0 ? "Loading models…" : currentText
                    // Follows the manager's choice; picking writes it back.
                    currentIndex: panel.ai.models.findIndex(m => m.id === panel.ai.model)
                    onActivated: index => panel.ai.model = panel.ai.models[index].id
                    palette.button: panel.theme.background
                    palette.buttonText: panel.theme.foreground
                    palette.window: panel.theme.background
                    palette.text: panel.theme.foreground
                    palette.highlight: panel.theme.accent
                    palette.highlightedText: panel.theme.background
                    palette.base: panel.theme.background
                }

                // OpenAI's in-product indicator.
                Row {
                    visible: panel.usingChatGpt
                    spacing: 6
                    Text {
                        text: "Using ChatGPT plan"
                        color: Qt.alpha(panel.theme.foreground, 0.7)
                        font.pixelSize: 12
                    }
                    Text { text: "·"; color: Qt.alpha(panel.theme.foreground, 0.5); font.pixelSize: 12 }
                    Link { text: "Manage usage"; url: panel.ai.manageUsageUrl() }
                }

                // OpenAI's first-sign-in notice.
                Rectangle {
                    visible: panel.ai.showPlanNotice
                    objectName: "planNotice"
                    width: parent.width
                    height: noticeColumn.implicitHeight + 20
                    radius: 6
                    color: Qt.alpha(panel.theme.accent, 0.12)
                    border.color: Qt.alpha(panel.theme.accent, 0.4)
                    Column {
                        id: noticeColumn
                        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
                        spacing: 6
                        Text {
                            text: "You're using your ChatGPT plan"
                            color: panel.theme.foreground
                            font.pixelSize: 13
                            font.bold: true
                        }
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: "Eligible usage in this app uses your ChatGPT plan. Manage usage in your ChatGPT settings."
                            color: panel.theme.foreground
                            font.pixelSize: 12
                        }
                        PanelButton { label: "Got it"; onClicked: panel.ai.dismissPlanNotice() }
                    }
                }


            }

            ListView {
                id: transcript
                objectName: "transcript"
                anchors { left: parent.left; right: parent.right; top: settings.bottom; topMargin: 10
                          bottom: errorBox.visible ? errorBox.top : modeRow.top; bottomMargin: 8 }
                clip: true
                spacing: 10
                model: panel.ai.transcript
                onCountChanged: positionViewAtEnd()
                delegate: Column {
                    id: entry
                    required property var modelData
                    required property int index
                    readonly property bool isEdit: modelData.kind === "edit"
                    readonly property bool isLast: index === ListView.view.count - 1
                    width: ListView.view.width
                    spacing: 2
                    Text {
                        text: entry.modelData.role === "user" ? (entry.isEdit ? "You · edit" : "You")
                              : (entry.isEdit ? "Document" : (panel.usingChatGpt ? "ChatGPT" : "OpenRouter"))
                        color: Qt.alpha(panel.theme.foreground, 0.55)
                        font.pixelSize: 11
                        font.bold: true
                    }
                    TextEdit {
                        width: parent.width
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        text: entry.modelData.text !== "" ? entry.modelData.text : "…"
                        color: entry.isEdit && entry.modelData.role === "assistant"
                               ? Qt.alpha(panel.theme.foreground, 0.7) : panel.theme.foreground
                        font.italic: entry.isEdit && entry.modelData.role === "assistant"
                        selectionColor: panel.theme.accent
                        font.pixelSize: 13
                    }
                    Link {
                        objectName: "undoEdit"
                        visible: entry.isEdit && entry.isLast && panel.ai.canUndoEdit
                        text: "Undo this edit"
                        onActivated: panel.ai.undoLastEdit()
                    }
                }
            }

            // Errors; usage limits get OpenAI's prescribed treatment.
            Column {
                id: errorBox
                anchors { left: parent.left; right: parent.right; bottom: modeRow.top; bottomMargin: 8 }
                spacing: 4
                visible: panel.ai.lastError !== ""
                Text {
                    visible: panel.ai.lastErrorCode === "usage_limit"
                    text: "Usage limit reached"
                    color: panel.theme.foreground
                    font.pixelSize: 13
                    font.bold: true
                }
                Text {
                    objectName: "aiError"
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: panel.ai.lastErrorCode === "usage_limit"
                          ? "Review your plan or this app's limit in ChatGPT settings." : panel.ai.lastError
                    color: panel.theme.red !== undefined ? panel.theme.red : panel.theme.foreground
                    font.pixelSize: 12
                }
                PanelButton {
                    visible: panel.ai.lastErrorCode === "usage_limit"
                    label: "Manage usage"
                    primary: true
                    onClicked: Qt.openUrlExternally(panel.ai.manageUsageUrl())
                }
                Link {
                    visible: panel.ai.lastErrorCode === "credits"
                    text: "Add credits on OpenRouter"
                    url: panel.ai.creditsUrl()
                }
            }

            Row {
                id: modeRow
                anchors { left: parent.left; bottom: input.top; bottomMargin: 6 }
                spacing: 6
                PanelButton {
                    objectName: "editMode"
                    label: "Edit document"
                    primary: panel.mode === "edit"
                    onClicked: panel.mode = "edit"
                }
                PanelButton {
                    objectName: "askMode"
                    label: "Ask"
                    primary: panel.mode === "ask"
                    onClicked: panel.mode = "ask"
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: panel.mode === "ask" ? "Sees your document; answers here"
                          : panel.controller.streamingEdit ? "Writing into the document…"
                          : panel.controller.hasSelection ? "Rewrites the selected text" : "Writes at the cursor"
                    color: Qt.alpha(panel.theme.foreground, 0.6)
                    font.pixelSize: 11
                }
            }

            Rectangle {
                id: input
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: Math.min(140, Math.max(64, prompt.implicitHeight + 40))
                radius: 6
                color: panel.theme.background
                border.color: prompt.activeFocus ? panel.theme.accent : Qt.alpha(panel.theme.foreground, 0.2)

                TextArea {
                    id: prompt
                    objectName: "aiPrompt"
                    anchors { left: parent.left; right: parent.right; top: parent.top; bottom: sendRow.top; margins: 4 }
                    placeholderText: panel.mode === "ask"
                                     ? "Ask about your document, or anything (Enter to send)"
                                     : "Tell the AI what to write or change (Enter to apply)"
                    placeholderTextColor: Qt.alpha(panel.theme.foreground, 0.4)
                    color: panel.theme.foreground
                    selectionColor: panel.theme.accent
                    wrapMode: TextArea.Wrap
                    font.pixelSize: 13
                    background: null
                    Keys.onReturnPressed: event => {
                        if (event.modifiers & Qt.ShiftModifier) {
                            event.accepted = false
                        } else {
                            panel.send()
                        }
                    }
                }
                Row {
                    id: sendRow
                    anchors { right: parent.right; bottom: parent.bottom; margins: 6 }
                    spacing: 8
                    Link {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: panel.ai.transcript.length > 0 && !panel.ai.busy
                        text: "Clear"
                        onActivated: panel.ai.clearTranscript()
                    }
                    PanelButton {
                        objectName: "aiSend"
                        label: panel.ai.busy ? "Stop" : "Send"
                        primary: !panel.ai.busy
                        enabledState: panel.ai.busy || prompt.text.trim() !== ""
                        onClicked: panel.ai.busy ? panel.ai.cancel() : panel.send()
                    }
                }
            }
        }
    }

    function send() {
        if (mode === "ask" ? ai.send(prompt.text) : ai.edit(prompt.text))
            prompt.text = ""
    }
}
