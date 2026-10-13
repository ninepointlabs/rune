import QtQuick
import QtQuick.Controls.Basic
import Rune

// The document's sources (CSL-JSON, see Sources) and citation style: add,
// edit, import BibTeX, cite, and the bibliography. Sources the AI added
// are flagged until reviewed.
Item {
    id: panel

    required property var appRoot   // the Main.qml Window instance
    required property DocumentController controller
    readonly property var theme: appRoot.theme

    readonly property var types: [["book", "Book"], ["article-journal", "Journal article"], ["chapter", "Book chapter"],
                                  ["webpage", "Web page"], ["report", "Report"], ["thesis", "Thesis"]]
    // The source being edited: its id, "new", or "" none; "bibtex" while
    // importing.
    property string editing: ""

    function startAdd() {
        editing = "new"
        form.load({type: "book"})
        form.focusFirst()
    }

    function startEdit(source) {
        editing = source.id
        form.load(source.item)
    }

    function save() {
        const item = form.item()
        if (editing === "new")
            controller.addSource(item)
        else
            controller.updateSource(editing, item)
        editing = ""
    }

    component Badge: Rectangle {
        property string label
        property color tint
        implicitWidth: badgeText.implicitWidth + 10
        implicitHeight: 16
        radius: 3
        color: Qt.alpha(tint, 0.2)
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: parent.label
            color: parent.tint
            font.pixelSize: 10
            font.bold: true
        }
    }

    Column {
        id: top
        anchors { left: parent.left; right: parent.right; top: parent.top }
        spacing: 8

        Row {
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Style"
                color: Qt.alpha(panel.theme.foreground, 0.7)
                font.pixelSize: 12
            }
            ComboBox {
                id: stylePicker
                objectName: "citationStylePicker"
                width: 190
                model: panel.controller.citationStyles()
                textRole: "name"
                valueRole: "id"
                currentIndex: model.findIndex(s => s.id === panel.controller.citationStyle)
                onActivated: index => panel.controller.citationStyle = model[index].id
                palette.button: panel.theme.background
                palette.buttonText: panel.theme.foreground
                palette.window: panel.theme.background
                palette.text: panel.theme.foreground
                palette.highlight: panel.theme.accent
                palette.highlightedText: panel.theme.background
                palette.base: panel.theme.background
            }
        }
        Flow {
            width: parent.width
            spacing: 6
            PanelButton {
                objectName: "insertCitationButton"
                theme: panel.theme
                label: "Insert citation…"
                primary: true
                available: panel.controller.sources.length > 0
                onClicked: panel.appRoot.openCitationPicker()
            }
            PanelButton {
                objectName: "addSource"
                theme: panel.theme
                label: "+ Add source"
                onClicked: panel.startAdd()
            }
            PanelButton {
                objectName: "importBibTeX"
                theme: panel.theme
                label: "Import BibTeX"
                onClicked: {
                    panel.editing = "bibtex"
                    bibtex.text = ""
                    bibtex.input.forceActiveFocus()
                }
            }
            PanelButton {
                objectName: "insertBibliographyButton"
                theme: panel.theme
                label: "Insert bibliography"
                onClicked: panel.appRoot.insertBibliography()
            }
        }
    }

    // BibTeX import.
    Column {
        id: importer
        anchors { left: parent.left; right: parent.right; top: top.bottom; topMargin: 10 }
        visible: panel.editing === "bibtex"
        spacing: 8
        PanelArea {
            id: bibtex
            objectName: "bibtexText"
            width: parent.width
            theme: panel.theme
            label: "Paste BibTeX (from Zotero, Google Scholar, a journal's “cite” link)"
            minimumHeight: 120
        }
        Row {
            spacing: 8
            PanelButton {
                objectName: "importBibTeXConfirm"
                theme: panel.theme
                label: "Import"
                primary: true
                available: bibtex.text.trim() !== ""
                onClicked: {
                    const n = panel.controller.importBibTeX(bibtex.text)
                    panel.appRoot.flash(n === 0 ? "No BibTeX entries found" : n === 1 ? "Imported 1 source" : "Imported " + n + " sources")
                    if (n > 0)
                        panel.editing = ""
                }
            }
            PanelButton {
                theme: panel.theme
                label: "Cancel"
                onClicked: panel.editing = ""
            }
        }
    }

    // Adding or editing one source.
    Flickable {
        id: formView
        anchors { left: parent.left; right: parent.right; top: top.bottom; bottom: parent.bottom; topMargin: 10 }
        visible: panel.editing !== "" && panel.editing !== "bibtex"
        contentHeight: form.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: form
            objectName: "sourceForm"
            width: formView.width - 10
            spacing: 6

            property string type: "book"
            readonly property string containerLabel: type === "article-journal" ? "Journal"
                                                   : type === "chapter" ? "Book title"
                                                   : type === "webpage" ? "Website" : "Series or collection"

            function focusFirst() { authors.input.forceActiveFocus() }

            function load(item) {
                type = item.type || "book"
                const people = item.author || []
                authors.text = people.map(p => p.literal ? p.literal : (p.given ? p.family + ", " + p.given : p.family)).join("\n")
                title.text = item.title || ""
                const parts = item.issued && item.issued["date-parts"]
                year.text = parts && parts.length && parts[0].length ? String(parts[0][0]) : ""
                container.text = item["container-title"] || ""
                publisher.text = item.publisher || ""
                place.text = item["publisher-place"] || ""
                volume.text = item.volume || ""
                issue.text = item.issue || ""
                pages.text = item.page || ""
                doi.text = item.DOI || ""
                url.text = item.URL || ""
                form.original = item
            }
            property var original: ({})

            // The form as CSL-JSON, over what it doesn't show (kept).
            function item() {
                const out = Object.assign({}, original)
                const set = (key, value) => { if (value.trim() !== "") out[key] = value.trim(); else delete out[key] }
                out.type = type
                out.author = authors.text.split("\n").map(l => l.trim()).filter(l => l !== "").map(line => {
                    if (line.includes(","))
                        return {family: line.split(",")[0].trim(), given: line.split(",").slice(1).join(",").trim()}
                    const words = line.split(/\s+/)
                    return words.length === 1 ? {family: line} : {family: words[words.length - 1], given: words.slice(0, -1).join(" ")}
                })
                if (out.author.length === 0)
                    delete out.author
                set("title", title.text)
                if (/^\d+$/.test(year.text.trim()))
                    out.issued = {"date-parts": [[parseInt(year.text.trim())]]}
                else if (year.text.trim() === "")
                    delete out.issued
                else
                    out.issued = {literal: year.text.trim()}
                set("container-title", container.text)
                set("publisher", publisher.text)
                set("publisher-place", place.text)
                set("volume", volume.text)
                set("issue", issue.text)
                set("page", pages.text)
                set("DOI", doi.text)
                set("URL", url.text)
                return out
            }

            ComboBox {
                objectName: "sourceType"
                width: parent.width
                model: panel.types.map(t => t[1])
                currentIndex: panel.types.findIndex(t => t[0] === form.type)
                onActivated: index => form.type = panel.types[index][0]
                palette.button: panel.theme.background
                palette.buttonText: panel.theme.foreground
                palette.window: panel.theme.background
                palette.text: panel.theme.foreground
                palette.highlight: panel.theme.accent
                palette.highlightedText: panel.theme.background
                palette.base: panel.theme.background
            }
            PanelArea {
                id: authors
                objectName: "sourceAuthors"
                width: parent.width
                theme: panel.theme
                label: "Authors, one per line (Family, Given)"
                minimumHeight: 48
            }
            PanelField { id: title; objectName: "sourceTitle"; width: parent.width; theme: panel.theme; label: "Title" }
            PanelField { id: year; objectName: "sourceYear"; width: parent.width; theme: panel.theme; label: "Year" }
            PanelField { id: container; objectName: "sourceContainer"; width: parent.width; theme: panel.theme; label: form.containerLabel }
            PanelField { id: publisher; width: parent.width; theme: panel.theme; label: "Publisher"
                         visible: form.type !== "article-journal" }
            PanelField { id: place; width: parent.width; theme: panel.theme; label: "Place"
                         visible: form.type === "book" || form.type === "chapter" || form.type === "report" }
            Row {
                width: parent.width
                spacing: 6
                visible: form.type === "article-journal"
                PanelField { id: volume; width: (parent.width - 12) / 3; theme: panel.theme; label: "Volume" }
                PanelField { id: issue; width: (parent.width - 12) / 3; theme: panel.theme; label: "Issue" }
                PanelField { id: pages; objectName: "sourcePages"; width: (parent.width - 12) / 3; theme: panel.theme; label: "Pages" }
            }
            PanelField { id: doi; objectName: "sourceDoi"; width: parent.width; theme: panel.theme; label: "DOI" }
            PanelField { id: url; width: parent.width; theme: panel.theme; label: "URL" }
            Row {
                spacing: 8
                PanelButton {
                    objectName: "saveSource"
                    theme: panel.theme
                    label: "Save"
                    primary: true
                    available: title.text.trim() !== ""
                    onClicked: panel.save()
                }
                PanelButton {
                    theme: panel.theme
                    label: "Cancel"
                    onClicked: panel.editing = ""
                }
                PanelButton {
                    objectName: "deleteSource"
                    visible: panel.editing !== "new"
                    theme: panel.theme
                    label: "Delete"
                    onClicked: {
                        panel.controller.removeSource(panel.editing)
                        panel.editing = ""
                    }
                }
            }
        }
    }

    Text {
        anchors { left: parent.left; right: parent.right; top: top.bottom; topMargin: 14 }
        visible: list.count === 0 && panel.editing === ""
        wrapMode: Text.WordWrap
        text: "Add the sources you cite, or import them as BibTeX. Insert a citation (Ctrl+Shift+C) and Rune "
              + "formats it in the style above and keeps the bibliography. The AI can cite them as well, and add "
              + "sources of its own, which are marked for you to check."
        color: Qt.alpha(panel.theme.foreground, 0.55)
        font.pixelSize: 12
    }

    ListView {
        id: list
        objectName: "sourcesList"
        anchors { left: parent.left; right: parent.right; top: importer.visible ? importer.bottom : top.bottom
                  bottom: parent.bottom; topMargin: 10 }
        visible: !formView.visible
        clip: true
        spacing: 4
        model: panel.controller.sources
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        delegate: Rectangle {
            id: card
            required property var modelData
            width: ListView.view.width
            height: cardColumn.implicitHeight + 12
            radius: 4
            color: cardMouse.containsMouse ? Qt.alpha(panel.theme.foreground, 0.08) : Qt.alpha(panel.theme.foreground, 0.03)
            border.color: card.modelData.unreviewed ? Qt.alpha(panel.theme.yellow !== undefined ? panel.theme.yellow : panel.theme.accent, 0.6)
                                                    : "transparent"
            MouseArea {
                id: cardMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: panel.startEdit(card.modelData)
            }
            Column {
                id: cardColumn
                anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
                spacing: 3
                Text {
                    width: parent.width
                    text: card.modelData.label
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    color: panel.theme.foreground
                    font.pixelSize: 12
                }
                Row {
                    spacing: 6
                    Text {
                        text: "@" + card.modelData.id
                        color: Qt.alpha(panel.theme.foreground, 0.5)
                        font.pixelSize: 11
                        font.family: "monospace"
                    }
                    Badge {
                        visible: card.modelData.cited
                        label: "CITED"
                        tint: panel.theme.accent
                    }
                    Badge {
                        visible: card.modelData.link === "broken"
                        label: "LINK BROKEN"
                        tint: panel.theme.red !== undefined ? panel.theme.red : panel.theme.accent
                    }
                    Badge {
                        visible: card.modelData.unreviewed
                        label: "ADDED BY AI · CHECK IT"
                        tint: panel.theme.yellow !== undefined ? panel.theme.yellow : panel.theme.accent
                    }
                }
                PanelButton {
                    objectName: "markReviewed_" + card.modelData.id
                    visible: card.modelData.unreviewed
                    theme: panel.theme
                    label: "Mark reviewed"
                    onClicked: panel.controller.markSourceReviewed(card.modelData.id)
                }
            }
        }
    }
}
