#include "theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QFont>

namespace {

QPalette darkPalette()
{
    QPalette p;
    const QColor window(31, 32, 36);
    const QColor base(22, 23, 26);
    const QColor alt(35, 37, 41);
    const QColor text(230, 230, 230);
    const QColor disabled(107, 112, 120);
    const QColor button(42, 44, 49);
    const QColor highlight(47, 111, 191);

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::ToolTipBase, button);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(138, 143, 152));
    p.setColor(QPalette::Link, QColor(110, 170, 255));
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    return p;
}

QString appStyleSheet(bool dark)
{
    return QStringLiteral(R"(
QWidget#mainSurface { background: %1; }
QFrame[panel="true"] { background: %2; border: 1px solid %5; border-radius: 5px; }
QFrame#scanSummary { background: %2; border: 0; border-bottom: 1px solid %5; }
QFrame[divider="true"] { color: %5; }
QLabel { color: %3; background: transparent; }
QLabel[heading="true"] { font-size: 14px; font-weight: 600; }
QLabel[summaryDetail="true"] { font-size: 12px; }
QLabel[muted="true"] { color: %4; }
QLabel#scanSize { font-size: 20px; font-weight: 600; padding-right: 6px; }
QLabel#scanState { color: %4; }
QFrame#selectionBar { border: 0; }
QTreeView, QTableView { font-size: 14px; background: %2; alternate-background-color: %1; color: %3; border: 0; outline: 0; selection-background-color: %7; selection-color: %9; }
QTreeView::item { height: 26px; }
QTreeView::item:selected, QTableView::item:selected { background: %7; color: %9; }
QTreeView::item:hover, QTableView::item:hover { background: %6; }
QHeaderView::section { font-size: 13px; background: %1; color: %4; border: 0; border-bottom: 1px solid %5; padding: 4px 6px; font-weight: 500; }
QTabWidget::pane { border: 0; }
QTabBar::tab { background: transparent; color: %4; border: 0; border-bottom: 2px solid transparent; padding: 4px 10px; }
QTabBar::tab:selected { background: %2; color: %8; border-bottom: 2px solid %8; font-weight: 600; }
QLineEdit, QComboBox, QSpinBox { background: %2; color: %3; border: 1px solid %5; border-radius: 3px; padding: 3px 6px; min-height: 18px; selection-background-color: %7; selection-color: %9; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid %8; }
QComboBox { padding-right: 24px; }
QComboBox::drop-down { width: 22px; border: 0; }
QPushButton { background: %2; color: %3; border: 1px solid %5; padding: 3px 8px; border-radius: 3px; min-height: 18px; }
QPushButton:hover { background: %6; border-color: %8; }
QPushButton:focus { border: 1px solid %8; }
QPushButton[segment="true"]:checked { background: %7; border-color: %8; color: %8; }
QPushButton[primary="true"], QPushButton#logicalMetric:checked, QPushButton#allocatedMetric:checked { background: #0875df; border-color: #0875df; color: white; font-weight: 600; }
QPushButton[primary="true"]:hover { background: #0065cb; }
QPushButton[iconOnly="true"] { padding: 2px; }
QPushButton:disabled, QToolButton:disabled { color: %4; background: %1; border-color: %5; }
QToolButton { color: %4; background: transparent; border: 0; padding: 2px; }
QToolButton:hover { background: %6; }
QMenuBar { background: %2; color: %3; padding: 2px 8px; }
QMenuBar::item:selected { background: %7; }
QMenu { background: %2; color: %3; border: 1px solid %5; padding: 5px; }
QMenu::item { padding: 4px 22px; }
QMenu::item:selected { background: %7; color: %9; }
QMenu::separator { height: 1px; background: %5; margin: 4px; }
QStatusBar { background: %1; color: %4; border: 0; }
QStatusBar::item { border: 0; }
QStatusBar QLabel { color: %4; font-size: 12px; }
QStatusBar QPushButton { font-size: 12px; min-height: 16px; padding: 1px 4px; }
QProgressBar { background: %5; border: 0; border-radius: 5px; }
QProgressBar::chunk { background: %8; border-radius: 5px; }
QSplitter::handle { background: %1; width: 6px; height: 6px; }
QDockWidget { color: %3; }
QDockWidget::title { background: %1; padding: 4px; }
QToolTip { background: %2; color: %3; border: 1px solid %5; padding: 6px; }
)").arg(dark ? QStringLiteral("#1f2024") : QStringLiteral("#f7f9fc"),
        dark ? QStringLiteral("#16171a") : QStringLiteral("#ffffff"),
        dark ? QStringLiteral("#e6eaf0") : QStringLiteral("#18263b"),
        dark ? QStringLiteral("#a3adbb") : QStringLiteral("#596b81"),
        dark ? QStringLiteral("#3a3f48") : QStringLiteral("#dce4ee"),
        dark ? QStringLiteral("#292f39") : QStringLiteral("#f0f6ff"),
        dark ? QStringLiteral("#263f60") : QStringLiteral("#dceeff"),
        dark ? QStringLiteral("#559eff") : QStringLiteral("#0875df"),
        dark ? QStringLiteral("#ffffff") : QStringLiteral("#14385e"));
}

}  // namespace

QString effectiveTheme(const QString& theme)
{
    if (theme == QLatin1String("light") || theme == QLatin1String("dark")) return theme;
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
}

void applyAppTheme(const QString& theme)
{
    QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) return;
    app->setProperty("theme", theme);
    if (!app->style() || app->style()->objectName().compare(QLatin1String("fusion"),
                                                            Qt::CaseInsensitive) != 0)
        app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    const bool dark = effectiveTheme(theme) == QLatin1String("dark");
    QFont font(QStringLiteral("Microsoft YaHei UI"));
    font.setPointSizeF(10.5);
    app->setFont(font);
    if (dark) {
        app->setPalette(darkPalette());
    } else {
        QPalette palette = app->style()->standardPalette();
        palette.setColor(QPalette::Window, QColor("#f7f9fc"));
        palette.setColor(QPalette::Base, Qt::white);
        palette.setColor(QPalette::AlternateBase, QColor("#f7f9fc"));
        palette.setColor(QPalette::Text, QColor("#18263b"));
        palette.setColor(QPalette::WindowText, QColor("#18263b"));
        palette.setColor(QPalette::ButtonText, QColor("#18263b"));
        palette.setColor(QPalette::Highlight, QColor("#dceeff"));
        palette.setColor(QPalette::HighlightedText, QColor("#14385e"));
        palette.setColor(QPalette::Link, QColor("#0875df"));
        palette.setColor(QPalette::PlaceholderText, QColor("#718299"));
        app->setPalette(palette);
    }
    app->setStyleSheet(appStyleSheet(dark));
}
