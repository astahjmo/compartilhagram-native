#pragma once
#include "PortalCapture.h"
#include "SystemAudio.h"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <libwebrtc.h>
#include <map>
#include <memory>
#include <rtc_desktop_capturer.h>
#include <rtc_desktop_media_list.h>
#include <rtc_peerconnection.h>

class FrameSink : public QObject,
                  public libwebrtc::RTCVideoRenderer<
                      libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>> {
  Q_OBJECT
public:
  explicit FrameSink(QObject *parent = nullptr);
  void OnFrame(libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>) override;
  QImage latest() const;
signals:
  void frame(QImage image);

private:
  mutable QMutex mutex_;
  QImage image_;
  bool dirty_ = false;
  QTimer timer_;
};

class RtcEngine : public QObject, private libwebrtc::DesktopCapturerObserver {
  Q_OBJECT
public:
  struct Source {
    QString name;
    libwebrtc::scoped_refptr<libwebrtc::MediaSource> media;
    uint32_t portalType = 0;
  };
  explicit RtcEngine(QObject *parent = nullptr);
  ~RtcEngine() override;
  bool initialized() const { return factory_ != nullptr; }
  QList<Source> sources();
  static QList<libwebrtc::DesktopType>
  desktopSourceTypes(const QString &sessionType, const QString &qtPlatform,
                     bool hasWaylandDisplay);
  bool capture(const Source &, QJsonObject quality);
  void setAudioSelection(AudioSelection selection);
  static libwebrtc::scoped_refptr<libwebrtc::RTCVideoFrame>
  imageFrame(const QImage &image);
  void stopCapture();
  void setQuality(QJsonObject quality);
  void setIce(QJsonArray ice) { ice_ = ice; }
  void connectPeer(QString id, bool publishing, bool initiate,
                   bool sfu = false);
  void signalPeer(QString id, QJsonObject signal);
  void removePeer(const QString &id);
  void closePeers();
  void gatherDescription(QString id);
  void volume(QString id, int percent);
  void stats(QString id);
  QImage preview() const { return preview_.latest(); }
  // Deterministic synthetic source for real encode/decode loopback tests.
  void createTestSource();
  void pushTestFrame(int width = 320, int height = 240);
  void pushTestAudio();
signals:
  void signal(QString peer, QJsonObject data);
  void localDescription(QString peer, QString sdp, QJsonArray tracks);
  void remoteDescriptionSet(QString peer);
  void frame(QString peer, QImage image);
  void localFrame(QImage image);
  void state(QString peer, QString state);
  void failure(QString peer, QString reason);
  void captureFailed(QString reason);
  void sourceSelectionFailed(QString reason);
  void audioLevel(double level);
  void statistics(QString peer, QJsonArray reports);

private:
  struct Peer;
  friend struct Peer;
  void offer(const std::shared_ptr<Peer> &, bool answer);
  void setLocal(const std::shared_ptr<Peer> &, QString sdp, QString type);
  void tune(const std::shared_ptr<Peer> &);
  void attach(const std::shared_ptr<Peer> &,
              libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack>);
  void installVideo(libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource>,
                    libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack>,
                    libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>,
                    QJsonObject quality);
  void
  OnStart(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
  void
  OnPaused(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
  void
  OnStop(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override {}
  void
      OnError(libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer>) override;
  libwebrtc::scoped_refptr<libwebrtc::RTCPeerConnectionFactory> factory_;
  libwebrtc::scoped_refptr<libwebrtc::RTCDesktopCapturer> capturer_;
  libwebrtc::scoped_refptr<libwebrtc::RTCVideoSource> videoSource_;
  libwebrtc::scoped_refptr<libwebrtc::RTCVideoTrack> video_;
  libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> audioSource_;
  libwebrtc::scoped_refptr<libwebrtc::RTCAudioTrack> audio_;
  FrameSink preview_;
  SystemAudio systemAudio_;
  AudioSelection audioSelection_;
  std::unique_ptr<PortalCapture> portal_, pendingPortal_;
  QJsonArray ice_;
  QJsonObject quality_;
  std::map<QString, std::shared_ptr<Peer>> peers_;
  QList<libwebrtc::scoped_refptr<libwebrtc::RTCDesktopMediaList>> lists_;
  int captureHeight_ = 1080;
};
