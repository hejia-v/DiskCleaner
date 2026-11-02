#include "theme.h"

#include <QApplication>
#include <QFont>
#include <QGuiApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace {
// Shared tokens for standard widgets, stylesheets and custom painters.
struct ThemeColors {
    QString window, base, subtle, text, muted, border, hover, selected, selectedText;
    QString accent, primary, primaryHover, onPrimary, disabled, controlBorder;
};

ThemeColors colors(bool dark)
{
    if (dark) return {"#242423", "#1d1d1c", "#292927", "#ecece7", "#acaca4", "#3b3b37",
                      "#343431", "#343a50", "#e1e6ff", "#a3b1e9", "#e4e4df", "#ffffff",
                      "#242423", "#777770", "#73736a"};
    return {"#f7f7f5", "#ffffff", "#f0f0ed", "#242423", "#656561", "#e4e4df",
            "#f0f0ec", "#eceff8", "#29345f", "#5268c7", "#292927", "#3a3a37",
            "#ffffff", "#92928a", "#85857d"};
}

QPalette appPalette(const ThemeColors& c)
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(c.window));
    p.setColor(QPalette::Base, QColor(c.base));
    p.setColor(QPalette::AlternateBase, QColor(c.window));
    p.setColor(QPalette::WindowText, QColor(c.text));
    p.setColor(QPalette::Text, QColor(c.text));
    p.setColor(QPalette::Button, QColor(c.base));
    p.setColor(QPalette::ButtonText, QColor(c.text));
    p.setColor(QPalette::BrightText, QColor(c.onPrimary));
    p.setColor(QPalette::Highlight, QColor(c.selected));
    p.setColor(QPalette::HighlightedText, QColor(c.selectedText));
    p.setColor(QPalette::ToolTipBase, QColor(c.base));
    p.setColor(QPalette::ToolTipText, QColor(c.text));
    p.setColor(QPalette::PlaceholderText, QColor(c.muted));
    p.setColor(QPalette::Link, QColor(c.accent));
    p.setColor(QPalette::LinkVisited, QColor(c.accent));
    p.setColor(QPalette::Mid, QColor(c.border));
    p.setColor(QPalette::Light, QColor(c.base));
    p.setColor(QPalette::Dark, QColor(c.muted));
    for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText, QPalette::PlaceholderText})
        p.setColor(QPalette::Disabled, role, QColor(c.disabled));
    return p;
}

QString appStyleSheet(const ThemeColors& c)
{
    QString sheet = QStringLiteral(R"(
QMainWindow, QDialog, QWidget#mainSurface { background: @window; color: @text; }
QFrame[panel="true"] { background: @base; border: 1px solid @border; border-radius: 6px; }
QFrame#scanSummary { background: @window; border: 0; border-bottom: 1px solid @border; }
QFrame[divider="true"] { color: @border; }
QFrame[segmentGroup="true"] { background: @subtle; border: 0; border-radius: 6px; }
QFrame#selectionBar { border: 0; background: transparent; }
QLabel { color: @text; background: transparent; }
QLabel[heading="true"] { font-size: 14px; font-weight: 600; }
QLabel[summaryDetail="true"] { font-size: 12px; }
QLabel[muted="true"], QLabel#scanState { color: @muted; }
QLabel#scanSize { font-size: 20px; font-weight: 600; padding-right: 6px; }
QLabel#emptyScanHint { padding: 12px; color: @muted; }
QToolButton#treemapPath { color: @muted; font-size: 12px; text-align: left; }
QLabel:disabled { color: @disabled; }

QTreeView, QTableView, QListView { font-size: 14px; background: @base; alternate-background-color: @window; color: @text; border: 0; selection-background-color: @selected; selection-color: @selectedText; }
QTreeView::item, QListView::item { min-height: 26px; }
QTreeView::item:hover, QTableView::item:hover, QListView::item:hover { background: @hover; }
QTreeView::item:selected, QTableView::item:selected, QListView::item:selected { background: @selected; color: @selectedText; }
QTreeView::branch { background: transparent; border-image: none; image: none; }
QTreeView::branch:has-children:closed { image: url(:/ui/chevron-right.svg); }
QTreeView::branch:has-children:open { image: url(:/ui/chevron-down.svg); }
QHeaderView { background: @base; }
QHeaderView::section { font-size: 12px; background: @base; color: @muted; border: 0; border-bottom: 1px solid @border; padding: 5px 6px; font-weight: 500; }
QHeaderView::section:hover { background: @hover; }
QHeaderView::up-arrow { image: url(:/ui/chevron-up.svg); width: 12px; height: 12px; }
QHeaderView::down-arrow { image: url(:/ui/chevron-down.svg); width: 12px; height: 12px; }
QTableCornerButton::section { background: @base; border: 0; }

QTabWidget::pane { border: 0; }
QTabBar::tab { background: transparent; color: @muted; border: 1px solid transparent; border-bottom: 2px solid transparent; padding: 3px 10px; }
QTabBar::tab:hover { color: @text; background: @hover; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @text; font-weight: 600; }
QTabBar::tab:focus { border: 1px solid @accent; border-bottom: 2px solid @accent; }
QTabBar::tab:disabled { color: @disabled; }

QLineEdit, QComboBox, QAbstractSpinBox, QTextEdit, QPlainTextEdit, QTextBrowser { background: @base; color: @text; border: 1px solid @border; border-radius: 6px; padding: 3px 7px; min-height: 18px; selection-background-color: @selected; selection-color: @selectedText; }
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover { border-color: @controlBorder; }
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus, QTextEdit:focus, QPlainTextEdit:focus, QTextBrowser:focus { border-color: @accent; }
QLineEdit:read-only { background: @subtle; }
QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled { color: @disabled; background: @subtle; border-color: @border; }
QLineEdit QToolButton { border: 0; padding: 0; }
QComboBox { padding-right: 25px; }
QComboBox::drop-down { width: 23px; border: 0; }
QComboBox::down-arrow { image: url(:/ui/chevron-down.svg); width: 14px; height: 14px; }
QComboBox QAbstractItemView { background: @base; color: @text; border: 1px solid @border; padding: 4px; selection-background-color: @selected; selection-color: @selectedText; }
QComboBox QAbstractItemView::item { min-height: 26px; padding: 0 6px; }
QAbstractSpinBox { padding-right: 23px; }
QSpinBox::up-button, QDoubleSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 21px; border: 0; border-top-right-radius: 6px; }
QSpinBox::down-button, QDoubleSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 21px; border: 0; border-bottom-right-radius: 6px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover, QDoubleSpinBox::up-button:hover, QDoubleSpinBox::down-button:hover { background: @hover; }
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(:/ui/chevron-up.svg); width: 10px; height: 10px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/ui/chevron-down.svg); width: 10px; height: 10px; }

QPushButton { background: @base; color: @text; border: 1px solid @border; padding: 3px 8px; border-radius: 6px; min-height: 18px; }
QPushButton:hover { background: @hover; border-color: @controlBorder; }
QPushButton:pressed { background: @subtle; }
QPushButton:checked { background: @selected; color: @selectedText; border-color: @border; }
QPushButton[segment="true"] { background: transparent; color: @muted; border: 1px solid transparent; padding: 1px 8px; min-height: 18px; border-radius: 4px; }
QPushButton[segment="true"]:hover { background: @hover; color: @text; }
QPushButton[segment="true"]:checked { background: @base; color: @text; border-color: @border; font-weight: 600; }
QPushButton[primary="true"], QDialogButtonBox QPushButton:default { background: @primary; border-color: @primary; color: @onPrimary; font-weight: 600; }
QPushButton[primary="true"]:hover, QDialogButtonBox QPushButton:default:hover { background: @primaryHover; border-color: @primaryHover; }
QPushButton[primary="true"]:pressed, QDialogButtonBox QPushButton:default:pressed { background: @muted; border-color: @muted; }
QPushButton[iconOnly="true"], QPushButton[quiet="true"] { background: transparent; border-color: transparent; }
QPushButton[iconOnly="true"] { padding: 2px; }
QPushButton[iconOnly="true"]:hover, QPushButton[quiet="true"]:hover { background: @hover; border-color: @border; }
QPushButton:focus, QPushButton[segment="true"]:focus, QPushButton[primary="true"]:focus, QPushButton[iconOnly="true"]:focus, QPushButton[quiet="true"]:focus, QDialogButtonBox QPushButton:default:focus { border: 1px solid @accent; }
QPushButton:disabled, QPushButton[primary="true"]:disabled, QPushButton[segment="true"]:disabled, QPushButton[quiet="true"]:disabled, QDialogButtonBox QPushButton:disabled { color: @disabled; background: @subtle; border-color: @border; }
QToolButton { color: @text; background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 3px; }
QToolButton:hover, QToolButton:checked { background: @hover; }
QToolButton:pressed { background: @selected; }
QToolButton:focus { border-color: @accent; }
QToolButton:disabled { color: @disabled; }
QToolButton::menu-indicator { image: none; }

QCheckBox, QRadioButton { color: @text; spacing: 6px; padding: 3px 0; }
QCheckBox:disabled, QRadioButton:disabled { color: @disabled; }
QCheckBox::indicator, QRadioButton::indicator, QAbstractItemView::indicator { width: 14px; height: 14px; background: @base; border: 1px solid @controlBorder; }
QCheckBox::indicator, QAbstractItemView::indicator { border-radius: 3px; }
QRadioButton::indicator { border-radius: 8px; }
QCheckBox::indicator:hover, QRadioButton::indicator:hover, QAbstractItemView::indicator:hover { border-color: @accent; background: @hover; }
QCheckBox::indicator:checked, QCheckBox::indicator:indeterminate, QRadioButton::indicator:checked, QAbstractItemView::indicator:checked, QAbstractItemView::indicator:indeterminate { background: #5268c7; border-color: #5268c7; }
QCheckBox::indicator:checked, QAbstractItemView::indicator:checked { image: url(:/ui/check.svg); }
QCheckBox::indicator:indeterminate, QAbstractItemView::indicator:indeterminate { image: url(:/ui/minus.svg); }
QRadioButton::indicator:checked { image: url(:/ui/dot.svg); }
QCheckBox::indicator:focus, QRadioButton::indicator:focus { border: 1px solid @text; }
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled, QAbstractItemView::indicator:disabled { background: @subtle; border-color: @border; }
QCheckBox::indicator:checked:disabled, QCheckBox::indicator:indeterminate:disabled, QRadioButton::indicator:checked:disabled, QAbstractItemView::indicator:checked:disabled { background: @disabled; }
QGroupBox { color: @muted; border: 1px solid @border; border-radius: 6px; margin-top: 12px; padding-top: 8px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }

QMenuBar { background: @window; color: @muted; padding: 1px 8px; }
QMenuBar::item { padding: 3px 8px; background: transparent; border-radius: 4px; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: @hover; color: @text; }
QMenu { background: @base; color: @text; border: 1px solid @border; padding: 5px; }
QMenu::item { padding: 5px 28px; border-radius: 4px; }
QMenu::item:selected { background: @selected; color: @selectedText; }
QMenu::item:disabled { color: @disabled; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
QMenu::indicator { width: 16px; height: 16px; }
QMenu::indicator:unchecked { image: none; }
QMenu::indicator:checked { image: url(:/ui/check-ink.svg); }
QMenu::right-arrow { image: url(:/ui/chevron-right.svg); width: 12px; height: 12px; }
QStatusBar { background: @window; color: @muted; border: 0; }
QStatusBar::item { border: 0; }
QStatusBar QLabel { color: @muted; font-size: 12px; }
QStatusBar QPushButton { font-size: 12px; min-height: 16px; padding: 1px 4px; }
QProgressBar { background: @subtle; color: @text; border: 0; border-radius: 3px; text-align: center; min-height: 4px; }
QProgressBar::chunk { background: @accent; border-radius: 2px; }
QSplitter::handle { background: @window; width: 6px; height: 6px; }
QSplitter::handle:hover, QMainWindow::separator:hover { background: @border; }
QDockWidget { color: @text; border: 1px solid @border; }
QDockWidget::title { background: @subtle; padding: 5px 8px; }
QDockWidget::close-button, QDockWidget::float-button { border: 0; padding: 2px; }
QDockWidget::close-button { image: url(:/ui/close.svg); }
QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: @hover; }
QToolTip { background: @base; color: @text; border: 1px solid @border; padding: 6px 8px; }
QDialogButtonBox { padding-top: 8px; }

QScrollBar:vertical { background: @base; width: 12px; margin: 0; }
QScrollBar:horizontal { background: @base; height: 12px; margin: 0; }
QScrollBar::handle:vertical { background: @controlBorder; min-height: 28px; border: 3px solid @base; border-radius: 6px; }
QScrollBar::handle:horizontal { background: @controlBorder; min-width: 28px; border: 3px solid @base; border-radius: 6px; }
QScrollBar::handle:hover, QScrollBar::handle:pressed { background: @muted; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; border: 0; background: transparent; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)");
    const std::pair<const char*, QString> tokens[] = {
        {"@window", c.window}, {"@base", c.base}, {"@subtle", c.subtle}, {"@text", c.text},
        {"@muted", c.muted}, {"@border", c.border}, {"@hover", c.hover},
        {"@selectedText", c.selectedText}, {"@selected", c.selected}, {"@accent", c.accent},
        {"@primaryHover", c.primaryHover}, {"@primary", c.primary}, {"@onPrimary", c.onPrimary},
        {"@disabled", c.disabled}, {"@controlBorder", c.controlBorder}
    };
    for (const auto& [token, value] : tokens) sheet.replace(QLatin1String(token), value);
    return sheet;
}
}  // namespace

QString effectiveTheme(const QString& theme)
{
    if (theme == QLatin1String("light") || theme == QLatin1String("dark")) return theme;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
               ? QStringLiteral("dark") : QStringLiteral("light");
}

void applyAppTheme(const QString& theme)
{
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!app) return;
    app->setProperty("theme", theme);
    if (!app->style() || app->style()->objectName().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0)
        app->setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFont font;
    font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Microsoft YaHei UI")});
    font.setPointSizeF(10.5);
    app->setFont(font);
    const auto themeColors = colors(effectiveTheme(theme) == QLatin1String("dark"));
    app->setPalette(appPalette(themeColors));
    app->setStyleSheet(appStyleSheet(themeColors));
}
