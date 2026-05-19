import QtQuick
import QtQuick.Window

Window {
    id: root
    width: 380
    height: 720
    visible: true
    title: "Rime Miner"
    color: "#0d0d10"

    // header -- RIME wordmark
    Column {
        x: 28
        y: 26
        spacing: 1
        Text {
            text: "RIME"
            color: "#ff9926"
            font.pixelSize: 30
            font.bold: true
            font.family: "monospace"
        }
        Text {
            text: "PROOF-OF-WORK MINER"
            color: "#7a7a7a"
            font.pixelSize: 10
            font.letterSpacing: 1.5
            font.family: "monospace"
        }
    }

    Text {
        anchors.centerIn: parent
        text: "Linux build\nscaffold OK"
        horizontalAlignment: Text.AlignHCenter
        color: "#9a9a9a"
        font.pixelSize: 14
        font.family: "monospace"
    }
}
