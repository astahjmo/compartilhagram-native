#pragma once
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <functional>
#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>

// Wayland capture: request exactly MONITOR or WINDOW, then consume the granted
// PipeWire node. Never enumerate windows outside the portal.
class PortalCapture : public QObject {
  Q_OBJECT
public:
  explicit PortalCapture(QObject *parent = nullptr);
  ~PortalCapture() override;
  void start(bool window, int fps);
  void stop();
  void setFps(int fps);
  static uint32_t sourceMask(bool window) { return window ? 2u : 1u; }
  static QImage copyFrame(const void *data, size_t available, int width,
                          int height, int stride, spa_video_format format);
signals:
  void frame(QImage image);
  void failed(QString reason);
private slots:
  void response(uint result, QVariantMap data);
  void sessionClosed();

private:
  void request(const QString &method, QVariantList args, QVariantMap options,
               std::function<void(QVariantMap)> done);
  void fail(const QString &reason);
  void openRemote(uint32_t node, QString serial);
  void connectPipeWire(int fd, uint32_t node, const QString &serial);
  static void formatChanged(void *, uint32_t, const spa_pod *);
  static void process(void *);
  QString session_, requestPath_;
  std::function<void(QVariantMap)> response_;
  QTimer timeout_, delivery_;
  QMutex mutex_;
  QImage latest_;
  bool dirty_ = false;
  int generation_ = 0;
  pw_thread_loop *loop_ = nullptr;
  pw_context *context_ = nullptr;
  pw_core *core_ = nullptr;
  pw_stream *stream_ = nullptr;
  spa_hook listener_{};
  spa_video_info_raw format_{};
};
