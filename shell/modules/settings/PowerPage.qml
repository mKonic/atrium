import QtQuick
import Atrium
import shell.components
import shell.services

// The battery and the lid, on computers that have them; the power mode and
// the rest follow as ordinary settings. atrium never locks or sleeps by
// itself when idle.
Column {
    spacing: 20

    Group {
        visible: Battery.present
        title: qsTr("Battery")

        ControlRow {
            title: `${Battery.percentage}%`
            note: Battery.remaining

            MaterialIcon {
                text: Battery.glyph
                font.pointSize: Theme.font.size.large + 6
            }
        }

        ControlRow {
            visible: Battery.chargeLimitSupported
            title: qsTr("Limit charging")
            note: qsTr("Stops at %1%, which keeps the battery healthy when it's mostly plugged in.").arg(Battery.chargeLimit)

            Switch {
                name: qsTr("Limit charging")
                checked: Battery.chargeLimitEnabled
                onToggled: Battery.setChargeLimitEnabled(!checked)
            }
        }
    }

    Group {
        visible: Battery.lidAction !== ""
        title: qsTr("Lid")

        ControlRow {
            title: qsTr("When the lid closes")
            note: qsTr("Set in logind.conf (HandleLidSwitch).")

            StyledText {
                text: SettingsPages.choiceOptions([Battery.lidAction])[0]?.label ?? ""
                color: Theme.palette.secondaryLabel
            }
        }
    }
}
