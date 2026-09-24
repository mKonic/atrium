pragma Singleton

import QtQuick
import Quickshell
import Atrium

// Design tokens: caelestia's sizes, rounding and motion, and the Material 3
// palette (palette.hpp: dark or light, with appearance.accent's tones).
Singleton {
    id: root

    readonly property bool light: Atrium.settings["appearance.style"] === "light"
    readonly property var colors: Atrium.palette(light, Atrium.settings["appearance.accent"] ?? "multicolor")

    readonly property QtObject palette: QtObject {
        readonly property color m3Primary: root.colors.m3Primary
        readonly property color m3OnPrimary: root.colors.m3OnPrimary
        readonly property color m3PrimaryContainer: root.colors.m3PrimaryContainer
        readonly property color m3OnPrimaryContainer: root.colors.m3OnPrimaryContainer
        readonly property color m3SecondaryContainer: root.colors.m3SecondaryContainer
        readonly property color m3OnSecondaryContainer: root.colors.m3OnSecondaryContainer
        readonly property color m3Surface: root.colors.m3Surface
        readonly property color m3SurfaceContainer: root.colors.m3SurfaceContainer
        readonly property color m3SurfaceContainerHigh: root.colors.m3SurfaceContainerHigh
        readonly property color m3OnSurface: root.colors.m3OnSurface
        readonly property color m3OnSurfaceVariant: root.colors.m3OnSurfaceVariant
        readonly property color m3Outline: root.colors.m3Outline
        readonly property color m3OutlineVariant: root.colors.m3OutlineVariant
    }

    readonly property QtObject rounding: QtObject {
        readonly property int small: 12
        readonly property int normal: 17
        readonly property int large: 25
        readonly property int full: 1000
    }

    readonly property QtObject spacing: QtObject {
        readonly property int small: 7
        readonly property int smaller: 10
        readonly property int normal: 12
        readonly property int larger: 15
        readonly property int large: 20
    }

    readonly property QtObject padding: QtObject {
        readonly property int small: 5
        readonly property int smaller: 7
        readonly property int normal: 10
        readonly property int larger: 12
        readonly property int large: 15
    }

    readonly property QtObject font: QtObject {
        readonly property string sans: "Rubik"
        readonly property string mono: "CaskaydiaCove NF"
        readonly property string icons: "Material Symbols Rounded"
        readonly property QtObject size: QtObject {
            readonly property int small: 11
            readonly property int smaller: 12
            readonly property int normal: 13
            readonly property int larger: 15
            readonly property int large: 18
        }
    }

    readonly property QtObject anim: QtObject {
        readonly property int small: 200
        readonly property int normal: 400
        readonly property int large: 600
        readonly property list<real> standard: [0.2, 0, 0, 1, 1, 1]
        readonly property list<real> standardDecel: [0, 0, 0, 1, 1, 1]
        readonly property list<real> emphasized: [0.05, 0, 2 / 15, 0.06, 1 / 6, 0.4, 5 / 24, 0.82, 0.25, 1, 1, 1]
        readonly property list<real> emphasizedDecel: [0.05, 0.7, 0.1, 1, 1, 1]
        readonly property list<real> expressiveDefault: [0.38, 1.21, 0.22, 1, 1, 1]
    }

    readonly property QtObject bar: QtObject {
        readonly property int height: 40
        readonly property int inner: 30  // height of the pills inside it
    }

    // Panels turn to frosted glass only with appearance.transparency on
    // (atrium blurs behind them then); otherwise they are solid.
    readonly property bool glass: Atrium.settings["appearance.transparency"] ?? false

    function panel(c: color, glassAlpha: real): color {
        return glass ? alpha(c, glassAlpha) : c;
    }

    // A color with its alpha replaced.
    function alpha(c: color, a: real): color {
        return Qt.rgba(c.r, c.g, c.b, a);
    }
}
