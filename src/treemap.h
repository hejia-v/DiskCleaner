#pragma once

#include <QColor>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QVector>
#include <QWidget>

#include "core.h"

class TreemapWidget : public QWidget {
    Q_OBJECT
public:
    explicit TreemapWidget(QWidget* parent = nullptr);
    void setTree(wt::Tree* tree, uint32_t root);
    void setRoot(uint32_t node);
    uint32_t root() const { return root_; }
    uint32_t selectedNode() const { return selected_; }
    uint32_t nodeAt(const QPointF& position);
    void selectNode(uint32_t node);
    void setShowNames(bool on);
    void setShowAllocated(bool on);
    void setUnit(wt::UnitMode unit);
    void setShowFreeSpace(bool on);
    void setShowUnknownSpace(bool on);
    bool showUnknownSpace() const { return showUnknownSpace_; }
    void setFreeSpace(quint64 freeBytes, quint64 totalBytes);
    void setScannedAllocated(quint64 bytes);
    void setPaletteColors(const QVector<QColor>& colors);
    void setFileClassesStyle(bool on);
    bool fileClassesStyle() const { return fileClassesStyle_; }
    void setDetailLevel(int level);
    int detailLevel() const { return detailLevel_; }
    void setMinimumCellSize(int pixels);
    int minimumCellSize() const { return minimumCellSize_; }
    bool canGoBack() const { return historyPosition_ > 0; }
    bool canGoForward() const { return historyPosition_ + 1 < history_.size(); }
    bool canGoUp() const;
    void goBack();
    void goForward();
    void goUp();
    void goHome();
    void toggleTag(uint32_t node, int tag);
    void clearTagsInRoot();
    void clearTags();
    void refresh();
    void remapTags(const QList<QPair<QString, QString>>& renames);
    void refreshAfterRemoval(const wt::Removal& removal, quint64 scannedAllocated,
                             quint64 freeBytes, quint64 totalBytes);
    // Offscreen export uses the same layout without changing navigation/selection.
    QImage renderImage(const QSize& size, bool grayscale, bool showFreeSpace) const;

signals:
    void nodeClicked(uint32_t node);
    void nodeActivated(uint32_t node);
    void rootChanged(uint32_t node);
    void navigationChanged();
    void detailLevelChanged(int level);
    void fileClassesStyleChanged(bool on);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void leaveEvent(QEvent* e) override;

private:
    friend class TreemapTests;
    enum class Space { None, Free, Unknown };
    struct Cell {
        QRectF rect;
        QRectF labelRect;
        uint32_t node = wt::kNone;
        quint64 bytes = 0;
        QColor color;
        Space space = Space::None;
        int depth = 0;
        bool expanded = false;
        int tag = 0;
    };
    QVector<Cell> buildCells(const QSizeF& size, bool showFree) const;
    void drawCells(QPainter& p, const QVector<Cell>& cells, const QSizeF& size,
                   bool grayscale, int fontSize, bool interactive) const;
    QColor nodeColor(uint32_t node) const;
    quint64 value(uint32_t node) const;
    void relayout();
    int hitCell(const QPointF& p) const;
    void changeRoot(uint32_t node);

    wt::Tree* tree_ = nullptr;
    uint32_t root_ = wt::kNone;
    uint32_t selected_ = wt::kNone;
    QVector<uint32_t> history_;
    int historyPosition_ = -1;
    bool showNames_ = true;
    bool showAllocated_ = true;
    wt::UnitMode unit_ = wt::UnitMode::Dynamic;
    bool showFreeSpace_ = false;
    bool showUnknownSpace_ = false;
    bool fileClassesStyle_ = false;
    int detailLevel_ = 3;
    int minimumCellSize_ = 10;
    quint64 freeBytes_ = 0;
    quint64 totalBytes_ = 0;
    quint64 scannedAllocated_ = 0;
    QVector<Cell> cells_;
    int hovered_ = -1;
    QVector<QColor> colors_;
    // Session-only annotations keyed by path, so a filtered copy retains its tags.
    QHash<QString, int> tags_;
    QSize lastSize_;
};
