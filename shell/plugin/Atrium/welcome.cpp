#include "welcome.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace atrium {

Welcome::Welcome(QObject* parent)
    : QObject(parent),
      marker_(QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/atrium/welcomed"),
      due_(!QFile::exists(marker_)) {}

void Welcome::done() {
    QDir().mkpath(QFileInfo(marker_).path());
    QFile f(marker_);
    f.open(QIODevice::WriteOnly);
    due_ = false;
    emit dueChanged();
}

} // namespace atrium
