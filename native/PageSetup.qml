import QtQuick
import QtQuick.Controls.Basic
import Rune

// File → Page setup…: paper size, orientation and margins for the
// document. Margins are in inches where the locale uses Letter paper,
// centimetres elsewhere. Apply (or Enter) sets them as one undo step;
// Cancel (or Escape) leaves the document alone.
Popup {
    id: setup

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme

    // Paper sizes offered, portrait, in millimetres.
    readonly property var papers: [["Letter", 215.9, 279.4], ["Legal", 215.9, 355.6],
                                   ["A4", 210, 297], ["A5", 148, 210]]
    readonly property bool inches: controller.localeUsesLetter()
    readonly property real mmPerUnit: inches ? 25.4 : 10
    readonly property string unit: inches ? "in" : "cm"

    // What the dialog shows, until Apply.
    property real widthMm: 0
    property real heightMm: 0
    readonly property bool landscape: widthMm > heightMm
    // The paper among `papers` matching the size either way round, or -1.
    readonly property int paperIndex: papers.findIndex(p => Math.abs(p[1] - Math.min(widthMm, heightMm)) < 0.5
                                                            && Math.abs(p[2] - Math.max(widthMm, heightMm)) < 0.5)
    readonly property bool valid: [topField, bottomField, leftField, rightField].every(f => f.valid)

    function choosePaper(index) {
        const shortSide = papers[index][1], longSide = papers[index][2]
        widthMm = landscape ? longSide : shortSide
        heightMm = landscape ? shortSide : longSide
    }

    function setLandscape(on) {
        if (on !== landscape) {
            const w = widthMm
            widthMm = heightMm
            heightMm = w
        }
    }

    function apply() {
        if (!valid)
            return
        controller.setPageLayout({width: widthMm, height: heightMm, top: topField.mm, bottom: bottomField.mm,
                                  left: leftField.mm, right: rightField.mm})
        close()
    }

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: 80
    width: 380
    padding: 16
    modal: true
    focus: true
    onOpened: {
        const layout = controller.pageLayout
        widthMm = layout.width
        heightMm = layout.height
        topField.setMm(layout.top)
        bottomField.setMm(layout.bottom)
        leftField.setMm(layout.left)
        rightField.setMm(layout.right)
        topField.forceActiveFocus()
    }
    Overlay.modal: Rectangle { color: Qt.alpha("black", 0.35) }
    background: Rectangle {
        color: setup.theme.background
        border.color: setup.theme.muted
        border.width: 1
        radius: 6
    }

    // A margin in the dialog's unit; `mm` is its value in millimetres.
    component MarginField: Row {
        id: field
        property string label
        readonly property real value: Number(input.text)
        readonly property bool valid: input.text.trim() !== "" && !isNaN(value) && value >= 0
                                      && value * setup.mmPerUnit < 200
        readonly property real mm: value * setup.mmPerUnit
        function setMm(millimetres) {
            input.text = String(Math.round(millimetres / setup.mmPerUnit * 100) / 100)
        }
        function forceActiveFocus() { input.forceActiveFocus() }
        spacing: 6
        Text {
            width: 52
            anchors.verticalCenter: parent.verticalCenter
            text: field.label
            color: setup.theme.foreground
            font.pixelSize: 13
        }
        TextField {
            id: input
            objectName: field.label.toLowerCase() + "MarginField"
            width: 70
            height: 28
            font.pixelSize: 13
            color: setup.theme.foreground
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            background: Rectangle {
                color: "transparent"
                border.color: !field.valid ? setup.theme.red
                            : input.activeFocus ? setup.theme.accent : setup.theme.muted
                border.width: 1
                radius: 3
            }
            onAccepted: setup.apply()
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: setup.unit
            color: Qt.alpha(setup.theme.foreground, 0.6)
            font.pixelSize: 12
        }
    }

    Column {
        width: parent.width
        spacing: 14

        Text {
            text: "Page setup"
            color: setup.theme.foreground
            font.pixelSize: 15
            font.bold: true
        }

        Column {
            spacing: 6
            Text { text: "Paper"; color: Qt.alpha(setup.theme.foreground, 0.7); font.pixelSize: 12 }
            Row {
                spacing: 4
                Repeater {
                    model: setup.papers
                    ToolButton {
                        required property var modelData
                        required property int index
                        objectName: "paper" + modelData[0]
                        appRoot: setup.appRoot
                        label: modelData[0]
                        tip: (setup.inches ? (modelData[1] / 25.4).toFixed(1) + " × " + (modelData[2] / 25.4).toFixed(1) + " in"
                                           : (modelData[1] / 10) + " × " + (modelData[2] / 10) + " cm")
                        active: setup.paperIndex === index
                        onClicked: setup.choosePaper(index)
                    }
                }
                Text {
                    visible: setup.paperIndex < 0
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 6
                    text: "Custom (" + (setup.widthMm / setup.mmPerUnit).toFixed(2) + " × "
                          + (setup.heightMm / setup.mmPerUnit).toFixed(2) + " " + setup.unit + ")"
                    color: Qt.alpha(setup.theme.foreground, 0.7)
                    font.pixelSize: 12
                }
            }
        }

        Column {
            spacing: 6
            Text { text: "Orientation"; color: Qt.alpha(setup.theme.foreground, 0.7); font.pixelSize: 12 }
            Row {
                spacing: 4
                ToolButton {
                    objectName: "portraitButton"
                    appRoot: setup.appRoot
                    label: "Portrait"
                    active: !setup.landscape
                    onClicked: setup.setLandscape(false)
                }
                ToolButton {
                    objectName: "landscapeButton"
                    appRoot: setup.appRoot
                    label: "Landscape"
                    active: setup.landscape
                    onClicked: setup.setLandscape(true)
                }
            }
        }

        Column {
            spacing: 6
            Text { text: "Margins"; color: Qt.alpha(setup.theme.foreground, 0.7); font.pixelSize: 12 }
            Grid {
                columns: 2
                columnSpacing: 20
                rowSpacing: 6
                MarginField { id: topField; label: "Top" }
                MarginField { id: bottomField; label: "Bottom" }
                MarginField { id: leftField; label: "Left" }
                MarginField { id: rightField; label: "Right" }
            }
        }

        Row {
            anchors.right: parent.right
            spacing: 6
            ToolButton {
                objectName: "pageSetupCancel"
                appRoot: setup.appRoot
                label: "Cancel"
                onClicked: setup.close()
            }
            ToolButton {
                objectName: "pageSetupApply"
                appRoot: setup.appRoot
                label: "Apply"
                active: setup.valid
                available: setup.valid
                onClicked: setup.apply()
            }
        }
    }
}
