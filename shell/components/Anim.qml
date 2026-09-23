import QtQuick
import qs.services

NumberAnimation {
    duration: Theme.anim.normal
    easing.type: Easing.BezierSpline
    easing.bezierCurve: Theme.anim.standard
}
