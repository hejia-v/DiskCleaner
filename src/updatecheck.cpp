#include "updatecheck.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace wt {

UpdateInfo checkForUpdates(const QString& url, int timeoutMs)
{
    UpdateInfo out;
    if (url.trimmed().isEmpty()) {
        out.error = QStringLiteral("no update endpoint configured");
        return out;
    }
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DiskCleaner/0.1"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();

    if (timedOut) {
        out.error = QStringLiteral("timeout");
        reply->deleteLater();
        return out;
    }
    if (reply->error() != QNetworkReply::NoError) {
        out.error = reply->errorString();
        reply->deleteLater();
        return out;
    }
    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    const QStringList lines = body.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        const int sep = line.indexOf(QLatin1Char('|'));
        if (sep <= 0) continue;
        const QString key = line.left(sep).trimmed().toUpper();
        const QString value = line.mid(sep + 1).trimmed();
        if (key == QLatin1String("VERSION")) out.version = value;
        else if (key == QLatin1String("INFOURL")) out.infoUrl = value;
    }
    if (out.version.isEmpty()) {
        out.error = QStringLiteral("response has no VERSION line");
        return out;
    }
    out.ok = true;
    return out;
}

bool isNewerVersion(const QString& a, const QString& b)
{
    const QStringList pa = a.split(QLatin1Char('.'));
    const QStringList pb = b.split(QLatin1Char('.'));
    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        const int va = i < pa.size() ? pa.at(i).toInt() : 0;
        const int vb = i < pb.size() ? pb.at(i).toInt() : 0;
        if (va != vb) return va > vb;
    }
    return false;
}

}  // namespace wt
