#include "CodecStatus.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using ReadStatus = size_t (*)(char *, size_t);
ReadStatus reader() {
#ifdef Q_OS_WIN
  static auto function = reinterpret_cast<ReadStatus>(GetProcAddress(
      GetModuleHandleW(L"libwebrtc.dll"), "CompartilhagramCodecStatus"));
#else
  static auto function = reinterpret_cast<ReadStatus>(
      dlsym(RTLD_DEFAULT, "CompartilhagramCodecStatus"));
#endif
  return function;
}
} // namespace
bool CodecStatus::available() { return reader() != nullptr; }
QJsonArray CodecStatus::streams() {
  auto function = reader();
  if (!function)
    return {};
  size_t size = function(nullptr, 0);
  for (int attempt = 0; attempt < 3 && size <= 1024 * 1024; ++attempt) {
    QByteArray buffer(qsizetype(size), '\0');
    size_t required = function(buffer.data(), size);
    if (required <= size)
      return QJsonDocument::fromJson(buffer.constData()).array();
    size = required;
  }
  return {};
}
QString CodecStatus::text() {
  QString reason;
  if (qEnvironmentVariable("COMPARTILHAGRAM_DISABLE_GPU") == "1")
    reason = "Disabled by COMPARTILHAGRAM_DISABLE_GPU=1";
#ifndef Q_OS_WIN
  else if (auto chosen = qEnvironmentVariable("COMPARTILHAGRAM_VAAPI_DEVICE");
           !chosen.isEmpty()) {
    if (!QFileInfo(chosen).isReadable() || !QFileInfo(chosen).isWritable())
      reason = "GPU device is missing or inaccessible: " + chosen;
  } else if (QDir("/dev/dri")
                 .entryList({"renderD*"}, QDir::Files | QDir::System)
                 .isEmpty())
    reason = "No GPU render device (/dev/dri/renderD*) is available";
#endif
  return describe(available(), reason, streams());
}
QString CodecStatus::describe(bool adapterAvailable,
                              const QString &unavailableReason,
                              const QJsonArray &streams) {
  if (!adapterAvailable)
    return "GPU acceleration not enabled — reason: this WebRTC SDK was built "
           "without system codec support";
  if (streams.isEmpty()) {
    if (!unavailableReason.isEmpty())
      return "GPU acceleration not enabled — reason: " + unavailableReason;
    return "GPU acceleration: idle — system codecs will be tried when video "
           "starts";
  }
  QStringList directions;
  for (const QString direction : {QString("encode"), QString("decode")}) {
    int hardware = 0, software = 0, pending = 0;
    QSet<QString> reasons, implementations;
    for (const auto &value : streams) {
      auto stream = value.toObject();
      if (stream.value("direction").toString() != direction)
        continue;
      auto mode = stream.value("mode").toString();
      if (mode == "hardware")
        ++hardware;
      else if (mode == "software")
        ++software;
      else
        ++pending;
      auto reason = stream.value("reason").toString();
      if (mode != "hardware" && !reason.isEmpty())
        reasons.insert(reason);
      auto implementation = stream.value("implementation").toString();
      if (!implementation.isEmpty())
        implementations.insert(implementation);
    }
    QString label = direction == "encode" ? "Encoding: " : "Decoding: ";
    if (!hardware && !software && !pending)
      label += "idle";
    else {
      if (hardware)
        label += QString("GPU enabled (%1)").arg(hardware);
      if (software)
        label +=
            (hardware ? " + " : QString()) +
            QString("GPU acceleration not enabled (%1 software)").arg(software);
      if (pending)
        label += ((hardware || software) ? " + " : QString()) + "initializing";
      auto names = implementations.values();
      names.sort();
      if (!names.isEmpty())
        label += " [" + names.join(", ") + ']';
      auto messages = reasons.values();
      messages.sort();
      if (!messages.isEmpty())
        label += " — reason: " + messages.join("; ");
    }
    directions.append(label);
  }
  return directions.join("\n");
}
