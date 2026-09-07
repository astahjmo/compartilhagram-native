#include "RtcEngine.h"
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTransform>
#include <algorithm>
#include <libyuv/convert.h>
#include <rtc_audio_device.h>

// Debug-only wire-level tracing: SDP m-line/codec summaries, ICE candidate
// types, and connection-state transitions, so a session that "shows sharing
// but the viewer sees nothing" can be diagnosed from stderr instead of
// guessed at. Enable with COMPARTILHAGRAM_RTC_DEBUG=1.
static bool rtcDebugEnabled() {
  static const bool enabled =
      qEnvironmentVariable("COMPARTILHAGRAM_RTC_DEBUG") == "1";
  return enabled;
}
static QString sdpSummary(const QString &sdp) {
  QStringList lines;
  for (const auto &line : sdp.split("\r\n"))
    if (line.startsWith("m=") || line.startsWith("a=mid:") ||
        line.startsWith("a=rtpmap:") || line.startsWith("a=fmtp:") ||
        line.startsWith("a=sendonly") || line.startsWith("a=recvonly") ||
        line.startsWith("a=sendrecv") || line.startsWith("a=inactive"))
      lines.append(line.trimmed());
  return lines.join(" | ");
}
static QString candidateSummary(const QString &candidate) {
  // "... typ host ..." / "... typ srflx ..." / "... typ relay raddr ...".
  QRegularExpression re(" typ (\\w+)");
  auto m = re.match(candidate);
  return m.hasMatch() ? m.captured(1) : QStringLiteral("?");
}

using namespace libwebrtc;
static string ws(const QString &s) { return string(s.toUtf8().constData()); }
static QString qs(const string &s) { return QString::fromUtf8(s.c_string()); }

FrameSink::FrameSink(QObject *parent) : QObject(parent) {
  timer_.setInterval(16);
  connect(&timer_, &QTimer::timeout, this, [this] {
    QImage image;
    {
      QMutexLocker lock(&mutex_);
      if (!dirty_)
        return;
      image = image_;
      dirty_ = false;
    }
    emit frame(image);
  });
  timer_.start();
}
void FrameSink::OnFrame(scoped_refptr<RTCVideoFrame> frame) {
  if (frame->width() <= 0 || frame->height() <= 0 || frame->width() > 8192 ||
      frame->height() > 8192)
    return;
  QImage image(frame->width(), frame->height(), QImage::Format_ARGB32);
  frame->ConvertToARGB(RTCVideoFrame::Type::kARGB, image.bits(),
                       image.bytesPerLine(), image.width(), image.height());
  if (frame->rotation())
    image = image.transformed(QTransform().rotate(frame->rotation()));
  QMutexLocker lock(&mutex_);
  image_ = std::move(image);
  dirty_ = true;
}
QImage FrameSink::latest() const {
  QMutexLocker lock(&mutex_);
  return image_;
}

struct RtcEngine::Peer : QObject,
                         RTCPeerConnectionObserver,
                         std::enable_shared_from_this<Peer> {
  RtcEngine *engine;
  QString id;
  scoped_refptr<RTCPeerConnection> pc;
  scoped_refptr<RTCVideoTrack> video;
  scoped_refptr<RTCAudioTrack> audio;
  FrameSink sink;
  QList<QJsonObject> candidates;
  bool publishing = false, sfu = false, remoteSet = false, closed = false,
       gathering = false, descriptionSent = false, answeredAsPublisher = false,
       renegotiated = false;
  int volume = 100;
  explicit Peer(RtcEngine *e, QString key) : engine(e), id(std::move(key)) {}
  void dispatch(std::function<void(std::shared_ptr<Peer>)> fn) {
    auto self = shared_from_this();
    QMetaObject::invokeMethod(
        this,
        [self, fn = std::move(fn)] {
          if (!self->closed)
            fn(self);
        },
        Qt::QueuedConnection);
  }
  void error(QString reason) {
    dispatch([reason](auto p) { emit p->engine->failure(p->id, reason); });
  }
  void OnSignalingState(RTCSignalingState) override {}
  void OnIceConnectionState(RTCIceConnectionState) override {}
  void OnRenegotiationNeeded() override {}
  void OnAddStream(scoped_refptr<RTCMediaStream>) override {}
  void OnRemoveStream(scoped_refptr<RTCMediaStream>) override {}
  void OnDataChannel(scoped_refptr<RTCDataChannel>) override {}
  void OnRemoveTrack(scoped_refptr<RTCRtpReceiver>) override {}
  void OnPeerConnectionState(RTCPeerConnectionState state) override {
    dispatch([state](auto p) {
      const QStringList names{"new",          "connecting", "connected",
                              "disconnected", "failed",     "closed"};
      const QString name = names.value(int(state), "unknown");
      if (rtcDebugEnabled())
        qDebug().noquote() << "[rtc]" << p->id
                           << (p->publishing ? "publish" : "receive")
                           << "connectionState ->" << name;
      emit p->engine->state(p->id, name);
      if (state == RTCPeerConnectionStateConnected) {
        p->engine->tune(p);
        std::weak_ptr<Peer> weak = p;
        QTimer::singleShot(2000, p.get(), [weak] {
          if (auto p = weak.lock(); p && !p->closed)
            p->engine->tune(p);
        });
        // CreateAnswer() on a pre-added sendonly transceiver never activates
        // a real RTP sender in this SDK build (confirmed: correct SDP
        // direction/track/encoding, yet GetStats() never reports an
        // outbound-rtp entry at all), while CreateOffer() on the same kind
        // of transceiver works correctly. A peer that answered while
        // publishing is renegotiated once, offering ourselves this time, to
        // route the sender through the working path.
        if (p->publishing && p->answeredAsPublisher && !p->renegotiated) {
          p->renegotiated = true;
          p->engine->offer(p, false);
        }
      }
    });
  }
  void OnIceGatheringState(RTCIceGatheringState state) override {
    if (state == RTCIceGatheringStateComplete)
      dispatch([](auto p) {
        if (p->gathering)
          p->engine->gatherDescription(p->id);
      });
  }
  void OnIceCandidate(scoped_refptr<RTCIceCandidate> c) override {
    QJsonObject candidate{{"candidate", qs(c->candidate())},
                          {"sdpMid", qs(c->sdp_mid())},
                          {"sdpMLineIndex", c->sdp_mline_index()}};
    if (rtcDebugEnabled())
      qDebug().noquote() << "[rtc]" << id << "local candidate type ="
                         << candidateSummary(qs(c->candidate()));
    dispatch([candidate](auto p) {
      if (!p->sfu)
        emit p->engine->signal(
            p->id, QJsonObject{{"type", "ice"}, {"candidate", candidate}});
    });
  }
  void OnTrack(scoped_refptr<RTCRtpTransceiver> t) override {
    auto track = t->receiver()->track();
    dispatch([track](auto p) { p->engine->attach(p, track); });
  }
  void OnAddTrack(vector<scoped_refptr<RTCMediaStream>>,
                  scoped_refptr<RTCRtpReceiver> receiver) override {
    auto track = receiver->track();
    dispatch([track](auto p) { p->engine->attach(p, track); });
  }
  void shutdown() {
    if (closed)
      return;
    closed = true;
    if (video)
      video->RemoveRenderer(&sink);
    if (audio)
      audio->SetVolume(0);
    if (pc) {
      pc->DeRegisterRTCPeerConnectionObserver();
      pc->Close();
      engine->factory_->Delete(pc);
      pc = nullptr;
    }
    video = nullptr;
    audio = nullptr;
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
  }
  ~Peer() override { shutdown(); }
};

RtcEngine::RtcEngine(QObject *parent) : QObject(parent) {
  if (LibWebRTC::Initialize())
    factory_ = LibWebRTC::CreateRTCPeerConnectionFactory();
  if (factory_ && !factory_->Initialize())
    factory_ = nullptr;
  connect(&preview_, &FrameSink::frame, this, [this](QImage image) {
    if (!video_)
      return;
    if (captureHeight_ != image.height()) {
      captureHeight_ = image.height();
      for (const auto &[id, p] : peers_)
        tune(p);
    }
    emit localFrame(image);
  });
  connect(&systemAudio_, &SystemAudio::failed, this, &RtcEngine::captureFailed);
  connect(&systemAudio_, &SystemAudio::levelChanged, this,
          &RtcEngine::audioLevel);
}
RtcEngine::~RtcEngine() {
  closePeers();
  stopCapture();
  lists_.clear();
  if (factory_) {
    factory_->Terminate();
    factory_ = nullptr;
  }
  LibWebRTC::Terminate();
}
QList<DesktopType> RtcEngine::desktopSourceTypes(const QString &sessionType,
                                                 const QString &qtPlatform,
                                                 bool hasWaylandDisplay) {
  if (qtPlatform == "offscreen" || qtPlatform == "minimal")
    return {};
  // The SDK's kWindow enumerator unconditionally dereferences the result of
  // CreateWindowCapturer(), which is null on Wayland. The kScreen backend
  // enables PipeWire and delegates selection to the desktop portal instead.
  // Check the session too: Qt may use xcb/XWayland within a Wayland session.
  if (sessionType == "wayland" || qtPlatform.startsWith("wayland") ||
      hasWaylandDisplay)
    return {kScreen};
  return {kScreen, kWindow};
}
QList<RtcEngine::Source> RtcEngine::sources() {
  QList<Source> out;
  if (!factory_)
    return out;
  lists_.clear();
  const auto types = desktopSourceTypes(
      qEnvironmentVariable("XDG_SESSION_TYPE"), QGuiApplication::platformName(),
      !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
  if (types == QList<DesktopType>{kScreen})
    return {{tr("Tela — seletor do sistema"), {}, 1},
            {tr("Janela — seletor do sistema"), {}, 2}};
  if (types.isEmpty())
    return out;
  auto device = factory_->GetDesktopDevice();
  if (!device)
    return out;
  for (auto type : types) {
    auto list = device->GetDesktopMediaList(type);
    if (!list)
      continue;
    list->UpdateSourceList(true, false);
    lists_.append(list);
    for (int i = 0; i < list->GetSourceCount(); ++i) {
      auto s = list->GetSource(i);
      out.append(
          {(type == kScreen ? tr("Tela: ") : tr("Janela: ")) + qs(s->name()),
           s});
    }
  }
  return out;
}
bool RtcEngine::capture(const Source &source, QJsonObject quality) {
  if (!factory_)
    return false;
  pendingPortal_.reset();
  if (source.portalType) {
    auto videoSource = factory_->CreateCustomVideoSource(
        "portal", RTCMediaConstraints::Create());
    if (!videoSource)
      return false;
    auto video =
        factory_->CreateVideoTrack(videoSource, "compartilhagram-video");
    if (!video)
      return false;
    pendingPortal_ = std::make_unique<PortalCapture>();
    auto capture = pendingPortal_.get();
    connect(capture, &PortalCapture::frame, this,
            [this, capture, videoSource, video, quality](QImage image) {
              if (pendingPortal_.get() == capture) {
                portal_ = std::move(pendingPortal_);
                installVideo(videoSource, video, {}, quality);
              }
              if (portal_.get() != capture)
                return;
              if (auto frame = imageFrame(image))
                videoSource->OnCapturedFrame(frame);
            });
    connect(capture, &PortalCapture::failed, this,
            [this, capture](QString reason) {
              if (pendingPortal_.get() == capture)
                emit sourceSelectionFailed(reason);
              else if (portal_.get() == capture)
                emit captureFailed(reason);
            });
    capture->start(source.portalType == 2, quality.value("fps").toInt(30));
    return true;
  }
  if (!source.media)
    return false;
  auto capturer =
      factory_->GetDesktopDevice()->CreateDesktopCapturer(source.media, true);
  if (!capturer)
    return false;
  auto constraints = RTCMediaConstraints::Create();
  auto videoSource =
      factory_->CreateDesktopSource(capturer, "screen", constraints);
  auto video = factory_->CreateVideoTrack(videoSource, "compartilhagram-video");
  if (!video)
    return false;
  capturer->RegisterDesktopCapturerObserver(this);
  if (capturer->Start(quality.value("fps").toInt(30)) ==
      RTCDesktopCapturer::CS_FAILED) {
    capturer->DeRegisterDesktopCapturerObserver();
    return false;
  }
  portal_.reset();
  installVideo(videoSource, video, capturer, quality);
  return true;
}
void RtcEngine::installVideo(scoped_refptr<RTCVideoSource> videoSource,
                             scoped_refptr<RTCVideoTrack> video,
                             scoped_refptr<RTCDesktopCapturer> capturer,
                             QJsonObject quality) {
  // Keep old capture alive until every sender points at the new source.
  for (const auto &[id, p] : peers_)
    if (p->publishing)
      for (const auto &sender : p->pc->senders().std_vector())
        if (sender->media_type() == RTCMediaType::VIDEO)
          sender->set_track(video);
  if (video_)
    video_->RemoveRenderer(&preview_);
  if (capturer_) {
    capturer_->DeRegisterDesktopCapturerObserver();
    capturer_->Stop();
  }
  capturer_ = capturer;
  videoSource_ = videoSource;
  video_ = video;
  video_->AddRenderer(&preview_);
  if (!audio_) {
    RTCAudioOptions options;
    options.echo_cancellation = false;
    options.auto_gain_control = false;
    options.noise_suppression = false;
    audioSource_ = factory_->CreateAudioSource(
        "system-output", RTCAudioSource::kCustom, options);
    audio_ = factory_->CreateAudioTrack(audioSource_, "compartilhagram-audio");
    systemAudio_.start(audioSource_, audioSelection_);
  }
  setQuality(quality);
}
void RtcEngine::setAudioSelection(AudioSelection selection) {
  audioSelection_ = std::move(selection);
  if (audioSource_)
    systemAudio_.start(audioSource_, audioSelection_);
}
scoped_refptr<RTCVideoFrame> RtcEngine::imageFrame(const QImage &image) {
  if (image.isNull())
    return nullptr;
  auto packed = image.convertToFormat(QImage::Format_ARGB32);
  int w = packed.width(), h = packed.height(), uvStride = (w + 1) / 2,
      uvHeight = (h + 1) / 2;
  QByteArray y(w * h, '\0'), u(uvStride * uvHeight, '\0'),
      v(uvStride * uvHeight, '\0');
  auto yp = reinterpret_cast<uint8_t *>(y.data()),
       up = reinterpret_cast<uint8_t *>(u.data()),
       vp = reinterpret_cast<uint8_t *>(v.data());
  if (libyuv::ARGBToI420(packed.constBits(), packed.bytesPerLine(), yp, w, up,
                         uvStride, vp, uvStride, w, h) != 0)
    return nullptr;
  return RTCVideoFrame::Create(w, h, yp, w, up, uvStride, vp, uvStride);
}
void RtcEngine::stopCapture() {
  pendingPortal_.reset();
  portal_.reset();
  systemAudio_.stop();
  if (video_)
    video_->RemoveRenderer(&preview_);
  if (capturer_) {
    capturer_->DeRegisterDesktopCapturerObserver();
    capturer_->Stop();
  }
  capturer_ = nullptr;
  video_ = nullptr;
  videoSource_ = nullptr;
  audio_ = nullptr;
  audioSource_ = nullptr;
}
void RtcEngine::OnError(scoped_refptr<RTCDesktopCapturer> capture) {
  auto identity = capture.get();
  QMetaObject::invokeMethod(
      this,
      [this, identity] {
        if (capturer_.get() == identity)
          emit captureFailed(tr("Captura interrompida. A janela pode ter sido "
                                "fechada ou a permissão revogada."));
      },
      Qt::QueuedConnection);
}
void RtcEngine::setQuality(QJsonObject quality) {
  const auto oldFps = quality_.value("fps").toInt(30);
  quality_ = quality;
  if (portal_)
    portal_->setFps(quality.value("fps").toInt(30));
  if (capturer_ && oldFps != quality_.value("fps").toInt(30)) {
    capturer_->Stop();
    capturer_->Start(quality_.value("fps").toInt(30));
  }
  for (const auto &[id, p] : peers_)
    tune(p);
}
void RtcEngine::tune(const std::shared_ptr<Peer> &p) {
  if (!p->publishing || p->closed)
    return;
  int resolution = quality_.value("resolution").toInt(720),
      fps = quality_.value("fps").toInt(30);
  bool doc = quality_.value("mode") == "doc";
  int bitrate = doc ? (resolution == 1080  ? 1500000
                       : resolution == 720 ? 1000000
                                           : 600000)
                    : (resolution == 1080  ? 4500000
                       : resolution == 720 ? 2500000
                                           : 1200000) *
                          (fps == 60 ? 1.5 : 1);
  for (auto sender : p->pc->senders().std_vector()) {
    auto params = sender->parameters();
    if (!params)
      continue;
    auto encodings = params->encodings().std_vector();
    if (rtcDebugEnabled())
      qDebug() << "[rtc] tune sender kind ="
               << int(sender->media_type()) << "encodings =" << encodings.size()
               << (encodings.empty() ? -1 : int(encodings[0]->active()));
    for (auto encoding : encodings) {
      const bool video = sender->media_type() == RTCMediaType::VIDEO;
      // Some encodings created by this SDK's AddTransceiver come back
      // inactive by default (mirrors the direction/track defaults also
      // found wrong here) — force it on or the sender stays silent even
      // once direction and track are both correct.
      encoding->set_active(true);
      encoding->set_max_bitrate_bps(video ? bitrate : 128000);
      if (video) {
        encoding->set_min_bitrate_bps(300000);
        encoding->set_max_framerate(fps);
        encoding->set_scale_resolution_down_by(
            std::max(1.0, double(captureHeight_) / resolution));
      }
    }
    params->set_encodings(encodings);
    params->SetDegradationPreference(
        doc ? RTCDegradationPreference::MAINTAIN_RESOLUTION
            : RTCDegradationPreference::MAINTAIN_FRAMERATE);
    sender->set_parameters(params);
  }
}
void RtcEngine::connectPeer(QString id, bool publishing, bool initiate,
                            bool sfu) {
  if (!factory_ || peers_.contains(id))
    return;
  RTCConfiguration config;
  config.bundle_policy = kBundlePolicyMaxBundle;
  config.sdp_semantics = SdpSemantics::kUnifiedPlan;
  QJsonArray ice =
      sfu ? QJsonArray{QJsonObject{{"urls", "stun:stun.cloudflare.com:3478"}}}
          : ice_;
  struct IceEntry {
    QString url, username, credential;
  };
  QList<IceEntry> relays, hosts;
  for (auto value : ice) {
    auto server = value.toObject();
    auto urls = server.value("urls").isArray()
                    ? server.value("urls").toArray()
                    : QJsonArray{server.value("urls")};
    for (auto url : urls) {
      IceEntry entry{url.toString(), server.value("username").toString(),
                     server.value("credential").toString()};
      auto &bucket = entry.url.startsWith("turn:", Qt::CaseInsensitive) ||
                             entry.url.startsWith("turns:", Qt::CaseInsensitive)
                         ? relays
                         : hosts;
      bucket.append(entry);
    }
  }
  // The SDK's ice_servers array is fixed at kMaxIceServerSize entries. TURN/TURNS
  // relays are what let viewers behind restrictive NATs or firewalls connect at
  // all, so they must never be silently dropped in favor of plain STUN entries
  // when the combined list overflows that limit.
  const QList<IceEntry> ordered = relays + hosts;
  if (ordered.size() > kMaxIceServerSize)
    qWarning("RtcEngine: %d servidor(es) ICE descartados (limite de %d)",
             ordered.size() - kMaxIceServerSize, kMaxIceServerSize);
  int index = 0;
  for (const auto &entry : ordered) {
    if (index >= kMaxIceServerSize)
      break;
    config.ice_servers[index++] = {ws(entry.url), ws(entry.username),
                                   ws(entry.credential)};
  }
  auto p = std::make_shared<Peer>(this, id);
  p->publishing = publishing;
  p->sfu = sfu;
  p->volume = publishing || sfu ? 0 : 100;
  p->pc = factory_->Create(config, RTCMediaConstraints::Create());
  if (!p->pc) {
    emit failure(id, tr("Falha ao criar PeerConnection"));
    return;
  }
  peers_[id] = p;
  p->pc->RegisterRTCPeerConnectionObserver(p.get());
  connect(&p->sink, &FrameSink::frame, this,
          [this, id](QImage image) { emit frame(id, image); });
  if (publishing) {
    auto init = RTCRtpTransceiverInit::Create(
        RTCRtpTransceiverDirection::kSendOnly,
        vector<string>(std::vector<string>{string("compartilhagram")}), {});
    if (video_)
      if (auto t = p->pc->AddTransceiver(video_, init)) {
        t->SetDirectionWithError(RTCRtpTransceiverDirection::kSendOnly);
        auto sender = t->sender();
        bool hadTrack = sender && sender->track();
        if (sender && !hadTrack)
          sender->set_track(video_);
        if (rtcDebugEnabled())
          qDebug() << "[rtc] video sender track after AddTransceiver, present ="
                   << hadTrack << "senderExists =" << bool(sender);
      }
    if (audio_)
      if (auto t = p->pc->AddTransceiver(audio_, init)) {
        t->SetDirectionWithError(RTCRtpTransceiverDirection::kSendOnly);
        auto sender = t->sender();
        if (sender && !sender->track())
          sender->set_track(audio_);
      }
  } else if (!sfu) {
    auto init = RTCRtpTransceiverInit::Create(
        RTCRtpTransceiverDirection::kRecvOnly, {}, {});
    if (auto t = p->pc->AddTransceiver(RTCMediaType::VIDEO, init))
      t->SetDirectionWithError(RTCRtpTransceiverDirection::kRecvOnly);
    if (auto t = p->pc->AddTransceiver(RTCMediaType::AUDIO, init))
      t->SetDirectionWithError(RTCRtpTransceiverDirection::kRecvOnly);
  }
  if (initiate)
    offer(p, false);
}

// Diagnosed on a real cross-network session: CreateAnswer() in this SDK build
// does not reliably encode the local transceivers' actual, confirmed
// direction (RTCRtpTransceiver::direction(), verified == kSendOnly via
// SetDirectionWithError) into the SDP text it returns — it can emit
// "a=inactive" for every m-line even though the transceivers are sendonly,
// so ICE/DTLS reaches "connected" but zero RTP ever leaves the process. Every
// m-section within one of our peer connections shares a single role (all
// sendonly for a publisher/SFU-publish peer, all recvonly for a
// viewer/SFU-subscribe peer — see connectPeer), so it is safe to force the
// direction attribute directly in the local SDP text as a last-resort
// correction, independent of whatever the SDK itself wrote there.
static QString forceDirection(const QString &sdp, bool sendonly) {
  const QString wanted = sendonly ? "a=sendonly" : "a=recvonly";
  QStringList lines = sdp.split("\r\n");
  for (auto &line : lines)
    if (line == "a=sendrecv" || line == "a=sendonly" ||
        line == "a=recvonly" || line == "a=inactive")
      line = wanted;
  return lines.join("\r\n");
}
// Match the web client's stereo negotiation and warm-start screen bitrate
// hints.
static QString tuneSdp(QString sdp) {
  const auto lines = sdp.split("\r\n");
  for (const auto &line : lines) {
    QRegularExpression re(
        "^a=rtpmap:(\\d+) (opus/48000/2|(?:VP8|VP9|H264|AV1)/90000)");
    auto m = re.match(line);
    if (!m.hasMatch())
      continue;
    const QString params =
        m.captured(2).startsWith("opus")
            ? "stereo=1;sprop-stereo=1"
            : "x-google-start-bitrate=2000;x-google-min-bitrate=300";
    QRegularExpression fmtp("a=fmtp:" + m.captured(1) + " ([^\\r\\n]*)");
    auto f = fmtp.match(sdp);
    if (f.hasMatch()) {
      if (!f.captured(1).contains(params))
        sdp.replace(f.capturedStart(), f.capturedLength(),
                    f.captured(0) + ';' + params);
    } else
      sdp.replace(line + "\r\n",
                  line + "\r\na=fmtp:" + m.captured(1) + ' ' + params + "\r\n");
  }
  return sdp;
}
void RtcEngine::offer(const std::shared_ptr<Peer> &p, bool answer) {
  std::weak_ptr<Peer> weak = p;
  auto success = [weak](string sdp, string type) {
    if (auto p = weak.lock())
      p->dispatch([sdp = qs(sdp), type = qs(type)](auto p) {
        p->engine->setLocal(
            p, forceDirection(tuneSdp(sdp), p->publishing), type);
      });
  };
  auto failure = [weak](const char *reason) {
    if (auto p = weak.lock())
      p->error(QString::fromUtf8(reason));
  };
  if (answer)
    p->pc->CreateAnswer(success, failure, RTCMediaConstraints::Create());
  else
    p->pc->CreateOffer(success, failure, RTCMediaConstraints::Create());
}
void RtcEngine::setLocal(const std::shared_ptr<Peer> &p, QString sdp,
                         QString type) {
  if (rtcDebugEnabled())
    qDebug().noquote() << "[rtc]" << p->id << "local" << type << ":"
                       << sdpSummary(sdp);
  std::weak_ptr<Peer> weak = p;
  p->pc->SetLocalDescription(
      ws(sdp), ws(type),
      [weak, sdp, type] {
        if (auto p = weak.lock())
          p->dispatch([sdp, type](auto p) {
            if (p->sfu) {
              p->gathering = true;
              if (p->pc->ice_gathering_state() == RTCIceGatheringStateComplete)
                p->engine->gatherDescription(p->id);
              else {
                std::weak_ptr<Peer> weak = p;
                QTimer::singleShot(2000, p.get(), [weak] {
                  if (auto p = weak.lock(); p && !p->closed)
                    p->engine->gatherDescription(p->id);
                });
              }
            } else
              emit p->engine->signal(p->id,
                                     QJsonObject{{"type", type}, {"sdp", sdp}});
          });
      },
      [weak](const char *error) {
        if (auto p = weak.lock())
          p->error(QString::fromUtf8(error));
      });
}
void RtcEngine::signalPeer(QString id, QJsonObject signal) {
  auto it = peers_.find(id);
  if (it == peers_.end())
    return;
  auto p = it->second;
  auto type = signal.value("type").toString();
  if (type == "ice") {
    auto c = signal.value("candidate").toObject();
    if (rtcDebugEnabled())
      qDebug().noquote() << "[rtc]" << id << "remote candidate type ="
                         << candidateSummary(c.value("candidate").toString());
    if (!p->remoteSet) {
      if (p->candidates.size() < 256)
        p->candidates.append(c);
    } else
      p->pc->AddCandidate(ws(c.value("sdpMid").toString()),
                          c.value("sdpMLineIndex").toInt(),
                          ws(c.value("candidate").toString()));
    return;
  }
  if (type != "offer" && type != "answer")
    return;
  if (rtcDebugEnabled())
    qDebug().noquote() << "[rtc]" << id << "remote" << type << ":"
                       << sdpSummary(signal.value("sdp").toString());
  std::weak_ptr<Peer> weak = p;
  p->pc->SetRemoteDescription(
      ws(signal.value("sdp").toString()), ws(type),
      [weak, type] {
        if (auto p = weak.lock())
          p->dispatch([type](auto p) {
            p->remoteSet = true;
            for (auto c : std::exchange(p->candidates, {}))
              p->pc->AddCandidate(ws(c.value("sdpMid").toString()),
                                  c.value("sdpMLineIndex").toInt(),
                                  ws(c.value("candidate").toString()));
            emit p->engine->remoteDescriptionSet(p->id);
            if (type == "offer") {
              if (p->publishing)
                p->answeredAsPublisher = true;
              p->engine->offer(p, true);
            }
            // Re-apply bitrate/active encoding parameters once this
            // negotiation round is actually in effect: the renegotiation
            // triggered on "connected" (see OnPeerConnectionState) can still
            // be in flight when the fixed 0ms/2000ms tune() calls run, so an
            // encoding reset by this round would otherwise stay untouched.
            if (p->publishing)
              p->engine->tune(p);
          });
      },
      [weak](const char *error) {
        if (auto p = weak.lock())
          p->error(QString::fromUtf8(error));
      });
}
void RtcEngine::gatherDescription(QString id) {
  auto it = peers_.find(id);
  if (it == peers_.end())
    return;
  auto p = it->second;
  if (p->descriptionSent)
    return;
  p->descriptionSent = true;
  std::weak_ptr<Peer> weak = p;
  p->pc->GetLocalDescription(
      [weak](const char *sdp, const char *) {
        if (auto p = weak.lock())
          p->dispatch([sdp = QString::fromUtf8(sdp)](auto p) {
            QJsonArray tracks;
            for (auto t : p->pc->transceivers().std_vector())
              if (t->sender()->track()) {
                tracks.append(
                    QJsonObject{{"mid", qs(t->mid())},
                                {"kind", t->media_type() == RTCMediaType::VIDEO
                                             ? "video"
                                             : "audio"}});
              }
            emit p->engine->localDescription(p->id, sdp, tracks);
          });
      },
      [weak](const char *error) {
        if (auto p = weak.lock())
          p->error(QString::fromUtf8(error));
      });
}
void RtcEngine::attach(const std::shared_ptr<Peer> &p,
                       scoped_refptr<RTCMediaTrack> track) {
  if (!track)
    return;
  if (qs(track->kind()) == "video") {
    auto video =
        scoped_refptr<RTCVideoTrack>(static_cast<RTCVideoTrack *>(track.get()));
    if (p->video == video)
      return;
    if (p->video)
      p->video->RemoveRenderer(&p->sink);
    p->video = video;
    video->AddRenderer(&p->sink);
  } else if (qs(track->kind()) == "audio") {
    p->audio =
        scoped_refptr<RTCAudioTrack>(static_cast<RTCAudioTrack *>(track.get()));
    p->audio->SetVolume(p->volume / 100.0);
  }
}
void RtcEngine::volume(QString id, int percent) {
  auto it = peers_.find(id);
  if (it != peers_.end()) {
    it->second->volume = std::clamp(percent, 0, 100);
    if (it->second->audio)
      it->second->audio->SetVolume(it->second->volume / 100.0);
  }
}
void RtcEngine::stats(QString id) {
  auto it = peers_.find(id);
  if (it == peers_.end())
    return;
  std::weak_ptr<Peer> weak = it->second;
  it->second->pc->GetStats(
      [weak](vector<scoped_refptr<MediaRTCStats>> reports) {
        QJsonArray data;
        for (auto r : reports.std_vector())
          data.append(
              QJsonDocument::fromJson(qs(r->ToJson()).toUtf8()).object());
        if (auto p = weak.lock())
          p->dispatch(
              [data](auto p) { emit p->engine->statistics(p->id, data); });
      },
      [](const char *) {});
}
void RtcEngine::removePeer(const QString &id) {
  auto it = peers_.find(id);
  if (it == peers_.end())
    return;
  it->second->shutdown();
  peers_.erase(it);
}
void RtcEngine::closePeers() {
  for (auto &[id, p] : peers_)
    p->shutdown();
  peers_.clear();
}
void RtcEngine::createTestSource() {
  stopCapture();
  videoSource_ =
      factory_->CreateCustomVideoSource("test", RTCMediaConstraints::Create());
  video_ = factory_->CreateVideoTrack(videoSource_, "test-video");
  audioSource_ =
      factory_->CreateAudioSource("test-audio", RTCAudioSource::kCustom);
  audio_ = factory_->CreateAudioTrack(audioSource_, "test-audio");
  quality_ = QJsonObject{{"mode", "media"}, {"fps", 30}, {"resolution", 480}};
  captureHeight_ = 240;
}
void RtcEngine::pushTestFrame(int width, int height) {
  if (!videoSource_ || width <= 0 || height <= 0 || width > 4096 ||
      height > 4096)
    return;
  int chromaWidth = (width + 1) / 2, chromaHeight = (height + 1) / 2;
  QByteArray y(width * height, char(90)),
      u(chromaWidth * chromaHeight, char(70)),
      v(chromaWidth * chromaHeight, char(200));
  videoSource_->OnCapturedFrame(RTCVideoFrame::Create(
      width, height, reinterpret_cast<const uint8_t *>(y.data()), width,
      reinterpret_cast<const uint8_t *>(u.data()), chromaWidth,
      reinterpret_cast<const uint8_t *>(v.data()), chromaWidth));
}
void RtcEngine::pushTestAudio() {
  if (!audioSource_)
    return;
  int16_t pcm[960];
  for (int i = 0; i < 480; ++i)
    pcm[i * 2] = pcm[i * 2 + 1] = (i % 48 < 24 ? 1000 : -1000);
  audioSource_->CaptureFrame(pcm, 16, 48000, 2, 480);
}
