#pragma once
#include "RtcEngine.h"
#include "ServerClient.h"
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>

class VideoWidget : public QWidget {
public:
  explicit VideoWidget(QWidget *parent = nullptr);
  void setFrame(QImage image);

protected:
  void paintEvent(QPaintEvent *) override;

private:
  QImage image_;
};

class MainWindow : public QMainWindow {
  Q_OBJECT
  friend class UiTest;

public:
  MainWindow();
  ~MainWindow() override;
  void promptSession();

protected:
  void closeEvent(QCloseEvent *) override;

private:
  void onEvent(QString event, QJsonValue payload);
  void renderLobby();
  void configure(bool update = false);
  void chooseSource(bool starting, QJsonObject config = {});
  void stopBroadcast();
  void join(QJsonObject share, QString password = {});
  void showViewer();
  void leave();
  void clearMedia();
  void syncPeers();
  void failSfu(QString peer, QString reason);
  void publishSfu();
  void subscribeSfu();
  void activateSfu();
  void openLink();
  void announce();
  void reportStats(QString peer, QJsonArray stats);
  void message(const QString &text);
  ServerClient server_;
  RtcEngine rtc_;
  QJsonObject user_, lobby_, own_, watch_, config_, pendingJoin_;
  QString watchPassword_, pendingPassword_, invite_;
  QString lobbyError_;
  QString publishPeer_, subscribePeer_, subscribeSession_;
  QImage sfuFrame_;
  QSet<QString> broadcastPeers_;
  QMap<QString, int> retries_;
  int epoch_ = 0, serial_ = 0, volume_ = 100, startAttempt_ = 0,
      joinAttempt_ = 0;
  bool pendingStart_ = false, waitingCapture_ = false;
  AudioSelection audioSelection_;
  QProgressBar *audioMeter_;
  bool sfuReady_ = false, sfuActive_ = false, announced_ = false;
  QPushButton *start_, *boost_, *announce_;
  QLabel *status_, *empty_, *ownInfo_, *roster_, *stats_;
  QGroupBox *broadcastPanel_;
  QGridLayout *grid_;
  VideoWidget *preview_;
  QPointer<QDialog> viewer_;
  QPointer<VideoWidget> video_;
  QPointer<QLabel> viewerRoster_;
  QTimer thumbnailTimer_, statsTimer_, lobbyTimer_;
};
