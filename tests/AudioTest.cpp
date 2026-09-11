#include "SystemAudio.h"
#include "AudioProcessTree.h"
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
  void windowsProcessTrees() {
    using namespace AudioProcessTree;
    Processes tree{{1, {0, "shell", 1}}, {2, {1, "browser", 2}},
                   {3, {2, "browser", 3}}, {4, {2, "helper", 4}},
                   {5, {1, "chat", 5}}, {6, {1, "self", 6}},
                   {7, {6, "self", 7}}};
    // Multiple browser processes and an embedded helper are a single choice.
    QCOMPARE(roots(tree, {2, 3, 4, 5, 6, 7}, 6), (QSet<quint32>{2, 5}));
    // The silent browser parent is still the capture root for its audio child.
    QCOMPARE(roots(tree, {3, 5}, 6), (QSet<quint32>{2, 5}));
    // A shell containing this app cannot capture its own received audio.
    QVERIFY(!roots(tree, {1, 5, 6}, 6).contains(1));
    // Preserve an earlier exclusion if a child becomes grouped under a parent.
    QVERIFY(containsExcludedApplication(tree, 2, {"helper"}));
    QVERIFY(!containsExcludedApplication(tree, 5, {"helper"}));
    // Parent PID 2 was reused after child 3 started: do not capture its tree.
    tree[2].created = 10;
    QCOMPARE(AudioProcessTree::parent(tree, 3), 0u);
    QCOMPARE(roots(tree, {3, 5}, 6), (QSet<quint32>{3, 5}));
    // Unknown/inaccessible processes and cycles fail closed.
    QCOMPARE(roots(tree, {99}, 6), QSet<quint32>{});
    tree[2].parent = 3;
    QVERIFY(!contains(tree, 2, 3));
  }
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
#ifdef Q_OS_WIN
  void windowsApplicationCapture() {
    if (qEnvironmentVariable("COMPARTILHAGRAM_TEST_WINDOWS_AUDIO") != "1")
      QSKIP("Set COMPARTILHAGRAM_TEST_WINDOWS_AUDIO=1 on Windows 11 with an audio output device");
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto fixture = QCoreApplication::applicationDirPath() + "/windows_audio_tone.exe";
    const auto firstPath = directory.path() + "/first.exe";
    const auto secondPath = directory.path() + "/second.exe";
    QVERIFY(QFile::copy(fixture, firstPath));
    QVERIFY(QFile::copy(fixture, secondPath));
    struct Player : QProcess {
      ~Player() { if (state() != NotRunning) { kill(); waitForFinished(); } }
    } first, second;
    first.start(firstPath, {"500"});
    second.start(secondPath, {"1500"});
    QVERIFY(first.waitForStarted());
    QVERIFY(second.waitForStarted());
    QByteArray firstOutput, secondOutput;
    QTRY_VERIFY_WITH_TIMEOUT((firstOutput += first.readAllStandardOutput()).contains("READY"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT((secondOutput += second.readAllStandardOutput()).contains("READY"), 5000);
    SystemAudio capture;
    QSignalSpy errors(&capture, &SystemAudio::failed),
        listings(&capture, &SystemAudio::applicationsChanged);
    capture.discover();
    QString firstKey, secondKey;
    auto discovered = [&] {
      if (listings.isEmpty()) return false;
      for (auto app : qvariant_cast<QJsonArray>(listings.last().first())) {
        auto key = app.toObject().value("key").toString();
        if (key.endsWith("first.exe")) firstKey = key;
        if (key.endsWith("second.exe")) secondKey = key;
      }
      return !firstKey.isEmpty() && !secondKey.isEmpty();
    };
    QTRY_VERIFY_WITH_TIMEOUT(discovered(), 5000);
    libwebrtc::scoped_refptr<libwebrtc::RTCAudioSource> source =
        new libwebrtc::RefCountedObject<TestAudioSource>();
    QByteArray recording;
    connect(&capture, &SystemAudio::samples, this,
            [&](QByteArray pcm) { recording.append(pcm); });
    auto amplitude = [&](int frequency) {
      double energy = 0;
      const auto blocks = recording.size() / 1920;
      for (qsizetype block = 0; block < blocks; ++block) {
        double re = 0, im = 0;
        for (int i = 0; i < 480; ++i) {
          const double value = qFromLittleEndian<int16_t>(recording.constData() + block * 1920 + i * 4);
          const double angle = 2 * 3.141592653589793 * frequency * i / 48000;
          re += value * std::cos(angle);
          im += value * std::sin(angle);
        }
        energy += std::hypot(re, im) / 240;
      }
      return blocks ? energy / blocks : 0.;
    };
    for (auto mode : {AudioSelection::Include, AudioSelection::Exclude, AudioSelection::All}) {
      capture.start(source, {mode, {mode == AudioSelection::Include ? firstKey : secondKey}});
      QTest::qWait(600);
      recording.clear();
      QTest::qWait(1000);
      QVERIFY2(errors.isEmpty(), "Windows process loopback capture failed");
      QVERIFY2(amplitude(500) > 1000, "Selected process audio missing");
      if (mode == AudioSelection::All)
        QVERIFY2(amplitude(1500) > 1000, "Second process audio missing");
      else
        QVERIFY2(amplitude(1500) < 100, "Excluded process leaked into capture");
    }
    capture.start(source, {AudioSelection::None, {}});
    recording.clear();
    QTest::qWait(100);
    QVERIFY(recording.isEmpty());
  }
#endif
  void isolatedApplicationCapture() {
#ifdef Q_OS_WIN
    QSKIP("Linux PipeWire integration test; run windowsApplicationCapture on Windows");
#else
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
#endif
  }
};
QTEST_GUILESS_MAIN(AudioTest)
#include "AudioTest.moc"
