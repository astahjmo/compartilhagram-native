#include "ServerClient.h"
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class ProtocolTest : public QObject {
  Q_OBJECT
private slots:
  void cookies() {
    QCOMPARE(ServerClient::sessionCookie("abcdefgh12345678.sig%3D"),
             QByteArray(
                 "__Secure-better-auth.session_token=abcdefgh12345678.sig%3D"));
    QCOMPARE(ServerClient::sessionCookie(
                 "better-auth.session_token=abcdefgh12345678.sig="),
             QByteArray("better-auth.session_token=abcdefgh12345678.sig="));
    QVERIFY(
        ServerClient::sessionCookie("abcdefgh12345678; other=bad").isEmpty());
    QVERIFY(ServerClient::sessionCookie("abcdefgh12345678\r\nHost: evil")
                .isEmpty());
    QVERIFY(ServerClient::sessionCookie("short").isEmpty());
  }
  void pollingHandshakeHeartbeatAndEvents_data() {
    QTest::addColumn<QString>("rejection");
    QTest::newRow("accepted") << QString();
    QTest::newRow("namespace-forbidden") << QString("Forbidden");
    QTest::newRow("http-forbidden") << QString("HTTP 403");
  }
  void pollingHandshakeHeartbeatAndEvents() {
    QFETCH(QString, rejection);
    QTcpServer http;
    QVERIFY(http.listen(QHostAddress::LocalHost));
    QList<QByteArray> posts;
    QList<QByteArray> headers;
    int gets = 0;
    connect(&http, &QTcpServer::newConnection, &http, [&] {
      auto socket = http.nextPendingConnection();
      auto buffer = std::make_shared<QByteArray>();
      connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
        *buffer += socket->readAll();
        int end = buffer->indexOf("\r\n\r\n");
        if (end < 0)
          return;
        auto head = buffer->left(end);
        int length = 0;
        for (auto line : head.split('\n'))
          if (line.toLower().startsWith("content-length:"))
            length = line.mid(15).trimmed().toInt();
        if (buffer->size() < end + 4 + length)
          return;
        headers.append(head);
        QByteArray response;
        QByteArray status = "200 OK";
        if (head.startsWith("GET /api/auth/get-session"))
          response =
              R"({"user":{"id":"native-test","name":"Test"},"session":{"id":"session"}})";
        else if (head.startsWith("POST ")) {
          posts.append(buffer->mid(end + 4, length));
          response = "ok";
        } else {
          ++gets;
          if (gets == 1)
            response =
                R"(0{"sid":"test-sid","pingInterval":5000,"pingTimeout":5000,"maxPayload":1000000})";
          else if (gets == 2) {
            // Match the production middleware's Origin/User-Agent gate.
            bool hasAgent = false, hasOrigin = false;
            for (auto line : head.split('\n')) {
              if (line.toLower().startsWith("user-agent:"))
                hasAgent = line.contains("Mozilla/") &&
                           line.contains("Compartilhagram-Native/");
              if (line.toLower().startsWith("origin:"))
                hasOrigin = line.mid(7).trimmed() ==
                            QByteArray("http://127.0.0.1:") +
                                QByteArray::number(http.serverPort());
            }
            if (!hasAgent || !hasOrigin || rejection == "Forbidden")
              response = R"(44{"message":"Forbidden"})";
            else if (rejection == "HTTP 403") {
              status = "403 Forbidden";
              response = R"({"message":"Forbidden"})";
            } else
              response = "40{\"sid\":\"socket-id\"}\x1e"
                         "2";
          } else if (gets == 3)
            response =
                R"(42["screenshare:state",{"shares":[],"maxConcurrentShares":3}])";
          else
            return; // Hold the long poll open.
        }
        socket->write("HTTP/1.1 " + status +
                      "\r\nContent-Type: text/plain\r\nConnection: "
                      "close\r\nContent-Length: " +
                      QByteArray::number(response.size()) + "\r\n\r\n" +
                      response);
        socket->disconnectFromHost();
        buffer->clear();
      });
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    ServerClient client;
    client.setOriginForTesting(
        QUrl(QString("http://127.0.0.1:%1").arg(http.serverPort())));
    QSignalSpy ready(&client, &ServerClient::ready);
    QSignalSpy events(&client, &ServerClient::event);
    QSignalSpy failed(&client, &ServerClient::loginFailed);
    connect(&client, &ServerClient::ready, &client,
            [&] { client.send("screenshare:subscribe"); });
    client.login("abcdefgh12345678.sig%3D");
    if (!rejection.isEmpty()) {
      QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 5000);
      QVERIFY(failed.first()[0].toString().contains(rejection));
      QVERIFY(!client.connected());
      QCOMPARE(ready.size(), 0);
      const int requests = gets;
      QTest::qWait(1200); // The first retry would occur after 1 second.
      QCOMPARE(gets, requests);
      QCOMPARE(failed.size(), 1);
      client.logout();
      return;
    }
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(events.size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        posts.join('\x1e').contains("42[\"screenshare:subscribe\"]"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(posts.join('\x1e').split('\x1e').contains("3"),
                             5000);
    QCOMPARE(posts.first(), QByteArray("40"));
    QCOMPARE(events.first()[0].toString(), QString("screenshare:state"));
    for (const auto &head : headers)
      QVERIFY(head.toLower().contains(
          "cookie: "
          "__secure-better-auth.session_token=abcdefgh12345678.sig%3d"));
    QSignalSpy closed(&client, &ServerClient::closed);
    client.disconnectGracefully();
    QTRY_COMPARE_WITH_TIMEOUT(closed.size(), 1, 5000);
    QVERIFY(posts.join('\x1e').split('\x1e').contains("41"));
    QVERIFY(posts.join('\x1e').split('\x1e').contains("1"));
    client.logout();
    QVERIFY(!client.connected());
    QTest::qWait(30);
    QCOMPARE(ready.size(), 1);
  }
};
QTEST_GUILESS_MAIN(ProtocolTest)
#include "ProtocolTest.moc"
