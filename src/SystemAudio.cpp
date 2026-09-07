#include "SystemAudio.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cmath>

struct SystemAudio::Input {
  SystemAudio *owner;
  pa_stream *stream = nullptr;
  uint32_t sink;
  QString key;
  QByteArray pending;
  ~Input() {
    if (!stream)
      return;
    pa_stream_set_read_callback(stream, nullptr, nullptr);
    pa_stream_set_state_callback(stream, nullptr, nullptr);
    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
  }
};
SystemAudio::~SystemAudio() { stop(); }
SystemAudio::SystemAudio(QObject *parent) : QObject(parent) {
  mixTimer_.setTimerType(Qt::PreciseTimer);
  mixTimer_.setInterval(10);
  connect(&mixTimer_, &QTimer::timeout, this, &SystemAudio::mix);
}
void SystemAudio::discover() {
  stop();
  connectServer();
}
void SystemAudio::start(
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source,
    AudioSelection selection) {
  stop();
  source_ = source;
  selection_ = std::move(selection);
  if (selection_.mode == AudioSelection::None)
    return;
  connectServer();
  mixTimer_.start();
}
void SystemAudio::connectServer() {
  loop_ = pa_threaded_mainloop_new();
  if (!loop_) {
    error();
    return;
  }
  context_ =
      pa_context_new(pa_threaded_mainloop_get_api(loop_), "Compartilhagram");
  if (!context_) {
    pa_threaded_mainloop_free(loop_);
    loop_ = nullptr;
    error();
    return;
  }
  pa_context_set_state_callback(context_, contextChanged, this);
  if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0 ||
      pa_threaded_mainloop_start(loop_) < 0)
    error();
}
void SystemAudio::stop() {
  mixTimer_.stop();
  if (loop_) {
    pa_threaded_mainloop_stop(loop_);
    streams_.clear();
    pa_context_set_state_callback(context_, nullptr, nullptr);
    pa_context_set_subscribe_callback(context_, nullptr, nullptr);
    pa_context_disconnect(context_);
    pa_context_unref(context_);
    pa_threaded_mainloop_free(loop_);
  }
  ++generation_; // The PA thread has stopped before changing the generation.
  context_ = nullptr;
  loop_ = nullptr;
  source_ = nullptr;
  selection_ = {};
  monitors_.clear();
  applications_.clear();
  refreshing_ = refreshAgain_ = false;
}
void SystemAudio::error() {
  const int generation = generation_;
  QMetaObject::invokeMethod(
      this,
      [this, generation] {
        if (generation == generation_)
          emit failed(tr("Falha na captura de áudio por aplicativo. Verifique "
                         "PulseAudio/PipeWire. O áudio completo não será usado "
                         "como alternativa."));
      },
      Qt::QueuedConnection);
}
void SystemAudio::contextChanged(pa_context *c, void *p) {
  auto self = static_cast<SystemAudio *>(p);
  if (pa_context_get_state(c) == PA_CONTEXT_READY) {
    pa_context_set_subscribe_callback(
        c,
        [](pa_context *, pa_subscription_event_type_t, uint32_t, void *p) {
          static_cast<SystemAudio *>(p)->refresh();
        },
        p);
    auto op = pa_context_subscribe(
        c,
        static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK_INPUT |
                                            PA_SUBSCRIPTION_MASK_SINK),
        nullptr, nullptr);
    if (op)
      pa_operation_unref(op);
    self->refresh();
  } else if (pa_context_get_state(c) == PA_CONTEXT_FAILED)
    self->error();
}
void SystemAudio::refresh() {
  if (refreshing_) {
    refreshAgain_ = true;
    return;
  }
  refreshing_ = true;
  monitors_.clear();
  applications_.clear();
  if (auto op = pa_context_get_sink_info_list(context_, sinks, this))
    pa_operation_unref(op);
  else {
    refreshing_ = false;
    error();
  }
}
void SystemAudio::sinks(pa_context *, const pa_sink_info *info, int end,
                        void *p) {
  auto self = static_cast<SystemAudio *>(p);
  if (end < 0) {
    self->refreshing_ = false;
    self->error();
    return;
  }
  if (info)
    self->monitors_[info->index] = QString::fromUtf8(info->monitor_source_name);
  if (end) {
    if (auto op =
            pa_context_get_sink_input_info_list(self->context_, inputs, p))
      pa_operation_unref(op);
    else {
      self->refreshing_ = false;
      self->error();
    }
  }
}
void SystemAudio::inputs(pa_context *, const pa_sink_input_info *info, int end,
                         void *p) {
  auto self = static_cast<SystemAudio *>(p);
  if (end < 0) {
    self->refreshing_ = false;
    self->error();
    return;
  }
  if (info) {
    auto property = [info](const char *key) {
      const char *v = pa_proplist_gets(info->proplist, key);
      return QString::fromUtf8(v ? v : "");
    };
    QString name = property(PA_PROP_APPLICATION_NAME);
    QString key = property(PA_PROP_APPLICATION_ID);
    if (key.isEmpty())
      key = property(PA_PROP_APPLICATION_PROCESS_BINARY);
    if (key.isEmpty())
      key = name;
    if (key.isEmpty())
      key = QString("stream:%1").arg(info->index);
    if (name.isEmpty())
      name = QString::fromUtf8(info->name ? info->name : "Aplicativo");
    bool own =
        property(PA_PROP_APPLICATION_PROCESS_ID).toLongLong() ==
            QCoreApplication::applicationPid() ||
        property(PA_PROP_APPLICATION_PROCESS_BINARY) == "compartilhagram";
    self->applications_[info->index] = {info->index, info->sink, key, name,
                                        own};
  }
  if (end) {
    self->reconcile();
    self->refreshing_ = false;
    if (std::exchange(self->refreshAgain_, false))
      self->refresh();
  }
}
void SystemAudio::reconcile() {
  for (auto it = streams_.begin(); it != streams_.end();) {
    const auto app = applications_.find(it->first);
    if (app == applications_.end() ||
        !selection_.accepts(app->second.key, app->second.own) ||
        app->second.sink != it->second->sink ||
        app->second.key != it->second->key)
      it = streams_.erase(it);
    else {
      if (pa_stream_get_state(it->second->stream) == PA_STREAM_FAILED)
        error();
      ++it;
    }
  }
  QJsonArray listing;
  QSet<QString> seen;
  for (const auto &[index, app] : applications_) {
    if (!app.own && !seen.contains(app.key)) {
      listing.append(QJsonObject{{"key", app.key}, {"name", app.name}});
      seen.insert(app.key);
    }
    if (!source_ || !selection_.accepts(app.key, app.own) ||
        streams_.contains(index))
      continue;
    auto monitor = monitors_.find(app.sink);
    if (monitor == monitors_.end())
      continue;
    auto input = std::make_unique<Input>();
    input->owner = this;
    input->sink = app.sink;
    input->key = app.key;
    const pa_sample_spec spec{PA_SAMPLE_S16LE, 48000, 2};
    input->stream = pa_stream_new(
        context_, "Compartilhagram: áudio selecionado", &spec, nullptr);
    if (!input->stream) {
      error();
      continue;
    }
    // Binding to the sink-input is mandatory: never fall back to its full
    // monitor.
    if (pa_stream_set_monitor_stream(input->stream, index) < 0) {
      error();
      continue;
    }
    pa_stream_set_read_callback(input->stream, read, input.get());
    pa_stream_set_state_callback(
        input->stream,
        [](pa_stream *s, void *p) {
          if (pa_stream_get_state(s) == PA_STREAM_FAILED)
            static_cast<Input *>(p)->owner->refresh();
        },
        input.get());
    pa_buffer_attr attr{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, 1920};
    if (pa_stream_connect_record(
            input->stream, monitor->second.toUtf8().constData(), &attr,
            static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY |
                                           PA_STREAM_DONT_MOVE)) < 0) {
      error();
      continue;
    }
    streams_[index] = std::move(input);
  }
  const int generation = generation_;
  QMetaObject::invokeMethod(
      this,
      [this, listing, generation] {
        if (generation == generation_)
          emit applicationsChanged(listing);
      },
      Qt::QueuedConnection);
}
void SystemAudio::read(pa_stream *s, size_t, void *p) {
  auto input = static_cast<Input *>(p);
  const void *data = nullptr;
  size_t size = 0;
  if (pa_stream_peek(s, &data, &size) < 0) {
    input->owner->error();
    return;
  }
  if (data && size <= 38400) {
    input->pending.append(static_cast<const char *>(data), int(size));
    if (input->pending.size() > 19200)
      input->pending.remove(0, input->pending.size() - 19200);
  }
  if (size)
    pa_stream_drop(s);
}
QByteArray SystemAudio::mixPcm(const QList<QByteArray> &inputs) {
  QByteArray out(1920, '\0'); // 10 ms, 48 kHz, stereo, signed 16-bit LE.
  for (int sample = 0; sample < 960; ++sample) {
    int32_t sum = 0;
    for (const auto &pcm : inputs)
      if (pcm.size() >= (sample + 1) * 2)
        sum += qFromLittleEndian<int16_t>(pcm.constData() + sample * 2);
    qToLittleEndian<int16_t>(std::clamp(sum, -32768, 32767),
                             out.data() + sample * 2);
  }
  return out;
}
void SystemAudio::mix() {
  if (!source_ || !loop_)
    return;
  QList<QByteArray> buffers;
  pa_threaded_mainloop_lock(loop_);
  for (auto &[index, input] : streams_) {
    buffers.append(input->pending.left(1920));
    input->pending.remove(0, std::min(qsizetype(1920), input->pending.size()));
  }
  pa_threaded_mainloop_unlock(loop_);
  auto pcm = mixPcm(buffers);
  source_->CaptureFrame(pcm.constData(), 16, 48000, 2, 480);
  int peak = 0;
  for (int i = 0; i < 960; ++i)
    peak = std::max(
        peak,
        std::abs(int(qFromLittleEndian<int16_t>(pcm.constData() + i * 2))));
  emit levelChanged(double(peak) / 32768.0);
  emit samples(pcm);
}
