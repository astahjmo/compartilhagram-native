#include "MainWindow.h"
#include "BrowserSession.h"
#include "CodecStatus.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QStatusBar>
#include <QToolBar>
#include <QUrlQuery>

// Same wire-level tracing switch as RtcEngine.cpp; also covers signaling
// events and periodic RTP stats. Enable with COMPARTILHAGRAM_RTC_DEBUG=1.
static bool rtcDebugEnabled() {
  static const bool enabled =
      qEnvironmentVariable("COMPARTILHAGRAM_RTC_DEBUG") == "1";
  return enabled;
}

static QString rosterText(const QJsonObject &share) {
  QStringList names;
  for (const auto &v : share.value("viewers").toArray()) {
    auto o = v.toObject();
    names.append(o.value("username").toString() +
                 (o.value("path") == "sfu"        ? " (SFU)"
                  : o.value("usingTurn").toBool() ? " (TURN)"
                                                  : " (direto)"));
  }
  return names.isEmpty() ? QObject::tr("Nenhum espectador") : names.join(", ");
}
static QString qualityText(const QJsonObject &s) {
  return QString("%1 · %2p · %3 fps · %4/%5 espectadores")
      .arg(s.value("mode") == "doc" ? "Documento" : "Mídia")
      .arg(s.value("resolution").toInt())
      .arg(s.value("fps").toInt())
      .arg(s.value("viewerCount").toInt())
      .arg(s.value("maxViewers").toInt());
}
VideoWidget::VideoWidget(QWidget *parent) : QWidget(parent) {
  setMinimumSize(160, 90);
}
void VideoWidget::setFrame(QImage image) {
  image_ = std::move(image);
  update();
}
void VideoWidget::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor("#111318"));
  if (image_.isNull()) {
    p.setPen(Qt::white);
    p.drawText(rect(), Qt::AlignCenter, tr("Aguardando vídeo…"));
    return;
  }
  QSize size = image_.size().scaled(this->size(), Qt::KeepAspectRatio);
  p.drawImage(QRect(QPoint((width() - size.width()) / 2,
                           (height() - size.height()) / 2),
                    size),
              image_);
}

MainWindow::MainWindow() {
  setWindowTitle("Compartilhagram");
  resize(1160, 800);
  auto codecStatus = new QLabel;
  codecStatus->setObjectName("codecStatus");
  codecStatus->setTextFormat(Qt::PlainText);
  codecStatus->setWordWrap(true);
  codecStatus->setMinimumWidth(0);
  statusBar()->addPermanentWidget(codecStatus, 1);
  auto updateCodecs = [codecStatus] {
    auto text = CodecStatus::text();
    codecStatus->setText(text);
    codecStatus->setToolTip(text);
  };
  auto codecTimer = new QTimer(this);
  connect(codecTimer, &QTimer::timeout, this, updateCodecs);
  codecTimer->start(1000);
  updateCodecs();
  auto root = new QWidget;
  auto layout = new QVBoxLayout(root);
  auto header = new QHBoxLayout;
  auto title = new QLabel("Compartilhagram");
  QFont font = title->font();
  font.setPointSize(23);
  font.setBold(true);
  title->setFont(font);
  header->addWidget(title);
  header->addStretch();
  start_ = new QPushButton(tr("Compartilhar tela"));
  start_->setEnabled(false);
  header->addWidget(start_);
  layout->addLayout(header);
  layout->addWidget(new QLabel(
      tr("é como compartilhar a tela, mas aí a tela fica on-the-line")));
  status_ = new QLabel(tr("Informe sua sessão para conectar"));
  status_->setTextFormat(Qt::PlainText);
  layout->addWidget(status_);
  broadcastPanel_ = new QGroupBox(tr("Sua transmissão"));
  auto panel = new QVBoxLayout(broadcastPanel_);
  auto info = new QHBoxLayout;
  preview_ = new VideoWidget;
  preview_->setFixedSize(320, 180);
  info->addWidget(preview_);
  auto details = new QVBoxLayout;
  ownInfo_ = new QLabel;
  ownInfo_->setTextFormat(Qt::PlainText);
  details->addWidget(ownInfo_);
  roster_ = new QLabel;
  roster_->setTextFormat(Qt::PlainText);
  roster_->setWordWrap(true);
  details->addWidget(roster_);
  stats_ = new QLabel;
  details->addWidget(stats_);
  audioMeter_ = new QProgressBar;
  audioMeter_->setRange(0, 100);
  audioMeter_->setValue(0);
  audioMeter_->setFormat(tr("Áudio enviado: %p%"));
  details->addWidget(audioMeter_);
  info->addLayout(details, 1);
  panel->addLayout(info);
  auto buttons = new QHBoxLayout;
  auto source = new QPushButton(tr("Trocar fonte"));
  auto quality = new QPushButton(tr("Qualidade / privacidade"));
  auto copy = new QPushButton(tr("Copiar link"));
  boost_ = new QPushButton(tr("Boost SFU"));
  announce_ = new QPushButton(tr("Anunciar no Discord"));
  auto stop = new QPushButton(tr("Encerrar"));
  for (auto b : {source, quality, copy, boost_, announce_, stop})
    buttons->addWidget(b);
  panel->addLayout(buttons);
  layout->addWidget(broadcastPanel_);
  broadcastPanel_->hide();
  auto scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  auto cards = new QWidget;
  grid_ = new QGridLayout(cards);
  grid_->setAlignment(Qt::AlignTop);
  scroll->setWidget(cards);
  layout->addWidget(scroll, 1);
  empty_ = new QLabel(tr("Carregando…"));
  empty_->setAlignment(Qt::AlignCenter);
  grid_->addWidget(empty_, 0, 0);
  setCentralWidget(root);
  auto bar = addToolBar(tr("Sessão"));
  bar->setMovable(false);
  auto link = bar->addAction(tr("Abrir link / convite"));
  auto session = bar->addAction(tr("Trocar sessão"));
  connect(link, &QAction::triggered, this, &MainWindow::openLink);
  connect(session, &QAction::triggered, this,
          [this] { promptSession(false); });
  connect(start_, &QPushButton::clicked, this, [this] { configure(); });
  connect(stop, &QPushButton::clicked, this, &MainWindow::stopBroadcast);
  connect(source, &QPushButton::clicked, this, [this] { chooseSource(false); });
  connect(quality, &QPushButton::clicked, this, [this] { configure(true); });
  connect(copy, &QPushButton::clicked, this, [this] {
    QApplication::clipboard()->setText(
        server_.origin().toString() +
        "/apps/compartilhagram?share=" + own_.value("id").toString());
  });
  connect(boost_, &QPushButton::clicked, this,
          [this] { server_.send("screenshare:boost"); });
  connect(announce_, &QPushButton::clicked, this, &MainWindow::announce);

  lobbyTimer_.setSingleShot(true);
  lobbyTimer_.setInterval(15000);
  connect(&lobbyTimer_, &QTimer::timeout, this, [this] {
    if (!lobby_.isEmpty())
      return;
    lobbyError_ = tr("O servidor não respondeu à inscrição no lobby. Use "
                     "Trocar sessão para reconectar.");
    renderLobby();
    status_->setText(lobbyError_);
  });
  connect(&server_, &ServerClient::authenticated, this,
          [this](QJsonObject user) {
            user_ = user;
            lobbyError_.clear();
            renderLobby();
            status_->setText(
                tr("Conectando como %1…").arg(user.value("name").toString()));
          });
  connect(&server_, &ServerClient::ready, this, [this] {
    lobbyError_.clear();
    lobbyTimer_.start();
    renderLobby();
    status_->setText(
        tr("Conectado como %1").arg(user_.value("name").toString()));
    server_.send("screenshare:subscribe");
    server_.api("/api/rtc/ice?purpose=screenshare", {},
                [this](QJsonObject data, QString error) {
                  if (!error.isEmpty()) {
                    message(error);
                    return;
                  }
                  rtc_.setIce(data.value("iceServers").toArray());
                  setProperty("iceReady", true);
                  renderLobby();
                });
    server_.api("/api/settings", {}, [this](QJsonObject data, QString) {
      thumbnailTimer_.setInterval(std::max(1, data.value("screenshare")
                                                  .toObject()
                                                  .value("thumbnailIntervalSec")
                                                  .toInt(10)) *
                                  1000);
    });
  });
  connect(&server_, &ServerClient::disconnected, this, [this](QString reason) {
    lobbyTimer_.stop();
    lobbyError_ = reason;
    clearMedia();
    setProperty("iceReady", false);
    lobby_ = {};
    renderLobby();
    status_->setText(reason);
  });
  connect(&server_, &ServerClient::loginFailed, this, [this](QString reason) {
    lobbyTimer_.stop();
    lobbyError_ = reason;
    renderLobby();
    status_->setText(reason);
  });
  connect(&server_, &ServerClient::event, this, &MainWindow::onEvent);
  connect(&rtc_, &RtcEngine::signal, this,
          [this](QString peer, QJsonObject data) {
            server_.send("rtc:signal", QJsonObject{{"channel", "screenshare"},
                                                   {"to", peer},
                                                   {"data", data}});
          });
  connect(&rtc_, &RtcEngine::localFrame, this, [this](QImage image) {
    preview_->setFrame(image);
    if (waitingCapture_) {
      waitingCapture_ = false;
      server_.send("screenshare:start", config_);
    }
  });
  connect(&rtc_, &RtcEngine::captureFailed, this, [this](QString reason) {
    stopBroadcast();
    message(reason);
  });
  connect(&rtc_, &RtcEngine::sourceSelectionFailed, this,
          [this](QString reason) {
            if (pendingStart_)
              stopBroadcast();
            message(reason);
          });
  connect(&rtc_, &RtcEngine::audioLevel, this,
          [this](double level) { audioMeter_->setValue(qRound(level * 100)); });
  connect(&rtc_, &RtcEngine::frame, this, [this](QString peer, QImage image) {
    if (peer == subscribePeer_) {
      sfuFrame_ = image;
      activateSfu();
    } else if (peer == watch_.value("broadcasterId").toString() &&
               !sfuActive_ && video_)
      video_->setFrame(image);
  });
  connect(&rtc_, &RtcEngine::failure, this,
          [this](QString peer, QString reason) {
            if (peer == publishPeer_ || peer == subscribePeer_)
              failSfu(peer, reason);
            else
              message(tr("WebRTC: %1").arg(reason));
          });
  connect(&rtc_, &RtcEngine::state, this, [this](QString peer, QString state) {
    if (state == "connected") {
      retries_.remove(peer);
      if (peer == watch_.value("broadcasterId").toString() ||
          (peer == subscribePeer_ && sfuActive_))
        rtc_.volume(peer, property("muted").toBool() ? 0 : volume_);
      return;
    }
    if (state != "failed")
      return;
    if (peer == publishPeer_ || peer == subscribePeer_) {
      failSfu(peer, tr("Conexão SFU falhou"));
      return;
    }
    int attempt = ++retries_[peer];
    if (attempt > 4) {
      message(tr("Conexão falhou. Feche e reabra a transmissão."));
      return;
    }
    int epoch = epoch_;
    QTimer::singleShot(1500 * attempt, this, [this, peer, epoch] {
      if (epoch != epoch_)
        return;
      if (!broadcastPeers_.contains(peer) &&
          peer != watch_.value("broadcasterId").toString())
        return;
      rtc_.removePeer(peer);
      rtc_.connectPeer(peer, broadcastPeers_.contains(peer),
                       user_.value("id").toString() < peer);
    });
  });
  connect(&rtc_, &RtcEngine::localDescription, this,
          [this](QString peer, QString sdp, QJsonArray tracks) {
            const int epoch = epoch_;
            if (peer == publishPeer_) {
              QJsonArray names;
              for (const auto &t : tracks)
                names.append(QJsonObject{
                    {"mid", t.toObject().value("mid")},
                    {"trackName", own_.value("id").toString() + '-' +
                                      t.toObject().value("kind").toString()}});
              server_.api(
                  "/api/compartilhagram/sfu/publish",
                  {{"sdp", sdp}, {"tracks", names}},
                  [this, peer, epoch](QJsonObject data, QString error) {
                    if (epoch != epoch_ || peer != publishPeer_)
                      return;
                    if (!error.isEmpty()) {
                      failSfu(peer, error);
                      return;
                    }
                    rtc_.signalPeer(
                        peer, {{"type", "answer"}, {"sdp", data.value("sdp")}});
                  },
                  true);
            } else if (peer == subscribePeer_) {
              server_.api(
                  "/api/compartilhagram/sfu/renegotiate",
                  {{"sessionId", subscribeSession_}, {"sdp", sdp}},
                  [this, peer, epoch](QJsonObject, QString error) {
                    if (epoch != epoch_ || peer != subscribePeer_)
                      return;
                    if (!error.isEmpty()) {
                      failSfu(peer, error);
                      return;
                    }
                    sfuReady_ = true;
                    activateSfu();
                  },
                  true);
            }
          });
  connect(&rtc_, &RtcEngine::remoteDescriptionSet, this, [this](QString peer) {
    if (peer == publishPeer_) {
      setProperty("sfuPublished", true);
      server_.send("screenshare:published");
    }
  });
  connect(&rtc_, &RtcEngine::statistics, this, &MainWindow::reportStats);
  thumbnailTimer_.setInterval(10000);
  connect(&thumbnailTimer_, &QTimer::timeout, this, [this] {
    if (own_.isEmpty() || own_.value("hideThumbnail").toBool())
      return;
    QImage image = rtc_.preview();
    if (image.isNull())
      return;
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    image.scaledToWidth(320, Qt::SmoothTransformation)
        .save(&buffer, "JPEG", 65);
    QByteArray data = "data:image/jpeg;base64," + jpeg.toBase64();
    if (data.size() <= 64000)
      server_.send("screenshare:thumbnail", QString::fromLatin1(data));
  });
  thumbnailTimer_.start();
  statsTimer_.setInterval(5000);
  connect(&statsTimer_, &QTimer::timeout, this, [this] {
    if (!watch_.isEmpty())
      rtc_.stats(sfuActive_ ? subscribePeer_
                            : watch_.value("broadcasterId").toString());
    for (const auto &peer : broadcastPeers_)
      rtc_.stats(peer);
    if (!publishPeer_.isEmpty())
      rtc_.stats(publishPeer_);
    const auto remaining = own_.value("nextBoostAt").toDouble() -
                           QDateTime::currentMSecsSinceEpoch();
    boost_->setEnabled(!own_.isEmpty() &&
                       lobby_.value("sfuAvailable").toBool() && remaining <= 0);
  });
  statsTimer_.start();
}
MainWindow::~MainWindow() {
  clearMedia();
  server_.logout();
}
void MainWindow::message(const QString &text) {
  statusBar()->showMessage(text, 20000);
}
void MainWindow::promptSession(bool tryBrowser) {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Entrar no Compartilhagram"));
  auto layout = new QVBoxLayout(&dialog);
  auto status = new QLabel(tryBrowser
                               ? tr("Procurando sua sessão no navegador…")
                               : QString());
  status->setTextFormat(Qt::PlainText);
  status->setWordWrap(true);
  layout->addWidget(status);
  layout->addWidget(new QLabel(tr("Ou cole a sessão manualmente:")));
  auto input = new QLineEdit;
  input->setEchoMode(QLineEdit::Password);
  input->setPlaceholderText(tr("Valor de better-auth.session_token"));
  layout->addWidget(input);
  auto error = new QLabel;
  error->setTextFormat(Qt::PlainText);
  error->setWordWrap(true);
  layout->addWidget(error);
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  // Keeps retrying the browser-cookie lookup in the background after we've
  // opened the site for the user, so logging in there brings this dialog
  // straight through without any extra click; gives up after a while so a
  // genuinely unsupported browser still leaves the manual field usable.
  auto poll = new QTimer(&dialog);
  poll->setInterval(2000);
  bool browserOpened = false;
  int attempts = 0;
  auto attemptLogin = [&, this](QString token, QString validating) {
    poll->stop();
    clearMedia();
    user_ = {};
    lobby_ = {};
    setProperty("iceReady", false);
    renderLobby();
    error->setText(validating);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    server_.login(token);
  };
  auto tryBrowserCookie = [&, this, attemptLogin] {
    auto result = BrowserSession::readCookie(
        server_.origin().host(), "__Secure-better-auth.session_token");
    if (!result.value.isEmpty()) {
      status->setText(tr("Sessão do %1 encontrada.").arg(result.browser));
      attemptLogin(result.value,
                  tr("Validando sessão do %1…").arg(result.browser));
      return;
    }
    if (!browserOpened) {
      browserOpened = true;
      status->setText(tr(
          "Não encontramos uma sessão ativa. Vamos abrir o site para você "
          "entrar — ao concluir o login, esta janela continua sozinha."));
      QDesktopServices::openUrl(
          QUrl(server_.origin().toString() + "/apps/compartilhagram"));
      poll->start();
    } else if (++attempts >= 30) {
      poll->stop();
      status->setText(result.error.isEmpty()
                          ? tr("Ainda não encontramos sua sessão. Cole o "
                              "valor manualmente abaixo.")
                          : result.error);
    }
  };
  connect(poll, &QTimer::timeout, &dialog, tryBrowserCookie);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, this, attemptLogin] {
    if (input->text().isEmpty())
      return;
    attemptLogin(input->text(), tr("Validando sessão…"));
    input->clear();
  });
  connect(&server_, &ServerClient::loginFailed, &dialog, [=](QString reason) {
    error->setText(reason);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
  });
  connect(&server_, &ServerClient::authenticated, &dialog, &QDialog::accept);
  if (tryBrowser)
    QTimer::singleShot(0, &dialog, tryBrowserCookie);
  dialog.resize(540, 250);
  if (dialog.exec() != QDialog::Accepted && user_.isEmpty()) {
    server_.logout();
    close();
    return;
  }
  if (!user_.isEmpty())
    show();
}

void MainWindow::renderLobby() {
  while (auto item = grid_->takeAt(0)) {
    if (item->widget() != empty_)
      item->widget()->deleteLater();
    delete item;
  }
  const auto shares = lobby_.value("shares").toArray();
  bool canConnect = server_.connected() && property("iceReady").toBool() &&
                    rtc_.initialized();
  start_->setEnabled(
      canConnect && !lobby_.isEmpty() && own_.isEmpty() && !pendingStart_ &&
      shares.size() < lobby_.value("maxConcurrentShares").toInt());
  start_->setText(pendingStart_ ? tr("Iniciando…") : tr("Compartilhar tela"));
  broadcastPanel_->setVisible(!own_.isEmpty());
  ownInfo_->setText(qualityText(own_));
  roster_->setText(rosterText(own_));
  boost_->setEnabled(lobby_.value("sfuAvailable").toBool() &&
                     own_.value("nextBoostAt").toDouble() <=
                         QDateTime::currentMSecsSinceEpoch());
  announce_->setEnabled(!announced_);
  int index = 0;
  for (auto value : shares) {
    auto share = value.toObject();
    if (share.value("id") == own_.value("id"))
      continue;
    auto card = new QPushButton;
    card->setMinimumSize(260, 220);
    card->setText(share.value("broadcasterName").toString() + '\n' +
                  qualityText(share) + '\n' +
                  (share.value("hasPassword").toBool()
                       ? tr("🔒 Assistir com senha")
                       : tr("▶ Assistir")));
    const auto thumbnail = share.value("thumbnail").toString();
    if (!share.value("hideThumbnail").toBool() &&
        thumbnail.startsWith("data:image/jpeg;base64,") &&
        thumbnail.size() <= 64000) {
      QPixmap pixmap;
      pixmap.loadFromData(QByteArray::fromBase64(thumbnail.mid(23).toLatin1()),
                          "JPEG");
      card->setIcon(QIcon(pixmap));
      card->setIconSize(QSize(150, 100));
    }
    card->setEnabled(canConnect && pendingJoin_.isEmpty() &&
                     (share.value("viewerCount").toInt() <
                          share.value("maxViewers").toInt() ||
                      share.value("id") == watch_.value("id")));
    connect(card, &QPushButton::clicked, this, [this, share] {
      if (share.value("id") == watch_.value("id") && viewer_) {
        viewer_->showNormal();
        viewer_->raise();
        return;
      }
      QString password;
      if (share.value("hasPassword").toBool()) {
        bool ok;
        password = QInputDialog::getText(
            this, tr("Transmissão protegida"),
            tr("Senha de %1:").arg(share.value("broadcasterName").toString()),
            QLineEdit::Password, {}, &ok);
        if (!ok)
          return;
      }
      join(share, password);
    });
    grid_->addWidget(card, index / 3, index % 3);
    ++index;
  }
  empty_->setVisible(index == 0);
  if (!index) {
    empty_->setText(!lobbyError_.isEmpty() ? lobbyError_
                    : lobby_.isEmpty()
                        ? tr("Conectando ao lobby…")
                        : tr("Ninguém está compartilhando agora. Seja a "
                             "primeira tela on-the-line!"));
    empty_->setWordWrap(true);
    empty_->setTextFormat(Qt::PlainText);
    grid_->addWidget(empty_, 0, 0);
  }
  if (viewerRoster_)
    viewerRoster_->setText(qualityText(watch_) + '\n' + rosterText(watch_));
}

void MainWindow::configure(bool update) {
  QDialog dialog(this);
  dialog.setWindowTitle(update ? tr("Atualizar transmissão")
                               : tr("Configurar transmissão"));
  auto form = new QFormLayout(&dialog);
  auto mode = new QComboBox;
  mode->addItem("Mídia", "media");
  mode->addItem("Documento", "doc");
  auto fps = new QComboBox;
  fps->addItem("30 fps", 30);
  fps->addItem("60 fps", 60);
  auto resolution = new QComboBox;
  for (int r : {1080, 720, 480})
    resolution->addItem(QString::number(r) + 'p', r);
  resolution->setCurrentIndex(1);
  auto hide = new QCheckBox(tr("Ocultar prévia no lobby"));
  auto password = new QLineEdit;
  password->setEchoMode(QLineEdit::Password);
  auto changePassword = new QCheckBox(tr("Alterar / remover senha"));
  changePassword->setChecked(!update);
  password->setEnabled(!update);
  auto audio = new QComboBox;
  audio->setObjectName("audioMode");
  audio->addItem(tr("Sem áudio"), AudioSelection::None);
  audio->addItem(tr("Todos os aplicativos"), AudioSelection::All);
  audio->addItem(tr("Somente aplicativos selecionados"),
                 AudioSelection::Include);
  audio->addItem(tr("Todos, exceto os selecionados"), AudioSelection::Exclude);
  audio->setCurrentIndex(audio->findData(audioSelection_.mode));
  auto applications = new QListWidget;
  applications->setObjectName("audioApplications");
  applications->setMinimumHeight(150);
  QSet<QString> chosen = audioSelection_.applications;
  QMap<QString, QString> knownNames;
  auto audioHint = new QLabel(
      tr("Reproduza áudio no aplicativo para ele aparecer aqui.\nEscolha o "
         "aplicativo correspondente à janela; essa associação não é "
         "automática. Navegadores podem agrupar várias abas."));
#ifdef Q_OS_WIN
  audioHint->setText(audioHint->text() +
      tr("\nNo Windows, a seleção inclui os subprocessos do aplicativo. "
         "Aplicativos na mesma árvore de processos aparecem agrupados."));
#endif
  audioHint->setWordWrap(true);
  SystemAudio discovery;
  connect(&discovery, &SystemAudio::applicationsChanged, &dialog,
          [&](QJsonArray listing) {
            QSignalBlocker block(applications);
            applications->clear();
            QSet<QString> active;
            for (auto value : listing) {
              auto item = value.toObject();
              knownNames[item.value("key").toString()] =
                  item.value("name").toString();
              active.insert(item.value("key").toString());
            }
            auto keys = active | chosen;
            for (const auto &key : keys) {
              auto item = new QListWidgetItem(
                  knownNames.value(key, key) + (active.contains(key)
                                                    ? QString()
                                                    : tr(" (sem áudio agora)")),
                  applications);
              item->setData(Qt::UserRole, key);
              item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
              item->setCheckState(chosen.contains(key) ? Qt::Checked
                                                       : Qt::Unchecked);
            }
          });
  connect(applications, &QListWidget::itemChanged, &dialog,
          [&](QListWidgetItem *item) {
            const auto key = item->data(Qt::UserRole).toString();
            if (item->checkState() == Qt::Checked)
              chosen.insert(key);
            else
              chosen.remove(key);
          });
  connect(&discovery, &SystemAudio::failed, &dialog,
          [=](QString error) { audioHint->setText(error); });
  auto updateAudioUi = [=] {
    applications->setEnabled(audio->currentData().toInt() >=
                             AudioSelection::Include);
  };
  connect(audio, &QComboBox::currentIndexChanged, &dialog, updateAudioUi);
  updateAudioUi();
  discovery.discover();
  if (update) {
    mode->setCurrentIndex(own_.value("mode") == "doc" ? 1 : 0);
    fps->setCurrentIndex(own_.value("fps").toInt() == 60 ? 1 : 0);
    resolution->setCurrentIndex(
        resolution->findData(own_.value("resolution").toInt()));
    hide->setChecked(own_.value("hideThumbnail").toBool());
  }
  form->addRow(tr("Modo"), mode);
  form->addRow(tr("Quadros por segundo"), fps);
  form->addRow(tr("Resolução"), resolution);
  form->addRow(hide);
  if (update)
    form->addRow(changePassword);
  form->addRow(tr("Senha (vazia = aberta)"), password);
  form->addRow(tr("Áudio"), audio);
  form->addRow(applications);
  form->addRow(audioHint);
  auto hint = new QLabel(
      tr("Documento usa 5 fps. Escolha tela ou janela na próxima etapa.\nAs "
         "exclusões de áudio não alteram o som que você ouve no computador."));
  hint->setWordWrap(true);
  form->addRow(hint);
  connect(mode, &QComboBox::currentIndexChanged, &dialog,
          [=] { fps->setEnabled(mode->currentData() != "doc"); });
  fps->setEnabled(mode->currentData() != "doc");
  connect(changePassword, &QCheckBox::toggled, password,
          &QLineEdit::setEnabled);
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    if (audio->currentData().toInt() == AudioSelection::Include &&
        chosen.isEmpty()) {
      audioHint->setText(
          tr("Selecione pelo menos um aplicativo para capturar áudio."));
      return;
    }
    dialog.accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted) {
    invite_.clear();
    return;
  }
  QJsonObject cfg{
      {"mode", mode->currentData().toString()},
      {"fps", mode->currentData() == "doc" ? 5 : fps->currentData().toInt()},
      {"resolution", resolution->currentData().toInt()},
      {"hideThumbnail", hide->isChecked()}};
  if (changePassword->isChecked())
    cfg["password"] = password->text().trimmed();
  audioSelection_ = {
      static_cast<AudioSelection::Mode>(audio->currentData().toInt()), chosen};
  rtc_.setAudioSelection(audioSelection_);
  audioMeter_->setValue(0);
  if (update) {
    rtc_.setQuality(cfg);
    server_.send("screenshare:update", cfg);
    config_ = cfg;
    config_.remove("password");
  } else {
    if (!invite_.isEmpty())
      cfg["inviteToken"] = invite_;
    invite_.clear();
    chooseSource(true, cfg);
  }
}
void MainWindow::chooseSource(bool starting, QJsonObject cfg) {
  if (!server_.connected())
    return;
  const auto sources = rtc_.sources();
  if (sources.isEmpty()) {
    message(tr("Nenhuma tela/janela disponível. Verifique a sessão gráfica e o "
               "portal de captura no Wayland."));
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Escolher tela ou janela"));
  auto layout = new QVBoxLayout(&dialog);
  auto combo = new QComboBox;
  for (const auto &source : sources)
    combo->addItem(source.name);
  layout->addWidget(combo);
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted || !server_.connected())
    return;
  if (starting) {
    config_ = cfg;
    pendingStart_ = true;
    waitingCapture_ = true;
  }
  if (!rtc_.capture(sources[combo->currentIndex()], starting ? cfg : config_)) {
    pendingStart_ = waitingCapture_ = false;
    message(tr("Não foi possível iniciar a captura"));
    renderLobby();
    return;
  }
  renderLobby();
  if (starting) {
    const int attempt = ++startAttempt_;
    QTimer::singleShot(sources[combo->currentIndex()].portalType ? 130000
                                                                 : 20000,
                       this, [this, attempt] {
                         if (attempt == startAttempt_ && pendingStart_) {
                           stopBroadcast();
                           message(tr("Tempo esgotado ao iniciar transmissão"));
                         }
                       });
  }
}
void MainWindow::stopBroadcast() {
  ++startAttempt_;
  if (!own_.isEmpty() || pendingStart_)
    server_.send("screenshare:stop");
  pendingStart_ = waitingCapture_ = false;
  for (const auto &peer : broadcastPeers_)
    rtc_.removePeer(peer);
  broadcastPeers_.clear();
  rtc_.removePeer(publishPeer_);
  publishPeer_.clear();
  setProperty("sfuPublished", false);
  rtc_.stopCapture();
  own_ = {};
  config_ = {};
  announced_ = false;
  renderLobby();
  audioMeter_->setValue(0);
}
void MainWindow::join(QJsonObject share, QString password) {
  if (!server_.connected() || !property("iceReady").toBool()) {
    message(tr("Aguarde a conexão e a configuração ICE"));
    return;
  }
  pendingJoin_ = share;
  pendingPassword_ = password;
  server_.send("screenshare:join", QJsonObject{{"shareId", share.value("id")},
                                               {"password", password}});
  renderLobby();
  const int attempt = ++joinAttempt_;
  QTimer::singleShot(15000, this, [this, attempt] {
    if (attempt == joinAttempt_ && !pendingJoin_.isEmpty()) {
      pendingJoin_ = {};
      pendingPassword_.clear();
      // A lost ack leaves server membership uncertain: clear it explicitly.
      leave();
      message(tr("Tempo esgotado ao entrar na transmissão"));
      renderLobby();
    }
  });
}
void MainWindow::showViewer() {
  if (viewer_) {
    viewer_->showNormal();
    viewer_->raise();
    return;
  }
  auto dialog = new QDialog(this);
  viewer_ = dialog;
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(watch_.value("broadcasterName").toString());
  dialog->resize(1000, 700);
  auto layout = new QVBoxLayout(dialog);
  auto video = new VideoWidget;
  video_ = video;
  layout->addWidget(video, 1);
  auto roster = new QLabel;
  viewerRoster_ = roster;
  roster->setTextFormat(Qt::PlainText);
  roster->setWordWrap(true);
  layout->addWidget(roster);
  auto controls = new QHBoxLayout;
  auto mute = new QCheckBox(tr("Mudo"));
  auto volume = new QSlider(Qt::Horizontal);
  volume->setRange(0, 100);
  volume->setValue(volume_);
  auto fullscreen = new QPushButton(tr("Tela cheia"));
  auto pip = new QPushButton(tr("Janela flutuante"));
  auto close = new QPushButton(tr("Sair"));
  controls->addWidget(mute);
  controls->addWidget(volume);
  controls->addWidget(fullscreen);
  controls->addWidget(pip);
  controls->addWidget(close);
  layout->addLayout(controls);
  connect(volume, &QSlider::valueChanged, dialog, [this, mute](int value) {
    volume_ = value;
    rtc_.volume(sfuActive_ ? subscribePeer_
                           : watch_.value("broadcasterId").toString(),
                mute->isChecked() ? 0 : value);
  });
  connect(mute, &QCheckBox::toggled, dialog, [this](bool muted) {
    rtc_.volume(sfuActive_ ? subscribePeer_
                           : watch_.value("broadcasterId").toString(),
                muted ? 0 : volume_);
    setProperty("muted", muted);
  });
  connect(fullscreen, &QPushButton::clicked, dialog, [dialog] {
    if (dialog->isFullScreen())
      dialog->showNormal();
    else
      dialog->showFullScreen();
  });
  connect(pip, &QPushButton::clicked, dialog, [dialog, pip] {
    bool floating = !dialog->windowFlags().testFlag(Qt::WindowStaysOnTopHint);
    dialog->setWindowFlag(Qt::WindowStaysOnTopHint, floating);
    dialog->resize(floating ? QSize(480, 350) : QSize(1000, 700));
    pip->setText(floating ? tr("Ampliar") : tr("Janela flutuante"));
    dialog->show();
  });
  connect(close, &QPushButton::clicked, dialog, &QDialog::close);
  connect(dialog, &QDialog::finished, this, [this] {
    viewer_.clear();
    video_.clear();
    viewerRoster_.clear();
    leave();
  });
  dialog->show();
  renderLobby();
}
void MainWindow::leave() {
  ++joinAttempt_;
  if (viewer_) {
    viewer_->close();
    return;
  }
  server_.send("screenshare:leave");
  rtc_.removePeer(watch_.value("broadcasterId").toString());
  rtc_.removePeer(subscribePeer_);
  subscribePeer_.clear();
  subscribeSession_.clear();
  sfuFrame_ = {};
  sfuReady_ = sfuActive_ = false;
  watch_ = {};
  watchPassword_.clear();
  pendingJoin_ = {};
  pendingPassword_.clear();
  setProperty("muted", false);
  setProperty("usingTurn", QVariant());
}
void MainWindow::clearMedia() {
  ++epoch_;
  leave();
  stopBroadcast();
  rtc_.closePeers();
  retries_.clear();
}
void MainWindow::syncPeers() {
  QSet<QString> wanted;
  for (const auto &value : own_.value("viewers").toArray())
    wanted.insert(value.toObject().value("userId").toString());
  for (const auto &old : broadcastPeers_)
    if (!wanted.contains(old))
      rtc_.removePeer(old);
  for (const auto &id : wanted)
    if (!broadcastPeers_.contains(id))
      rtc_.connectPeer(id, true, user_.value("id").toString() < id);
  broadcastPeers_ = wanted;
}

void MainWindow::onEvent(QString event, QJsonValue payload) {
  const auto o = payload.toObject();
  if (event == "screenshare:state") {
    lobbyTimer_.stop();
    lobbyError_.clear();
    lobby_ = o;
    bool ownFound = false, watchFound = false;
    for (auto value : o.value("shares").toArray()) {
      auto share = value.toObject();
      if (!own_.isEmpty() && share.value("id") == own_.value("id")) {
        own_ = share;
        ownFound = true;
      }
      if (!watch_.isEmpty() && share.value("id") == watch_.value("id")) {
        watch_ = share;
        watchFound = true;
      }
    }
    if (!own_.isEmpty() && !ownFound)
      stopBroadcast();
    if (!watch_.isEmpty() && !watchFound)
      leave();
    syncPeers();
    renderLobby();
  } else if (event == "screenshare:started") {
    if (!pendingStart_) {
      server_.send("screenshare:stop");
      return;
    }
    pendingStart_ = false;
    own_ = o;
    config_.remove("password");
    config_.remove("inviteToken");
    renderLobby();
  } else if (event == "screenshare:joined") {
    if (pendingJoin_.isEmpty() || pendingJoin_.value("id") != o.value("id")) {
      server_.send("screenshare:leave");
      return;
    }
    const QString password = pendingPassword_;
    // Server already switched membership: close old local media without a leave
    // event.
    if (viewer_) {
      disconnect(viewer_, nullptr, this, nullptr);
      viewer_->close();
      viewer_.clear();
    }
    rtc_.removePeer(watch_.value("broadcasterId").toString());
    rtc_.removePeer(subscribePeer_);
    subscribePeer_.clear();
    sfuActive_ = sfuReady_ = false;
    sfuFrame_ = {};
    watch_ = o;
    watchPassword_ = password;
    pendingJoin_ = {};
    pendingPassword_.clear();
    rtc_.connectPeer(o.value("broadcasterId").toString(), false,
                     user_.value("id").toString() <
                         o.value("broadcasterId").toString());
    showViewer();
  } else if (event == "screenshare:viewer_joined") {
    const auto id = o.value("userId").toString();
    if (!own_.isEmpty()) {
      if (rtcDebugEnabled())
        qDebug().noquote() << "[rtc] viewer_joined" << id << "initiate ="
                           << (user_.value("id").toString() < id);
      broadcastPeers_.insert(id);
      rtc_.connectPeer(id, true, user_.value("id").toString() < id);
      QApplication::beep();
    }
  } else if (event == "screenshare:viewer_left") {
    const auto id = o.value("userId").toString();
    if (rtcDebugEnabled())
      qDebug().noquote() << "[rtc] viewer_left" << id;
    broadcastPeers_.remove(id);
    rtc_.removePeer(id);
  } else if (event == "rtc:signal" && o.value("channel") == "screenshare") {
    const QString from = o.value("from").toString();
    if (rtcDebugEnabled())
      qDebug().noquote() << "[rtc] signal from" << from << "type ="
                         << o.value("data").toObject().value("type").toString();
    if (sfuActive_ && from == watch_.value("broadcasterId").toString())
      return;
    if (!broadcastPeers_.contains(from) &&
        from != watch_.value("broadcasterId").toString())
      return;
    rtc_.connectPeer(from, broadcastPeers_.contains(from), false);
    rtc_.signalPeer(from, o.value("data").toObject());
  } else if (event == "screenshare:ended" && payload == watch_.value("id")) {
    leave();
    message(tr("A transmissão foi encerrada"));
  } else if (event == "screenshare:viewer_error") {
    pendingJoin_ = {};
    pendingPassword_.clear();
    message(payload.toString());
    renderLobby();
  } else if (event == "screenshare:broadcast_error") {
    if (pendingStart_)
      stopBroadcast();
    message(payload.toString());
  } else if (event == "screenshare:publish_needed")
    publishSfu();
  else if (event == "screenshare:migrate" &&
           o.value("shareId") == watch_.value("id"))
    subscribeSfu();
  else if (event == "screenshare:announced") {
    announced_ = true;
    renderLobby();
    message(tr("Transmissão anunciada"));
  } else if (event == "screenshare:invite_failed")
    message(payload.toString());
  else if (event == "user:banned") {
    clearMedia();
    server_.logout();
    user_ = {};
    hide();
    QTimer::singleShot(0, this, [this] { promptSession(false); });
  }
}
void MainWindow::publishSfu() {
  if (own_.isEmpty() || !publishPeer_.isEmpty())
    return;
  publishPeer_ = "#publish-" + QString::number(++serial_);
  setProperty("sfuPublished", false);
  rtc_.connectPeer(publishPeer_, true, true, true);
  const auto peer = publishPeer_;
  QTimer::singleShot(20000, this, [this, peer] {
    if (peer == publishPeer_ && !property("sfuPublished").toBool())
      failSfu(peer, tr("Tempo esgotado na publicação SFU"));
  });
}
void MainWindow::subscribeSfu() {
  if (watch_.isEmpty() || !subscribePeer_.isEmpty())
    return;
  subscribePeer_ = "#subscribe-" + QString::number(++serial_);
  const auto peer = subscribePeer_;
  server_.api(
      "/api/compartilhagram/sfu/subscribe", {{"shareId", watch_.value("id")}},
      [this, peer](QJsonObject data, QString error) {
        if (peer != subscribePeer_)
          return;
        if (!error.isEmpty()) {
          failSfu(peer, error);
          return;
        }
        subscribeSession_ = data.value("sessionId").toString();
        rtc_.connectPeer(peer, false, false, true);
        rtc_.signalPeer(peer, {{"type", "offer"}, {"sdp", data.value("sdp")}});
      },
      true);
  QTimer::singleShot(15000, this, [this, peer] {
    if (peer == subscribePeer_ && !sfuActive_)
      failSfu(peer, tr("SFU sem mídia"));
  });
}
void MainWindow::activateSfu() {
  if (!sfuReady_ || sfuFrame_.isNull())
    return;
  if (!sfuActive_) {
    sfuActive_ = true;
    rtc_.removePeer(watch_.value("broadcasterId").toString());
    rtc_.volume(subscribePeer_, property("muted").toBool() ? 0 : volume_);
  }
  if (video_)
    video_->setFrame(sfuFrame_);
}
void MainWindow::failSfu(QString peer, QString reason) {
  rtc_.removePeer(peer);
  if (peer == publishPeer_) {
    publishPeer_.clear();
    setProperty("sfuPublished", false);
    server_.send("screenshare:publish_failed");
  } else if (peer == subscribePeer_) {
    subscribePeer_.clear();
    subscribeSession_.clear();
    sfuReady_ = sfuActive_ = false;
    sfuFrame_ = {};
    server_.send("screenshare:subscribe_failed");
    if (!watch_.isEmpty())
      rtc_.connectPeer(watch_.value("broadcasterId").toString(), false,
                       user_.value("id").toString() <
                           watch_.value("broadcasterId").toString());
  }
  message(reason + tr(" — mantendo conexão direta quando disponível"));
}
void MainWindow::openLink() {
  bool ok;
  auto text = QInputDialog::getText(this, tr("Abrir transmissão / convite"),
                                    tr("Link do Compartilhagram:"),
                                    QLineEdit::Normal, {}, &ok);
  if (!ok)
    return;
  QUrl url(text);
  QUrlQuery query(url);
  if (url.host() != server_.origin().host() ||
      url.path() != "/apps/compartilhagram") {
    message(tr("Link inválido"));
    return;
  }
  if (query.hasQueryItem("invite")) {
    const QString token = query.queryItemValue("invite");
    server_.api(
        "/api/compartilhagram/invites/" +
            QString::fromLatin1(QUrl::toPercentEncoding(token)),
        {}, [this, token](QJsonObject data, QString error) {
          if (!error.isEmpty()) {
            message(error);
            return;
          }
          if (data.value("valid").toBool() && start_->isEnabled()) {
            invite_ = token;
            configure();
          } else
            message(tr("Você já está transmitindo ou o limite foi atingido"));
        });
  } else {
    for (auto value : lobby_.value("shares").toArray()) {
      auto share = value.toObject();
      if (share.value("id") != query.queryItemValue("share") ||
          share.value("id") == own_.value("id"))
        continue;
      QString password;
      if (share.value("hasPassword").toBool()) {
        password =
            QInputDialog::getText(this, tr("Senha"), tr("Senha da transmissão"),
                                  QLineEdit::Password, {}, &ok);
        if (!ok)
          return;
      }
      join(share, password);
      return;
    }
    message(tr("Essa transmissão não existe mais"));
  }
}
void MainWindow::announce() {
  const auto shareId = own_.value("id").toString();
  server_.api(
      "/api/compartilhagram/channels", {},
      [this, shareId](QJsonObject data, QString error) {
        if (shareId != own_.value("id").toString())
          return;
        if (!error.isEmpty()) {
          message(error);
          return;
        }
        auto channels = data.value("channels").toArray();
        QStringList names;
        for (auto c : channels)
          names.append(c.toObject().value("name").toString() + " (" +
                       c.toObject().value("type").toString() + ')');
        if (names.isEmpty()) {
          message(tr("Nenhum canal disponível"));
          return;
        }
        bool ok;
        QString selected =
            QInputDialog::getItem(this, tr("Anunciar transmissão"),
                                  tr("Canal do Discord"), names, 0, false, &ok);
        if (ok && shareId == own_.value("id").toString())
          server_.send(
              "screenshare:announce",
              QJsonObject{
                  {"channelId",
                   channels[names.indexOf(selected)].toObject().value("id")}});
      });
}
void MainWindow::reportStats(QString peer, QJsonArray reports) {
  QMap<QString, QJsonObject> byId;
  QString pairId;
  QJsonObject outboundVideo;
  for (auto value : reports) {
    auto s = value.toObject();
    byId[s.value("id").toString()] = s;
    if (s.value("type") == "transport")
      pairId = s.value("selectedCandidatePairId").toString();
    if (s.value("type") == "outbound-rtp" && s.value("kind") == "video") {
      outboundVideo = s;
      stats_->setText(tr("Enviando %1×%2 · %3 fps · limite: %4")
                          .arg(s.value("frameWidth").toInt())
                          .arg(s.value("frameHeight").toInt())
                          .arg(s.value("framesPerSecond").toDouble())
                          .arg(s.value("qualityLimitationReason").toString()));
    }
  }
  if (rtcDebugEnabled() &&
      (broadcastPeers_.contains(peer) || peer == publishPeer_)) {
    QStringList allTypes;
    for (auto value : reports) {
      auto s = value.toObject();
      allTypes.append(s.value("type").toString() + ":" +
                       s.value("kind").toString());
    }
    qDebug().noquote() << "[rtc-stats-raw] peer=" << peer
                       << "reportCount=" << reports.size()
                       << "outboundVideoEmpty=" << outboundVideo.isEmpty()
                       << "types=" << allTypes.join(",");
    // For an outgoing (broadcast) peer: bytes/packets actually leaving the
    // machine, plus nack/pli/fir counts the remote side reports back. Real
    // bytesSent with the remote repeatedly asking for keyframes (rising
    // pliCount/firCount) means the far end cannot decode what we send —
    // almost always a codec/profile mismatch, not a network problem.
    const auto pair = byId.value(pairId);
    const auto local =
        byId.value(pair.value("localCandidateId").toString());
    const auto remote =
        byId.value(pair.value("remoteCandidateId").toString());
    qDebug().noquote()
        << QString("[rtc-stats] peer=%1 candidates=%2/%3 codec=%4 "
                   "bytesSent=%5 packetsSent=%6 framesEncoded=%7 "
                   "nack=%8 pli=%9 fir=%10")
               .arg(peer,
                    local.value("candidateType").toString("?"),
                    remote.value("candidateType").toString("?"),
                    byId.value(outboundVideo.value("codecId").toString())
                        .value("mimeType")
                        .toString("?"))
               .arg(outboundVideo.value("bytesSent").toDouble())
               .arg(outboundVideo.value("packetsSent").toDouble())
               .arg(outboundVideo.value("framesEncoded").toDouble())
               .arg(outboundVideo.value("nackCount").toDouble())
               .arg(outboundVideo.value("pliCount").toDouble())
               .arg(outboundVideo.value("firCount").toDouble());
  }
  if (peer != watch_.value("broadcasterId").toString() &&
      peer != subscribePeer_)
    return;
  auto pair = byId.value(pairId);
  if (pair.isEmpty())
    return;
  const bool turn = byId.value(pair.value("localCandidateId").toString())
                            .value("candidateType") == "relay" ||
                    byId.value(pair.value("remoteCandidateId").toString())
                            .value("candidateType") == "relay";
  if (!property("usingTurn").isValid() ||
      property("usingTurn").toBool() != turn) {
    setProperty("usingTurn", turn);
    server_.send("screenshare:turn_status", QJsonObject{{"usingTurn", turn}});
  }
}
void MainWindow::closeEvent(QCloseEvent *event) {
  clearMedia();
  hide();
  connect(&server_, &ServerClient::closed, qApp, &QApplication::quit,
          Qt::SingleShotConnection);
  server_.disconnectGracefully();
  QTimer::singleShot(3000, qApp, &QApplication::quit);
  event->accept();
}
