#include "MainWindow.h"
#include <QApplication>
#include <QMessageBox>
#include <QTimer>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName("Compartilhagram");
  app.setOrganizationName("Buteco dos Devs");
  app.setQuitOnLastWindowClosed(false);
  app.setStyle("Fusion");
  app.setStyleSheet(
      "QWidget { font-size: 14px; } QPushButton { padding: 9px; } QGroupBox { "
      "margin-top: 10px; padding-top: 15px; } QLineEdit { padding: 7px; }");
  MainWindow window;
  QTimer::singleShot(0, &window, &MainWindow::promptSession);
  return app.exec();
}
