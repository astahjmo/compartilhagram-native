#include "SystemAudio.h"
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>

class TestAudioSource : public libwebrtc::RTCAudioSource {
public:
  void CaptureFrame(const void *, int bits, int rate, size_t channels,
                    size_t frames) override {
    Q_ASSERT(bits == 16 && rate == 48000 && channels == 2 && frames == 480);
  }
  SourceType GetSourceType() const override { return kCustom; }
};
class AudioTest : public QObject {
  Q_OBJECT
private slots:
  void selectionPolicy() {
    AudioSelection s;
    QVERIFY(!s.accepts("browser"));
    s.mode = AudioSelection::All;
    QVERIFY(s.accepts("browser"));
    QVERIFY(!s.accepts("self", true));
    s.mode = AudioSelection::Include;
    s.applications = {"browser"};
    QVERIFY(s.accepts("browser"));
    QVERIFY(!s.accepts("chat"));
    s.mode = AudioSelection::Exclude;
    QVERIFY(!s.accepts("browser"));
    QVERIFY(s.accepts("chat"));
    QVERIFY(!s.accepts("self", true));
    s.mode = AudioSelection::Include;
    s.applications.clear();
    QVERIFY(!s.accepts("browser"));
  }
  void mixing() {
    QByteArray positive(1920, '\0'), negative(1920, '\0');
    for (int i = 0; i < 960; ++i) {
      qToLittleEndian<int16_t>(24000, positive.data() + i * 2);
      qToLittleEndian<int16_t>(-24000, negative.data() + i * 2);
    }
    QCOMPARE(SystemAudio::mixPcm({positive, negative}), QByteArray(1920, '\0'));
    auto clipped = SystemAudio::mixPcm({positive, positive});
    QCOMPARE(qFromLittleEndian<int16_t>(clipped.constData()), 32767);
    clipped = SystemAudio::mixPcm({negative, negative});
    QCOMPARE(qFromLittleEndian<int16_t>(clipped.constData()), -32768);
    QCOMPARE(SystemAudio::mixPcm({}), QByteArray(1920, '\0'));
  }
  void isolatedApplicationCapture() {
    for (const auto &tool :
         {"pipewire", "pipewire-pulse", "wireplumber", "paplay", "pactl"})
      if (QStandardPaths::findExecutable(tool).isEmpty())
        QSKIP("Requires PipeWire and PulseAudio command-line tools");
    QTemporaryDir runtime;
    QVERIFY(runtime.isValid());
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("XDG_RUNTIME_DIR", runtime.path());
    env.insert("PIPEWIRE_RUNTIME_DIR", runtime.path());
    env.insert("XDG_STATE_HOME", runtime.path());
    env.insert("XDG_CONFIG_HOME", runtime.path());
    const auto address = "unix:" + runtime.path() + "/pulse/native";
    env.insert("PULSE_SERVER", address);
    struct Daemon : QProcess {
      ~Daemon() {
        if (state() != NotRunning) {
          terminate();
          if (!waitForFinished(2000)) {
            kill();
            waitForFinished();
          }
        }
      }
    };
    Daemon pipewire, pulse, policy, first, second;
    for (auto p : {&pipewire, &pulse, &policy, &first, &second})
      p->setProcessEnvironment(env);
    pipewire.start("pipewire");
    QVERIFY(pipewire.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(runtime.path() + "/pipewire-0"),
                             5000);
    // Policy-only profile links virtual streams, without discovering hardware.
    policy.start("wireplumber", {"--profile=policy"});
    QVERIFY(policy.waitForStarted());
    pulse.start("pipewire-pulse");
    QVERIFY(pulse.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(runtime.path() + "/pulse/native"),
                             5000);
    QProcess command;
    command.setProcessEnvironment(env);
    command.start("pactl", {"load-module", "module-null-sink",
                            "sink_name=compartilhagram_test"});
    QVERIFY(command.waitForFinished());
    QCOMPARE(command.exitCode(), 0);
    auto tone = [&](QString name, int frequency) {
      QString path = runtime.path() + '/' + name + ".raw";
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly))
        return QString();
      QByteArray pcm(48000 * 4 * 15, '\0');
      for (int i = 0; i < 48000 * 15; ++i) {
        int16_t sample = int16_t(
            6000 * std::sin(2 * 3.141592653589793 * frequency * i / 48000.));
        qToLittleEndian(sample, pcm.data() + i * 4);
        qToLittleEndian(sample, pcm.data() + i * 4 + 2);
      }
      file.write(pcm);
      return path;
    };
    first.start("paplay",
                {"--raw", "--format=s16le", "--rate=48000", "--channels=2",
                 "--device=compartilhagram_test",
                 "--property=application.id=test.first", tone("first", 500)});
    second.start("paplay", {"--raw", "--format=s16le", "--rate=48000",
                            "--channels=2", "--device=compartilhagram_test",
                            "--property=application.id=test.second",
                            tone("second", 1500)});
    QVERIFY(first.waitForStarted());
    QVERIFY(second.waitForStarted());
    const auto previous = qgetenv("PULSE_SERVER");
    const bool wasSet = qEnvironmentVariableIsSet("PULSE_SERVER");
    struct Restore {
      QByteArray value;
      bool set;
      ~Restore() {
        if (set)
          qputenv("PULSE_SERVER", value);
        else
          qunsetenv("PULSE_SERVER");
      }
    } restore{previous, wasSet};
    qputenv("PULSE_SERVER", address.toUtf8());
    SystemAudio capture;
    QSignalSpy errors(&capture, &SystemAudio::failed),
        listings(&capture, &SystemAudio::applicationsChanged);
    capture.discover();
    auto hasBoth = [&] {
      if (listings.isEmpty())
        return false;
      QSet<QString> keys;
      for (auto app : qvariant_cast<QJsonArray>(listings.last().first()))
        keys.insert(app.toObject().value("key").toString());
      return keys.contains("test.first") && keys.contains("test.second");
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasBoth(), 5000);
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source =
        new libwebrtc::RefCountedObject<TestAudioSource>();
    QByteArray recording;
    connect(&capture, &SystemAudio::samples, this,
            [&](QByteArray pcm) { recording.append(pcm); });
    auto amplitude = [&](int frequency) {
      double energy = 0;
      int blocks = recording.size() / 1920;
      // Per-block DFT tolerates asynchronous stream-start phase and short gaps.
      for (int b = 0; b < blocks; ++b) {
        double re = 0, im = 0;
        for (int i = 0; i < 480; ++i) {
          double value = qFromLittleEndian<int16_t>(recording.constData() +
                                                    b * 1920 + i * 4);
          double angle = 2 * 3.141592653589793 * frequency * i / 48000.;
          re += value * std::cos(angle);
          im += value * std::sin(angle);
        }
        energy += std::hypot(re, im) / 240;
      }
      return blocks ? energy / blocks : 0.;
    };
    for (auto mode : {AudioSelection::Include, AudioSelection::Exclude,
                      AudioSelection::All}) {
      capture.start(
          source,
          {mode,
           {mode == AudioSelection::Include ? "test.first" : "test.second"}});
      QTest::qWait(400);
      recording.clear();
      QTest::qWait(1000);
      QVERIFY2(errors.isEmpty(), "Per-application capture failed");
      qInfo() << "Mode" << mode << "tone amplitudes:" << amplitude(500)
              << amplitude(1500);
      QVERIFY2(amplitude(500) > 1000, "Selected application audio missing");
      if (mode == AudioSelection::All)
        QVERIFY2(amplitude(1500) > 1000,
                 "Audio mixer missed second application");
      else
        QVERIFY2(amplitude(1500) < 100,
                 "Excluded application leaked into recording");
    }
    capture.start(source, {AudioSelection::None, {}});
    recording.clear();
    QTest::qWait(100);
    QVERIFY(recording.isEmpty());
    capture.stop();
  }
};
QTEST_GUILESS_MAIN(AudioTest)
#include "AudioTest.moc"
