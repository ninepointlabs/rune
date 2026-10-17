import QtQuick
import QtQuick.Controls.Basic
import Rune

// File → About Rune (and "About Rune" in the command palette): the
// version, who makes it, the license (readable in place), the support
// address, what leaves the machine, and what it's built with. Escape or
// Close closes it.
Popup {
    id: about

    required property var appRoot   // the Main.qml Window instance
    readonly property var theme: appRoot.theme
    property bool showLicense: false

    readonly property string privacy:
        "No account with us, no telemetry, no analytics. Your documents stay on your disk, and .docx/.doc "
        + "conversion runs LibreOffice on your machine. Rune goes online only for the AI: to sign in, to list "
        + "models when you open the AI panel while signed in, and when you send a request. A request sends your "
        + "prompt, the conversation so far, the document text around your cursor or selection (up to 60,000 "
        + "characters either side) and the document's story notes and sources to the provider you signed in "
        + "with: OpenAI (your ChatGPT plan) or OpenRouter (Claude, GPT, Grok and others). A source the AI adds "
        + "is checked by fetching its DOI or URL. Sign-in tokens and keys are kept in your desktop keyring."

    function show() {
        open()
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: 60
    width: Math.min(520, parent.width - 40)
    padding: 16
    modal: true
    focus: true
    onOpened: {
        showLicense = false
        AppInfo.checkLibreOffice()
        forceActiveFocus()
    }
    Overlay.modal: Rectangle { color: Qt.alpha("black", 0.35) }
    background: Rectangle {
        color: about.theme.background
        border.color: about.theme.muted
        border.width: 1
        radius: 6
    }

    component FieldLabel: Text {
        width: 76
        color: Qt.alpha(about.theme.foreground, 0.6)
        font.pixelSize: 12
    }
    component Value: Text {
        width: column.width - 76 - 12
        color: about.theme.foreground
        font.pixelSize: 13
        wrapMode: Text.WordWrap
    }

    Column {
        id: column
        width: parent.width
        spacing: 14

        Row {
            spacing: 14
            // The Rune mark, as on the File button and the app icon.
            Rectangle {
                width: 56
                height: 56
                radius: 10
                color: about.appRoot.barColor
                border.color: about.theme.muted
                Canvas {
                    id: mark
                    anchors.fill: parent
                    onPaint: {
                        const ctx = getContext("2d")
                        const s = width / 100
                        ctx.reset()
                        ctx.strokeStyle = about.theme.accent
                        ctx.lineWidth = 6 * s
                        ctx.lineCap = "square"
                        ctx.beginPath()
                        ctx.moveTo(30 * s, 20 * s); ctx.lineTo(30 * s, 80 * s)
                        ctx.moveTo(30 * s, 20 * s); ctx.lineTo(62 * s, 40 * s); ctx.lineTo(30 * s, 55 * s)
                        ctx.moveTo(62 * s, 40 * s); ctx.lineTo(70 * s, 80 * s)
                        ctx.stroke()
                    }
                    Connections {
                        target: about
                        function onThemeChanged() { mark.requestPaint() }
                    }
                }
            }
            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text {
                    text: AppInfo.name
                    color: about.theme.foreground
                    font.pixelSize: 20
                    font.bold: true
                }
                Text {
                    text: AppInfo.tagline
                    color: Qt.alpha(about.theme.foreground, 0.8)
                    font.pixelSize: 13
                }
                Text {
                    objectName: "aboutVersion"
                    text: "Version " + AppInfo.version
                    color: Qt.alpha(about.theme.foreground, 0.6)
                    font.pixelSize: 12
                }
            }
        }

        Grid {
            columns: 2
            columnSpacing: 12
            rowSpacing: 10
            verticalItemAlignment: Grid.AlignVCenter

            FieldLabel { text: "Made by" }
            Value { text: AppInfo.organization }

            FieldLabel { text: "Created" }
            Value { text: AppInfo.created }

            FieldLabel { text: "License" }
            Row {
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: AppInfo.license + ", " + AppInfo.copyright
                    color: about.theme.foreground
                    font.pixelSize: 13
                }
                PanelButton {
                    objectName: "aboutLicense"
                    theme: about.theme
                    label: about.showLicense ? "Hide" : "Read it"
                    onClicked: about.showLicense = !about.showLicense
                }
            }

            FieldLabel { text: "Support" }
            Row {
                spacing: 8
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: AppInfo.supportEmail
                    color: about.theme.foreground
                    font.pixelSize: 13
                }
                PanelButton {
                    objectName: "aboutEmail"
                    theme: about.theme
                    label: "Email"
                    onClicked: Qt.openUrlExternally("mailto:" + AppInfo.supportEmail + "?subject="
                        + encodeURIComponent("Rune " + AppInfo.version) + "&body="
                        + encodeURIComponent("\n\n--\n" + AppInfo.details))
                }
                PanelButton {
                    objectName: "aboutCopyEmail"
                    theme: about.theme
                    label: "Copy"
                    onClicked: AppInfo.copy(AppInfo.supportEmail)
                }
            }

            FieldLabel { text: "Privacy" }
            Value { objectName: "aboutPrivacy"; text: about.privacy; font.pixelSize: 12 }

            FieldLabel { text: "Built with" }
            Value {
                objectName: "aboutBuiltWith"
                text: "Qt " + AppInfo.qtVersion + ", QuaZip, OpenSSL, libsecret. For .docx/.doc: "
                    + (AppInfo.libreOfficeVersion === "" ? "LibreOffice (checking…)"
                       : AppInfo.libreOfficeVersion.startsWith("LibreOffice") ? AppInfo.libreOfficeVersion
                       : "LibreOffice, " + AppInfo.libreOfficeVersion)
                color: Qt.alpha(about.theme.foreground, 0.8)
                font.pixelSize: 12
            }
        }

        Rectangle {
            width: parent.width
            height: 180
            visible: about.showLicense
            color: about.appRoot.barColor
            border.color: about.theme.muted
            radius: 4
            Flickable {
                id: licenseView
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                contentHeight: licenseText.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Text {
                    id: licenseText
                    objectName: "aboutLicenseText"
                    width: licenseView.width - 10
                    text: AppInfo.licenseText
                    color: Qt.alpha(about.theme.foreground, 0.8)
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }

        Row {
            anchors.right: parent.right
            spacing: 6
            PanelButton {
                objectName: "aboutCopyDetails"
                theme: about.theme
                label: "Copy version details"
                onClicked: AppInfo.copy(AppInfo.details)
            }
            PanelButton {
                objectName: "aboutClose"
                theme: about.theme
                label: "Close"
                primary: true
                onClicked: about.close()
            }
        }
    }
}
