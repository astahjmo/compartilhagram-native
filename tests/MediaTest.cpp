#include "CodecStatus.h"
#include "PortalCapture.h"
#include "RtcEngine.h"
#include <QScopeGuard>
#include <QSignalSpy>
#include <QtTest>

class MediaTest : public QObject {
  Q_OBJECT
private slots:
  void codecStatusSummary() {
    QVERIFY(CodecStatus::describe(false, {}, {})
                .contains("built without system codec support"));
    QVERIFY(CodecStatus::describe(true, "Permission denied", {})
                .contains("reason: Permission denied"));
    QVERIFY(CodecStatus::describe(true, {}, {}).contains("idle"));
    QJsonArray streams{QJsonObject{{"direction", "encode"},
                                   {"mode", "hardware"},
                                   {"implementation", "VA-API"}},
                       QJsonObject{{"direction", "decode"},
                                   {"mode", "software"},
                                   {"reason", "Unsupported codec"}}};
    auto text = CodecStatus::describe(true, {}, streams);
    QVERIFY(text.contains("Encoding: GPU enabled"));
    QVERIFY(text.contains("Decoding: GPU acceleration not enabled"));
    QVERIFY(text.contains("reason: Unsupported codec"));
  }
  void portalPixels() {
    QCOMPARE(PortalCapture::sourceMask(false), 1u);
    QCOMPARE(PortalCapture::sourceMask(true), 2u);
    const uint8_t rgba[] = {255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 0, 0};
    auto image = PortalCapture::copyFrame(rgba, sizeof(rgba), 2, 1, 12,
                                          SPA_VIDEO_FORMAT_RGBA);
    QCOMPARE(image.pixelColor(0, 0), QColor(Qt::red));
    QCOMPARE(image.pixelColor(1, 0), QColor(Qt::blue));
    QVERIFY(PortalCapture::copyFrame(rgba, 4, 2, 1, 12, SPA_VIDEO_FORMAT_RGBA)
                .isNull());
    QVERIFY(PortalCapture::copyFrame(rgba, sizeof(rgba), 2, 1, 4,
                                     SPA_VIDEO_FORMAT_RGBA)
                .isNull());
    QVERIFY(PortalCapture::copyFrame(rgba, sizeof(rgba), 2, 1, 12,
                                     SPA_VIDEO_FORMAT_I420)
                .isNull());
    QImage odd(17, 13, QImage::Format_ARGB32);
    odd.fill(Qt::red);
    auto frame = RtcEngine::imageFrame(odd);
    QVERIFY(frame);
    QCOMPARE(frame->width(), 17);
    QCOMPARE(frame->height(), 13);
    QImage decoded(17, 13, QImage::Format_ARGB32);
    frame->ConvertToARGB(libwebrtc::RTCVideoFrame::Type::kARGB, decoded.bits(),
                         decoded.bytesPerLine(), 17, 13);
    QVERIFY(decoded.pixelColor(16, 12).red() > 240);
    QVERIFY(decoded.pixelColor(16, 12).blue() < 10);
  }
  void desktopSourcePolicy() {
    using namespace libwebrtc;
    // Never call the SDK's unsupported kWindow constructor on Wayland,
    // including Qt apps running through XWayland.
    QCOMPARE(RtcEngine::desktopSourceTypes("wayland", "wayland", true),
             QList<DesktopType>{kScreen});
    QCOMPARE(RtcEngine::desktopSourceTypes("wayland", "xcb", true),
             QList<DesktopType>{kScreen});
    QCOMPARE(RtcEngine::desktopSourceTypes("", "wayland-egl", false),
             QList<DesktopType>{kScreen});
    QCOMPARE(RtcEngine::desktopSourceTypes("", "xcb", true),
             QList<DesktopType>{kScreen});
    QCOMPARE(RtcEngine::desktopSourceTypes("x11", "xcb", false),
             (QList<DesktopType>{kScreen, kWindow}));
    QVERIFY(
        RtcEngine::desktopSourceTypes("wayland", "offscreen", true).isEmpty());
    QVERIFY(RtcEngine::desktopSourceTypes("x11", "minimal", false).isEmpty());
  }
  void desktopEnumeration() {
    if (!qEnvironmentVariableIsSet("COMPARTILHAGRAM_TEST_DESKTOP_CAPTURE"))
      QSKIP("Opt-in desktop test; may open the system screen-sharing picker");
    RtcEngine rtc;
    QVERIFY(rtc.initialized());
    const auto sources = rtc.sources();
    QVERIFY2(!sources.isEmpty(),
             "The graphical session returned no capture sources");
    qInfo() << "Enumerated" << sources.size()
            << "capture sources without a crash";
    // Repeat the source-replacement path, too, without recording frames.
    QVERIFY(!rtc.sources().isEmpty());
  }
  void nativeEncodeDecode() {
    RtcEngine rtc;
    QVERIFY(rtc.initialized());
    rtc.createTestSource();
    QSignalSpy frames(&rtc, &RtcEngine::frame);
    QSignalSpy errors(&rtc, &RtcEngine::failure);
    connect(&rtc, &RtcEngine::signal, &rtc,
            [&rtc](QString peer, QJsonObject signal) {
              rtc.signalPeer(peer == "sender" ? "receiver" : "sender", signal);
            });
    rtc.connectPeer("receiver", false, false);
    rtc.connectPeer("sender", true, true);
    QTimer source;
    QTimer audio;
    connect(&audio, &QTimer::timeout, &rtc, &RtcEngine::pushTestAudio);
    audio.start(10);
    int sourceWidth = 320, sourceHeight = 240;
    connect(&source, &QTimer::timeout, &rtc,
            [&] { rtc.pushTestFrame(sourceWidth, sourceHeight); });
    source.start(33);
    QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !errors.isEmpty(), 15000);
    if (!errors.isEmpty())
      qWarning() << errors.first();
    QVERIFY(errors.isEmpty());
    QVERIFY(!frames.isEmpty());
    auto image = qvariant_cast<QImage>(frames.first()[1]);
    QVERIFY(!image.isNull());
    QCOMPARE(image.width(), 320);
    QCOMPARE(image.height(), 240);
    // Red-dominant source verifies the I420 -> ARGB -> QImage channel order.
    QVERIFY(image.pixelColor(160, 120).red() >
            image.pixelColor(160, 120).blue());
    bool receivedAudio = false;
    connect(&rtc, &RtcEngine::statistics, &rtc,
            [&](QString id, QJsonArray reports) {
              if (id != "receiver")
                return;
              for (auto value : reports) {
                const auto report = value.toObject();
                if (report["type"] == "inbound-rtp" &&
                    report["kind"] == "audio" &&
                    report["packetsReceived"].toInt() > 0)
                  receivedAudio = true;
              }
            });
    QTimer stats;
    connect(&stats, &QTimer::timeout, &rtc, [&] { rtc.stats("receiver"); });
    stats.start(100);
    QTRY_VERIFY_WITH_TIMEOUT(receivedAudio, 5000);
    if (qEnvironmentVariableIsSet("COMPARTILHAGRAM_TEST_GPU") ||
        (CodecStatus::available() &&
         qEnvironmentVariable("COMPARTILHAGRAM_DISABLE_GPU") == "1")) {
      const bool hardware =
          qEnvironmentVariableIsSet("COMPARTILHAGRAM_TEST_GPU");
      auto matched = [&] {
        QSet<QString> directions;
        for (auto value : CodecStatus::streams()) {
          auto entry = value.toObject();
          if (entry.value("mode") == (hardware ? "hardware" : "software"))
            directions.insert(entry.value("direction").toString());
        }
        return directions.contains("encode") && directions.contains("decode");
      };
      QTRY_VERIFY_WITH_TIMEOUT(matched(), 5000);
      qInfo().noquote() << CodecStatus::text();
      if (hardware) {
        for (int height : {720, 1080}) {
          sourceHeight = height;
          sourceWidth = height * 16 / 9;
          rtc.setQuality(
              {{"mode", "media"}, {"fps", 60}, {"resolution", height}});
          source.start(16);
          frames.clear();
          QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 30, 10000);
          auto latest = qvariant_cast<QImage>(frames.last()[1]);
          QCOMPARE(latest.width(), sourceWidth);
          QCOMPARE(latest.height(), sourceHeight);
          QVERIFY(matched());
        }
        // Force a real initialization failure at the next size change.
        // Hardware decoding should continue while encoding falls back.
        // Encoding tries NVENC before VA-API, so both must fail here to
        // reach software — otherwise a working NVIDIA GPU papers over the
        // VA-API device failure this test is inducing.
        auto previous = qgetenv("COMPARTILHAGRAM_VAAPI_DEVICE");
        bool wasSet = qEnvironmentVariableIsSet("COMPARTILHAGRAM_VAAPI_DEVICE");
        auto previousNvenc = qgetenv("COMPARTILHAGRAM_NVENC_DEVICE");
        bool nvencWasSet = qEnvironmentVariableIsSet("COMPARTILHAGRAM_NVENC_DEVICE");
        auto restore = qScopeGuard([&] {
          if (wasSet)
            qputenv("COMPARTILHAGRAM_VAAPI_DEVICE", previous);
          else
            qunsetenv("COMPARTILHAGRAM_VAAPI_DEVICE");
          if (nvencWasSet)
            qputenv("COMPARTILHAGRAM_NVENC_DEVICE", previousNvenc);
          else
            qunsetenv("COMPARTILHAGRAM_NVENC_DEVICE");
        });
        qputenv("COMPARTILHAGRAM_VAAPI_DEVICE",
                "/dev/dri/compartilhagram-missing-test-device");
        // CUDA silently accepts a non-numeric device string and falls back to
        // device 0, unlike VA-API's file-path check — an out-of-range index
        // is what actually fails device creation.
        qputenv("COMPARTILHAGRAM_NVENC_DEVICE", "99");
        sourceWidth = 640;
        sourceHeight = 360;
        frames.clear();
        QTRY_VERIFY_WITH_TIMEOUT(frames.size() >= 10, 5000);
        auto fallback = [&] {
          for (auto value : CodecStatus::streams()) {
            auto s = value.toObject();
            if (s.value("direction") == "encode" &&
                s.value("mode") == "software" &&
                s.value("reason").toString().contains(
                    "compartilhagram-missing-test-device"))
              return true;
          }
          return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(fallback(), 5000);
        qInfo().noquote() << "After GPU failure:" << CodecStatus::text();
      }
    }
    source.stop();
    audio.stop();
    stats.stop();
    rtc.closePeers();
    rtc.stopCapture();
    const auto count = frames.size();
    QTest::qWait(100);
    QCOMPARE(frames.size(), count);
  }
};
QTEST_MAIN(MediaTest)
#include "MediaTest.moc"
