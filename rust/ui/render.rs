mod chrome;
const WORKSPACE_PAD: f32 = 8.;

use super::*;
use gpui_component::menu::{ContextMenuExt, PopupMenuItem};
use gpui_component::scroll::{Scrollbar, ScrollbarShow};

const COLUMNS: [(&str, &str); 9] = [
    ("Name", "名称"),
    ("Path", "路径"),
    ("File size", "文件大小"),
    ("Allocated", "磁盘占用"),
    ("Scan %", "占扫描范围"),
    ("Modified", "修改时间"),
    ("Attributes", "属性"),
    ("Group files", "组内文件数"),
    ("Group size", "组内总大小"),
];
const TREE_COLUMNS: [(&str, &str); 9] = [
    ("Name", "名称"),
    ("Scan %", "占扫描范围"),
    ("File size", "文件大小"),
    ("Allocated", "磁盘占用"),
    ("Items", "项目数"),
    ("Files", "文件数"),
    ("Folders", "目录数"),
    ("Modified", "修改时间"),
    ("Attributes", "属性"),
];
// The presentation order can change without changing persisted column IDs.
const TREE_ORDER: [usize; 9] = [0, 7, 1, 2, 3, 4, 5, 6, 8];
fn tree_detail_width(i: usize, short: bool) -> f32 {
    match i {
        1 if short => 68.,
        7 if short => 76.,
        7 => 136.,
        4..=6 => 60.,
        _ => 78.,
    }
}
fn tree_column(i: usize, short: bool) -> Div {
    if i == 0 {
        return div().flex_1().min_w_0().truncate();
    }
    div()
        .flex_none()
        .w(px(tree_detail_width(i, short)))
        .text_right()
        .truncate()
}
fn file_detail_width(i: usize) -> f32 {
    match i {
        1 => 260.,
        5 => 145.,
        _ => 78.,
    }
}
fn file_column(i: usize, narrow: bool) -> Div {
    let col = div().truncate().when(narrow && i != 0, |d| d.flex_none());
    match i {
        0 => col.flex_1().min_w_0(),
        1 if narrow => col.flex_none().w(px(file_detail_width(i))),
        1 => col.w(relative(0.24)),
        5 => col.w(px(145.)).text_right(),
        _ => col.w(px(78.)).text_right(),
    }
}

fn fit_name(
    label: SharedString,
    width: Pixels,
    weight: FontWeight,
    window: &Window,
) -> SharedString {
    let mut style = window.text_style();
    style.font_weight = weight;
    let mut runs = vec![style.to_run(label.len())];
    window
        .text_system()
        .line_wrapper(style.font(), px(14.))
        .truncate_line(label, width.max(px(0.)), "…", &mut runs)
}

fn parent_label(tree: &Tree, parent: usize, width: Pixels, window: &Window) -> String {
    let path = tree.path(parent);
    let root = tree.path(0);
    let relative = path.strip_prefix(&root).unwrap_or(&path);
    if relative.as_os_str().is_empty() {
        return ".".into();
    }
    let label: SharedString = relative.display().to_string().into();
    let run = window.text_style().to_run(label.len());
    let line = window
        .text_system()
        .shape_line(label.clone(), px(14.), &[run], None);
    if line.width <= width {
        return label.to_string();
    }
    let budget = (width - px(24.)).max(px(12.));
    let head = line.closest_index_for_x(budget * 0.25);
    let tail = line.closest_index_for_x(line.width - budget * 0.75);
    format!("{}…{}", &label[..head], &label[tail..])
}

impl Dashboard {
    fn rule_chips(&self, mut row: Div, cx: &Context<Self>) -> Div {
        let t = |en, zh| self.text(en, zh);
        for (action, title, value) in [
            ("clear-include", t("Include", "包含"), &self.options.include),
            ("clear-exclude", t("Exclude", "排除"), &self.options.exclude),
        ] {
            if value.trim().is_empty() {
                continue;
            }
            let field = if self.options.full_path {
                t("path", "路径")
            } else {
                t("name", "名称")
            };
            let label = format!("{field} · {title} {value}");
            row = row.child(
                div()
                    .id(action)
                    .max_w(px(240.))
                    .flex()
                    .items_center()
                    .gap_1()
                    .px_2()
                    .rounded_md()
                    .bg(self.palette().accent_soft)
                    .cursor_pointer()
                    .child(div().min_w_0().truncate().child(label.clone()))
                    .child("×")
                    .tooltip(move |w, cx| {
                        gpui_component::tooltip::Tooltip::new(label.clone()).build(w, cx)
                    })
                    .on_click(cx.listener(move |s, _, w, cx| s.action(action, w, cx))),
            );
        }
        row
    }
    fn result_summary(&self, cx: &Context<Self>) -> Div {
        let t = |en, zh| self.text(en, zh);
        let total = self.view.result_totals;
        let metric = if self.options.allocated {
            t("on disk", "磁盘占用")
        } else {
            t("file size", "文件大小")
        };
        let mut label = format!(
            "{}  {} {}",
            t("Current results", "当前结果"),
            total.files,
            t("files", "个文件")
        );
        if total.folders > 0 {
            label.push_str(&format!(" · {} {}", total.folders, t("folders", "个目录")));
        }
        label.push_str(&format!(
            " · {metric} {}",
            format_size(total.weight(self.options.allocated), self.unit)
        ));
        if self.view.file_range.len() < self.view.files.len() {
            label.push_str(&format!(
                " · {} {}–{} / {}",
                t("Showing", "显示"),
                self.view.file_range.start + 1,
                self.view.file_range.end,
                self.view.files.len()
            ));
        }
        let row = self
            .line()
            .child(div().font_weight(FontWeight::MEDIUM).child(label))
            .when(self.view.file_range.start > 0, |d| {
                d.child(self.button("prev-page", t("Previous page", "上一页"), false, cx))
            })
            .when(self.view.file_range.end < self.view.files.len(), |d| {
                d.child(self.button("next-page", t("Next page", "下一页"), false, cx))
            })
            .when(!self.other_page_selection.is_empty(), |d| {
                d.child(self.button(
                    "selection-page",
                    t("Back to selection", "返回选中项"),
                    false,
                    cx,
                ))
            })
            .when(self.options.only && self.options.dup != 0, |d| {
                d.child(self.button("only", t("Candidates only ×", "只看疑似重复 ×"), true, cx))
            });
        self.rule_chips(row, cx)
    }
    fn group_heading(
        &self,
        group: usize,
        first: usize,
        live: bool,
        two_lines: bool,
        cx: &Context<Self>,
    ) -> Stateful<Div> {
        let t = |en, zh| self.text(en, zh);
        let result = self.content_checks.get(&group);
        let page = &self.view.file_rows()[first..];
        let id = page[0];
        let node = &self.tree.as_ref().unwrap().nodes[id];
        let verified = self.view.content_classes.contains_key(&id);
        let first_section =
            first == 0 || self.view.group_ids[self.view.file_rows()[first - 1]] != Some(group);
        let key = self.view.display_group(page[0]);
        let count = self.view.display_count(page[0]);
        let label = match result {
            Some(ContentCheck::Failed(_)) => t("Could not verify", "未能校验"),
            _ if verified && count == 1 => t("Distinct version", "不同版本"),
            _ if verified => t("Identical content", "内容相同"),
            _ => t("Possible duplicates", "疑似重复"),
        };
        let visible = page.partition_point(|&id| self.view.display_group(id) == key);
        let mut copies = if self.zh {
            format!("共 {count} 个文件")
        } else {
            format!("{count} files in group")
        };
        if (visible as u64) < count {
            copies.push_str(&format!(" · {} {visible}", t("shown here", "当前显示")));
        }
        let summary = if verified {
            format!("{label} · {copies}")
        } else {
            label.to_owned()
        };
        let identity = format!(
            "{} · {} · {} {}",
            node.name.to_string_lossy(),
            format_size(node.bytes, self.unit),
            self.view.counts[id],
            t("copies", "份")
        );
        let details = div()
            .min_w_0()
            .flex()
            .gap_1()
            .when(two_lines, |d| d.flex_col().gap_0())
            .when(first_section, |d| {
                d.child(
                    div()
                        .truncate()
                        .font_weight(FontWeight::SEMIBOLD)
                        .child(identity),
                )
            })
            .child(div().truncate().text_color(self.muted()).child(summary));
        let mut heading = div()
            .id(("duplicate-group", page[0]))
            .w_full()
            .h(px(if two_lines { 46. } else { RESULT_ROW_HEIGHT }))
            .px_2()
            .bg(self.palette().panel_alt)
            .flex()
            .items_center()
            .gap_2()
            .child(details);
        if !first_section {
            return heading;
        }
        if live {
            let text = match result {
                Some(ContentCheck::Running(..)) => t("Comparing…", "正在校验…"),
                Some(ContentCheck::Failed(_)) => t("Retry", "重试校验"),
                None if verified => t("Recheck group", "重新校验"),
                None => t("Compare content", "校验内容"),
            };
            let p = self.palette();
            heading = heading.child(
                div()
                    .id(("compare-group", page[0]))
                    .flex_none()
                    .h(px(24.))
                    .px(px(8.))
                    .rounded(px(6.))
                    .flex()
                    .items_center()
                    .text_color(p.text_secondary)
                    .bg(p.panel)
                    .border_1()
                    .border_color(p.border_strong)
                    .cursor_pointer()
                    .hover(move |d| d.bg(p.hover))
                    .child(text)
                    .when_some(
                        result.and_then(|r| {
                            if let ContentCheck::Failed(e) = r {
                                Some(e.clone())
                            } else {
                                None
                            }
                        }),
                        |d, reason| {
                            d.tooltip(move |w, cx| {
                                gpui_component::tooltip::Tooltip::new(reason.clone()).build(w, cx)
                            })
                        },
                    )
                    .on_click(cx.listener(move |s, _, _, cx| s.verify_group(group, cx))),
            );
        } else {
            heading = heading.child(t("Snapshot · unverified", "快照 · 未校验内容"));
        }
        heading
    }
    fn scope_name(&self, scope: usize) -> String {
        if scope == 0 {
            self.text("Entire scan", "整个扫描").to_owned()
        } else {
            self.tree
                .as_ref()
                .map(|tree| tree.path(scope).display().to_string())
                .unwrap_or_default()
        }
    }
    fn file_table(&self, narrow: bool, width: f32, cx: &Context<Self>) -> Div {
        let order = self.file_order(narrow);
        let leading = 3;
        let columns_width = |cols: &[usize]| {
            cols.iter()
                .filter(|&&i| self.file_mask(narrow) & (1 << i) != 0)
                .map(|&i| file_detail_width(i) + 8.)
                .sum::<f32>()
        };
        let name_width =
            narrow.then(|| px((width - 16. - columns_width(&order[1..leading])).max(180.)));
        let table = div()
            .flex()
            .flex_col()
            .gap_2()
            .h_full()
            .min_h_0()
            .child(self.table_header(true, name_width, false, cx))
            .child(self.listing(true, name_width, false, cx));
        let Some(name_width) = name_width else {
            return table.flex_1();
        };
        let table = table
            .w(name_width + px(16. + columns_width(&order[1..])))
            .pb(px(12.));
        div()
            .relative()
            .flex_1()
            .min_h_0()
            .min_w_0()
            .child(
                div()
                    .id("file-horizontal")
                    .size_full()
                    .overflow_x_scroll()
                    .track_scroll(&self.file_horizontal)
                    .child(table),
            )
            .child(
                div()
                    .absolute()
                    .top_0()
                    .left_0()
                    .right_0()
                    .bottom_0()
                    .child(
                        Scrollbar::horizontal(&self.file_horizontal)
                            .scrollbar_show(ScrollbarShow::Always),
                    ),
            )
    }
    fn empty_files(&self, cx: &Context<Self>) -> Div {
        let o = &self.options;
        let t = |en, zh| self.text(en, zh);
        let mut conditions = Vec::new();
        if let Some((scope, ext)) = &o.type_filter {
            conditions.push(format!(
                "{} · {}",
                self.scope_name(*scope),
                if ext.is_empty() {
                    t("No extension", "无扩展名").to_owned()
                } else {
                    format!(".{ext}")
                }
            ));
        }
        if !o.include.is_empty() {
            conditions.push(format!("{} {}", t("Include", "包含"), o.include));
        }
        if !o.exclude.is_empty() {
            conditions.push(format!("{} {}", t("Exclude", "排除"), o.exclude));
        }
        if o.only && o.dup != 0 {
            conditions.push(t("Possible duplicates only", "只看疑似重复").to_owned());
        }
        let message = if !o.search.is_empty() {
            let count = self.view.before_search;
            if self.zh {
                format!(
                    "在 {count} 个{}中未找到“{}”",
                    if o.show_dirs && o.type_filter.is_none() {
                        "项目"
                    } else {
                        "文件"
                    },
                    o.search
                )
            } else {
                format!("No matches for “{}” in {count} items", o.search)
            }
        } else if conditions.is_empty() {
            t("No files in this scan", "此次扫描中没有文件").to_owned()
        } else {
            t("No files match these filters", "没有符合以下条件的文件").to_owned()
        };
        div()
            .flex_1()
            .min_h_0()
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap_2()
            .child(
                div()
                    .max_w_full()
                    .truncate()
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(message),
            )
            .when(!conditions.is_empty(), |d| {
                d.child(
                    div()
                        .max_w_full()
                        .truncate()
                        .text_color(self.muted())
                        .child(conditions.join("  ·  ")),
                )
            })
            .when(!o.search.is_empty(), |d| {
                d.child(self.button("clear-search", t("Clear search", "清除搜索"), false, cx))
            })
    }
    fn selection_accent(&self) -> Rgba {
        self.palette().accent
    }
    fn divider(&self) -> Rgba {
        self.palette().border
    }
    fn button(
        &self,
        id: &'static str,
        label: impl Into<SharedString>,
        active: bool,
        cx: &Context<Self>,
    ) -> Stateful<Div> {
        let p = self.palette();
        let primary = active && matches!(id, "scan" | "apply-edit" | "csv");
        let danger = matches!(id, "recycle");
        let (bg, fg) = if primary {
            (p.primary, p.primary_ink)
        } else if active {
            (p.accent_soft, p.accent_ink)
        } else if danger {
            (rgba(0x00000000), p.danger)
        } else {
            (rgba(0x00000000), p.text_secondary)
        };
        div()
            .id(id)
            .focusable()
            .tab_stop(true)
            .flex_none()
            .h(px(30.))
            .px(px(9.))
            .flex()
            .items_center()
            .justify_center()
            .gap(px(6.))
            .rounded(px(6.))
            .bg(bg)
            .text_size(px(13.))
            .text_color(fg)
            .border_1()
            .border_color(transparent_black())
            .cursor_pointer()
            .when(primary, |d| d.font_weight(FontWeight::MEDIUM))
            .hover(move |s| {
                if primary {
                    s.opacity(0.9)
                } else if active {
                    s.bg(p.accent_soft)
                } else {
                    s.bg(p.hover)
                }
            })
            .active(|d| d.bg(p.pressed))
            .focus(|d| d.border_color(p.accent))
            .child(label.into())
            .on_key_down(cx.listener(move |s, key: &KeyDownEvent, w, cx| {
                if matches!(key.keystroke.key.as_str(), "enter" | "space") {
                    cx.stop_propagation();
                    s.action(id, w, cx);
                }
            }))
            .on_click(cx.listener(move |s, _, w, cx| s.action(id, w, cx)))
    }
    fn line(&self) -> Div {
        div().flex().items_center().gap_2().flex_wrap()
    }
    fn panel(&self) -> Div {
        let p = self.palette();
        div()
            .bg(p.panel)
            .border_1()
            .border_color(p.border)
            .rounded(px(10.))
            .overflow_hidden()
    }
    fn file_order(&self, narrow: bool) -> [usize; 9] {
        if !narrow {
            return [0, 1, 2, 3, 4, 5, 6, 7, 8];
        }
        let (size, secondary_size) = if self.options.allocated && self.columns & (1 << 3) != 0
            || self.columns & (1 << 2) == 0
        {
            (3, 2)
        } else {
            (2, 3)
        };
        [0, size, 4, 5, secondary_size, 6, 7, 8, 1]
    }
    fn file_mask(&self, narrow: bool) -> u16 {
        if narrow && self.options.dup != 0 {
            self.columns & !(1 << 7)
        } else {
            self.columns
        }
    }
    fn tree_order(&self, short: bool) -> [usize; 9] {
        if !short {
            return TREE_ORDER;
        }
        let (active, other) =
            if self.options.allocated && self.tree_columns & 8 != 0 || self.tree_columns & 4 == 0 {
                (3, 2)
            } else {
                (2, 3)
            };
        [0, active, 7, 1, other, 4, 5, 6, 8]
    }
    fn table_header(
        &self,
        flat: bool,
        name_width: Option<Pixels>,
        short: bool,
        cx: &Context<Self>,
    ) -> Div {
        let narrow = name_width.is_some();
        let mut header = div()
            .w_full()
            .px_2()
            .flex()
            .items_center()
            .gap_2()
            .h(px(28.))
            .flex_none()
            .bg(transparent_black())
            .border_b_1()
            .border_color(self.palette().border)
            .text_color(self.palette().muted)
            .text_xs()
            .font_weight(FontWeight::MEDIUM);
        let (mask, sort, descending) = if flat {
            (
                self.file_mask(narrow),
                self.options.column,
                self.options.desc,
            )
        } else {
            (
                self.tree_columns,
                self.view.tree_sort,
                !self.view.tree_ascending,
            )
        };
        let order = if flat {
            self.file_order(narrow)
        } else {
            self.tree_order(short)
        };
        for i in order {
            if mask & (1 << i) == 0 {
                continue;
            }
            let selected = sort == i as u8;
            let label = format!(
                "{}{}",
                self.column_label(flat, i),
                if selected {
                    if descending { " ↓" } else { " ↑" }
                } else {
                    ""
                }
            );
            header = header.child(
                (if flat {
                    file_column(i, narrow)
                } else {
                    tree_column(i, short)
                })
                .id(("header", i))
                .when_some(name_width.filter(|_| i == 0), |d, width| {
                    d.flex_none().flex_basis(width).w(width)
                })
                .cursor_pointer()
                .child(label)
                .on_click(cx.listener(move |s, _, _, cx| {
                    if flat {
                        s.options.desc = if selected { !descending } else { i >= 2 };
                        s.options.column = i as u8;
                        s.rebuild();
                    } else {
                        s.view.tree_ascending = if selected { descending } else { i == 0 };
                        s.view.tree_sort = i as u8;
                        if let Some(t) = &s.tree {
                            s.view.tree_rows(t, &s.expanded);
                        }
                    }
                    if let Err(e) = s.persist_view() {
                        s.fail(e);
                    }
                    cx.notify()
                })),
            )
        }
        header
    }
    fn column_label(&self, flat: bool, i: usize) -> &'static str {
        if self.view.filtered() && i == if flat { 4 } else { 1 } {
            return self.text("Filtered %", "占筛选结果");
        }
        let (en, zh) = if flat { COLUMNS[i] } else { TREE_COLUMNS[i] };
        self.text(en, zh)
    }
    fn column_menu(&self, flat: bool, cx: &Context<Self>) -> Div {
        let mut row = self.line();
        let (labels, mask) = if flat {
            (&COLUMNS, self.columns)
        } else {
            (&TREE_COLUMNS, self.tree_columns)
        };
        for i in 1..labels.len() {
            row = row.child(
                div()
                    .id(("column", i))
                    .px_2()
                    .cursor_pointer()
                    .child(format!(
                        "{} {}",
                        if mask & (1 << i) != 0 { "☑" } else { "□" },
                        self.column_label(flat, i)
                    ))
                    .on_click(cx.listener(move |s, _, _, cx| {
                        if flat {
                            s.columns ^= 1 << i;
                        } else {
                            s.tree_columns ^= 1 << i;
                        }
                        if let Err(e) = s.persist_view() {
                            s.fail(e)
                        }
                        cx.notify()
                    })),
            )
        }
        row
    }
    fn listing(
        &self,
        flat: bool,
        name_width: Option<Pixels>,
        short: bool,
        cx: &Context<Self>,
    ) -> AnyElement {
        let narrow = flat && name_width.is_some();
        let two_lines = flat && (narrow || (self.options.dup != 0 && self.columns & 2 == 0));
        let file_order = self.file_order(narrow);
        let len = if flat {
            self.view.file_rows().len() + self.view.group_headers.len()
        } else {
            self.view.rows.len()
        };
        let scroll = if flat {
            &self.list_scroll
        } else {
            &self.tree_scroll
        };
        let list = uniform_list(
            if flat { "files-list" } else { "tree-list" },
            len,
            cx.processor(move |s, range, window, cx| {
                let mut rows = Vec::new();
                let Some(t) = &s.tree else { return rows };
                for i in range {
                    let (i, heading) = if flat {
                        s.view.file_at_row(i)
                    } else {
                        (i, false)
                    };
                    let (id, depth) = if flat {
                        (s.view.file_rows()[i], 0)
                    } else {
                        s.view.rows[i]
                    };
                    let n = &t.nodes[id];
                    if heading {
                        rows.push(if let Some(group) = s.view.group_ids[id] {
                            s.group_heading(group, i, t.live, two_lines, cx)
                        } else {
                            div()
                                .id("ungrouped-files")
                                .w_full()
                                .h(px(if two_lines { 46. } else { RESULT_ROW_HEIGHT }))
                                .px_2()
                                .flex()
                                .items_center()
                                .font_weight(FontWeight::SEMIBOLD)
                                .bg(s.palette().panel_alt)
                                .child(s.text("Other results", "其他结果"))
                        });
                        continue;
                    }
                    let path = t.path(id);
                    let deleted =
                        !s.removed.is_empty() && path.ancestors().any(|p| s.removed.contains(p));
                    let selected = if flat {
                        s.multi.contains(&id)
                    } else {
                        s.selected == id
                    };
                    let p = s.palette();
                    let color = if deleted { p.danger } else { p.text };
                    let bg = if deleted {
                        if s.dark {
                            rgba(0x2E1B1BFF)
                        } else {
                            rgba(0xFEF3F2FF)
                        }
                    } else if selected {
                        p.accent_soft
                    } else {
                        rgba(0x00000000)
                    };
                    let whole = s.view.weights[0];
                    let percent = if whole == 0 {
                        0.
                    } else {
                        100. * s.view.weights[id] as f64 / whole as f64
                    };
                    let mut row = div()
                        .id(i)
                        .w_full()
                        .h(px(if two_lines { 46. } else { RESULT_ROW_HEIGHT }))
                        .px_2()
                        .flex()
                        .items_center()
                        .gap_2()
                        .bg(bg)
                        .relative()
                        .text_color(color)
                        .when(!selected && !deleted, |d| {
                            d.hover(move |d| d.bg(p.row_hover))
                        })
                        .cursor_pointer()
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |s, e: &MouseDownEvent, _, _| {
                                if !deleted && id != 0 {
                                    s.drag_origin = Some((id, e.position));
                                }
                            }),
                        )
                        .on_click(cx.listener(move |s, e: &ClickEvent, w, cx| {
                            let mods = w.modifiers();
                            if flat && mods.shift {
                                if !mods.control {
                                    s.multi.clear();
                                }
                                s.multi.insert(id);
                                if let Some(prev) =
                                    s.view.file_rows().iter().position(|&n| n == s.selected)
                                {
                                    s.multi.extend(
                                        s.view.file_rows()[prev.min(i)..=prev.max(i)]
                                            .iter()
                                            .copied(),
                                    );
                                }
                            } else if flat && mods.control {
                                if !s.multi.insert(id) {
                                    s.multi.remove(&id);
                                    if let Some(&next) = s.multi.iter().min() {
                                        s.select(next, false, cx);
                                    } else {
                                        s.explicit_selection = false;
                                        cx.notify();
                                    }
                                    return;
                                }
                            } else {
                                s.multi.clear();
                                if flat {
                                    s.multi.insert(id);
                                }
                            }
                            s.focus.focus(w);
                            s.select(id, e.click_count() > 1, cx);
                            if e.click_count() > 1
                                && s.tree.as_ref().is_some_and(|t| t.live && !t.nodes[id].dir)
                            {
                                s.action("open", w, cx);
                            }
                        }));
                    let mut name_cell = div()
                        .when(!narrow, |d| d.flex_1())
                        .when_some(name_width.filter(|_| flat), |d, width| {
                            d.flex_none().w(width)
                        })
                        .min_w_0()
                        .flex()
                        .items_center()
                        .gap_1()
                        .overflow_hidden();
                    if !flat {
                        name_cell = name_cell.child(
                            div()
                                .id(("expander", i))
                                .ml(px((depth.min(10) * 10) as f32))
                                .flex_none()
                                .w(px(14.))
                                .flex()
                                .items_center()
                                .justify_center()
                                .when(n.dir, |d| {
                                    d.child(chrome::icon(
                                        if s.expanded.contains(&id) {
                                            "chevron-down"
                                        } else {
                                            "chevron-right"
                                        },
                                        12.,
                                        s.palette().muted,
                                    ))
                                })
                                .on_click(cx.listener(move |s, _, w, cx| {
                                    cx.stop_propagation();
                                    s.focus.focus(w);
                                    s.select(id, true, cx)
                                })),
                        );
                        name_cell = name_cell.child(s.row_icons.element(
                            &path,
                            n.dir,
                            t.live && !deleted,
                            n.modified,
                            window,
                            cx,
                        ));
                    }
                    let name: SharedString = if flat {
                        n.name.as_os_str()
                    } else {
                        n.display_name()
                    }
                    .to_string_lossy()
                    .into_owned()
                    .into();
                    let name = if !flat && let Some(column) = name_width {
                        // Expander, shell icon, two gaps and the indentation precede the text.
                        let available = column - px(12. + 16. + 8. + (depth.min(10) * 10) as f32);
                        fit_name(name, available, FontWeight::NORMAL, window)
                    } else {
                        name
                    };
                    let path_hint = path;
                    let parent = two_lines
                        .then(|| parent_label(t, n.parent, name_width.unwrap_or(px(220.)), window));
                    row = row.child(
                        name_cell
                            .when(two_lines, |d| {
                                d.flex_col().items_start().justify_center().gap_0()
                            })
                            .child(
                                div()
                                    .id(("entry-name", i))
                                    .when(!two_lines, |d| d.flex_1())
                                    .when_some(name_width.filter(|_| flat), |d, width| {
                                        d.w(width).font_weight(FontWeight::MEDIUM)
                                    })
                                    .min_w_0()
                                    .truncate()
                                    .when(deleted, |s| s.line_through())
                                    .child(name)
                                    .tooltip(move |w, cx| {
                                        gpui_component::tooltip::Tooltip::new(
                                            path_hint.display().to_string(),
                                        )
                                        .build(w, cx)
                                    }),
                            )
                            .when_some(parent, |d, label| {
                                d.child(
                                    div()
                                        .w_full()
                                        .truncate()
                                        .text_xs()
                                        .text_color(s.muted())
                                        .child(label),
                                )
                            }),
                    );
                    let totals = s.view.totals(t, id);
                    if flat {
                        for col in file_order.into_iter().skip(1) {
                            if s.file_mask(narrow) & (1 << col) == 0 {
                                continue;
                            }
                            let value = match col {
                                1 => {
                                    let width = f32::from(window.viewport_size().width);
                                    let column = if narrow {
                                        file_detail_width(1)
                                    } else {
                                        (width - chrome::rail_width(width) - 60.) * 0.24
                                    };
                                    parent_label(t, n.parent, px(column - 10.), window)
                                }
                                2 => format_size(totals.bytes, s.unit),
                                3 => format_size(totals.alloc, s.unit),
                                4 => format!("{percent:.1}%"),
                                5 => table::date(n.modified),
                                6 => table::attrs(n.attrs),
                                7 | 8 if s.options.dup == 0 || n.dir => "—".into(),
                                7 => s.view.display_count(id).to_string(),
                                _ => format_size(s.view.display_count(id) * n.bytes, s.unit),
                            };
                            let cell = file_column(col, narrow).child(value);
                            row = if col == 1 {
                                let parent = t.path(n.parent).display().to_string();
                                row.child(cell.id(("parent-path", id)).tooltip(move |w, cx| {
                                    gpui_component::tooltip::Tooltip::new(parent.clone())
                                        .build(w, cx)
                                }))
                            } else {
                                row.child(cell)
                            };
                        }
                    } else {
                        for col in s.tree_order(short).into_iter().skip(1) {
                            if s.tree_columns & (1 << col) == 0 {
                                continue;
                            }
                            let value = match col {
                                1 => format!("{percent:.1}%"),
                                2 => format_size(totals.bytes, s.unit),
                                3 => format_size(totals.alloc, s.unit),
                                4 => (totals.files + totals.folders).to_string(),
                                5 => totals.files.to_string(),
                                6 => totals.folders.to_string(),
                                7 if n.modified == 0 => "—".into(),
                                7 if short => table::date(n.modified)
                                    .split_whitespace()
                                    .next()
                                    .unwrap_or("—")
                                    .to_owned(),
                                7 => table::date(n.modified),
                                _ => table::attrs(n.attrs),
                            };
                            let cell = tree_column(col, short)
                                .when(col == 7 && !deleted, |d| d.text_color(s.muted()))
                                .child(value);
                            row = row.child(if col == 1 {
                                cell.child(
                                    div()
                                        .w_full()
                                        .h(px(3.))
                                        .rounded_sm()
                                        .overflow_hidden()
                                        .bg(s.palette().bar_track)
                                        .child(
                                            div()
                                                .h_full()
                                                .w(relative((percent / 100.).clamp(0., 1.) as f32))
                                                .bg(s.palette().accent),
                                        ),
                                )
                            } else {
                                cell.when(short && col == 7 && n.modified != 0, |cell| {
                                    let timestamp = table::date(n.modified);
                                    cell.child(
                                        div().absolute().inset_0().id(("tree-date", id)).tooltip(
                                            move |w, cx| {
                                                gpui_component::tooltip::Tooltip::new(
                                                    timestamp.clone(),
                                                )
                                                .build(w, cx)
                                            },
                                        ),
                                    )
                                })
                                .relative()
                            });
                        }
                    }
                    let handle = cx.weak_entity();
                    let zh = s.zh;
                    let live = t.live && id != 0 && !deleted;
                    rows.push(div().id(("row-menu", i)).w_full().child(row.context_menu(
                        move |mut menu, _, cx| {
                            let recycle_caption = handle
                                .update(cx, |s, cx| {
                                    if !s.multi.contains(&id) {
                                        s.multi.clear();
                                        if flat {
                                            s.multi.insert(id);
                                        }
                                    }
                                    s.select(id, false, cx);
                                    let tree = s.tree.as_ref()?;
                                    let roots = s.recycle_roots(tree);
                                    Some(s.recycle_caption(
                                        s.filtered_folder_scope(tree, &roots).is_some(),
                                        roots.len(),
                                    ))
                                })
                                .ok()
                                .flatten();
                            for (cmd, en, cn) in [
                                ("copy", "Copy path", "复制路径"),
                                ("info", "Copy tree info", "复制目录信息"),
                                ("open", "Open", "打开"),
                                ("reveal", "Explore", "定位"),
                                ("file-copy", "Copy file", "复制文件"),
                                ("file-cut", "Cut file", "剪切文件"),
                                ("rename", "Rename", "重命名"),
                                ("props", "Properties", "属性"),
                                ("terminal", "Command prompt here", "在此打开命令提示符"),
                                ("recycle", "Recycle", "移至回收站"),
                            ] {
                                if !live && !matches!(cmd, "copy" | "info") {
                                    continue;
                                }
                                let h = handle.clone();
                                menu = menu.item(
                                    PopupMenuItem::new(if cmd == "recycle" {
                                        recycle_caption.unwrap_or(if zh { cn } else { en })
                                    } else if zh {
                                        cn
                                    } else {
                                        en
                                    })
                                    .on_click(
                                        move |_, w, cx| {
                                            let _ = h.update(cx, |s, cx| s.action(cmd, w, cx));
                                        },
                                    ),
                                );
                            }
                            menu
                        },
                    )));
                }
                rows
            }),
        )
        .track_scroll(scroll.clone())
        .size_full();
        let viewport = self.list_viewports[flat as usize];
        let handle = cx.weak_entity();
        div()
            .relative()
            .flex_1()
            .min_h_0()
            .child(list)
            .child(
                canvas(
                    move |bounds, _, cx| {
                        if bounds.size != viewport {
                            cx.defer(move |cx| {
                                let _ = handle.update(cx, |s, cx| {
                                    s.list_viewports[flat as usize] = bounds.size;
                                    if s.explicit_selection && s.pane == flat as u8 {
                                        if flat {
                                            s.sync_file_selection(true);
                                        } else if let Some(row) =
                                            s.view.rows.iter().position(|&(id, _)| id == s.selected)
                                        {
                                            s.tree_scroll
                                                .scroll_to_item(row, ScrollStrategy::Center);
                                        }
                                    }
                                    cx.notify();
                                });
                            });
                        }
                    },
                    |_, _, _, _| {},
                )
                .absolute()
                .size_full(),
            )
            .into_any_element()
    }
    fn treemap(&self, cx: &Context<Self>) -> Div {
        let mut chart = div()
            .relative()
            .flex_1()
            .min_h_0()
            .overflow_hidden()
            .bg(self.palette().panel_alt);
        let Some(t) = &self.tree else { return chart };
        let view = cx.weak_entity();
        let measured = self.chart_size;
        let dragging_split = self.split_drag.is_some();
        chart = chart.child(
            canvas(
                move |bounds, _, cx| {
                    if bounds.size != measured && !dragging_split {
                        cx.defer(move |cx| {
                            let _ = view.update(cx, |s, cx| {
                                s.chart_size = bounds.size;
                                cx.notify();
                            });
                        });
                    }
                },
                |_, _, _, _| {},
            )
            .absolute()
            .size_full(),
        );
        let width = f32::from(measured.width);
        let height = f32::from(measured.height);
        if width <= 0. || height <= 0. {
            return chart;
        }
        let style = self.map_style();
        let mut cache = self.chart.borrow_mut();
        if !cache.as_ref().is_some_and(|(root, w, h, opt, _)| {
            *root == self.root && *w == width as u32 && *h == height as u32 && *opt == style
        }) {
            *cache = Some((
                self.root,
                width as u32,
                height as u32,
                style.clone(),
                Rc::new(map::layout_with_selection(
                    t,
                    self.root,
                    &self.view.weights,
                    Rect {
                        x: 0.,
                        y: 0.,
                        w: width,
                        h: height,
                    },
                    &style,
                    self.explicit_selection.then_some(self.selected),
                )),
            ))
        }
        let cells = cache.as_ref().unwrap().4.clone();
        drop(cache);
        for c in cells.iter() {
            let id = c.id;
            let r = c.rect;
            if id < map::UNKNOWN
                && (r.w < style.min || r.h < style.min)
                && !(self.explicit_selection && id == self.selected)
            {
                continue;
            }
            let color = self.tags.get(&id).copied().unwrap_or(c.color);
            let snapshot = t.clone();
            let unit = self.unit;
            let zh = self.zh;
            let weight = c.weight;
            let total = self.view.weights[0];
            let leaf = id >= map::UNKNOWN || !t.nodes[id].dir;
            chart = chart.child(
                div()
                    .id(("cell", id))
                    .when(!dragging_split, |d| d.block_mouse_except_scroll())
                    .tooltip(move |w, cx| {
                        let name = if id < map::UNKNOWN {
                            snapshot.path(id).display().to_string()
                        } else {
                            map::label(&snapshot, id, zh)
                        };
                        let percent = if total == 0 {
                            0.
                        } else {
                            100. * weight as f64 / total as f64
                        };
                        gpui_component::tooltip::Tooltip::new(format!(
                            "{name}   {}   {percent:.2}%",
                            format_size(weight, unit)
                        ))
                        .build(w, cx)
                    })
                    .absolute()
                    .left(relative(r.x / width))
                    .top(relative(r.y / height))
                    .w(relative(r.w / width))
                    .h(relative(r.h / height))
                    .bg(rgb(color))
                    .border_1()
                    .border_color(self.palette().panel)
                    .overflow_hidden()
                    .cursor_pointer()
                    .hover(|s| s.border_color(self.palette().accent))
                    .when(c.header > 0., |s| {
                        s.child(
                            div()
                                .w_full()
                                .h(px(c.header))
                                .line_height(px(c.header))
                                .px_2()
                                .truncate()
                                .bg(rgb(map::folder_band(color, self.dark)))
                                .text_color(self.palette().text)
                                .text_xs()
                                .font_weight(FontWeight::SEMIBOLD)
                                .child(map::label(t, id, self.zh)),
                        )
                    })
                    .when(self.labels && leaf && r.fits_name(), |s| {
                        let (primary, secondary) = map::label_colors(color);
                        s.pt_1()
                            .px_2()
                            .text_sm()
                            .text_color(rgb(primary))
                            .child(
                                div()
                                    .truncate()
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(map::label(t, id, self.zh)),
                            )
                            .when(r.fits_size(), |d| {
                                d.child(
                                    div()
                                        .truncate()
                                        .text_xs()
                                        .text_color(rgb(secondary))
                                        .child(format_size(weight, self.unit)),
                                )
                            })
                    })
                    .on_mouse_down(
                        MouseButton::Right,
                        cx.listener(move |s, _, w, cx| {
                            s.focus.focus(w);
                            if id < map::UNKNOWN {
                                s.select_map_item(id, false, cx);
                            }
                        }),
                    )
                    .on_click(cx.listener(move |s, e: &ClickEvent, w, cx| {
                        cx.stop_propagation();
                        s.focus.focus(w);
                        if id >= map::UNKNOWN {
                            return;
                        }
                        s.select_map_item(id, e.click_count() > 1, cx);
                    })),
            );
        }
        if self.explicit_selection
            && let Some(cell) = cells.iter().find(|c| c.id == self.selected)
        {
            let r = cell.rect;
            let accent = self.selection_accent();
            let contrast = rgb(if self.dark { 0x16243b } else { 0xffffff });
            if self.motion_enabled {
                chart = chart.child(
                    div()
                        .absolute()
                        .left(relative(r.x / width))
                        .top(relative(r.y / height))
                        .w(relative(r.w / width))
                        .h(relative(r.h / height))
                        .bg(accent)
                        .with_animation(
                            ("selection-flash", self.selected),
                            Animation::new(Duration::from_millis(95)),
                            |d, t| d.opacity(0.12 * (1. - t)),
                        ),
                );
            }
            // Paint last so a directory outline sits above its children, without capturing clicks.
            chart = chart.child(
                canvas(
                    |_, _, _| (),
                    move |bounds, _, w, _| {
                        let bounds = if bounds.size.width.min(bounds.size.height) < px(6.) {
                            bounds.dilate(px(2.))
                        } else {
                            bounds
                        };
                        let mut outer = outline(bounds, accent, BorderStyle::default());
                        outer.border_widths = (2.).into();
                        w.paint_quad(outer);
                        if bounds.size.width.min(bounds.size.height) > px(4.) {
                            w.paint_quad(outline(
                                bounds.inset(px(2.)),
                                contrast,
                                BorderStyle::default(),
                            ));
                        }
                    },
                )
                .absolute()
                .left(relative(r.x / width))
                .top(relative(r.y / height))
                .w(relative(r.w / width))
                .h(relative(r.h / height)),
            );
        }
        if let Some((start, previous, from)) = self.map_transition
            && start.elapsed() < Duration::from_millis(150)
        {
            // Enter from the clicked block, or return to its footprint in the parent.
            // Only the outline moves. Hit testing always uses the final layout.
            let target = from.or_else(|| cells.iter().find(|c| c.id == previous).map(|c| c.rect));
            if let Some(r) = target {
                let accent = self.selection_accent();
                chart = chart.child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, _, w, _| {
                            let t = (start.elapsed().as_secs_f32() / 0.15).min(1.);
                            if t >= 1. {
                                return;
                            }
                            let remaining = (1. - t).powi(2);
                            let fraction = if from.is_some() {
                                remaining
                            } else {
                                1. - remaining
                            };
                            let region = Bounds::new(
                                bounds.origin + point(px(r.x * fraction), px(r.y * fraction)),
                                size(
                                    px(width + (r.w - width) * fraction),
                                    px(height + (r.h - height) * fraction),
                                ),
                            );
                            w.paint_quad(outline(
                                region,
                                Hsla::from(accent).opacity(1. - t),
                                BorderStyle::default(),
                            ));
                            w.request_animation_frame();
                        },
                    )
                    .absolute()
                    .size_full(),
                );
            }
        }
        chart
    }
}
impl Render for Dashboard {
    fn render(&mut self, w: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.row_icons.begin_frame(w, cx);
        self.update_type_scope();
        let t = |en, zh| self.text(en, zh);
        let b = |id, label, active| self.button(id, label, active, cx);
        let width = f32::from(w.viewport_size().width);
        let rail = chrome::rail_width(width);
        let content_width = width - rail - 2. * WORKSPACE_PAD;
        let split_width = content_width - 3.; // Outer frame and the 1 px divider.
        let compact = width < 960.;
        let analysis = !self.menu && self.pane != 2;
        let loaded = analysis && self.tree.is_some();
        let shell = div()
            .size_full()
            .flex()
            .flex_col()
            .bg(self.palette().canvas)
            .text_color(self.palette().text)
            .text_size(px(13.))
            .font(Font {
                features: theme::tnum(),
                ..font(theme::UI_FONT)
            });
        // A focus-tracked ancestor would consume caption mouse-down before Win32 handles it.
        let end_drag =
            move |s: &mut Self, event: &MouseUpEvent, _: &mut Window, cx: &mut Context<Self>| {
                s.drag_origin = None;
                if let Some((start_x, start_split)) = s.split_drag.take() {
                    s.split = (start_split
                        + f32::from(event.position.x - start_x) / split_width.max(1.))
                    .clamp(0.2, 0.8);
                    if s.args.shot.is_none()
                        && let Ok(p) = platform::settings()
                    {
                        let _ = p.set_value("splitPercent", &((s.split * 100.).round() as u32));
                    }
                    cx.notify();
                }
            };
        let workspace = div()
            .relative()
            .flex()
            .flex_1()
            .min_h_0()
            .key_context("Snapshot")
            .track_focus(&self.focus)
            .on_mouse_move(cx.listener(|s, e: &MouseMoveEvent, w, cx| {
                if let Some((id, start)) = s.drag_origin {
                    if e.pressed_button != Some(MouseButton::Left) {
                        s.drag_origin = None;
                    } else if (e.position - start).magnitude() >= 5. {
                        s.drag_origin = None;
                        s.start_drag(id, w, cx);
                    }
                }
            }))
            .on_mouse_up(MouseButton::Left, cx.listener(end_drag))
            .on_mouse_up_out(MouseButton::Left, cx.listener(end_drag))
            .on_action(cx.listener(|s, _: &Refresh, w, cx| s.action("refresh", w, cx)))
            .on_action(cx.listener(|s, _: &GoUp, w, cx| s.action("up", w, cx)))
            .on_action(cx.listener(|s, _: &Back, w, cx| s.action("back", w, cx)))
            .on_action(cx.listener(|s, _: &Forward, w, cx| s.action("forward", w, cx)))
            .on_action(cx.listener(|s, _: &Home, w, cx| s.action("home", w, cx)))
            .on_action(cx.listener(|s, _: &CopyInfo, w, cx| s.action("info", w, cx)))
            .on_action(cx.listener(|s, _: &PickFolder, w, cx| s.action("folder", w, cx)))
            .on_action(cx.listener(|s, _: &SaveCsv, w, cx| s.action("csv", w, cx)))
            .on_action(cx.listener(|s, _: &SaveMap, w, cx| s.action("png", w, cx)))
            .on_action(cx.listener(|s, _: &ShowFilter, w, cx| s.action("filter", w, cx)))
            .on_key_down(cx.listener(|s, event: &KeyDownEvent, w, cx| {
                if event.keystroke.key == "escape"
                    && (!s.editing.is_empty()
                        || s.location_editor
                        || s.menu
                        || s.export_panel
                        || s.filters
                        || s.tree_columns_open
                        || s.columns_open)
                {
                    if !s.editing.is_empty() {
                        s.editing = "";
                    } else if s.menu {
                        s.menu = false;
                    } else if s.location_editor {
                        s.action("close-location", w, cx);
                    } else if s.export_panel {
                        s.export_panel = false;
                    } else if s.columns_open || s.tree_columns_open {
                        s.columns_open = false;
                        s.tree_columns_open = false;
                    } else {
                        s.filters = false;
                    }
                    s.focus.focus(w);
                    cx.notify();
                    cx.stop_propagation();
                    return;
                }
                if !s.focus.is_focused(w) || s.menu || s.pane == 2 {
                    return;
                }
                let k = &event.keystroke;
                let m = k.modifiers;
                if k.key == "p"
                    && m.alt
                    && s.explicit_selection
                    && (s.pane != 1 || !s.multi.is_empty())
                {
                    s.path_focus.focus(w);
                    cx.stop_propagation();
                    return;
                }
                if s.pane == 1
                    && !m.control
                    && !m.alt
                    && !m.shift
                    && !m.platform
                    && matches!(k.key.as_str(), "pageup" | "pagedown")
                {
                    s.action(
                        if k.key == "pageup" {
                            "prev-page"
                        } else {
                            "next-page"
                        },
                        w,
                        cx,
                    );
                    cx.stop_propagation();
                    return;
                }
                if !m.control
                    && !m.alt
                    && !m.shift
                    && !m.platform
                    && (matches!(k.key.as_str(), "up" | "down")
                        || s.pane == 0 && matches!(k.key.as_str(), "left" | "right"))
                {
                    s.browse_row(k.key.as_str(), cx);
                    cx.stop_propagation();
                    return;
                }
                let cmd = match (k.key.as_str(), m.control, m.shift) {
                    ("enter", false, false) => "open",
                    ("f2", false, false) => "rename",
                    ("c", true, false) => "file-copy",
                    ("x", true, false) => "file-cut",
                    _ if s.pane != 0 => return,
                    ("backspace", false, false) => "back",
                    ("backspace", false, true) => "forward",
                    ("up", true, false) => "up",
                    ("=" | "+", true, _) => "deeper",
                    ("-", true, false) => "shallower",
                    ("t", true, false) => "colors",
                    ("0", _, false) => "clear-tags",
                    ("1", _, false) => "tag1",
                    ("2", _, false) => "tag2",
                    ("3", _, false) => "tag3",
                    ("4", _, false) => "tag4",
                    _ => return,
                };
                s.action(cmd, w, cx);
                cx.stop_propagation();
            }))
            .on_action(cx.listener(|s, _: &Trash, w, cx| {
                if s.focus.is_focused(w) && !s.menu && s.pane != 2 {
                    s.action("recycle", w, cx);
                } else {
                    cx.propagate();
                }
            }))
            .on_drop(cx.listener(|s, paths: &ExternalPaths, w, cx| {
                if s.dragging {
                    return;
                }
                let paths = paths.paths().to_vec();
                let value = paths
                    .iter()
                    .map(|p| p.display().to_string())
                    .collect::<Vec<_>>()
                    .join(";");
                s.target.update(cx, |t, cx| t.set_value(value, w, cx));
                if s.tree.is_none() && s.cancel.is_none() {
                    s.menu = false;
                    s.pane = 0;
                    cx.notify();
                } else {
                    s.scan_location(paths, w, cx)
                }
            }));
        let mut page = div()
            .flex_1()
            .min_w_0()
            .h_full()
            .p(px(WORKSPACE_PAD))
            .flex()
            .flex_col()
            .gap_1()
            .child(self.heading(content_width, cx));
        if analysis && (self.scan_items.is_some() || (!loaded && self.cancel.is_some())) {
            page = page.child(self.location_row(cx));
        }
        if loaded {
            let filter_count = [&self.options.include, &self.options.exclude]
                .iter()
                .filter(|s| !s.trim().is_empty())
                .count();
            let filter_label = t("Filter", "筛选");
            let controls = self
                .line()
                .when(compact && self.pane == 0, |d| {
                    d.child(
                        self.command("show-tree", !self.compact_map, cx)
                            .child(t("Directory tree", "目录树")),
                    )
                    .child(
                        self.command("show-map", self.compact_map, cx)
                            .child(t("Treemap", "空间图")),
                    )
                })
                .child(
                    self.command("types", self.types, cx)
                        .child(chrome::icon("table", 14., self.palette().text_secondary))
                        .child(t("File types", "文件类型")),
                )
                .child(
                    self.command("filter", self.filters || filter_count > 0, cx)
                        .child(chrome::icon("funnel", 14., self.palette().text_secondary))
                        .child(if filter_count == 0 {
                            filter_label.to_owned()
                        } else {
                            format!("{filter_label} · {filter_count}")
                        }),
                )
                .when(filter_count > 0, |d| {
                    d.child(
                        self.command("clear-filters", false, cx)
                            .child(t("Clear filters", "清除筛选")),
                    )
                });
            page = page.child(controls.child(self.size_selector(w, cx)));
        }
        if loaded && self.export_panel {
            let mut pane = self.panel().p_2().flex().gap_1().flex_wrap();
            for (id, name, on) in [
                ("ex-files", t("Files", "文件"), self.export_opts.files),
                ("ex-folders", t("Folders", "目录"), self.export_opts.folders),
                (
                    "ex-selected",
                    t("Selection only", "仅选中项"),
                    self.export_opts.selected,
                ),
                ("ex-record", "MFT", self.export_opts.fields[0]),
                ("ex-dates", t("Dates", "日期"), self.export_opts.fields[1]),
                (
                    "ex-folder-size",
                    t("Folder totals", "目录大小"),
                    self.export_opts.fields[2],
                ),
                (
                    "ex-split",
                    t("Split name", "拆分名称"),
                    self.export_opts.fields[3],
                ),
                ("ex-percent", "%", self.export_opts.fields[4]),
                (
                    "ex-volume",
                    t("Capacity", "容量"),
                    self.export_opts.fields[5],
                ),
            ] {
                pane = pane.child(b(id, name, on))
            }
            page = page.child(
                pane.child(self.button(
                    "ex-depth",
                    format!("{} {}", t("Max depth", "最大深度"), self.export_opts.depth),
                    false,
                    cx,
                ))
                .child(b("csv", t("Save CSV…", "保存 CSV…"), true))
                .child(b("export-panel", t("Close", "关闭"), false)),
            )
        }

        if analysis && self.filters {
            let field = |label, input| {
                div()
                    .flex_1()
                    .min_w(px(100.))
                    .flex()
                    .flex_col()
                    .child(div().text_xs().text_color(self.muted()).child(label))
                    .child(Input::new(input))
            };
            page = page.child(
                self.line()
                    .child(field(
                        t("Include text · e.g. .log", "包含文字 · 如 .log"),
                        &self.filter,
                    ))
                    .child(field(
                        t("Exclude text · e.g. cache", "排除文字 · 如 cache"),
                        &self.exclude,
                    ))
                    .child(b(
                        "fullpath",
                        t("Match full path", "匹配完整路径"),
                        self.options.full_path,
                    )),
            )
        }
        if loaded
            && self.pane == 0
            && (!self.options.include.trim().is_empty() || !self.options.exclude.trim().is_empty())
        {
            page = page.child(self.rule_chips(self.line(), cx));
        }
        if !self.editing.is_empty() {
            page = page.child(
                self.line()
                    .child(if self.editing == "rename" {
                        t("New name", "新名称")
                    } else if self.editing == "ex-depth" {
                        t("Max depth (0 = all)", "最大深度（0 为全部）")
                    } else if self.editing == "cell" {
                        t("Minimum cell (1–40 px)", "最小色块（1–40 像素）")
                    } else if self.editing == "image-size" {
                        t("PNG dimensions", "PNG 尺寸")
                    } else {
                        t("Update URL", "更新地址")
                    })
                    .child(div().flex_1().child(Input::new(&self.edit)))
                    .child(b("apply-edit", t("Apply", "应用"), true))
                    .child(b("dismiss", t("Cancel", "取消"), false)),
            )
        }
        if loaded && self.types {
            let scope = self.type_scope();
            let scope_name = self.scope_name(scope);
            let mut header = div()
                .flex()
                .gap_2()
                .px_3()
                .items_center()
                .h(px(24.))
                .font_weight(FontWeight::SEMIBOLD);
            for (en, zh) in [
                ("Extension", "扩展名"),
                ("File type", "文件类型"),
                if self.view.filtered() {
                    ("Filtered %", "占筛选结果")
                } else {
                    ("Scan %", "占扫描范围")
                },
                ("File size", "文件大小"),
                ("Allocated", "磁盘占用"),
                ("Files", "文件数"),
            ] {
                header = header.child(div().flex_1().truncate().child(t(en, zh)));
            }
            page = page.child(
                self.panel()
                    .h(px((48. + 24. * self.type_rows().len() as f32).min(120.)))
                    .flex_none()
                    .flex()
                    .flex_col()
                    .child(
                        div()
                            .px_3()
                            .text_xs()
                            .text_color(self.muted())
                            .child(format!("{} · {scope_name}", t("File types", "文件类型"))),
                    )
                    .child(header)
                    .child(
                        uniform_list(
                            "types-table",
                            self.type_rows().len(),
                            cx.processor(move |s, range: std::ops::Range<usize>, _, cx| {
                                range
                                    .map(|i| {
                                        let (ext, total) = &s.type_rows()[i];
                                        let pct = 100. * total.weight(s.options.allocated) as f64
                                            / s.view.weights[0].max(1) as f64;
                                        let label = if ext.is_empty() {
                                            s.text("(No extension)", "（无扩展名）").to_owned()
                                        } else {
                                            format!(".{ext}")
                                        };
                                        let mut row = div()
                                            .w_full()
                                            .flex()
                                            .gap_2()
                                            .px_3()
                                            .items_center()
                                            .h(px(24.));
                                        for value in [
                                            label,
                                            view::type_name(ext, s.zh),
                                            format!("{pct:.1}%"),
                                            format_size(total.bytes, s.unit),
                                            format_size(total.alloc, s.unit),
                                            total.files.to_string(),
                                        ] {
                                            row = row.child(div().flex_1().truncate().child(value));
                                        }
                                        let ext = ext.clone();
                                        row.id(("extension", i)).cursor_pointer().on_click(
                                            cx.listener(move |s, e: &ClickEvent, _, cx| {
                                                if e.click_count() > 1 {
                                                    s.options.type_filter =
                                                        Some((scope, ext.clone()));
                                                    s.rebuild();
                                                    s.switch_pane(1);
                                                    cx.notify();
                                                }
                                            }),
                                        )
                                    })
                                    .collect()
                            }),
                        )
                        .flex_1()
                        .min_h_0(),
                    ),
            );
        }
        let body = if self.menu {
            self.settings_page(cx).into_any_element()
        } else if self.pane == 2 {
            self.about_page().into_any_element()
        } else if self.tree.is_none() {
            self.welcome(cx).into_any_element()
        } else if self.pane == 1 {
            self.panel()
                .p_2()
                .flex()
                .flex_col()
                .gap_2()
                .flex_1()
                .min_h_0()
                .child(
                    self.line()
                        .child(div().flex_1().min_w(px(112.)).child(
                            Input::new(&self.search).border_color(self.palette().input_border),
                        ))
                        .child(
                            div()
                                .flex()
                                .items_center()
                                .gap_1()
                                .child(
                                    self.button(
                                        "dup",
                                        match self.options.dup {
                                            1 => t("Name + size", "同名同大小"),
                                            2 => t("Name + size + date", "同名同大小同日期"),
                                            _ => t("Find duplicates", "查找重复项"),
                                        },
                                        self.options.dup != 0,
                                        cx,
                                    )
                                    .h(px(30.)),
                                )
                                .when(self.options.dup != 0, |d| {
                                    d.when(!self.options.only, |d| {
                                        d.child(
                                            b("only", t("Candidates only", "只看疑似重复"), false)
                                                .h(px(30.)),
                                        )
                                    })
                                    .child(
                                        div().text_xs().text_color(self.muted()).child(format!(
                                            "{} {}",
                                            self.view.duplicate_groups,
                                            t("candidate groups", "个候选组")
                                        )),
                                    )
                                }),
                        )
                        .child(b("columns", t("Display", "显示"), self.columns_open).h(px(30.))),
                )
                .child(self.result_summary(cx))
                .when_some(self.options.type_filter.as_ref(), |p, (scope, ext)| {
                    p.child(
                        self.line()
                            .child(div().min_w_0().truncate().child(format!(
                                "{} · {}",
                                self.scope_name(*scope),
                                if ext.is_empty() {
                                    t("No extension", "无扩展名").to_owned()
                                } else {
                                    format!(".{ext}")
                                }
                            )))
                            .when(*scope != 0, |d| {
                                d.child(b(
                                    "whole-type-scan",
                                    t("View entire scan", "查看整个扫描"),
                                    false,
                                ))
                            })
                            .child(b("clear-type", t("Clear type ×", "移除类型 ×"), false)),
                    )
                })
                .when(self.columns_open, |p| {
                    p.child(self.file_display_options(cx))
                })
                .when(self.view.files.is_empty(), |p| {
                    p.child(self.empty_files(cx))
                })
                .when(!self.view.files.is_empty(), |p| {
                    p.child(self.file_table(compact, width - rail - 42., cx))
                })
                .into_any_element()
        } else {
            // Leave room for the expander, Shell icon and name before the fixed detail columns.
            let available = split_width;
            let tree_width = if compact {
                available
            } else {
                available * self.split
            };
            let short = tree_width < 530.;
            let details = (1..TREE_COLUMNS.len())
                .filter(|i| self.tree_columns & (1 << i) != 0)
                .map(|i| tree_detail_width(i, short) + 8.)
                .sum::<f32>();
            let name_min =
                (tree_width - 20. - details).clamp((tree_width - 108.).clamp(70., 116.), 180.);
            let tree_min_width = px(name_min + 16. + details);
            let name_width = tree_min_width.max(px(tree_width)) - px(16. + details);
            let left = div()
                .when(!compact, |d| d.flex_none().w(px(tree_width)))
                .when(compact, |d| d.flex_1())
                .h_full()
                .flex()
                .flex_col()
                .child(
                    self.line()
                        .px_2()
                        .py(px(2.))
                        .child(
                            div()
                                .flex_1()
                                .font_weight(FontWeight::SEMIBOLD)
                                .child(t("Directory tree", "目录树")),
                        )
                        .child(b(
                            "tree-columns",
                            t("Columns", "列"),
                            self.tree_columns_open,
                        )),
                )
                .when(self.tree_columns_open, |p| {
                    p.child(self.column_menu(false, cx))
                })
                .child(
                    div()
                        .relative()
                        .flex_1()
                        .min_h_0()
                        .child(
                            div()
                                .id("tree-horizontal")
                                .size_full()
                                .overflow_x_scroll()
                                .track_scroll(&self.tree_horizontal)
                                .child(
                                    div()
                                        .flex()
                                        .flex_col()
                                        .h_full()
                                        .w_full()
                                        .min_w(tree_min_width)
                                        .when(f32::from(tree_min_width) > tree_width - 2., |d| {
                                            d.pb(px(10.))
                                        })
                                        .child(
                                            self.table_header(false, None, short, cx)
                                                .min_w(tree_min_width),
                                        )
                                        .child(self.listing(false, Some(name_width), short, cx)),
                                ),
                        )
                        .child(
                            div().absolute().inset_0().child(
                                Scrollbar::horizontal(&self.tree_horizontal)
                                    .scrollbar_show(ScrollbarShow::Always),
                            ),
                        ),
                );
            let map_width = if compact {
                available
            } else {
                available - tree_width
            };
            let stacked = map_width < 420.;
            let narrow_header = map_width < 265.;
            let controls = div()
                .flex_none()
                .flex()
                .when(narrow_header, |d| d.w_full())
                .items_center()
                .gap_1()
                .child(
                    div()
                        .mr_1()
                        .font_weight(FontWeight::SEMIBOLD)
                        .child(t("Treemap", "空间图")),
                )
                .child(
                    div()
                        .flex()
                        .items_center()
                        .p(px(2.))
                        .h(px(28.))
                        .rounded(px(6.))
                        .bg(Hsla::from(self.divider()).opacity(0.30))
                        .child(self.map_nav_button(
                            "back",
                            "arrow-left",
                            t("Back (Alt+Left)", "后退 (Alt+←)"),
                            !self.history.is_empty(),
                            cx,
                        ))
                        .child(self.map_nav_button(
                            "forward",
                            "arrow-right",
                            t("Forward (Alt+Right)", "前进 (Alt+→)"),
                            !self.future.is_empty(),
                            cx,
                        ))
                        .child(self.map_nav_button(
                            "up",
                            "arrow-up",
                            t("Parent folder (Alt+Up)", "上一级 (Alt+↑)"),
                            self.root != 0,
                            cx,
                        )),
                );
            let colors = self.command("colors", false, cx)
                    .child(if self.by_type {t("Color: type", "按类型着色")} else {t("Color: branch", "按分支着色")})
                    .tooltip({
                        let hint = if self.by_type {
                            t("Known file types share a color; other files use their branch color. Click for branch colors.",
                              "已分类文件按类型着色，其他文件沿用目录分支色。点击切换为按分支着色。")
                        } else {
                            t("Each top-level branch of this view has a color. Click for file type colors.",
                              "当前视图的每个一级分支使用一种颜色。点击切换为按文件类型着色。")
                        };
                        move |w,cx| gpui_component::tooltip::Tooltip::new(hint).build(w,cx)
                    });
            let trail = self.breadcrumb_trail(
                (map_width
                    - if narrow_header {
                        116.
                    } else if stacked {
                        16.
                    } else {
                        252.
                    })
                .max(24.),
                w,
                cx,
            );
            let mut right = div().flex().flex_col().flex_1().min_w_0().h_full().child(
                div()
                    .flex()
                    .px_2()
                    .py(px(2.))
                    .gap_1()
                    .items_center()
                    .when(stacked, |d| d.flex_wrap())
                    .child(controls)
                    .child(colors)
                    .child(
                        trail
                            .when(stacked && !narrow_header, |d| {
                                d.flex_none().w_full().h(px(30.))
                            })
                            .when(!stacked || narrow_header, |d| d.flex_1()),
                    ),
            );
            if self.view.filtered()
                && let Some(tree) = &self.tree
            {
                right = right.child(
                    div()
                        .px_2()
                        .pb_1()
                        .text_xs()
                        .text_color(self.muted())
                        .child(format!(
                            "{} {} / {} {}",
                            t("Showing", "当前显示"),
                            format_size(self.view.weights[self.root], self.unit),
                            t("Scan total", "扫描总计"),
                            format_size(tree.nodes[0].weight(self.options.allocated), self.unit)
                        )),
                );
            }
            let handle = cx.weak_entity();
            let zh = self.zh;
            right = right.child(self.treemap(cx).id("treemap-menu").context_menu(
                move |mut menu, _, _| {
                    for (cmd, en, cn) in [
                        ("back", "Back", "后退"),
                        ("forward", "Forward", "前进"),
                        ("up", "Up one level", "上一级"),
                        ("home", "Scan root", "扫描根目录"),
                        ("zoom", "Zoom into folder", "进入目录"),
                        ("copy", "Copy path", "复制路径"),
                        ("tag1", "Red tag (1)", "红色标记 (1)"),
                        ("tag2", "Yellow tag (2)", "黄色标记 (2)"),
                        ("tag3", "Green tag (3)", "绿色标记 (3)"),
                        ("tag4", "Blue tag (4)", "蓝色标记 (4)"),
                        (
                            "clear-tags",
                            "Clear tags in this view (0)",
                            "清除当前范围的标记 (0)",
                        ),
                        ("unknown", "Unknown space", "未知空间"),
                        ("cell", "Minimum cell size…", "最小色块…"),
                        ("png", "Save PNG…", "保存 PNG…"),
                    ] {
                        let h = handle.clone();
                        menu = menu.item(PopupMenuItem::new(if zh { cn } else { en }).on_click(
                            move |_, w, cx| {
                                let _ = h.update(cx, |s, cx| s.action(cmd, w, cx));
                            },
                        ));
                    }
                    menu
                },
            ));
            let panes = self
                .panel()
                .relative()
                .flex_1()
                .min_h_0()
                .flex()
                .when(!compact || !self.compact_map, |d| d.child(left))
                .when(!compact, |d| {
                    d.child(div().flex_none().w(px(1.)).h_full().bg(self.divider()))
                })
                .when(!compact || self.compact_map, |d| d.child(right))
                .when(!compact, |d| {
                    d.child(
                        div()
                            .id("analysis-splitter")
                            .absolute()
                            .left(px(tree_width - 4.))
                            .h_full()
                            .w(px(9.))
                            .flex()
                            .justify_center()
                            .group("splitter")
                            .cursor_col_resize()
                            .child(
                                div()
                                    .w(px(2.))
                                    .h_full()
                                    .when(self.split_drag.is_some(), |d| {
                                        d.bg(self.selection_accent())
                                    })
                                    .group_hover("splitter", |d| d.bg(self.palette().accent)),
                            )
                            .on_mouse_down(
                                MouseButton::Left,
                                cx.listener(|s, e: &MouseDownEvent, _, cx| {
                                    cx.stop_propagation();
                                    s.drag_origin = None;
                                    s.split_drag = Some((e.position.x, s.split));
                                    cx.notify();
                                }),
                            ),
                    )
                });
            if compact && self.motion_enabled {
                panes
                    .with_animation(
                        ("compact-view", self.compact_map as usize),
                        Animation::new(Duration::from_millis(110)),
                        |d, t| d.opacity(0.80 + 0.20 * t),
                    )
                    .into_any_element()
            } else {
                panes.into_any_element()
            }
        };
        page = page.child(body);
        let status = if self.status.is_empty() {
            t("Ready", "就绪").to_owned()
        } else {
            self.status.clone()
        };
        let status_text = div()
            .id("result-status")
            .truncate()
            .text_xs()
            .text_color(self.muted())
            .child(status.clone())
            .tooltip(move |w, cx| {
                gpui_component::tooltip::Tooltip::new(status.clone()).build(w, cx)
            });
        let mut footer = div()
            .flex()
            .items_center()
            .gap_2()
            .min_w_0()
            .flex_none()
            .when(loaded, |d| d.h(px(30.)));
        if loaded
            && let Some(tree) = &self.tree
            && ((self.pane == 0 && self.explicit_selection && self.view.included[self.selected])
                || (self.pane == 1 && !self.multi.is_empty()))
        {
            let path = tree.path(self.selected);
            let live = tree.live
                && self.selected != 0
                && !path.ancestors().any(|p| self.removed.contains(p));
            let multiple = self.pane == 1 && self.multi.len() > 1;
            let roots = self.recycle_roots(tree);
            let scope = self.filtered_folder_scope(tree, &roots);
            if let Some((label, _)) = &scope {
                page = page.child(
                    div()
                        .flex_none()
                        .font_weight(FontWeight::MEDIUM)
                        .child(label.clone()),
                );
            }
            let label = if multiple {
                format!("{} {}", self.multi.len(), t("items selected", "项已选中"))
            } else {
                tree.nodes[self.selected]
                    .display_name()
                    .to_string_lossy()
                    .into_owned()
            };
            let weight = roots.iter().map(|&id| self.view.weights[id]).sum();
            let parent = path
                .parent()
                .map(|p| p.display().to_string())
                .unwrap_or_default();
            let full = path.display().to_string();
            if self.path_focus.is_focused(w) {
                page = page.child(self.panel().px_2().py_1().child(full.clone()));
            }
            footer = footer.child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .min_w_0()
                    .max_w(px(content_width * 0.34))
                    .id("selection-path")
                    .track_focus(&self.path_focus)
                    .focus(|d| d.bg(self.palette().accent_soft))
                    .on_click(cx.listener(|s, _, w, _| s.path_focus.focus(w)))
                    .on_key_down(cx.listener(|s, key: &KeyDownEvent, w, cx| {
                        if key.keystroke.key == "escape" {
                            s.focus.focus(w);
                            cx.stop_propagation();
                        }
                    }))
                    .child(
                        div()
                            .min_w_0()
                            .truncate()
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(label),
                    )
                    .when(scope.is_none(), |d| {
                        d.child(div().flex_none().child(format_size(weight, self.unit)))
                    })
                    .tooltip(move |w, cx| {
                        gpui_component::tooltip::Tooltip::new(full.clone()).build(w, cx)
                    }),
            );
            footer = footer.child(div().flex_none().w(px(1.)).h(px(16.)).bg(self.divider()));
            if live {
                footer = footer
                    .child(b("reveal", t("Explore", "定位"), false))
                    .child(b("open", t("Open", "打开"), false));
            }
            footer = footer.child(self.selection_more(cx));
            if !multiple && !parent.is_empty() && content_width > 960. {
                footer = footer.child(
                    div()
                        .min_w_0()
                        .truncate()
                        .text_xs()
                        .text_color(self.muted())
                        .child(parent),
                );
            }
            footer = footer.child(
                div()
                    .flex_1()
                    .min_w_0()
                    .flex()
                    .justify_end()
                    .child(status_text),
            );
            if live {
                footer = footer.child(
                    div()
                        .ml_1()
                        .pl_3()
                        .border_l_1()
                        .border_color(self.divider())
                        .child(b(
                            "recycle",
                            self.recycle_caption(scope.is_some(), roots.len()),
                            false,
                        )),
                );
            }
        } else {
            footer = footer.child(status_text);
        }
        shell
            .when(!w.is_fullscreen(), |d| d.child(self.title_bar(w)))
            .child(
                workspace
                    .child(self.sidebar(rail, cx))
                    .child(page.child(footer))
                    .when_some(self.split_drag, |d, (start_x, start_split)| {
                        d.child(
                            div()
                                .absolute()
                                .inset_0()
                                .block_mouse_except_scroll()
                                .cursor_col_resize()
                                .on_mouse_move(cx.listener(move |s, e: &MouseMoveEvent, _, cx| {
                                    if e.pressed_button == Some(MouseButton::Left) {
                                        s.split = (start_split
                                            + f32::from(e.position.x - start_x)
                                                / split_width.max(1.))
                                        .clamp(0.2, 0.8);
                                    } else {
                                        s.split_drag = None;
                                    }
                                    cx.notify();
                                }))
                                .on_mouse_up(MouseButton::Left, cx.listener(end_drag)),
                        )
                    }),
            )
    }
}
