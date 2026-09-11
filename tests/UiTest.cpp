#include "MainWindow.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QScopeGuard>
#include <QtTest>

class UiTest : public QObject {
  Q_OBJECT
private slots:
  void audioConfiguration() {
    MainWindow window;
    bool checked = false;
    QTimer::singleShot(20, &window, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      auto cleanup = qScopeGuard([dialog] { dialog->reject(); });
      auto mode = dialog->findChild<QComboBox *>("audioMode");
      auto apps = dialog->findChild<QListWidget *>("audioApplications");
      auto buttons = dialog->findChild<QDialogButtonBox *>();
      if (!mode || !apps || !buttons) {
        dialog->reject();
        return;
      }
      QCOMPARE(mode->count(), 4);
      QCOMPARE(mode->currentData().toInt(), int(AudioSelection::None));
      QVERIFY(!apps->isEnabled());
      mode->setCurrentIndex(mode->findData(AudioSelection::Include));
      QVERIFY(apps->isEnabled());
      buttons->button(QDialogButtonBox::Ok)->click();
      QVERIFY(dialog->isVisible()); // Empty allowlist cannot accidentally
                                    // capture everything.
      auto item = new QListWidgetItem("Test browser", apps);
      item->setData(Qt::UserRole, "test.browser");
      item->setCheckState(Qt::Checked);
      checked = true;
      cleanup.dismiss();
      buttons->button(QDialogButtonBox::Ok)->click();
    });
    window.configure(true);
    QVERIFY(checked);
    QCOMPARE(window.audioSelection_.mode, AudioSelection::Include);
    QCOMPARE(window.audioSelection_.applications,
             QSet<QString>{"test.browser"});
    QVERIFY(window.own_.isEmpty());
  }
  void lobbyAndSessionPrompt() {
    MainWindow window;
    window.user_ = {{"id", "local"}, {"name", "Teste"}};
    QJsonObject share{{"id", "share"},
                      {"broadcasterId", "remote"},
                      {"broadcasterName", "Tela de demonstração"},
                      {"mode", "media"},
                      {"fps", 30},
                      {"resolution", 720},
                      {"viewerCount", 2},
                      {"maxViewers", 6},
                      {"hasPassword", true},
                      {"viewers", QJsonArray{QJsonObject{{"userId", "a"},
                                                         {"username", "Ana"},
                                                         {"path", "direct"}},
                                             QJsonObject{{"userId", "b"},
                                                         {"username", "Bruno"},
                                                         {"path", "sfu"}}}}};
    window.onEvent("screenshare:state",
                   QJsonObject{{"shares", QJsonArray{share}},
                               {"maxConcurrentShares", 3},
                               {"sfuAvailable", true}});
    QCOMPARE(window.grid_->count(), 1);
    QVERIFY(!window.start_->isEnabled()); // Never enable sharing before
                                          // authenticated signaling + ICE.
    window.show();
    QTest::qWait(50);
    auto codecStatus = window.findChild<QLabel *>("codecStatus");
    QVERIFY(codecStatus);
    QVERIFY(codecStatus->isVisible());
    QVERIFY(!codecStatus->text().isEmpty());
    window.message("Temporary server message");
    QVERIFY(codecStatus->isVisible());
    QVERIFY(window.grab().save("ui-preview.png"));
    window.onEvent(
        "screenshare:state",
        QJsonObject{{"shares", QJsonArray{}}, {"maxConcurrentShares", 3}});
    QCOMPARE(window.grid_->count(), 1);
    QVERIFY(window.empty_->isVisible());
    window.server_.disconnected("Forbidden");
    QCOMPARE(window.empty_->text(), QString("Forbidden"));
    QVERIFY(!window.lobbyTimer_.isActive());
    QVERIFY(!window.start_->isEnabled());
    window.lobbyError_.clear();
    window.lobbyTimer_.start(1);
    QTRY_VERIFY(window.empty_->text().contains("não respondeu"));
    window.onEvent(
        "screenshare:state",
        QJsonObject{{"shares", QJsonArray{}}, {"maxConcurrentShares", 3}});
    QVERIFY(window.lobbyError_.isEmpty());
    // A late start ack cannot resurrect a cancelled capture.
    window.onEvent("screenshare:started", share);
    QVERIFY(window.own_.isEmpty());
    bool sawPrompt = false;
    QTimer::singleShot(50, &window, [&] {
      auto dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      auto input = dialog->findChild<QLineEdit *>();
      sawPrompt = input && input->echoMode() == QLineEdit::Password &&
                  input->text().isEmpty();
      dialog->grab().save("session-preview.png");
      dialog->reject();
    });
    window.promptSession(false); // avoid touching real browser profiles/opening a browser in CI
    QVERIFY(sawPrompt);
  }
};
QTEST_MAIN(UiTest)
#include "UiTest.moc"
