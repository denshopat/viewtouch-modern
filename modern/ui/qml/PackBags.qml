import QtQuick
import QtQuick.Controls.Fusion
import QtQuick.Layouts

// A tuckshop's Bags screen (legacy ViewTouch's split check, for bags): the
// order packed into bags, a box a bag side by side. Touch items (in any
// box), then Put Here on the bag they go in; + New Bag makes one. Each bag
// says whose it is (its name: touch it) and its break (Little Lunch, Big
// Lunch). The order stays one order, paid once; a bag is a slip of its own.
Popup {
    id: p
    objectName: "packBags"
    property PosService pos
    property string face: Ui.font

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 800
    height: parent ? parent.height : 600
    modal: true
    focus: true
    closePolicy: Popup.NoAutoClose
    padding: 0
    background: Rectangle { color: "#14181f" }

    readonly property real unit: Math.max(14, Math.min(26, width * 0.0135)) * Ui.textScale
    readonly property real tapH: Math.max(unit * 2.6, 52)
    readonly property var breaks: pos && pos.service ? (pos.service.breaks ?? []) : []
    readonly property var check: pos ? pos.check : ({})
    readonly property var lines: pos && pos.hasCheck ? pos.lines.filter(l => !l.voided && !l.fee) : []
    // The bags there are: those with something in them, named, or given a break.
    readonly property var bagNumbers: {
        const set = {}
        for (const l of lines)
            if (l.seat > 0) set[l.seat] = true
        for (const k in (check.seatNames ?? {})) set[k] = true
        for (const k in (check.seatBreaks ?? {})) set[k] = true
        return Object.keys(set).map(Number).filter(n => n > 0).sort((a, b) => a - b)
    }
    function bagName(seat) { return (check.seatNames ?? {})[String(seat)] ?? "" }
    function bagBreak(seat) { return (check.seatBreaks ?? {})[String(seat)] ?? 1 }
    function nextBag() { return bagNumbers.length ? bagNumbers[bagNumbers.length - 1] + 1 : 1 }

    // The touched items: line id -> true.
    property var selected: ({})
    readonly property var selectedIds: Object.keys(selected).map(Number).filter(id => lines.some(l => l.id === id))
    function toggle(id) {
        const s = Object.assign({}, selected)
        if (s[id]) delete s[id]; else s[id] = true
        selected = s
    }
    onOpened: selected = ({})
    onClosed: selected = ({})

    // Put the touched items in bag `seat` (0: in no bag; -1: a new one). One
    // line of several: how many go is asked first.
    function putIn(seat) {
        const ids = selectedIds
        if (ids.length === 0)
            return
        const one = ids.length === 1 ? lines.find(l => l.id === ids[0]) : null
        if (one && one.quantity > 1 && one.countable) {
            howMany.ask(one, seat)
            return
        }
        move(ids, seat, 0)
    }
    function move(ids, seat, count) {
        pos.moveToBag(ids, seat, count)
        selected = ({})
    }
    function newBag() {
        if (selectedIds.length > 0)
            putIn(-1)
        else
            pos.setBag(nextBag(), "", 1)
    }

    contentItem: ColumnLayout {
        spacing: p.unit * 0.5

        // Title, what to do, Done.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: p.unit * 0.6
            Layout.bottomMargin: 0
            spacing: p.unit
            Text {
                textFormat: Text.PlainText
                text: qsTr("Pack the bags") + (p.check.label ? "  ·  " + p.check.label : "")
                color: "white"
                font.family: p.face
                font.pixelSize: p.unit * 1.4
                font.bold: true
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: p.selectedIds.length > 0 ? qsTr("%n item(s) touched: now Put Here on their bag", "", p.selectedIds.length)
                                               : qsTr("Touch items, then Put Here on the bag they go in")
                color: p.selectedIds.length > 0 ? "#ffd479" : Ui.muted
                font.family: p.face
                font.pixelSize: p.unit
                elide: Text.ElideRight
            }
            WidgetKey {
                objectName: "bagsDone"
                Layout.preferredWidth: p.unit * 8
                Layout.preferredHeight: p.tapH
                text: qsTr("Done")
                baseColor: Ui.go
                fontScale: 0.4
                takesTouch: true
                onClicked: p.close()
            }
        }

        // The boxes, side by side: what's in no bag, each bag, + New Bag.
        Flickable {
            id: strip
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: p.unit * 0.6
            Layout.topMargin: 0
            clip: true
            flickableDirection: Flickable.HorizontalFlick
            boundsBehavior: Flickable.StopAtBounds
            readonly property int boxes: p.bagNumbers.length + 2
            readonly property real gap: p.unit * 0.5
            readonly property real boxW: Math.max(p.unit * 16, (width - gap * (Math.min(boxes, 4) - 1)) / Math.min(boxes, 4))
            contentWidth: row.width
            contentHeight: height
            Row {
                id: row
                height: strip.height
                spacing: strip.gap
                Repeater {
                    model: [0].concat(p.bagNumbers)
                    delegate: Rectangle {
                        id: box
                        required property int modelData
                        readonly property int seat: modelData
                        readonly property var items: p.lines.filter(l => (l.seat > 0 ? l.seat : 0) === seat)
                        readonly property int count: items.filter(l => !l.comment).reduce((n, l) => n + l.quantity, 0)
                        // Touched items from another box: they can come here.
                        readonly property bool canTake: p.selectedIds.some(id => !items.some(l => l.id === id))
                        objectName: "bag" + seat
                        width: strip.boxW
                        height: row.height
                        radius: 10
                        color: seat === 0 ? "#1b2028" : "#1f2733"
                        border.color: seat === 0 ? "#3a4250" : "#4a6fa5"
                        border.width: 2

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: p.unit * 0.5
                            spacing: p.unit * 0.4

                            // Whose it is: touch to name it.
                            WidgetKey {
                                objectName: "bagName" + box.seat
                                visible: box.seat > 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: p.tapH
                                text: p.bagName(box.seat) || qsTr("Bag %1: touch to name").arg(box.seat)
                                baseColor: p.bagName(box.seat) ? "#2b3a52" : "#a86a12"
                                fontScale: 0.4
                                takesTouch: true
                                onClicked: naming.ask(box.seat)
                            }
                            Text {
                                visible: box.seat === 0
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                Layout.preferredHeight: p.tapH
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("Not in a bag yet")
                                color: Ui.muted
                                font.family: p.face
                                font.pixelSize: p.unit * 1.1
                                font.bold: true
                            }
                            // Its break.
                            RowLayout {
                                visible: box.seat > 0
                                Layout.fillWidth: true
                                spacing: p.unit * 0.3
                                Repeater {
                                    model: box.seat > 0 ? p.breaks.length : 0
                                    delegate: WidgetKey {
                                        required property int index
                                        objectName: "bag" + box.seat + "Break" + (index + 1)
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: p.tapH * 0.85
                                        text: p.breaks[index]
                                        accent: p.bagBreak(box.seat) === index + 1
                                        fontScale: 0.36
                                        takesTouch: true
                                        onClicked: p.pos.setBag(box.seat, p.bagName(box.seat), index + 1)
                                    }
                                }
                            }
                            // What's in it: touch to choose (touched again: not).
                            ListView {
                                id: list
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                spacing: 4
                                boundsBehavior: Flickable.StopAtBounds
                                model: box.items
                                delegate: Rectangle {
                                    id: item
                                    required property var modelData
                                    readonly property bool chosen: !!p.selected[modelData.id]
                                    objectName: "bagItem-" + modelData.id
                                    width: ListView.view.width
                                    height: Math.max(p.tapH, itemText.implicitHeight + p.unit * 0.6)
                                    radius: 8
                                    color: chosen ? Ui.accent : modelData.comment ? "transparent" : Ui.button
                                    TapHandler {
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: { if (TouchGuard.covered(point.scenePressPosition)) return; p.toggle(item.modelData.id) }
                                    }
                                    Text {
                                        id: itemText
                                        textFormat: Text.PlainText
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.margins: p.unit * 0.5
                                        text: item.modelData.comment ? "** " + item.modelData.name + " **"
                                              : (item.modelData.quantity > 1 ? item.modelData.quantity + " × " : "") + item.modelData.name
                                                + (item.modelData.modifiers.length ? "\n   " + item.modelData.modifiers.map(m => m.name).join(", ") : "")
                                        color: "white"
                                        font.family: p.face
                                        font.pixelSize: p.unit
                                        wrapMode: Text.WordWrap
                                    }
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: box.items.length === 0
                                    textFormat: Text.PlainText
                                    text: box.seat === 0 ? qsTr("Everything is packed") : qsTr("Empty")
                                    color: Ui.muted
                                    font.family: p.face
                                    font.pixelSize: p.unit
                                }
                            }
                            // Put the touched items here.
                            WidgetKey {
                                objectName: "bag" + box.seat + "Put"
                                Layout.fillWidth: true
                                Layout.preferredHeight: p.tapH
                                enabled: box.canTake
                                opacity: enabled ? 1 : 0.35
                                text: box.seat === 0 ? qsTr("Take Out of Bags") : qsTr("Put Here")
                                baseColor: box.canTake ? Ui.go : Ui.key
                                fontScale: 0.4
                                takesTouch: true
                                onClicked: p.putIn(box.seat)
                            }
                            // An empty bag can go.
                            WidgetKey {
                                objectName: "bag" + box.seat + "Remove"
                                visible: box.seat > 0 && box.items.length === 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: p.tapH * 0.8
                                text: qsTr("Remove This Bag")
                                fontScale: 0.34
                                takesTouch: true
                                onClicked: p.pos.removeBag(box.seat)
                            }
                        }
                    }
                }
                // + New Bag: with the touched items in it, or empty.
                Rectangle {
                    width: strip.boxW
                    height: row.height
                    radius: 10
                    color: "transparent"
                    border.color: "#3a4250"
                    border.width: 2
                    WidgetKey {
                        objectName: "bagNew"
                        anchors.centerIn: parent
                        width: parent.width - p.unit
                        height: p.tapH * 1.6
                        text: p.selectedIds.length > 0 ? qsTr("+ New Bag with These") : qsTr("+ New Bag")
                        baseColor: p.selectedIds.length > 0 ? Ui.go : Ui.key
                        fontScale: 0.36
                        takesTouch: true
                        onClicked: p.newBag()
                    }
                }
            }
            onContentWidthChanged: if (contentWidth > width) contentX = Math.max(0, contentWidth - width)   // a new bag in view
        }
    }

    // How many of a line of several go in the bag.
    Popup {
        id: howMany
        objectName: "bagHowMany"
        property var line: null
        property int seat: 0
        function ask(l, s) { line = l; seat = s; open() }
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent ? parent.width - 32 : 600, 640)
        modal: true
        padding: 18
        background: Rectangle { color: "#1f2733"; radius: 12; border.color: "#4a6fa5"; border.width: 2 }
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: howMany.line ? qsTr("How many of the %1 %2 go in?").arg(howMany.line.quantity).arg(howMany.line.name) : ""
                color: "white"
                font.family: p.face
                font.pixelSize: p.unit * 1.2
                font.bold: true
                wrapMode: Text.Wrap
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 5
                columnSpacing: 8
                rowSpacing: 8
                Repeater {
                    model: howMany.line ? howMany.line.quantity : 0
                    delegate: WidgetKey {
                        required property int index
                        objectName: "bagHowMany-" + (index + 1)
                        Layout.fillWidth: true
                        Layout.preferredHeight: p.tapH
                        text: index + 1 === howMany.line.quantity ? qsTr("All %1").arg(index + 1) : String(index + 1)
                        fontScale: 0.4
                        takesTouch: true
                        onClicked: {
                            const id = howMany.line.id, seat = howMany.seat
                            const n = index + 1 === howMany.line.quantity ? 0 : index + 1
                            howMany.close()
                            p.move([id], seat, n)
                        }
                    }
                }
            }
            WidgetKey {
                Layout.fillWidth: true
                Layout.preferredHeight: p.tapH
                text: qsTr("Cancel")
                fontScale: 0.4
                takesTouch: true
                onClicked: howMany.close()
            }
        }
    }

    // Whose a bag is: a name ("Sam 3B").
    Popup {
        id: naming
        objectName: "bagNaming"
        property int seat: 0
        function ask(s) {
            seat = s
            nameField.text = p.bagName(s)
            open()
            nameField.selectAll()
            nameField.forceActiveFocus()
        }
        function save() {
            const s = seat, name = nameField.text
            close()
            p.pos.setBag(s, name, p.bagBreak(s))
        }
        parent: Overlay.overlay
        x: ((parent ? parent.width : 800) - width) / 2
        y: p.unit * 2   // above the screen's keyboard
        width: Math.min(parent ? parent.width - 32 : 640, 700)
        modal: true
        padding: 18
        background: Rectangle { color: "#1f2733"; radius: 12; border.color: "#4a6fa5"; border.width: 2 }
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: qsTr("Bag %1: whose is it?").arg(naming.seat)
                color: "white"
                font.family: p.face
                font.pixelSize: p.unit * 1.2
                font.bold: true
            }
            TextField {
                id: nameField
                objectName: "bagNameField"
                Layout.fillWidth: true
                Layout.preferredHeight: p.tapH
                font.family: p.face
                font.pixelSize: p.unit * 1.1
                placeholderText: qsTr("Their name (and class)")
                onAccepted: naming.save()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                WidgetKey {
                    objectName: "bagNameCancel"
                    Layout.fillWidth: true
                    Layout.preferredHeight: p.tapH
                    text: qsTr("Cancel")
                    fontScale: 0.4
                    takesTouch: true
                    onClicked: naming.close()
                }
                WidgetKey {
                    objectName: "bagNameSave"
                    Layout.fillWidth: true
                    Layout.preferredHeight: p.tapH
                    text: qsTr("Save")
                    baseColor: Ui.go
                    fontScale: 0.4
                    takesTouch: true
                    onClicked: naming.save()
                }
            }
        }
    }

    // The order went (paid, put away): nothing to pack.
    Connections {
        target: p.pos
        function onCheckChanged() { if (p.opened && !p.pos.hasCheck) p.close() }
    }
}
