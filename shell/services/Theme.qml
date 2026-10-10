pragma Singleton

import QtQuick
import Atrium.Shell
import Atrium

// Design tokens: caelestia's sizes, rounding and motion, and macOS's colours
// (palette.hpp): semantic roles for dark or light and appearance.accent.
// Name what a colour is for (palette.label, palette.separator,
// material.regular), never a shade or an opacity.
Singleton {
    id: root

    readonly property bool light: Atrium.settings["appearance.style"] === "light"
    readonly property string accent: Atrium.settings["appearance.accent"] ?? "multicolor"
    // Increase Contrast (macOS's, Settings > Accessibility): firmer colours,
    // and solid panels (it reduces transparency too).
    readonly property bool highContrast: Atrium.settings["appearance.high_contrast"] ?? false
    readonly property var colors: Atrium.palette(light, accent, highContrast)
    readonly property var darkColors: Atrium.palette(false, accent, highContrast)
    // Settings > Accessibility's text size: every font size below scales with it.
    readonly property real textScale: Atrium.settings["accessibility.text_size"] ?? 1

    // The palette for the current appearance.
    readonly property QtObject palette: QtObject {
        readonly property color label: root.colors.label
        readonly property color secondaryLabel: root.colors.secondaryLabel
        readonly property color tertiaryLabel: root.colors.tertiaryLabel
        readonly property color quaternaryLabel: root.colors.quaternaryLabel
        readonly property color fill: root.colors.fill
        readonly property color secondaryFill: root.colors.secondaryFill
        readonly property color tertiaryFill: root.colors.tertiaryFill
        readonly property color quaternaryFill: root.colors.quaternaryFill
        readonly property color separator: root.colors.separator
        readonly property color windowBackground: root.colors.windowBackground
        readonly property color controlBackground: root.colors.controlBackground
        readonly property color control: root.colors.control
        readonly property color thumb: root.colors.thumb
        readonly property color groupedBackground: root.colors.groupedBackground
        readonly property color accent: root.colors.accent
        readonly property color labelOnAccent: root.colors.labelOnAccent
        readonly property color accentFill: root.colors.accentFill
        readonly property color focusRing: root.colors.focusRing
        readonly property color red: root.colors.red
        readonly property color orange: root.colors.orange
        readonly property color yellow: root.colors.yellow
        readonly property color green: root.colors.green
        readonly property color mint: root.colors.mint
        readonly property color teal: root.colors.teal
        readonly property color cyan: root.colors.cyan
        readonly property color blue: root.colors.blue
        readonly property color indigo: root.colors.indigo
        readonly property color purple: root.colors.purple
        readonly property color pink: root.colors.pink
        readonly property color brown: root.colors.brown
        readonly property color gray: root.colors.gray
        readonly property color shadow: root.colors.shadow
        readonly property color scrim: root.colors.scrim
    }

    // Dark whatever the appearance, as macOS draws HUDs: for what sits over
    // the wallpaper or a screenshot (desktop labels, the capture overlay).
    readonly property QtObject dark: QtObject {
        readonly property color label: root.darkColors.label
        readonly property color secondaryLabel: root.darkColors.secondaryLabel
        readonly property color tertiaryLabel: root.darkColors.tertiaryLabel
        readonly property color quaternaryLabel: root.darkColors.quaternaryLabel
        readonly property color fill: root.darkColors.fill
        readonly property color secondaryFill: root.darkColors.secondaryFill
        readonly property color tertiaryFill: root.darkColors.tertiaryFill
        readonly property color quaternaryFill: root.darkColors.quaternaryFill
        readonly property color separator: root.darkColors.separator
        readonly property color windowBackground: root.darkColors.windowBackground
        readonly property color controlBackground: root.darkColors.controlBackground
        readonly property color control: root.darkColors.control
        readonly property color thumb: root.darkColors.thumb
        readonly property color groupedBackground: root.darkColors.groupedBackground
        readonly property color accent: root.darkColors.accent
        readonly property color labelOnAccent: root.darkColors.labelOnAccent
        readonly property color accentFill: root.darkColors.accentFill
        readonly property color focusRing: root.darkColors.focusRing
        readonly property color red: root.darkColors.red
        readonly property color orange: root.darkColors.orange
        readonly property color yellow: root.darkColors.yellow
        readonly property color green: root.darkColors.green
        readonly property color mint: root.darkColors.mint
        readonly property color teal: root.darkColors.teal
        readonly property color cyan: root.darkColors.cyan
        readonly property color blue: root.darkColors.blue
        readonly property color indigo: root.darkColors.indigo
        readonly property color purple: root.darkColors.purple
        readonly property color pink: root.darkColors.pink
        readonly property color brown: root.darkColors.brown
        readonly property color gray: root.darkColors.gray
        readonly property color shadow: root.darkColors.shadow
        readonly property color scrim: root.darkColors.scrim
    }

    // What panels are made of, thinnest to thickest: with transparency off a
    // solid window background, otherwise frosted glass (atrium blurs behind
    // it) letting less through the thicker it is. Liquid Glass is the
    // compositor's: its lens, tint and lit rim are drawn behind the panel,
    // which only leaves a breath of colour to mark where the glass is.
    readonly property QtObject material: QtObject {
        // Bar, Dock and bar pills.
        readonly property color thin: root.glassy(0.7)
        // Popovers: notifications, Control Center, OSD, pickers.
        readonly property color regular: root.glassy(0.8)
        // Dialogs and sheets.
        readonly property color thick: root.glassy(0.92)
        // A bar pill: clear glass, or a step off the bar when solid.
        readonly property color pill: root.glass ? root.glassy(0.7) : Qt.tint(root.palette.windowBackground, root.palette.quaternaryFill)
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
            readonly property int small: Math.round(11 * root.textScale)
            readonly property int smaller: Math.round(12 * root.textScale)
            readonly property int normal: Math.round(13 * root.textScale)
            readonly property int larger: Math.round(15 * root.textScale)
            readonly property int large: Math.round(18 * root.textScale)
        }
    }

    // Motion as the compositor's: appearance.animations off stops it all
    // (reduced motion), appearance.animation_speed scales it. Durations
    // written out elsewhere multiply by `factor`; endless ones run only `on`.
    readonly property QtObject anim: QtObject {
        readonly property bool on: Atrium.settings["appearance.animations"] ?? true
        readonly property real factor: on ? 1 / (Atrium.settings["appearance.animation_speed"] ?? 1) : 0
        readonly property int small: 200 * factor
        readonly property int normal: 400 * factor
        readonly property int large: 600 * factor
        readonly property list<real> standard: [0.2, 0, 0, 1, 1, 1]
        readonly property list<real> standardDecel: [0, 0, 0, 1, 1, 1]
        readonly property list<real> emphasized: [0.05, 0, 2 / 15, 0.06, 1 / 6, 0.4, 5 / 24, 0.82, 0.25, 1, 1, 1]
        readonly property list<real> emphasizedDecel: [0.05, 0.7, 0.1, 1, 1, 1]
        readonly property list<real> emphasizedAccel: [0.3, 0, 0.8, 0.15, 1, 1]
        readonly property list<real> expressiveDefault: [0.38, 1.21, 0.22, 1, 1, 1]
    }

    readonly property QtObject bar: QtObject {
        readonly property int height: 40
        readonly property int inner: 30  // height of the pills inside it
    }

    // Liquid Glass (appearance.liquid_glass): panels clearer still, lensed
    // and lit by the compositor, whatever the windows do.
    readonly property bool liquid: !safeMode && !highContrast && (Atrium.settings["appearance.liquid_glass"] ?? false)
    // The shell kept crashing: atrium restarts it without effects.
    readonly property bool safeMode: Shell.env("ATRIUM_SAFE_MODE") === "1"
    readonly property bool glass: liquid || (!highContrast && (Atrium.settings["appearance.transparency"] ?? false))
    // atrium draws Liquid Glass behind the panels (only with blur on): its
    // tint, rim light and shadow. Panels then leave all three to it.
    readonly property bool lens: liquid && (Atrium.settings["appearance.blur"] ?? true)

    function glassy(a: real): color {
        return lens ? alpha(palette.windowBackground, 0.06) : glass ? alpha(palette.windowBackground, a) : palette.windowBackground;
    }

    // A color with its alpha replaced.
    function alpha(c: color, a: real): color {
        return Qt.rgba(c.r, c.g, c.b, a);
    }
}
