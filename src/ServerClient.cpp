#include "ServerClient.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkCookieJar>
#include <QRegularExpression>
#include <QUrlQuery>
#include <algorithm>

ServerClient::ServerClient(QObject *parent) : QObject(parent) {
  heartbeat_.setSingleShot(true);
  reconnect_.setSingleShot(true);
  connect(&heartbeat_, &QTimer::timeout, this,
          [this] { fail(tr("Servidor sem resposta")); });
  connect(&reconnect_, &QTimer::timeout, this, &ServerClient::open);
}

QByteArray ServerClient::sessionCookie(const QString &input, QString *error) {
  // Accept a raw Better Auth value or one named cookie, never an arbitrary
  // header.
  QString value = input.trimmed();
  QString name = "__Secure-better-auth.session_token";
  const QStringList names{name, "better-auth.session_token"};
  for (const auto &candidate : names) {
    if (value.startsWith(candidate + '=')) {
      name = candidate;
      value = value.mid(candidate.size() + 1);
      break;
    }
  }
  static const QRegularExpression valid("^[A-Za-z0-9._%+/=-]{16,4096}$");
  if (!valid.match(value).hasMatch()) {
    if (error)
      *error = tr("Cole somente o valor da sessão Better Auth ou nome=valor, "
                  "sem outros cookies.");
    return {};
  }
  return name.toUtf8() + '=' +
         value.toUtf8(); // Preserve %3D; never double encode.
}

QNetworkRequest ServerClient::request(const QUrl &url) const {
  QNetworkRequest req(url);
  req.setRawHeader("Cookie", cookie_);
  req.setRawHeader("Origin", origin_.toEncoded());
  // socket/middleware.ts requires the Mozilla/ compatibility token even for
  // authenticated clients. Keep our native identity explicit; no browser is
  // embedded.
  req.setRawHeader(
      "User-Agent",
      "Mozilla/5.0 (compatible; Compartilhagram-Native/0.1)");
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::ManualRedirectPolicy);
  req.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                   QNetworkRequest::Manual);
  req.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                   QNetworkRequest::Manual);
  req.setTransferTimeout(60000);
  return req;
}

void ServerClient::api(const QString &path, const QJsonObject &body,
                       Result done, bool post) {
  const int generation = generation_;
  auto req = request(origin_.resolved(QUrl(path)));
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  auto reply =
      post ? http_.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact))
           : http_.get(req);
  connect(
      reply, &QNetworkReply::finished, this,
      [this, reply, generation, done = std::move(done)] {
        const auto data = reply->readAll();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto obj = QJsonDocument::fromJson(data).object();
        QString error;
        if (reply->error() != QNetworkReply::NoError || status < 200 ||
            status >= 300)
          error = obj.value("error").toString(
              tr("Falha de rede / HTTP %1").arg(status));
        else if (status != 204 && !QJsonDocument::fromJson(data).isObject())
          error = tr("Resposta inválida do servidor");
        reply->deleteLater();
        if (generation == generation_)
          done(obj, error);
      });
}

void ServerClient::login(const QString &session) {
  logout();
  retries_ = 0;
  QString error;
  cookie_ = sessionCookie(session, &error);
  if (cookie_.isEmpty()) {
    emit loginFailed(error);
    return;
  }
  api("/api/auth/get-session", {}, [this](QJsonObject data, QString error) {
    if (!error.isEmpty() ||
        data.value("user").toObject().value("id").toString().isEmpty()) {
      cookie_.fill('\0');
      cookie_.clear();
      emit loginFailed(error.isEmpty() ? tr("Sessão inválida ou expirada")
                                       : error);
      return;
    }
    enabled_ = true;
    emit authenticated(data.value("user").toObject());
    open();
  });
}

void ServerClient::reset() {
  ++generation_;
  online_ = false;
  closing_ = false;
  heartbeat_.stop();
  if (poll_) {
    poll_->abort();
    poll_.clear();
  }
  if (post_) {
    post_->abort();
    post_.clear();
  }
  outgoing_.clear();
  sid_.clear();
}
void ServerClient::logout() {
  enabled_ = false;
  reconnect_.stop();
  reset();
  cookie_.fill('\0');
  cookie_.clear();
}
void ServerClient::disconnectGracefully() {
  reconnect_.stop();
  heartbeat_.stop();
  enabled_ = false;
  if (sid_.isEmpty()) {
    emit closed();
    return;
  }
  closing_ = true;
  online_ = false;
  // Serialized behind any stop/leave POST; close both protocol layers.
  outgoing_.enqueue("41");
  outgoing_.enqueue("1");
  flush();
}
QUrl ServerClient::socketUrl() const {
  QUrl url = origin_.resolved(QUrl("/socket.io/"));
  QUrlQuery query;
  query.addQueryItem("EIO", "4");
  query.addQueryItem("transport", "polling");
  if (!sid_.isEmpty())
    query.addQueryItem("sid", sid_);
  url.setQuery(query);
  return url;
}
void ServerClient::open() {
  if (!enabled_)
    return;
  reset();
  heartbeat_.start(15000);
  poll();
}
void ServerClient::poll() {
  if (!enabled_ || poll_)
    return;
  const int generation = generation_;
  auto reply = http_.get(request(socketUrl()));
  poll_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
    const auto body = reply->readAll();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
    reply->deleteLater();
    if (generation != generation_)
      return;
    poll_.clear();
    if (!ok) {
      if (status == 401 || status == 403)
        fail(tr("Acesso ao lobby recusado (HTTP %1). Use Trocar sessão para "
                "tentar novamente.")
                 .arg(status),
             false);
      else
        fail(tr("Falha na conexão ao lobby (HTTP %1).").arg(status));
      return;
    }
    for (const auto &packet : body.split('\x1e')) {
      consume(packet);
      if (generation != generation_)
        return;
    }
    QTimer::singleShot(0, this, [this, generation] {
      if (generation == generation_)
        poll();
    });
  });
}
void ServerClient::consume(const QByteArray &p) {
  if (p.startsWith('0')) {
    auto obj = QJsonDocument::fromJson(p.mid(1)).object();
    sid_ = obj.value("sid").toString();
    if (sid_.isEmpty()) {
      fail(tr("Handshake inválido"));
      return;
    }
    heartbeatMs_ = std::clamp(obj.value("pingInterval").toInt(25000) +
                                  obj.value("pingTimeout").toInt(20000),
                              1000, 120000);
    maxPayload_ =
        std::clamp(obj.value("maxPayload").toInt(1000000), 1024, 1000000);
    heartbeat_.start(heartbeatMs_);
    enqueue("40");
  } else if (p.startsWith('2')) {
    heartbeat_.start(heartbeatMs_);
    enqueue("3" + p.mid(1));
  } else if (p.startsWith("40")) {
    online_ = true;
    retries_ = 0;
    emit ready();
  } else if (p.startsWith("42")) {
    const auto a = QJsonDocument::fromJson(p.mid(2)).array();
    if (!a.isEmpty() && a[0].isString())
      emit event(a[0].toString(), a.size() > 1 ? a[1] : QJsonValue());
  } else if (p.startsWith("44")) {
    const QString reason =
        QJsonDocument::fromJson(p.mid(2)).object().value("message").toString(
            tr("Sessão recusada"));
    fail(tr("Conexão ao lobby recusada: %1. Use Trocar sessão para tentar "
            "novamente.")
             .arg(reason),
         false);
  } else if (p == "1" || p.startsWith("41")) {
    fail(tr("Servidor encerrou a sessão"));
  }
}
void ServerClient::send(const QString &event, QJsonValue payload) {
  if (!online_)
    return;
  QJsonArray data{event};
  if (!payload.isUndefined())
    data.append(payload);
  enqueue("42" + QJsonDocument(data).toJson(QJsonDocument::Compact));
}
void ServerClient::enqueue(QByteArray packet) {
  if (packet.size() > maxPayload_ || outgoing_.size() > 1024) {
    fail(tr("Fila de sinalização excedida"));
    return;
  }
  outgoing_.enqueue(std::move(packet));
  flush();
}
void ServerClient::flush() {
  if (post_ || outgoing_.isEmpty() || sid_.isEmpty())
    return;
  QByteArray body = outgoing_.dequeue();
  while (!outgoing_.isEmpty() &&
         body.size() + outgoing_.head().size() + 1 <= maxPayload_)
    body += '\x1e' + outgoing_.dequeue();
  const int generation = generation_;
  auto req = request(socketUrl());
  req.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain;charset=UTF-8");
  auto reply = http_.post(req, body);
  post_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200 &&
                    reply->readAll() == "ok";
    reply->deleteLater();
    if (generation != generation_)
      return;
    post_.clear();
    if (closing_ && outgoing_.isEmpty()) {
      reset();
      closing_ = false;
      emit closed();
      return;
    }
    if (!ok) {
      fail(tr("Falha ao enviar sinalização (HTTP %1).").arg(status),
           status != 401 && status != 403);
      return;
    }
    flush();
  });
}
void ServerClient::fail(const QString &reason, bool retry) {
  const bool willRetry = enabled_ && retry && retries_ < 5;
  const QString message =
      willRetry ? reason + tr(" Tentando novamente…")
                : reason + (retry ? tr(" Reconexão interrompida. Use Trocar "
                                       "sessão para tentar novamente.")
                                  : QString());
  reset();
  emit disconnected(message);
  if (willRetry)
    reconnect_.start(std::min(30000, 1000 * (1 << std::min(retries_++, 5))));
  else {
    enabled_ = false;
    reconnect_.stop();
    emit loginFailed(message);
  }
}
