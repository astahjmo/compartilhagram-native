#pragma once
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <map>
#include <memory>
#ifndef Q_OS_WIN
#include <pulse/pulseaudio.h>
#endif
#include <rtc_audio_source.h>

struct AudioSelection {
  enum Mode { None, All, Include, Exclude } mode = None;
  QSet<QString> applications;
  bool accepts(const QString &key, bool ownApplication = false) const {
    if (ownApplication || mode == None)
      return false;
    return mode == All || (mode == Include ? applications.contains(key)
                                           : !applications.contains(key));
  }
};

// Monitors individual playback streams without moving them or recording a mic.
// The same connection can list applications without enabling any recording.
class SystemAudio : public QObject {
  Q_OBJECT
public:
  explicit SystemAudio(QObject *parent = nullptr);
  ~SystemAudio() override;
  void discover();
  void start(libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source,
             AudioSelection selection);
  void stop();
  static QByteArray mixPcm(const QList<QByteArray> &inputs);
signals:
  void failed(QString message);
  void applicationsChanged(QJsonArray applications);
  void levelChanged(double level);
  void samples(QByteArray pcm);

private:
  libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source_;
  AudioSelection selection_;
  int generation_ = 0;
#ifdef Q_OS_WIN
  struct WindowsState;
  std::unique_ptr<WindowsState> windows_;
  void launch();
#else
  struct Input;
  struct Application {
    uint32_t index, sink;
    QString key, name;
    bool own;
  };
  void connectServer();
  void error();
  void refresh();
  void reconcile();
  void mix();
  static void contextChanged(pa_context *, void *);
  static void sinks(pa_context *, const pa_sink_info *, int, void *);
  static void inputs(pa_context *, const pa_sink_input_info *, int, void *);
  static void read(pa_stream *, size_t, void *);
  pa_threaded_mainloop *loop_ = nullptr;
  pa_context *context_ = nullptr;
  std::map<uint32_t, std::unique_ptr<Input>> streams_;
  std::map<uint32_t, QString> monitors_;
  std::map<uint32_t, Application> applications_;
  bool refreshing_ = false, refreshAgain_ = false;
  QTimer mixTimer_;
#endif
};
