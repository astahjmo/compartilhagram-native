#include "PortalCapture.h"
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QSignalSpy>
#include <QtTest>

class FakePortal : public QDBusVirtualObject {
  Q_OBJECT
public:
  uint available = 3, selected = 0;
  int starts = 0, sessionsClosed = 0, requestsClosed = 0;
  bool holdSelection = false;
  QString introspect(const QString &) const override { return {}; }
  bool handleMessage(const QDBusMessage &msg,
                     const QDBusConnection &bus) override {
    if (msg.member() == "Get") {
      bus.send(msg.createReply({QVariant::fromValue(QDBusVariant(available))}));
      return true;
    }
    if (msg.member() == "Close") {
      if (msg.interface() == "org.freedesktop.portal.Session")
        ++sessionsClosed;
      else
        ++requestsClosed;
      bus.send(msg.createReply());
      return true;
    }
    const auto options = qdbus_cast<QVariantMap>(msg.arguments().last());
    QString sender = msg.service().mid(1);
    sender.replace('.', '_');
    QString request = "/org/freedesktop/portal/desktop/request/" + sender +
                      '/' + options.value("handle_token").toString();
    QVariantMap results;
    uint response = 0;
    if (msg.member() == "CreateSession")
      results["session_handle"] =
          "/org/freedesktop/portal/desktop/session/" + sender + '/' +
          options.value("session_handle_token").toString();
    else if (msg.member() == "SelectSources") {
      selected = options.value("types").toUInt();
      if (options.value("multiple").toBool())
        return false;
    } else if (msg.member() == "Start") {
      ++starts;
      response = 1;
    } else
      return false;
    bus.send(msg.createReply({QVariant::fromValue(QDBusObjectPath(request))}));
    if (!(holdSelection && msg.member() == "SelectSources"))
      QTimer::singleShot(10, this, [bus, request, response, results] {
        auto signal = QDBusMessage::createSignal(
            request, "org.freedesktop.portal.Request", "Response");
        signal.setArguments({response, results});
        bus.send(signal);
      });
    return true;
  }
};
class PortalTest : public QObject {
  Q_OBJECT
  FakePortal portal;
private slots:
  void initTestCase() {
    auto bus = QDBusConnection::sessionBus();
    QVERIFY2(bus.registerService("org.freedesktop.portal.Desktop"),
             "Run this test using dbus-run-session (private bus)");
    QVERIFY(bus.registerVirtualObject("/org/freedesktop/portal/desktop",
                                      &portal, QDBusConnection::SubPath));
  }
  void screenAndWindowCancellation() {
    for (bool window : {false, true}) {
      PortalCapture capture;
      QSignalSpy errors(&capture, &PortalCapture::failed),
          frames(&capture, &PortalCapture::frame);
      auto closed = portal.sessionsClosed;
      capture.start(window, 30);
      QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
      QVERIFY(errors.first().first().toString().contains("cancelada"));
      QCOMPARE(portal.selected, window ? 2u : 1u);
      QTRY_COMPARE(portal.sessionsClosed, closed + 1);
      QVERIFY(frames.isEmpty());
    }
  }
  void unavailableWindow() {
    portal.available = 1;
    auto starts = portal.starts;
    PortalCapture capture;
    QSignalSpy errors(&capture, &PortalCapture::failed);
    capture.start(true, 30);
    QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
    QVERIFY(errors.first().first().toString().contains("não oferece"));
    QCOMPARE(portal.starts, starts);
    portal.available = 3;
  }
  void stopPendingRequest() {
    portal.holdSelection = true;
    portal.selected = 0;
    PortalCapture capture;
    QSignalSpy errors(&capture, &PortalCapture::failed);
    auto closed = portal.requestsClosed;
    auto starts = portal.starts;
    capture.start(true, 30);
    QTRY_COMPARE_WITH_TIMEOUT(portal.selected, 2u, 3000);
    capture.stop();
    QTRY_COMPARE(portal.requestsClosed, closed + 1);
    QTest::qWait(50);
    QVERIFY(errors.isEmpty());
    QCOMPARE(portal.starts, starts);
  }
};
QTEST_GUILESS_MAIN(PortalTest)
#include "PortalTest.moc"
