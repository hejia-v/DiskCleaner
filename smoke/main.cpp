#include <QApplication>
#include <QLabel>
#include <QTimer>
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QLabel l(QStringLiteral("Qt %1 OK").arg(QT_VERSION_STR));
    l.resize(320, 80);
    l.show();
    QTimer::singleShot(300, &app, [&]{ qInfo("smoke ok"); app.quit(); });
    return app.exec();
}
