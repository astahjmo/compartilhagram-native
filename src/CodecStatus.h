#pragma once
#include <QJsonArray>
#include <QString>
struct CodecStatus {
  static bool available();
  static QJsonArray streams();
  static QString text();
  static QString describe(bool adapterAvailable,
                          const QString &unavailableReason,
                          const QJsonArray &streams);
};
