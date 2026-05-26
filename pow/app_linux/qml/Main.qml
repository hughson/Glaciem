// Main.qml -- Glaciem Miner dashboard.
//
// Mirrors the Mac/Windows/Android layout: GLACIEM header, large hashrate
// card, stats row, wallet card with copy-address and intensity, latest
// hash, START/STOP button. Settings + Generate/Restore dialogs hang off
// the same screen.

import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: root
    width: 460
    height: 880
    visible: true
    title: "Glaciem Miner"
    color: bg

    // palette -- mirrors RimeMiner.swift / MainActivity.kt
    readonly property color bg:    "#0D0D10"
    readonly property color card:  "#1A1A20"
    readonly property color amber: "#3FC1E0"
    readonly property color white_: "#F0F0F4"
    readonly property color dim:   Qt.rgba(1, 1, 1, 0.42)
    readonly property color green_: "#46C86E"
    readonly property color red_:   "#E65050"

    readonly property string monoFamily: "Menlo, monospace"

    // ====== layout ===========================================================

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        // -- header --
        RowLayout {
            Layout.fillWidth: true
            ColumnLayout {
                spacing: 2
                Text { text: "GLACIEM"; color: amber
                    font { family: monoFamily; pixelSize: 28; bold: true; letterSpacing: 4 } }
                Text { text: "PROOF-OF-WORK MINER  ·  v" + GlaciemVersion; color: dim
                    font { family: monoFamily; pixelSize: 10; letterSpacing: 1.8 } }
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                width: 64; height: 26
                color: card
                radius: 6
                Text {
                    anchors.centerIn: parent
                    text: "SETTINGS"
                    color: amber
                    font { family: monoFamily; pixelSize: 9; bold: true; letterSpacing: 1 }
                }
                MouseArea { anchors.fill: parent; onClicked: settingsDialog.open() }
            }
        }

        // -- hashrate card --
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 170
            radius: 12
            color: card
            ColumnLayout {
                anchors.fill: parent
                anchors.topMargin: 18
                spacing: 0
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: MinerEngine.mining ? Math.round(MinerEngine.hashrate).toString() : "0"
                    color: white_
                    font { family: monoFamily; pixelSize: 56; bold: true }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 6
                    text: "HASHES / SECOND"
                    color: dim
                    font { family: monoFamily; pixelSize: 10; letterSpacing: 1.5 }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 6
                    text: MinerEngine.device
                    color: amber
                    font { family: monoFamily; pixelSize: 11 }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.topMargin: 8
                    text: !MinerEngine.mining ? "Idle"
                        : (MinerEngine.daemonConnected ? "Mining" : "No daemon")
                    color: !MinerEngine.mining ? dim
                        : (MinerEngine.daemonConnected ? green_ : red_)
                    font { family: monoFamily; pixelSize: 11; bold: true }
                }
                Item { Layout.fillHeight: true }
            }
        }

        // -- stats row (blocks / height / best bits) --
        RowLayout {
            Layout.fillWidth: true
            spacing: 18
            Repeater {
                model: [
                    { label: "BLOCKS FOUND", value: MinerEngine.blocksFound.toString() },
                    { label: "BLOCK HEIGHT", value: MinerEngine.height.toString() },
                    { label: "BEST BITS",    value: MinerEngine.bestBits.toString() },
                ]
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: modelData.value
                        color: white_
                        font { family: monoFamily; pixelSize: 22; bold: true }
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: modelData.label
                        color: dim
                        font { family: monoFamily; pixelSize: 9; letterSpacing: 1 }
                    }
                }
            }
        }

        // -- wallet card --
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 200
            radius: 12
            color: card

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "WALLET"; color: dim
                        font { family: monoFamily; pixelSize: 10; letterSpacing: 1.5 } }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: !MinerEngine.hasWallet ? "NO WALLET"
                            : !MinerEngine.walletConnected ? "OFFLINE"
                            : MinerEngine.walletSyncing ? "SYNCING"
                            : "CONNECTED"
                        color: !MinerEngine.hasWallet ? dim
                            : !MinerEngine.walletConnected ? red_
                            : MinerEngine.walletSyncing ? amber
                            : green_
                        font { family: monoFamily; pixelSize: 10; bold: true; letterSpacing: 1 }
                    }
                }
                Text {
                    Layout.topMargin: 8
                    text: MinerEngine.walletSyncing
                        ? ("catching up... block " + MinerEngine.walletHeight + " / " + MinerEngine.targetHeight)
                        : (MinerEngine.hasWallet
                            ? (MinerEngine.balance / 1e12).toFixed(6) + " GLAC"
                            : "—")
                    color: MinerEngine.walletSyncing ? amber : white_
                    font { family: monoFamily; pixelSize: 22; bold: !MinerEngine.walletSyncing }
                }
                Text {
                    Layout.topMargin: 8
                    text: {
                        let a = MinerEngine.walletAddress;
                        if (!a) return "no wallet generated"
                        if (a.length > 24) return a.substring(0, 11) + "..." + a.substring(a.length - 11);
                        return a;
                    }
                    color: amber
                    font { family: monoFamily; pixelSize: 11 }
                }
                Item { Layout.fillHeight: true }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    SmallButton { text: "SEND";    enabled: MinerEngine.hasWallet
                        onClicked: sendDialog.open() }
                    SmallButton { text: "RECEIVE"; enabled: MinerEngine.hasWallet
                        onClicked: receiveDialog.open() }
                    SmallButton { text: "HISTORY"; enabled: MinerEngine.hasWallet
                        onClicked: { historyDialog.text = "loading..."; historyDialog.open();
                                     MinerEngine.requestHistory() } }
                }
            }
        }

        Item { Layout.fillHeight: true }

        // -- start/stop button --
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            radius: 12
            color: MinerEngine.hasWallet
                ? (MinerEngine.mining ? red_ : amber)
                : card
            opacity: MinerEngine.hasWallet ? 1.0 : 0.6
            Text {
                anchors.centerIn: parent
                text: !MinerEngine.hasWallet ? "GENERATE OR RESTORE A WALLET TO MINE"
                    : (MinerEngine.mining ? "STOP" : "START MINING")
                color: !MinerEngine.hasWallet ? dim
                    : (MinerEngine.mining ? white_ : bg)
                font { family: monoFamily; pixelSize: 14; bold: true; letterSpacing: 1.5 }
            }
            MouseArea {
                anchors.fill: parent
                enabled: MinerEngine.hasWallet
                onClicked: MinerEngine.toggleMining()
            }
        }
    }

    // ====== generated-wallet dialog (shown when MinerEngine emits) ===========

    Connections {
        target: MinerEngine
        function onGeneratedWallet(address, seed) {
            generatedDialog.addr = address;
            generatedDialog.seed = seed;
            generatedDialog.open();
        }
    }

    Dialog {
        id: generatedDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 480)
        property string addr: ""
        property string seed: ""
        background: Rectangle { color: bg; border.color: card; radius: 12 }
        title: "NEW WALLET"
        contentItem: ColumnLayout {
            spacing: 10
            Text { text: "Address:"; color: dim
                font { family: root.monoFamily; pixelSize: 11 } }
            Text {
                Layout.fillWidth: true
                text: generatedDialog.addr
                color: root.amber; wrapMode: Text.WrapAnywhere
                font { family: root.monoFamily; pixelSize: 11 }
            }
            Text { text: "25-word recovery seed:"; color: dim
                font { family: root.monoFamily; pixelSize: 11 } }
            Text {
                Layout.fillWidth: true
                text: generatedDialog.seed
                color: root.white_; wrapMode: Text.WordWrap
                font { family: root.monoFamily; pixelSize: 12 }
            }
            Text {
                Layout.fillWidth: true
                text: "IMPORTANT — write this seed down now. It is the only way to recover this wallet."
                color: root.amber; wrapMode: Text.WordWrap
                font { family: root.monoFamily; pixelSize: 10 }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: bg }
            Button {
                text: "USE THIS WALLET"
                onClicked: { MinerEngine.useGeneratedWallet(); generatedDialog.close() }
            }
            Button {
                text: "DISCARD"
                onClicked: { MinerEngine.discardGeneratedWallet(); generatedDialog.close() }
            }
        }
    }

    // ====== restore-from-seed dialog =========================================

    Dialog {
        id: restoreDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 480)
        background: Rectangle { color: bg; border.color: card; radius: 12 }
        title: "RESTORE WALLET"
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "Paste the 25-word recovery seed. Replaces the wallet currently in this app."
                font { family: root.monoFamily; pixelSize: 10 }
            }
            TextArea {
                id: seedInput
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                color: root.white_
                wrapMode: TextEdit.WordWrap
                font { family: root.monoFamily; pixelSize: 12 }
                background: Rectangle { color: root.card; radius: 6 }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: bg }
            Button {
                text: "RESTORE"
                onClicked: {
                    let words = seedInput.text.trim().split(/\s+/);
                    if (words.length < 24) { return }
                    MinerEngine.restoreWallet(seedInput.text);
                    seedInput.text = "";
                    restoreDialog.close();
                }
            }
            Button { text: "CANCEL"; onClicked: { seedInput.text = ""; restoreDialog.close() } }
        }
    }

    // ====== send dialog ======================================================

    Connections {
        target: MinerEngine
        // Clear the recipient + amount on a successful send so the user
        // can't accidentally hit SEND a second time and double-pay.
        // Keep them on failure so they can fix the issue and retry
        // without re-typing.
        function onSendResult(line) {
            sendDialog.resultText = line
            if (line && line.indexOf("sent") === 0) {
                addrField.text = ""
                amountField.text = ""
            }
        }
        function onSweepResult(line) { sendDialog.resultText = line }
        function onHistoryResult(text) { historyDialog.text = text || "no sends or receives yet" }
    }

    Dialog {
        id: sendDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 480)
        background: Rectangle { color: bg; border.color: card; radius: 12 }
        title: "SEND GLAC"
        property string resultText: ""
        onOpened: { addrField.text = ""; amountField.text = ""; resultText = "" }
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "Unlocked balance: " + (MinerEngine.unlockedBalance / 1e12).toFixed(6) + " GLAC"
                font { family: root.monoFamily; pixelSize: 10 }
            }
            Text { text: "recipient address"; color: dim
                font.family: root.monoFamily; font.pixelSize: 10 }
            TextField {
                id: addrField
                Layout.fillWidth: true; color: root.white_
                font { family: root.monoFamily; pixelSize: 11 }
                background: Rectangle { color: root.card; radius: 6 }
            }
            Text { text: "amount (GLAC)"; color: dim
                font.family: root.monoFamily; font.pixelSize: 10 }
            TextField {
                id: amountField
                Layout.fillWidth: true; color: root.white_
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                font { family: root.monoFamily; pixelSize: 12 }
                background: Rectangle { color: root.card; radius: 6 }
            }
            Text {
                Layout.fillWidth: true
                text: sendDialog.resultText
                visible: sendDialog.resultText.length > 0
                color: sendDialog.resultText.startsWith("sent") ||
                       sendDialog.resultText.startsWith("swept")
                       ? root.green_ : root.amber
                wrapMode: Text.WordWrap
                font { family: root.monoFamily; pixelSize: 10 }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "Mined coins won't send (\"not enough outputs\")? Sweep them into spendable form:"
                font { family: root.monoFamily; pixelSize: 9 }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: bg }
            Button { text: "SEND"
                onClicked: {
                    sendDialog.resultText = "sending…";
                    MinerEngine.requestSend(addrField.text, parseFloat(amountField.text) || 0);
                }
            }
            Button { text: "SWEEP UNMIXABLE"
                onClicked: { sendDialog.resultText = "sweeping…"; MinerEngine.requestSweep() }
            }
            Button { text: "CLOSE"; onClicked: sendDialog.close() }
        }
    }

    // ====== receive dialog ==================================================

    Dialog {
        id: receiveDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 480)
        background: Rectangle { color: bg; border.color: card; radius: 12 }
        title: "RECEIVE"
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "Send GLAC to this address. Use the full address (no shortening)."
                font { family: root.monoFamily; pixelSize: 10 }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WrapAnywhere
                text: MinerEngine.walletAddress
                color: root.amber
                font { family: root.monoFamily; pixelSize: 12 }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: bg }
            Button { text: "COPY"; onClicked: MinerEngine.copyAddressToClipboard() }
            Button { text: "CLOSE"; onClicked: receiveDialog.close() }
        }
    }

    // ====== history dialog ==================================================

    Dialog {
        id: historyDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 520)
        height: Math.min(parent.height - 40, 480)
        background: Rectangle { color: bg; border.color: card; radius: 12 }
        title: "HISTORY"
        property string text: ""
        contentItem: ScrollView {
            clip: true
            TextArea {
                readOnly: true
                wrapMode: TextArea.NoWrap
                text: historyDialog.text
                color: root.white_
                background: Rectangle { color: root.card; radius: 6 }
                font { family: root.monoFamily; pixelSize: 11 }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: bg }
            Button { text: "REFRESH"; onClicked: { historyDialog.text = "loading..."; MinerEngine.requestHistory() } }
            Button { text: "CLOSE"; onClicked: historyDialog.close() }
        }
    }

    // ====== settings dialog ==================================================

    Dialog {
        id: settingsDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width - 40, 480)
        background: Rectangle { color: bg; border.color: card; radius: 12 }
        title: "SETTINGS"
        property alias hostText: hostField.text
        property alias portText: portField.text
        // v1.1.14+: local thread-count buffer so the Settings dialog can show
        // the user's tentative pick before they hit SAVE.
        property int pendingThreads: 1
        onAboutToShow: {
            hostField.text = MinerEngine.nodeHost
            portField.text = MinerEngine.nodePort
            poolToggle.checked = MinerEngine.poolEnabled
            poolUrlField.text = MinerEngine.poolUrl
            settingsDialog.pendingThreads = MinerEngine.threadCount
        }
        contentItem: ColumnLayout {
            spacing: 10
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "The Glaciem node the wallet syncs from. The miner ignores this setting — it uses an automatic multi-node fallback (Cloudflare Worker + bootstrap nodes + discovered peers). Set 127.0.0.1 port 19081 to point the wallet at a local rimed."
                font { family: root.monoFamily; pixelSize: 10 }
            }
            Text { text: "node host"; color: dim; font.family: root.monoFamily; font.pixelSize: 10 }
            TextField {
                id: hostField
                Layout.fillWidth: true; color: root.white_
                font { family: root.monoFamily; pixelSize: 12 }
                background: Rectangle { color: root.card; radius: 6 }
            }
            Text { text: "node port"; color: dim; font.family: root.monoFamily; font.pixelSize: 10 }
            TextField {
                id: portField
                Layout.fillWidth: true; color: root.white_
                inputMethodHints: Qt.ImhDigitsOnly
                font { family: root.monoFamily; pixelSize: 12 }
                background: Rectangle { color: root.card; radius: 6 }
            }

            // -- v1.1.14+: thread count picker --
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: dim; opacity: 0.4 }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "CPU THREADS — how many cores the miner uses. More threads = more hashrate, more heat. Recommended is half your cores."
                font { family: root.monoFamily; pixelSize: 10 }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    Layout.preferredWidth: 40; Layout.preferredHeight: 40
                    radius: 6
                    color: settingsDialog.pendingThreads > 1 ? amber : root.card
                    Text {
                        anchors.centerIn: parent; text: "−"
                        color: settingsDialog.pendingThreads > 1 ? bg : dim
                        font { family: root.monoFamily; pixelSize: 18; bold: true }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: settingsDialog.pendingThreads > 1
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: settingsDialog.pendingThreads = Math.max(1, settingsDialog.pendingThreads - 1)
                    }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 40
                    radius: 6
                    color: root.card
                    Text {
                        anchors.centerIn: parent
                        text: settingsDialog.pendingThreads + " of " + MinerEngine.maxCores
                        color: amber
                        font { family: root.monoFamily; pixelSize: 14; bold: true }
                    }
                }
                Rectangle {
                    Layout.preferredWidth: 40; Layout.preferredHeight: 40
                    radius: 6
                    color: settingsDialog.pendingThreads < MinerEngine.maxCores ? amber : root.card
                    Text {
                        anchors.centerIn: parent; text: "+"
                        color: settingsDialog.pendingThreads < MinerEngine.maxCores ? bg : dim
                        font { family: root.monoFamily; pixelSize: 18; bold: true }
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: settingsDialog.pendingThreads < MinerEngine.maxCores
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: settingsDialog.pendingThreads = Math.min(MinerEngine.maxCores, settingsDialog.pendingThreads + 1)
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 32
                    radius: 6
                    property int rec: Math.max(1, Math.floor((MinerEngine.maxCores + 1) / 2))
                    color: settingsDialog.pendingThreads === rec ? amber : root.card
                    Text {
                        anchors.centerIn: parent
                        text: "RECOMMENDED (" + parent.rec + ")"
                        color: settingsDialog.pendingThreads === parent.rec ? bg : root.white_
                        font { family: root.monoFamily; pixelSize: 10; bold: true; letterSpacing: 1 }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: settingsDialog.pendingThreads = parent.parent.rec
                    }
                }
                Rectangle {
                    Layout.fillWidth: true; Layout.preferredHeight: 32
                    radius: 6
                    color: settingsDialog.pendingThreads === MinerEngine.maxCores ? amber : root.card
                    Text {
                        anchors.centerIn: parent
                        text: "ALL (" + MinerEngine.maxCores + ")"
                        color: settingsDialog.pendingThreads === MinerEngine.maxCores ? bg : root.white_
                        font { family: root.monoFamily; pixelSize: 10; bold: true; letterSpacing: 1 }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: settingsDialog.pendingThreads = MinerEngine.maxCores
                    }
                }
            }

            // -- v1.1.8: pool mode --
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: dim; opacity: 0.4 }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: poolToggle.checked
                    ? "MINING MODE — POOL. Shares submitted to the pool URL below; payouts arrive once your contribution crosses the pool's threshold."
                    : "MINING MODE — SOLO. Direct daemon submission; you keep 100% of any block you find but finds are rare with one CPU."
                font { family: root.monoFamily; pixelSize: 10 }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                Text {
                    text: "POOL MODE"
                    color: poolToggle.checked ? amber : dim
                    font { family: root.monoFamily; pixelSize: 11; bold: true; letterSpacing: 1 }
                }
                Item { Layout.fillWidth: true }
                Switch {
                    id: poolToggle
                    checked: false
                }
            }
            Text {
                visible: poolToggle.checked
                text: "pool URL"; color: dim
                font.family: root.monoFamily; font.pixelSize: 10
            }
            TextField {
                id: poolUrlField
                visible: poolToggle.checked
                Layout.fillWidth: true; color: root.white_
                font { family: root.monoFamily; pixelSize: 11 }
                background: Rectangle { color: root.card; radius: 6 }
            }

            // -- wallet management --
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: dim; opacity: 0.4 }
            Text {
                Layout.fillWidth: true; wrapMode: Text.WordWrap; color: dim
                text: "Generate a fresh wallet to mine to, or restore one from its 25-word seed."
                font { family: root.monoFamily; pixelSize: 10 }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 10
                SmallButton { text: "NEW WALLET"
                    onClicked: { settingsDialog.close(); MinerEngine.generateWallet() } }
                SmallButton { text: "RESTORE"
                    onClicked: { settingsDialog.close(); restoreDialog.open() } }
            }
        }
        footer: DialogButtonBox {
            background: Rectangle { color: bg }
            Button {
                text: "SAVE"
                onClicked: {
                    MinerEngine.nodeHost = hostField.text.trim();
                    MinerEngine.nodePort = parseInt(portField.text) || 443;
                    MinerEngine.poolEnabled = poolToggle.checked;
                    var u = poolUrlField.text.trim();
                    if (u.length === 0) u = "https://glaciem-pool.frostmine.workers.dev";
                    MinerEngine.poolUrl = u;
                    MinerEngine.threadCount = settingsDialog.pendingThreads;
                    settingsDialog.close();
                }
            }
            Button { text: "CANCEL"; onClicked: settingsDialog.close() }
        }
    }

    // small button component used in the wallet card
    component SmallButton: Rectangle {
        property alias text: lbl.text
        property bool enabled: true
        signal clicked()
        implicitHeight: 32
        Layout.fillWidth: true
        radius: 6
        color: enabled ? Qt.rgba(63/255, 193/255, 224/255, 0.12) : "transparent"
        border.color: enabled ? amber : dim
        border.width: 1
        opacity: enabled ? 1.0 : 0.45
        Text {
            id: lbl
            anchors.centerIn: parent
            color: enabled ? amber : dim
            font { family: root.monoFamily; pixelSize: 10; bold: true; letterSpacing: 1 }
        }
        MouseArea {
            anchors.fill: parent
            enabled: parent.enabled
            onClicked: parent.clicked()
        }
    }
}
