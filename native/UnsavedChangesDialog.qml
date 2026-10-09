import QtQuick
import QtQuick.Dialogs

// "Save changes?" with Save / Discard / Cancel; reports the answer as
// chosen("save" | "discard" | "cancel"). Closing the dialog without a
// button (Escape, the window's close button) counts as cancel.
//
// Built without qmlcachegen (see CMakeLists.txt): Qt 6.11's ahead-of-time
// compiled code resolves MessageDialog's button enums, which come from an
// extension namespace, to undefined ("Cannot read property 'Save' of
// undefined"), leaving the dialog with only an OK button.
MessageDialog {
    id: dialog

    signal chosen(string choice)

    // Set when a button answered, so the rejected() that may follow a
    // button (or come alone) is reported at most once.
    property bool answered: false

    buttons: MessageDialog.Save | MessageDialog.Discard | MessageDialog.Cancel
    onVisibleChanged: if (visible) answered = false
    onButtonClicked: (button, role) => {
        answered = true
        chosen(button === MessageDialog.Save ? "save" : button === MessageDialog.Discard ? "discard" : "cancel")
    }
    // Deferred: a button's buttonClicked() may arrive after rejected().
    onRejected: Qt.callLater(() => {
        if (!answered) {
            answered = true
            chosen("cancel")
        }
    })
}
