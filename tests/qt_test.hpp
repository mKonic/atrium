#pragma once
// For the Qt unit tests (unit-qt): Qt strings printed readably when an
// expectation fails.

#include <QString>
#include <QStringList>

#include <ostream>

QT_BEGIN_NAMESPACE
inline void PrintTo(const QString& s, std::ostream* os) {
    *os << '"' << s.toStdString() << '"';
}
QT_END_NAMESPACE
