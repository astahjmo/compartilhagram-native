#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <functional>

// Engine.IO 4 polling + Socket.IO default namespace. Media never uses polling.
class ServerClient : public QObject {
  Q_OBJECT
public:
  using Result = std::function<void(QJsonObject, QString)>;
  explicit ServerClient(QObject *parent = nullptr);
  static QByteArray sessionCookie(const QString &input,
                                  QString *error = nullptr);
  void login(const QString &session);
  void logout();
  void disconnectGracefully();
  void send(const QString &event, QJsonValue payload = QJsonValue::Undefined);
  void api(const QString &path, const QJsonObject &body, Result done,
           bool post = false);
  bool connected() const { return online_; }
  QUrl origin() const { return origin_; }
  // Explicit override for local protocol tests; production defaults to HTTPS.
  void setOriginForTesting(QUrl origin) { origin_ = std::move(origin); }
signals:
  void authenticated(QJsonObject user);
  void ready();
  void event(QString name, QJsonValue payload);
  void disconnected(QString reason);
  void loginFailed(QString reason);
  void closed();

private:
  QNetworkRequest request(const QUrl &url) const;
  QUrl socketUrl() const;
  void open();
  void poll();
  void consume(const QByteArray &packet);
  void enqueue(QByteArray packet);
  void flush();
  void fail(const QString &reason, bool retry = true);
  void reset();
  QNetworkAccessManager http_;
  QUrl origin_{QStringLiteral("https://games.butecodosdevs.com")};
  QByteArray cookie_;
  QString sid_;
  QQueue<QByteArray> outgoing_;
  QPointer<QNetworkReply> poll_, post_;
  QTimer heartbeat_, reconnect_;
  int generation_ = 0, heartbeatMs_ = 45000, retries_ = 0,
      maxPayload_ = 1000000;
  bool online_ = false, enabled_ = false, closing_ = false;
};
