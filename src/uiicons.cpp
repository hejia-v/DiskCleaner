#include "theme.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>

namespace {
class AppIconEngine final : public QIconEngine {
public:
    AppIconEngine(AppIcon icon, bool primary) : icon_(icon), primary_(primary) {}
    QIconEngine* clone() const override { return new AppIconEngine(icon_, primary_); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        const auto palette = QApplication::palette();
        QColor ink = palette.color(QPalette::Text);
        if (primary_) ink = palette.color(QPalette::BrightText);
        if (mode == QIcon::Disabled) ink = palette.color(QPalette::Disabled, QPalette::Text);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const qreal side = qMin(rect.width(), rect.height());
        painter->translate(rect.x() + (rect.width() - side) / 2, rect.y() + (rect.height() - side) / 2);
        painter->scale(side / 16, side / 16);
        painter->setPen(QPen(ink, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter->setBrush(Qt::NoBrush);
        QPainterPath path;
        switch (icon_) {
        case AppIcon::Drive:
            painter->drawRoundedRect(QRectF(2, 3, 12, 10), 2, 2);
            painter->drawLine(QPointF(2, 9), QPointF(14, 9));
            painter->drawPoint(QPointF(11.5, 11));
            break;
        case AppIcon::Folder:
            path.moveTo(2, 5); path.lineTo(2, 3); path.lineTo(6, 3); path.lineTo(8, 5);
            path.lineTo(14, 5); path.lineTo(14, 13); path.lineTo(2, 13); path.closeSubpath();
            painter->drawPath(path);
            break;
        case AppIcon::Play:
            path.moveTo(5, 3); path.lineTo(12, 8); path.lineTo(5, 13); path.closeSubpath();
            painter->setBrush(ink); painter->drawPath(path);
            break;
        case AppIcon::Info:
        case AppIcon::Help:
            painter->drawEllipse(QRectF(2, 2, 12, 12));
            if (icon_ == AppIcon::Info) {
                painter->drawPoint(QPointF(8, 5));
                painter->drawLine(QPointF(8, 7), QPointF(8, 11));
            } else {
                path.moveTo(6, 6); path.cubicTo(6, 3.7, 10.5, 3.7, 10, 6.4);
                path.cubicTo(9.8, 7.5, 8, 7.5, 8, 9);
                painter->drawPath(path); painter->drawPoint(QPointF(8, 11.2));
            }
            break;
        case AppIcon::Back:
        case AppIcon::Forward:
        case AppIcon::Up:
            painter->translate(8, 8);
            if (icon_ == AppIcon::Forward) painter->rotate(180);
            if (icon_ == AppIcon::Up) painter->rotate(90);
            painter->drawLine(QPointF(-5, 0), QPointF(5, 0));
            path.moveTo(-1, -4); path.lineTo(-5, 0); path.lineTo(-1, 4);
            painter->drawPath(path);
            break;
        case AppIcon::Home:
            path.moveTo(2, 7); path.lineTo(8, 2); path.lineTo(14, 7);
            path.moveTo(4, 6); path.lineTo(4, 14); path.lineTo(7, 14); path.lineTo(7, 10);
            path.lineTo(10, 10); path.lineTo(10, 14); path.lineTo(12, 14); path.lineTo(12, 6);
            painter->drawPath(path);
            break;
        case AppIcon::Copy:
            painter->drawRoundedRect(QRectF(5, 5, 8, 9), 1.5, 1.5);
            path.moveTo(3, 11); path.lineTo(2, 11); path.lineTo(2, 2); path.lineTo(10, 2); path.lineTo(10, 3);
            painter->drawPath(path);
            break;
        case AppIcon::Search:
            painter->drawEllipse(QRectF(2, 2, 8, 8));
            painter->drawLine(QPointF(9, 9), QPointF(14, 14));
            break;
        case AppIcon::Settings:
            for (int y : {4, 8, 12}) painter->drawLine(QPointF(2, y), QPointF(14, y));
            painter->setBrush(palette.color(QPalette::Base));
            painter->drawEllipse(QPointF(6, 4), 1.6, 1.6);
            painter->drawEllipse(QPointF(10, 8), 1.6, 1.6);
            painter->drawEllipse(QPointF(5, 12), 1.6, 1.6);
            break;
        case AppIcon::Close:
            painter->drawLine(QPointF(5, 5), QPointF(11, 11));
            painter->drawLine(QPointF(11, 5), QPointF(5, 11));
            break;
        }
        painter->restore();
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        return scaledPixmap(size, mode, state, 1);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QPixmap result(size * scale);
        result.setDevicePixelRatio(scale);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);
        return result;
    }
private:
    AppIcon icon_;
    bool primary_;
};
}

QIcon appIcon(AppIcon icon, bool primary)
{
    return QIcon(new AppIconEngine(icon, primary));
}
