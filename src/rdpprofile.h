#pragma once

#include <QByteArray>
#include <QString>

namespace RdpProfile {
inline constexpr qint64 maximumBytes = 1024 * 1024;
// Pure validation of the application's restrictive supported serialization/settings subset.
// Success never transforms the supplied bytes; errors never include supplied content.
bool validate(const QByteArray& bytes, QString* error = nullptr);
bool readAndValidate(const QString& path, QByteArray* bytes, QString* error = nullptr);
}
