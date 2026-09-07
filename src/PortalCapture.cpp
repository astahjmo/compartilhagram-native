#include "PortalCapture.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusVariant>
#include <QMutexLocker>
#include <QUuid>
#include <algorithm>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <unistd.h>

static const QString service = "org.freedesktop.portal.Desktop";
static const QString path = "/org/freedesktop/portal/desktop";
static const QString iface = "org.freedesktop.portal.ScreenCast";
static QString token() {
  return "compartilhagram_" + QUuid::createUuid().toString(QUuid::Id128);
}

PortalCapture::PortalCapture(QObject *parent) : QObject(parent) {
  timeout_.setSingleShot(true);
  timeout_.setInterval(120000);
  connect(&timeout_, &QTimer::timeout, this, [this] {
    fail(tr("Tempo esgotado no seletor de compartilhamento"));
  });
  delivery_.setTimerType(Qt::PreciseTimer);
  connect(&delivery_, &QTimer::timeout, this, [this] {
    QImage image;
    {
      QMutexLocker lock(&mutex_);
      if (!dirty_)
        return;
      image = latest_;
      dirty_ = false;
    }
    timeout_.stop();
    emit frame(image);
  });
}
PortalCapture::~PortalCapture() { stop(); }
void PortalCapture::setFps(int fps) {
  delivery_.setInterval(std::max(1, 1000 / std::clamp(fps, 1, 60)));
}
void PortalCapture::request(const QString &method, QVariantList args,
                            QVariantMap options,
                            std::function<void(QVariantMap)> done) {
  auto bus = QDBusConnection::sessionBus();
  const auto handle = token();
  QString sender = bus.baseService().mid(1);
  sender.replace('.', '_');
  requestPath_ =
      "/org/freedesktop/portal/desktop/request/" + sender + '/' + handle;
  options["handle_token"] = handle;
  args.append(options);
  response_ = std::move(done);
  bus.connect(service, requestPath_, "org.freedesktop.portal.Request",
              "Response", this, SLOT(response(uint, QVariantMap)));
  auto msg = QDBusMessage::createMethodCall(service, path, iface, method);
  msg.setArguments(args);
  const int generation = generation_;
  const auto expectedPath = requestPath_;
  auto watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, generation, expectedPath](QDBusPendingCallWatcher *watcher) {
            QDBusPendingReply<QDBusObjectPath> reply = *watcher;
            watcher->deleteLater();
            if (generation != generation_ || requestPath_ != expectedPath)
              return;
            if (reply.isError()) {
              fail(tr("Portal: %1").arg(reply.error().message()));
              return;
            }
            if (reply.value().path() != requestPath_ && response_) {
              auto bus = QDBusConnection::sessionBus();
              bus.disconnect(service, requestPath_,
                             "org.freedesktop.portal.Request", "Response", this,
                             SLOT(response(uint, QVariantMap)));
              requestPath_ = reply.value().path();
              bus.connect(service, requestPath_,
                          "org.freedesktop.portal.Request", "Response", this,
                          SLOT(response(uint, QVariantMap)));
            }
          });
}
void PortalCapture::response(uint result, QVariantMap data) {
  QDBusConnection::sessionBus().disconnect(
      service, requestPath_, "org.freedesktop.portal.Request", "Response", this,
      SLOT(response(uint, QVariantMap)));
  requestPath_.clear();
  auto done = std::move(response_);
  response_ = {};
  if (result != 0) {
    fail(result == 1 ? tr("Seleção de compartilhamento cancelada")
                     : tr("O portal recusou o compartilhamento"));
    return;
  }
  if (done)
    done(data);
}
void PortalCapture::start(bool window, int fps) {
  stop();
  setFps(fps);
  timeout_.start();
  const QString sessionToken = token();
  request(
      "CreateSession", {}, {{"session_handle_token", sessionToken}},
      [this, window](QVariantMap data) {
        session_ = data.value("session_handle").toString();
        if (session_.isEmpty()) {
          fail(tr("O portal não retornou uma sessão"));
          return;
        }
        QDBusConnection::sessionBus().connect(
            service, session_, "org.freedesktop.portal.Session", "Closed", this,
            SLOT(sessionClosed()));
        // Query capabilities before requesting a window, so unsupported desktop
        // backends give a clear error rather than silently sharing a monitor.
        auto msg = QDBusMessage::createMethodCall(
            service, path, "org.freedesktop.DBus.Properties", "Get");
        msg.setArguments({iface, "AvailableSourceTypes"});
        auto watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(msg), this);
        const int generation = generation_;
        connect(
            watcher, &QDBusPendingCallWatcher::finished, this,
            [this, window, generation](QDBusPendingCallWatcher *w) {
              QDBusPendingReply<QDBusVariant> reply = *w;
              w->deleteLater();
              if (generation != generation_)
                return;
              if (reply.isError() ||
                  !(reply.value().variant().toUInt() & sourceMask(window))) {
                fail(tr("Este portal não oferece a fonte selecionada "
                        "(tela/janela)."));
                return;
              }
              request(
                  "SelectSources",
                  {QVariant::fromValue(QDBusObjectPath(session_))},
                  {{"types", sourceMask(window)}, {"multiple", false}},
                  [this](QVariantMap) {
                    request(
                        "Start",
                        {QVariant::fromValue(QDBusObjectPath(session_)),
                         QString()},
                        {}, [this](QVariantMap data) {
                          const auto streams = qvariant_cast<QDBusArgument>(
                              data.value("streams"));
                          uint32_t node = 0;
                          QVariantMap properties;
                          streams.beginArray();
                          if (!streams.atEnd()) {
                            streams.beginStructure();
                            streams >> node >> properties;
                            streams.endStructure();
                          }
                          streams.endArray();
                          if (!node) {
                            fail(tr("O portal não retornou vídeo"));
                            return;
                          }
                          openRemote(
                              node,
                              properties.value("pipewire-serial").toString());
                        });
                  });
            });
      });
}
void PortalCapture::openRemote(uint32_t node, QString serial) {
  auto msg = QDBusMessage::createMethodCall(service, path, iface,
                                            "OpenPipeWireRemote");
  msg.setArguments(
      {QVariant::fromValue(QDBusObjectPath(session_)), QVariantMap{}});
  auto watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(msg), this);
  const int generation = generation_;
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, node, serial, generation](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QDBusUnixFileDescriptor> reply = *w;
            w->deleteLater();
            if (generation != generation_)
              return;
            if (reply.isError()) {
              fail(tr("Não foi possível abrir o vídeo PipeWire"));
              return;
            }
            const int fd = dup(reply.value().fileDescriptor());
            if (fd < 0) {
              fail(tr("Descritor PipeWire inválido"));
              return;
            }
            connectPipeWire(fd, node, serial);
          });
}
void PortalCapture::connectPipeWire(int fd, uint32_t node,
                                    const QString &serial) {
  pw_init(nullptr, nullptr);
  loop_ = pw_thread_loop_new("compartilhagram-video", nullptr);
  if (!loop_) {
    close(fd);
    fail(tr("Falha ao criar loop PipeWire"));
    return;
  }
  context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
  if (!context_) {
    close(fd);
    fail(tr("Falha ao criar contexto PipeWire"));
    return;
  }
  core_ = pw_context_connect_fd(context_, fd, nullptr, 0);
  if (!core_) {
    fail(tr("Falha ao conectar ao PipeWire"));
    return;
  }
  auto props =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                        "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
  if (!serial.isEmpty())
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, serial.toUtf8().constData());
  stream_ = pw_stream_new(core_, "Compartilhagram", props);
  if (!stream_) {
    fail(tr("Falha ao criar stream PipeWire"));
    return;
  }
  static const pw_stream_events events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.param_changed = formatChanged;
    e.process = process;
    e.state_changed = [](void *p, pw_stream_state, pw_stream_state state,
                         const char *) {
      if (state == PW_STREAM_STATE_ERROR ||
          state == PW_STREAM_STATE_UNCONNECTED) {
        auto self = static_cast<PortalCapture *>(p);
        const int generation = self->generation_;
        QMetaObject::invokeMethod(
            self,
            [self, generation] {
              if (generation == self->generation_)
                self->fail(tr("O vídeo PipeWire foi interrompido"));
            },
            Qt::QueuedConnection);
      }
    };
    return e;
  }();
  pw_stream_add_listener(stream_, &listener_, &events, this);
  uint8_t buffer[1024];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const spa_rectangle initial{1280, 720}, minimum{1, 1}, maximum{8192, 8192};
  const spa_fraction rate{30, 1}, minRate{0, 1}, maxRate{60, 1};
  const spa_pod *format =
      static_cast<const spa_pod *>(spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
          SPA_FORMAT_VIDEO_format,
          SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRx,
                                 SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                                 SPA_VIDEO_FORMAT_RGBA),
          SPA_FORMAT_VIDEO_size,
          SPA_POD_CHOICE_RANGE_Rectangle(&initial, &minimum, &maximum),
          SPA_FORMAT_VIDEO_framerate,
          SPA_POD_CHOICE_RANGE_Fraction(&rate, &minRate, &maxRate)));
  if (pw_stream_connect(
          stream_, PW_DIRECTION_INPUT, node,
          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                       PW_STREAM_FLAG_MAP_BUFFERS),
          &format, 1) < 0 ||
      pw_thread_loop_start(loop_) < 0) {
    fail(tr("Não foi possível iniciar o vídeo PipeWire"));
    return;
  }
  delivery_.start();
}
void PortalCapture::formatChanged(void *p, uint32_t id, const spa_pod *param) {
  if (id != SPA_PARAM_Format || !param)
    return;
  auto self = static_cast<PortalCapture *>(p);
  if (spa_format_video_raw_parse(param, &self->format_) < 0)
    return;
  // Request CPU-readable buffers explicitly; an unmapped DMA-BUF cannot be
  // safely read as pixel memory.
  uint8_t storage[512];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  const spa_pod *params[] = {
      static_cast<const spa_pod *>(spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
          SPA_PARAM_BUFFERS_dataType,
          SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) |
                                   (1 << SPA_DATA_MemFd)))),
      static_cast<const spa_pod *>(spa_pod_builder_add_object(
          &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
          SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
          SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region))))};
  pw_stream_update_params(self->stream_, params, 2);
}
QImage PortalCapture::copyFrame(const void *data, size_t available, int width,
                                int height, int stride,
                                spa_video_format format) {
  if (!data || width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
      stride < width * 4 ||
      size_t(height - 1) * stride + size_t(width) * 4 > available)
    return {};
  QImage::Format qtFormat;
  if (format == SPA_VIDEO_FORMAT_BGRx)
    qtFormat = QImage::Format_RGB32;
  else if (format == SPA_VIDEO_FORMAT_BGRA)
    qtFormat = QImage::Format_ARGB32;
  else if (format == SPA_VIDEO_FORMAT_RGBx)
    qtFormat = QImage::Format_RGBX8888;
  else if (format == SPA_VIDEO_FORMAT_RGBA)
    qtFormat = QImage::Format_RGBA8888;
  else
    return {};
  return QImage(static_cast<const uchar *>(data), width, height, stride,
                qtFormat)
      .convertToFormat(QImage::Format_ARGB32)
      .copy();
}
void PortalCapture::process(void *p) {
  auto self = static_cast<PortalCapture *>(p);
  auto b = pw_stream_dequeue_buffer(self->stream_);
  if (!b)
    return;
  auto buffer = b->buffer;
  if (buffer->n_datas && buffer->datas[0].data && buffer->datas[0].chunk) {
    const auto &d = buffer->datas[0];
    if (d.chunk->offset <= d.maxsize &&
        !(d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
      auto image =
          copyFrame(static_cast<const uint8_t *>(d.data) + d.chunk->offset,
                    std::min(d.chunk->size, d.maxsize - d.chunk->offset),
                    self->format_.size.width, self->format_.size.height,
                    d.chunk->stride, self->format_.format);
      auto crop = static_cast<spa_meta_region *>(spa_buffer_find_meta_data(
          buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
      if (crop && spa_meta_region_is_valid(crop)) {
        QRect region(crop->region.position.x, crop->region.position.y,
                     crop->region.size.width, crop->region.size.height);
        image = image.rect().contains(region) ? image.copy(region) : QImage();
      }
      if (!image.isNull()) {
        QMutexLocker lock(&self->mutex_);
        self->latest_ = std::move(image);
        self->dirty_ = true;
      }
    }
  }
  pw_stream_queue_buffer(self->stream_, b);
}
void PortalCapture::sessionClosed() {
  fail(tr("Compartilhamento encerrado pelo sistema"));
}
void PortalCapture::fail(const QString &reason) {
  stop();
  emit failed(reason);
}
void PortalCapture::stop() {
  timeout_.stop();
  delivery_.stop();
  if (loop_)
    pw_thread_loop_stop(loop_);
  ++generation_;
  if (stream_) {
    spa_hook_remove(&listener_);
    pw_stream_destroy(stream_);
  }
  if (core_)
    pw_core_disconnect(core_);
  if (context_)
    pw_context_destroy(context_);
  if (loop_)
    pw_thread_loop_destroy(loop_);
  loop_ = nullptr;
  context_ = nullptr;
  core_ = nullptr;
  stream_ = nullptr;
  auto bus = QDBusConnection::sessionBus();
  if (!requestPath_.isEmpty()) {
    bus.disconnect(service, requestPath_, "org.freedesktop.portal.Request",
                   "Response", this, SLOT(response(uint, QVariantMap)));
    bus.asyncCall(QDBusMessage::createMethodCall(
        service, requestPath_, "org.freedesktop.portal.Request", "Close"));
  }
  if (!session_.isEmpty()) {
    bus.disconnect(service, session_, "org.freedesktop.portal.Session",
                   "Closed", this, SLOT(sessionClosed()));
    bus.asyncCall(QDBusMessage::createMethodCall(
        service, session_, "org.freedesktop.portal.Session", "Close"));
  }
  session_.clear();
  requestPath_.clear();
  response_ = {};
  QMutexLocker lock(&mutex_);
  latest_ = {};
  dirty_ = false;
}
