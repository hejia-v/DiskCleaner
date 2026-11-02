#include "treemap.h"
#include <QDir>

#include <QDateTime>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace {
struct Item {
    uint32_t node;
    double area;
    int space = 0;
};

// SpaceSniffer-style squarify; running extrema avoid quadratic row evaluation.
std::vector<QRectF> squarify(const std::vector<Item>& items, QRectF rect)
{
    std::vector<QRectF> result(items.size());
    double total = 0;
    for (const auto& item : items) total += item.area;
    if (total <= 0 || rect.isEmpty()) return result;
    const double scale = rect.width() * rect.height() / total;
    size_t start = 0;
    while (start < items.size() && rect.width() > 0.5 && rect.height() > 0.5) {
        const double side2 = std::pow(std::min(rect.width(), rect.height()), 2);
        double sum = 0, low = 1e300, high = 0, worst = 1e300;
        size_t end = start;
        while (end < items.size()) {
            const double area = items[end].area * scale;
            const double next = sum + area;
            const double nextLow = std::min(low, area), nextHigh = std::max(high, area);
            const double ratio = std::max(side2 * nextHigh / (next * next),
                                          next * next / (side2 * nextLow));
            if (end > start && ratio > worst) break;
            sum = next;
            low = nextLow;
            high = nextHigh;
            worst = ratio;
            ++end;
        }
        if (rect.width() >= rect.height()) {
            const double width = std::min(rect.width(), sum / rect.height());
            double y = rect.top();
            for (size_t i = start; i < end; ++i) {
                const double height = std::min(rect.bottom() - y, items[i].area * scale / width);
                result[i] = QRectF(rect.left(), y, width, height);
                y += height;
            }
            rect.setLeft(rect.left() + width);
        } else {
            const double height = std::min(rect.height(), sum / rect.width());
            double x = rect.left();
            for (size_t i = start; i < end; ++i) {
                const double width = std::min(rect.right() - x, items[i].area * scale / height);
                result[i] = QRectF(x, rect.top(), width, height);
                x += width;
            }
            rect.setTop(rect.top() + height);
        }
        start = end;
    }
    return result;
}

QColor scaleColor(const QColor& color, double factor)
{
    return QColor::fromRgbF(std::clamp(color.redF() * factor, 0.0, 1.0),
                           std::clamp(color.greenF() * factor, 0.0, 1.0),
                           std::clamp(color.blueF() * factor, 0.0, 1.0));
}
const QColor tagColors[] = {Qt::transparent, QColor(220, 35, 40), QColor(245, 210, 20),
                            QColor(20, 175, 65), QColor(35, 95, 230)};
}  // namespace

TreemapWidget::TreemapWidget(QWidget* parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("treemap"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(80);
    setAutoFillBackground(false);
}
void TreemapWidget::setTree(wt::Tree* tree, uint32_t root)
{
    tree_ = tree;
    root_ = tree && root < tree->nodes.size() ? root : wt::kNone;
    selected_ = wt::kNone;
    history_.clear();
    historyPosition_ = -1;
    if (root_ != wt::kNone) {
        history_.push_back(root_);
        historyPosition_ = 0;
    }
    freeBytes_ = totalBytes_ = 0;
    scannedAllocated_ = tree && tree->root < tree->nodes.size() ? tree->nodes[tree->root].alloc : 0;
    refresh();
    emit navigationChanged();
}

void TreemapWidget::remapNodes(const std::vector<uint32_t>& mapping)
{
    const auto remap = [&](uint32_t id) { return id < mapping.size() ? mapping[id] : wt::kNone; };
    root_ = remap(root_);
    if (root_ == wt::kNone) root_ = tree_->root;
    selected_ = remap(selected_);
    QVector<uint32_t> history;
    int position = -1;
    for (int i = 0; i < history_.size(); ++i) {
        const auto id = remap(history_[i]);
        if (id == wt::kNone) continue;
        history.append(id);
        if (i <= historyPosition_) position = int(history.size()) - 1;
    }
    history_ = std::move(history);
    historyPosition_ = position;
    refresh();
    emit navigationChanged();
}
void TreemapWidget::changeRoot(uint32_t node)
{
    root_ = node;
    selected_ = node;
    refresh();
    emit rootChanged(node);
    emit navigationChanged();
}
void TreemapWidget::setRoot(uint32_t node)
{
    if (!tree_ || node >= tree_->nodes.size() || node == root_) return;
    if (tree_->nodes[node].removed) return;
    if (!tree_->nodes[node].isDir) { selectNode(node); return; }
    history_.resize(historyPosition_ + 1);
    history_.push_back(node);
    historyPosition_ = int(history_.size()) - 1;
    changeRoot(node);
}
bool TreemapWidget::canGoUp() const
{
    return tree_ && root_ < tree_->nodes.size() && root_ != tree_->root &&
           tree_->nodes[root_].parent < tree_->nodes.size();
}
void TreemapWidget::goBack() { if (canGoBack()) changeRoot(history_[--historyPosition_]); }
void TreemapWidget::goForward() { if (canGoForward()) changeRoot(history_[++historyPosition_]); }
void TreemapWidget::goUp() { if (canGoUp()) setRoot(tree_->nodes[root_].parent); }
void TreemapWidget::goHome() { if (tree_) setRoot(tree_->root); }
void TreemapWidget::selectNode(uint32_t node)
{
    selected_ = tree_ && node < tree_->nodes.size() && !tree_->nodes[node].removed ? node : wt::kNone;
    update();
}

void TreemapWidget::refreshAfterRemoval(const wt::Removal& removal, quint64 scannedAllocated,
                                       quint64 freeBytes, quint64 totalBytes)
{
    if (!tree_) return;
    const auto previousRoot = root_;
    root_ = tree_->survivingAncestor(root_);
    if (selected_ != wt::kNone) selected_ = tree_->survivingAncestor(selected_);
    QVector<uint32_t> history;
    int position = -1;
    for (int i = 0; i < history_.size(); ++i) {
        const auto node = tree_->survivingAncestor(history_[i]);
        if (history.isEmpty() || history.back() != node) history.append(node);
        if (i <= historyPosition_) position = int(history.size()) - 1;
    }
    history_ = std::move(history);
    historyPosition_ = position;
    if (!tags_.isEmpty())
        for (auto node : removal.nodes) tags_.remove(tree_->path(node).toCaseFolded());
    scannedAllocated_ = scannedAllocated;
    freeBytes_ = freeBytes;
    totalBytes_ = totalBytes;
    refresh();
    if (root_ != previousRoot) emit rootChanged(root_);
    emit navigationChanged();
}

void TreemapWidget::remapTags(const QList<QPair<QString, QString>>& renames)
{
    for (const auto& rename : renames) {
        const auto oldPath = QDir::toNativeSeparators(rename.first).toCaseFolded();
        const auto newPath = QDir::toNativeSeparators(rename.second).toCaseFolded();
        const auto keys = tags_.keys();
        for (const auto& key : keys)
            if (key == oldPath || key.startsWith(oldPath + '\\')) {
                const auto value = tags_.take(key);
                tags_.insert(newPath + key.mid(oldPath.size()), value);
            }
    }
}
void TreemapWidget::setShowNames(bool on) { showNames_ = on; refresh(); }
void TreemapWidget::setShowAllocated(bool on) { showAllocated_ = on; refresh(); }
void TreemapWidget::setUnit(wt::UnitMode unit) { unit_ = unit; refresh(); }
void TreemapWidget::setShowFreeSpace(bool on) { showFreeSpace_ = on; refresh(); }
void TreemapWidget::setShowUnknownSpace(bool on) { showUnknownSpace_ = on; refresh(); }
void TreemapWidget::setFreeSpace(quint64 freeBytes, quint64 totalBytes)
{
    freeBytes_ = freeBytes;
    totalBytes_ = totalBytes;
    refresh();
}
void TreemapWidget::setScannedAllocated(quint64 bytes) { scannedAllocated_ = bytes; refresh(); }
void TreemapWidget::setPaletteColors(const QVector<QColor>& colors) { colors_ = colors; refresh(); }
void TreemapWidget::setFileClassesStyle(bool on)
{
    if (fileClassesStyle_ == on) return;
    fileClassesStyle_ = on;
    refresh();
    emit fileClassesStyleChanged(on);
}
void TreemapWidget::setDetailLevel(int level)
{
    level = std::clamp(level, 1, 15);
    if (detailLevel_ == level) return;
    detailLevel_ = level;
    refresh();
    emit detailLevelChanged(level);
}
void TreemapWidget::setMinimumCellSize(int pixels)
{
    minimumCellSize_ = std::clamp(pixels, 1, 40);
    refresh();
}
quint64 TreemapWidget::value(uint32_t node) const
{
    return showAllocated_ ? tree_->nodes[node].alloc : tree_->nodes[node].size;
}
QColor TreemapWidget::nodeColor(uint32_t node) const
{
    if (node == root_) return palette().color(QPalette::Base);
    const QString name = tree_->name(node);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString ext = dot >= 0 ? name.mid(dot + 1).toLower() : QString();
    if (fileClassesStyle_) {
        // Ordered C# defaults: first match wins, including 3gp in Video before Audio.
        static const QStringList classes[] = {
            QStringLiteral("qt;mov;mpeg;mpg;mpe;m1s;mp4;avi;mts;vob;wmv;rm;3gp;mkv").split(';'),
            QStringLiteral("exe;dll;sys;ini;msi;log").split(';'),
            QStringLiteral("zip;7zip;rar;iso").split(';'),
            QStringLiteral("jpg;jpeg;gif;png;tif;tiff;nef;raw;rw2;cr2;bmp;psd").split(';'),
            QStringLiteral("mp2;mp3;wav;ogg;aif;alp;au;3gp;ac3;amr;aac;m4a;wma;aiff;flac;voc").split(';')};
        static const QColor colors[] = {QColor("#dfb9b1"), QColor("#e3d2a7"),
                                         QColor("#bec8e2"), QColor("#b3d1c5"), QColor("#cfc2dd")};
        for (int i = 0; i < 5; ++i)
            if (classes[i].contains(ext)) return colors[i];
    } else if (!colors_.isEmpty()) return colors_[qHash(ext) % colors_.size()];
    uint32_t branch = node;
    while (tree_->nodes[branch].parent != wt::kNone && tree_->nodes[branch].parent != root_)
        branch = tree_->nodes[branch].parent;
    static const QColor colors[] = {QColor("#c9c0dc"), QColor("#b5d2c9"),
                                    QColor("#b9cce0"), QColor("#e2d1af"),
                                    QColor("#c2cbe0"), QColor("#ddc0c6")};
    return colors[qHash(tree_->name(branch)) % std::size(colors)];
}
QVector<TreemapWidget::Cell> TreemapWidget::buildCells(const QSizeF& size, bool showFree) const
{
    QVector<Cell> cells;
    if (!tree_ || root_ >= tree_->nodes.size() || size.width() < 16 || size.height() < 4) return cells;
    const bool drive = tree_->name(tree_->root).endsWith(QStringLiteral(":\\"));
    std::function<void(uint32_t, const QRectF&, int)> layout;
    layout = [&](uint32_t node, const QRectF& rect, int depth) {
        if (rect.width() < 1 || rect.height() < 1) return;
        const auto& nd = tree_->nodes[node];
        const int index = int(cells.size());
        Cell cell;
        cell.rect = rect;
        cell.labelRect = rect.adjusted(3, 2, -3, -2);
        cell.node = node;
        cell.bytes = value(node);
        cell.depth = depth;
        cell.color = nodeColor(node).lighter(100 + std::max(0, depth - 1) * 3);
        if (!tags_.isEmpty()) cell.tag = tags_.value(tree_->path(node).toCaseFolded());
        cells.push_back(cell);
        if (!nd.isDir || depth >= detailLevel_) return;
        const double header = showNames_ && rect.height() > 40 && rect.width() > 60 ? 26 : 0;
        const QRectF client = rect.adjusted(1, header + 1, -1, -1);
        if (client.width() < 4 || client.height() < 4) return;
        std::vector<Item> items;
        items.reserve(nd.childCount + 2);
        for (uint32_t i = 0; i < nd.childCount; ++i) {
            const auto child = tree_->children[nd.childBegin + i];
            if (value(child)) items.push_back({child, double(value(child)), 0});
        }
        // Free and unknown capacity are allocation measures, not logical file sizes.
        if (drive && showAllocated_ && node == tree_->root) {
            if (showFree && freeBytes_) items.push_back({wt::kNone, double(freeBytes_), 1});
            const quint64 used = totalBytes_ > freeBytes_ ? totalBytes_ - freeBytes_ : 0;
            if (showUnknownSpace_ && used > scannedAllocated_)
                items.push_back({wt::kNone, double(used - scannedAllocated_), 2});
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            return a.area != b.area ? a.area > b.area : a.node < b.node;
        });
        const auto rectangles = squarify(items, client);
        bool expanded = false;
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& childRect = rectangles[i];
            // Tiny cells retain their area; skipping painting must not inflate larger files.
            if (childRect.width() < minimumCellSize_ || childRect.height() < minimumCellSize_) continue;
            expanded = true;
            if (items[i].space) {
                Cell space;
                space.rect = childRect;
                space.labelRect = childRect.adjusted(3, 2, -3, -2);
                space.bytes = quint64(items[i].area);
                space.space = items[i].space == 1 ? Space::Free : Space::Unknown;
                space.depth = depth + 1;
                space.color = items[i].space == 1 ? QColor("#bbd3b9") : QColor("#d2d1c9");
                cells.push_back(space);
            } else layout(items[i].node, childRect, depth + 1);
        }
        cells[index].expanded = expanded;
        if (expanded) cells[index].labelRect = QRectF(rect.x() + 3, rect.y(), rect.width() - 6, header);
    };
    layout(root_, QRectF(0, 0, size.width(), size.height()), 0);
    return cells;
}
void TreemapWidget::drawCells(QPainter& painter, const QVector<Cell>& cells, const QSizeF& size,
                              bool grayscale, int fontSize, bool interactive) const
{
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto gray = [grayscale](QColor c) {
        return grayscale ? QColor(qGray(c.rgb()), qGray(c.rgb()), qGray(c.rgb())) : c;
    };
    painter.fillRect(QRectF(QPointF(), size), gray(palette().color(QPalette::Base)));
    if (!tree_ || root_ >= tree_->nodes.size() || value(root_) == 0) {
        painter.setPen(gray(palette().color(QPalette::PlaceholderText)));
        painter.drawText(QRectF(16, 16, size.width() - 32, size.height() - 32),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         tree_ ? tr("No non-zero files in this view")
                               : tr("Scan a drive or folder to see its space distribution"));
        return;
    }
    QFont textFont = font();
    textFont.setPointSize(std::max(7, fontSize));
    painter.setFont(textFont);
    const QFontMetrics metrics(textFont);
    for (int i = 0; i < cells.size(); ++i) {
        const Cell& cell = cells[i];
        QColor color = gray(cell.color);
        if (interactive && i == hovered_) color = color.lighter(106);
        painter.fillRect(cell.rect, color);
        painter.setPen(gray(palette().color(QPalette::Base)));
        painter.drawRect(cell.rect.adjusted(0, 0, -1, -1));
        if (cell.expanded && cell.labelRect.height() > 0)
            painter.fillRect(QRectF(cell.rect.x() + 1, cell.rect.y() + 1,
                                    cell.rect.width() - 2, cell.labelRect.height() - 1), color);
        if (cell.tag > 0 && cell.tag <= 4 && cell.rect.width() >= 14 && cell.rect.height() >= 14) {
            painter.fillRect(QRectF(cell.rect.x() + 3, cell.rect.y() + 3, 8, 8), gray(tagColors[cell.tag]));
            painter.setPen(Qt::black);
            painter.drawRect(QRectF(cell.rect.x() + 3, cell.rect.y() + 3, 8, 8));
        }
        if (!showNames_ || cell.labelRect.width() < 26 || cell.labelRect.height() < 12) continue;
        QString name = cell.space == Space::Free ? tr("Free space")
                             : cell.space == Space::Unknown ? tr("Unknown space") : tree_->name(cell.node);
        // The full path remains in the navigation tooltip; don't repeat it across the chart.
        if (cell.node == root_ && name.size() > 3) {
            const auto separator = std::max(name.lastIndexOf(QLatin1Char('\\')), name.lastIndexOf(QLatin1Char('/')));
            if (separator >= 0 && separator + 1 < name.size()) name = name.mid(separator + 1);
        }
        const QString bytes = wt::formatSize(cell.bytes, unit_, 1);
        QRectF textRect = cell.labelRect;
        if (cell.tag) textRect.adjust(12, 0, 0, 0);
        painter.setPen(qGray(color.rgb()) < 110 ? Qt::white : gray(QColor("#30332f")));
        painter.save();
        painter.setClipRect(textRect);
        if (cell.expanded) {
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                             metrics.elidedText(name + QStringLiteral(" · ") + bytes,
                                                Qt::ElideMiddle, int(textRect.width())));
        } else {
            const QString title = metrics.elidedText(name, Qt::ElideMiddle, int(textRect.width()));
            if (textRect.height() >= 2 * metrics.height() + 4) {
                const double y = textRect.center().y() - metrics.height();
                painter.drawText(QRectF(textRect.x(), y, textRect.width(), metrics.height()), Qt::AlignCenter, title);
                painter.drawText(QRectF(textRect.x(), y + metrics.height(), textRect.width(), metrics.height()), Qt::AlignCenter, bytes);
            } else painter.drawText(textRect, Qt::AlignCenter, title);
        }
        painter.restore();
    }
    // Overlay selection after descendants so a folder's outline stays visible.
    if (interactive) {
        for (int i = 0; i < cells.size(); ++i) {
            const auto& cell = cells[i];
            const bool selected = cell.node != wt::kNone && cell.node == selected_;
            if (!selected && i != hovered_) continue;
            painter.setPen(QPen(selected ? palette().color(QPalette::Link) : QColor(255, 255, 255), 2));
            painter.drawRect(cell.rect.adjusted(1, 1, -2, -2));
        }
    }
}
void TreemapWidget::refresh() { relayout(); update(); }
void TreemapWidget::relayout()
{
    hovered_ = -1;
    QToolTip::hideText();
    cells_ = buildCells(QSizeF(size()), showFreeSpace_);
    lastSize_ = size();
}
void TreemapWidget::paintEvent(QPaintEvent*)
{
    if (lastSize_ != size()) relayout();
    QPainter painter(this);
    drawCells(painter, cells_, QSizeF(size()), false, std::max(9, font().pointSize()), true);
}
QImage TreemapWidget::renderImage(const QSize& size, bool grayscale, bool showFreeSpace) const
{
    if (size.isEmpty()) return {};
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return image;
    QPainter painter(&image);
    drawCells(painter, buildCells(QSizeF(size), showFreeSpace), QSizeF(size), grayscale,
              std::clamp(int(10.0 * size.width() / 1280), 7, 42), false);
    return image;
}
int TreemapWidget::hitCell(const QPointF& point) const
{
    for (int i = int(cells_.size()) - 1; i >= 0; --i)
        if (cells_[i].rect.contains(point)) return i;
    return -1;
}
uint32_t TreemapWidget::nodeAt(const QPointF& position)
{
    if (lastSize_ != size()) relayout();
    const int index = hitCell(position);
    return index >= 0 ? cells_[index].node : wt::kNone;
}
void TreemapWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (lastSize_ != size()) relayout();
    const int index = hitCell(event->position());
    if (index == hovered_) return;
    hovered_ = index;
    if (index >= 0) {
        const auto& cell = cells_[index];
        QString tip;
        if (cell.node != wt::kNone) {
            const auto& node = tree_->nodes[cell.node];
            const double total = double(value(tree_->root));
            tip = tr("%1\nLogical: %2\nAllocated: %3\n%4% of scanned size")
                      .arg(tree_->path(cell.node), wt::formatSize(node.size, unit_, 1),
                           wt::formatSize(node.alloc, unit_, 1))
                      .arg(total > 0 ? 100.0 * value(cell.node) / total : 0, 0, 'f', 2);
            if (node.mtime > 0)
                tip += QLatin1Char('\n') + tr("Modified: %1").arg(QDateTime::fromMSecsSinceEpoch(
                    node.mtime / 10000 - 11644473600000LL).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        } else tip = (cell.space == Space::Free ? tr("Free space") : tr("Unknown space")) +
                    QLatin1Char('\n') + wt::formatSize(cell.bytes, unit_, 1);
        QToolTip::showText(event->globalPosition().toPoint(), tip, this);
    } else QToolTip::hideText();
    update();
}
void TreemapWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::BackButton) { goBack(); return; }
    if (event->button() == Qt::ForwardButton) { goForward(); return; }
    if (event->button() != Qt::LeftButton && event->button() != Qt::RightButton) return;
    setFocus(Qt::MouseFocusReason);
    const auto node = nodeAt(event->position());
    selectNode(node);
    if (node != wt::kNone) emit nodeClicked(node);
}
void TreemapWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) return;
    const auto node = nodeAt(event->position());
    if (!tree_ || node >= tree_->nodes.size() || !tree_->nodes[node].isDir) return;
    setRoot(node);
    emit nodeActivated(node);
}
void TreemapWidget::keyPressEvent(QKeyEvent* event)
{
    const auto modifiers = event->modifiers();
    const int key = event->key();
    const bool ctrl = modifiers.testFlag(Qt::ControlModifier);
    const bool alt = modifiers.testFlag(Qt::AltModifier);
    if ((key == Qt::Key_Left && alt) || (key == Qt::Key_Backspace && modifiers == Qt::NoModifier)) goBack();
    else if ((key == Qt::Key_Right && alt) || (key == Qt::Key_Backspace && modifiers == Qt::ShiftModifier)) goForward();
    else if (key == Qt::Key_Up && (ctrl || alt)) goUp();
    else if (key == Qt::Key_Home && ctrl) goHome();
    else if (ctrl && (key == Qt::Key_Plus || key == Qt::Key_Equal)) setDetailLevel(detailLevel_ + 1);
    else if (ctrl && key == Qt::Key_Minus) setDetailLevel(detailLevel_ - 1);
    else if (ctrl && key == Qt::Key_T) setFileClassesStyle(!fileClassesStyle_);
    else if ((modifiers == Qt::NoModifier || ctrl) && key >= Qt::Key_0 && key <= Qt::Key_4) {
        if (key == Qt::Key_0) clearTagsInRoot();
        else toggleTag(hovered_ >= 0 ? cells_[hovered_].node : selected_, key - Qt::Key_0);
    } else { QWidget::keyPressEvent(event); return; }
    event->accept();
}
void TreemapWidget::toggleTag(uint32_t node, int tag)
{
    if (!tree_ || node >= tree_->nodes.size() || tag < 1 || tag > 4) return;
    const QString path = tree_->path(node).toCaseFolded();
    if (tags_.value(path) == tag) tags_.remove(path);
    else tags_.insert(path, tag);
    refresh();
}
void TreemapWidget::clearTagsInRoot()
{
    if (!tree_ || root_ >= tree_->nodes.size()) return;
    const QString path = tree_->path(root_).toCaseFolded();
    const QString prefix = path.endsWith('\\') ? path : path + QLatin1Char('\\');
    for (auto it = tags_.begin(); it != tags_.end();) {
        if (it.key() == path || it.key().startsWith(prefix)) it = tags_.erase(it);
        else ++it;
    }
    refresh();
}
void TreemapWidget::clearTags() { tags_.clear(); refresh(); }
void TreemapWidget::leaveEvent(QEvent*)
{
    hovered_ = -1;
    QToolTip::hideText();
    update();
}
