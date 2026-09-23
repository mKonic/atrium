import QtQuick
import qs.services

ColorAnimation {
    duration: Theme.anim.normal
    easing.type: Easing.BezierSpline
    easing.bezierCurve: Theme.anim.standard
}
