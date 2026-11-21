use super::*;
use gpui::{Svg, svg};
use gpui_component::{
    button::{Button, ButtonCustomVariant, ButtonVariants},
    menu::DropdownMenu,
    switch::Switch,
    tooltip::Tooltip,
};

pub(super) fn rail_width(width: f32) -> f32 {
    if width >= 1200. { 166. } else { 56. }
}

pub(super) fn glyph(symbol: &'static str, size: f32) -> Div {
    div()
        .flex_none()
        .font_family("Segoe MDL2 Assets")
        .text_size(px(size))
        .child(symbol)
}

pub(super) fn icon(name: &str, size: f32, color: Rgba) -> Svg {
    svg()
        .path(SharedString::from(format!("icons/{name}.svg")))
        .flex_none()
        .size(px(size))
        .text_color(color)
}

impl Dashboard {
    fn command_surface(&self, id: &'static str) -> Stateful<Div> {
        div()
            .id(id)
            .flex_none()
            .h(px(30.))
            .rounded(px(6.))
            .flex()
            .items_center()
            .justify_center()
            .gap(px(6.))
            .border_1()
            .border_color(transparent_black())
            .text_size(px(13.))
            .text_color(self.palette().text_secondary)
            // Mouse commands keep keyboard focus in the working pane. Tab still shows a focus ring.
            .on_mouse_down(MouseButton::Left, |_, w, _| w.prevent_default())
    }

    pub(super) fn command(
        &self,
        id: &'static str,
        selected: bool,
        cx: &Context<Self>,
    ) -> Stateful<Div> {
        let p = self.palette();
        self.command_surface(id)
            .px(px(8.))
            .cursor_pointer()
            .focusable()
            .tab_stop(true)
            .when(selected, |d| {
                d.bg(p.accent_soft)
                    .text_color(p.accent_ink)
                    .font_weight(FontWeight::MEDIUM)
            })
            .hover(move |d| d.bg(p.hover))
            .active(move |d| d.bg(p.pressed))
            .focus(move |d| d.border_color(p.accent))
            .on_click(cx.listener(move |s, _, w, cx| s.action(id, w, cx)))
    }

    fn menu_command(&self, id: &'static str, outlined: bool, cx: &App) -> Button {
        let p = self.palette();
        let mut style = ButtonCustomVariant::new(cx)
            .foreground(p.text.into())
            .hover(p.hover.into())
            .active(p.pressed.into());
        if outlined {
            style = style.color(p.panel.into()).border(p.border_strong.into());
        }
        Button::new(id)
            .compact()
            .rounded(px(6.))
            .h(px(30.))
            .custom(style)
    }

    fn menu_label(&self, label: impl Into<SharedString>) -> Div {
        let p = self.palette();
        div()
            .flex()
            .items_center()
            .gap(px(6.))
            .text_size(px(13.))
            .text_color(p.text)
            .child(label.into())
            .child(icon("chevron-down", 12., p.muted))
    }
    pub(super) fn selection_more(&self, cx: &Context<Self>) -> AnyElement {
        let view = cx.weak_entity();
        let focus = self.focus.clone();
        let copy = self.text("Copy path", "复制路径");
        let tag = self.text("Tag", "标记");
        Button::new("selection-more")
            .compact()
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(6.))
                    .child(self.text("More", "更多"))
                    .child(icon("chevron-down", 12., self.palette().muted)),
            )
            .dropdown_menu_with_anchor(Corner::BottomRight, move |mut menu, _, _| {
                menu = menu.action_context(focus.clone());
                for (cmd, label) in [("copy", copy), ("tag", tag)] {
                    let view = view.clone();
                    menu = menu.item(PopupMenuItem::new(label).on_click(move |_, w, cx| {
                        let _ = view.update(cx, |s, cx| s.action(cmd, w, cx));
                    }));
                }
                menu
            })
            .into_any_element()
    }
    pub(super) fn map_nav_button(
        &self,
        id: &'static str,
        icon_name: &'static str,
        hint: &'static str,
        enabled: bool,
        cx: &Context<Self>,
    ) -> Stateful<Div> {
        let p = self.palette();
        let button = if enabled {
            self.command(id, false, cx)
        } else {
            self.command_surface(id).text_color(p.muted)
        };
        button
            .w(px(26.))
            .h(px(24.))
            .px_0()
            .rounded(px(6.))
            .child(icon(icon_name, 14., p.text_secondary))
            .tooltip(move |w, cx| Tooltip::new(hint).build(w, cx))
    }

    pub(super) fn file_display_options(&self, cx: &Context<Self>) -> Div {
        let limit = self.options.limit;
        let unlimited = self.text("Unlimited", "不限");
        let prefix = self.text("Display limit", "显示上限");
        let label = if limit == 0 {
            format!("{prefix} · {unlimited} ▾")
        } else if self.zh {
            format!("{prefix} {limit} 行 ▾")
        } else {
            format!("{prefix} {limit} rows ▾")
        };
        let view = cx.weak_entity();
        let focus = self.focus.clone();
        let limits = Button::new("file-limit")
            .label(label)
            .dropdown_menu_with_anchor(Corner::TopLeft, move |mut menu, _, _| {
                menu = menu.action_context(focus.clone());
                for value in [100, 1000, 10000, 50000, 0] {
                    let view = view.clone();
                    let text = if value == 0 {
                        unlimited.to_owned()
                    } else {
                        value.to_string()
                    };
                    menu = menu.item(PopupMenuItem::new(text).checked(value == limit).on_click(
                        move |_, _, cx| {
                            let _ = view.update(cx, |s, cx| {
                                s.options.limit = value;
                                s.rebuild();
                                if let Err(error) = s.persist_view() {
                                    s.fail(error);
                                }
                                cx.notify();
                            });
                        },
                    ));
                }
                menu
            });
        div()
            .flex()
            .flex_col()
            .gap_1()
            .child(self.line().child(limits).child(self.button(
                "show-dirs",
                self.text("Include folders", "含目录"),
                self.options.show_dirs,
                cx,
            )))
            .child(
                self.line()
                    .child(
                        div()
                            .text_color(self.muted())
                            .child(self.text("Columns", "列")),
                    )
                    .child(self.column_menu(true, cx)),
            )
    }

    pub(super) fn breadcrumb_trail(&self, available: f32, w: &Window, cx: &Context<Self>) -> Div {
        let mut trail = div()
            .flex()
            .items_center()
            .min_w_0()
            .overflow_hidden()
            .text_size(px(14.));
        let Some(tree) = &self.tree else { return trail };
        let mut segments = Vec::new();
        let mut id = self.root;
        loop {
            let label: SharedString = tree.nodes[id]
                .display_name()
                .to_string_lossy()
                .replace(['\n', '\r'], " ")
                .into();
            let mut style = w.text_style();
            if id == self.root {
                style.font_weight = FontWeight::SEMIBOLD;
            }
            let run = style.to_run(label.len());
            let line = w
                .text_system()
                .shape_line(label.clone(), px(14.), &[run], None);
            segments.push((id, label, f32::from(line.width).ceil() + 10.));
            if id == 0 {
                break;
            }
            id = tree.nodes[id].parent;
        }
        segments.reverse();
        let mut total = segments.iter().map(|s| s.2 + 12.).sum::<f32>() - 12.;
        let mut first = 0;
        if total > available && segments.len() > 2 {
            total += 40.;
            while total > available && first + 2 < segments.len() {
                total -= segments[first].2 + 12.;
                first += 1;
            }
        }
        if first > 0 {
            let hidden = segments[..first].to_vec();
            let view = cx.weak_entity();
            let snapshot = Arc::downgrade(tree);
            let focus = self.focus.clone();
            trail = trail.child(
                Button::new("crumb-overflow")
                    .label("…")
                    .w(px(28.))
                    .tooltip(self.text("Parent folders", "上层目录"))
                    .dropdown_menu_with_anchor(Corner::TopLeft, move |mut menu, _, _| {
                        menu = menu.action_context(focus.clone());
                        for (id, label, _) in &hidden {
                            let view = view.clone();
                            let snapshot = snapshot.clone();
                            let id = *id;
                            menu = menu.item(PopupMenuItem::new(label.clone()).on_click(
                                move |_, w, cx| {
                                    let _ = view.update(cx, |s, cx| {
                                        if s.tree.as_ref().is_some_and(|tree| {
                                            snapshot.ptr_eq(&Arc::downgrade(tree))
                                        }) {
                                            s.navigate(id);
                                            s.focus.focus(w);
                                            cx.notify();
                                        }
                                    });
                                },
                            ));
                        }
                        menu
                    }),
            );
        }
        let count = segments.len() - first;
        let room =
            (available - if first > 0 { 40. } else { 0. } - 12. * (count - 1) as f32).max(0.);
        // When only the last two names fit, let a short name give its spare width to its neighbor.
        let cap = if total <= available {
            f32::INFINITY
        } else if count == 2 {
            room - segments[first].2.min(segments[first + 1].2).min(room / 2.)
        } else {
            room
        };
        for (index, (id, label, width)) in segments.into_iter().enumerate().skip(first) {
            if index > 0 {
                trail = trail.child(
                    div()
                        .w(px(12.))
                        .flex_none()
                        .text_center()
                        .text_color(self.muted())
                        .child("›"),
                );
            }
            let current = id == self.root;
            let weight = if current {
                FontWeight::SEMIBOLD
            } else {
                FontWeight::NORMAL
            };
            let label = if width > cap {
                fit_name(label, px(cap - 8.), weight, w)
            } else {
                label
            };
            let tree = tree.clone();
            let crumb = div()
                .id(("crumb", id))
                .min_w_0()
                .px(px(4.))
                .py_1()
                .rounded_sm()
                .max_w(px(width.min(cap)))
                .tooltip(move |w, cx| {
                    Tooltip::new(tree.path(id).display().to_string()).build(w, cx)
                })
                .when(current, |d| d.font_weight(FontWeight::SEMIBOLD))
                .when(!current, |d| {
                    d.focusable()
                        .tab_stop(true)
                        .cursor_pointer()
                        .focus(|d| d.underline())
                        .text_color(self.selection_accent())
                        .hover(|d| d.bg(self.palette().hover))
                        .on_click(cx.listener(move |s, _, w, cx| {
                            s.navigate(id);
                            s.focus.focus(w);
                            cx.notify();
                        }))
                })
                .child(div().truncate().child(label));
            trail = trail.child(crumb);
        }
        trail
    }

    pub(crate) fn metric_position(&self) -> (f32, bool) {
        let target = self.options.allocated as u8 as f32;
        let Some((start, from)) = self.metric_slide else {
            return (target, false);
        };
        let t = (start.elapsed().as_secs_f32() / 0.14).min(1.);
        (target + (from - target) * (1. - t).powi(3), t < 1.)
    }

    pub(super) fn size_selector(&self, w: &Window, cx: &Context<Self>) -> Div {
        let p = self.palette();
        let (file_width, disk_width) = if self.zh { (84., 84.) } else { (88., 112.) };
        let (position, moving) = self.metric_position();
        if moving {
            w.request_animation_frame();
        }
        div()
            .flex_none()
            .relative()
            .flex()
            .items_center()
            .p(px(2.))
            .h(px(30.))
            .rounded(px(8.))
            .border_1()
            .border_color(p.border)
            .bg(p.seg_track)
            .child(
                div()
                    .absolute()
                    .top(px(2.))
                    .left(px(2. + position * file_width))
                    .w(px(file_width + position * (disk_width - file_width)))
                    .h(px(26.))
                    .rounded(px(6.))
                    .shadow_sm()
                    .border_1()
                    .border_color(p.thumb_border)
                    .bg(p.thumb),
            )
            .children(
                [
                    (
                        "logical-size",
                        self.text("File size", "文件大小"),
                        !self.options.allocated,
                        file_width,
                    ),
                    (
                        "allocated-size",
                        self.text("Space on disk", "磁盘占用"),
                        self.options.allocated,
                        disk_width,
                    ),
                ]
                .into_iter()
                .map(|(id, label, on, width)| {
                    self.command_surface(id)
                        .focusable()
                        .tab_stop(true)
                        .cursor_pointer()
                        .w(px(width))
                        .h(px(26.))
                        .rounded(px(6.))
                        .gap(px(5.))
                        .text_color(if on { p.text } else { p.muted })
                        .when(on, |d| d.font_weight(FontWeight::MEDIUM))
                        .hover(move |d| d.bg(p.hover))
                        .active(move |d| d.bg(p.pressed))
                        .focus(move |d| d.border_color(p.accent))
                        .child(label)
                        .on_click(cx.listener(move |s, _, w, cx| s.action(id, w, cx)))
                }),
            )
    }

    pub(super) fn title_bar(&self, window: &Window) -> Div {
        let maximized = window.is_maximized();
        let p = self.palette();
        let caption_button = |id: &'static str, area, symbol, label: &'static str| {
            div()
                .id(id)
                .window_control_area(area)
                .w(px(46.))
                .h_full()
                .flex()
                .items_center()
                .justify_center()
                .hover(move |d| {
                    if id == "window-close" {
                        d.bg(rgb(0xc42b38)).text_color(rgb(0xffffff))
                    } else {
                        d.bg(p.hover)
                    }
                })
                .child(glyph(symbol, 11.))
                .tooltip(move |w, cx| Tooltip::new(label).build(w, cx))
        };
        div()
            .flex_none()
            .flex()
            .h(px(36.))
            // Leave the top edge to the platform resize hit test in restored windows.
            .when(!maximized, |d| d.pt(px(4.)))
            .bg(p.canvas)
            .border_b_1()
            .border_color(p.border)
            .child(
                div()
                    .id("title-drag")
                    .window_control_area(WindowControlArea::Drag)
                    .flex_1()
                    .h_full()
                    .pl_3()
                    .gap_2()
                    .flex()
                    .items_center()
                    .child(img(self.brand.clone()).size(px(20.)))
                    .child(
                        div()
                            .text_size(px(12.5))
                            .text_color(p.text_secondary)
                            .font_weight(FontWeight::MEDIUM)
                            .child("DiskCleaner"),
                    ),
            )
            .child(caption_button(
                "window-minimize",
                WindowControlArea::Min,
                "\u{E921}",
                self.text("Minimize", "最小化"),
            ))
            .child(caption_button(
                "window-maximize",
                WindowControlArea::Max,
                if maximized { "\u{E923}" } else { "\u{E922}" },
                if maximized {
                    self.text("Restore", "还原")
                } else {
                    self.text("Maximize", "最大化")
                },
            ))
            .child(caption_button(
                "window-close",
                WindowControlArea::Close,
                "\u{E8BB}",
                self.text("Close", "关闭"),
            ))
    }

    pub(super) fn muted(&self) -> Rgba {
        self.palette().muted
    }

    fn nav_item(
        &self,
        id: &'static str,
        icon_name: &'static str,
        label: &'static str,
        active: bool,
        wide: bool,
        cx: &Context<Self>,
    ) -> Stateful<Div> {
        let p = self.palette();
        div()
            .id(id)
            .focusable()
            .tab_stop(true)
            .focus(move |d| d.bg(p.rail_hover))
            .h(px(30.))
            .rounded(px(6.))
            .flex()
            .items_center()
            .when(wide, |d| d.px_2().gap_2())
            .when(!wide, |d| d.justify_center())
            .when(active, move |d| d.bg(p.rail_active))
            .text_color(if active { p.text } else { p.rail_text })
            .hover(move |d| d.bg(p.rail_hover))
            .cursor_pointer()
            .child(icon(
                icon_name,
                16.,
                if active { p.text } else { p.rail_text },
            ))
            .when(wide, |d| d.child(label))
            .tooltip(move |w, cx| Tooltip::new(label).build(w, cx))
            .on_click(cx.listener(move |s, _, w, cx| s.action(id, w, cx)))
    }

    pub(super) fn sidebar(&self, width: f32, cx: &Context<Self>) -> Div {
        let wide = width > 60.;
        let p = self.palette();
        let t = |en, zh| self.text(en, zh);
        let nav = |id, ic, label, on| self.nav_item(id, ic, label, on, wide, cx);
        let rule = || div().h(px(1.)).w_full().bg(p.border);
        let mut drives = div()
            .id("drive-shortcuts")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap_1();
        for drive in platform::drives() {
            let letter = drive.trim_end_matches('\\').to_owned();
            let label = self
                .drive_labels
                .get(&drive)
                .filter(|label| !label.is_empty());
            let name = label.map_or_else(|| letter.clone(), |label| format!("{label} ({letter})"));
            let hint = format!("{} {name}", t("Scan", "扫描"));
            let active = self.active_drive.as_ref() == Some(&drive);
            drives = drives.child(
                div()
                    .id(SharedString::from(drive.clone()))
                    .focusable()
                    .tab_stop(true)
                    .focus(move |d| d.bg(p.rail_hover))
                    .flex_none()
                    .h(px(30.))
                    .flex()
                    .items_center()
                    .gap_3()
                    .rounded_md()
                    .relative()
                    .when(active, move |d| d.bg(p.rail_active))
                    .cursor_pointer()
                    .hover(move |d| d.bg(p.rail_hover))
                    .when(wide, move |d| {
                        d.px_2()
                            .gap_1()
                            .child(icon("hard-drive", 15., p.text_secondary))
                    })
                    .when(wide && label.is_some(), |d| {
                        d.child(
                            div()
                                .min_w_0()
                                .truncate()
                                .child(label.cloned().unwrap_or_default()),
                        )
                    })
                    .when(!wide, |d| d.justify_center())
                    .child(div().flex_none().child(if wide && label.is_some() {
                        format!("({letter})")
                    } else {
                        letter
                    }))
                    .tooltip(move |w, cx| Tooltip::new(hint.clone()).build(w, cx))
                    .on_click(cx.listener(move |s, _, w, cx| {
                        s.target
                            .update(cx, |v, cx| v.set_value(drive.clone(), w, cx));
                        s.scan_location(vec![PathBuf::from(&drive)], w, cx);
                    })),
            );
        }
        div()
            .w(px(width))
            .flex_none()
            .h_full()
            .bg(p.rail)
            .text_color(p.rail_text)
            .flex()
            .flex_col()
            .px_2()
            .py_3()
            .gap_1()
            .child(nav(
                "tree",
                "dashboard",
                t("Disk overview", "空间概览"),
                !self.menu && self.pane == 0,
            ))
            .child(nav(
                "files",
                "list",
                t("File list", "文件列表"),
                !self.menu && self.pane == 1,
            ))
            .child(div().my_2().child(rule()))
            .when(wide, |d| {
                d.child(
                    div()
                        .px_3()
                        .text_xs()
                        .text_color(p.rail_muted)
                        .child(t("Drives", "磁盘")),
                )
            })
            .child(drives)
            .child(rule())
            .child(nav("menu", "settings", t("Settings", "设置"), self.menu))
            .child(nav(
                "about",
                "info",
                t("About", "关于"),
                !self.menu && self.pane == 2,
            ))
    }

    pub(super) fn heading(&self, width: f32, cx: &Context<Self>) -> Div {
        let t = |en, zh| self.text(en, zh);
        let loaded = !self.menu && self.pane != 2 && self.tree.is_some();
        if loaded && self.location_editor && self.cancel.is_none() {
            return div()
                .flex()
                .items_center()
                .gap_2()
                .child(div().text_color(self.muted()).child(t("Location", "位置")))
                .child(self.location_input(true, cx));
        }
        let title = if self.menu {
            t("Settings", "设置")
        } else {
            match self.pane {
                1 => t("File list", "文件列表"),
                2 => t("About DiskCleaner", "关于 DiskCleaner"),
                _ => t("Disk overview", "空间概览"),
            }
        };
        if !loaded {
            return div()
                .font_weight(FontWeight::SEMIBOLD)
                .text_size(px(20.))
                .child(title);
        }
        let location = self.snapshot_location();
        let hint = format!("{} · {}", location, t("Change location", "更改位置"));
        let display_location = if width < 710. && self.sources.len() == 1 {
            std::path::Path::new(&location)
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or(&location)
        } else {
            &location
        };
        let refresh_hint = if self.snapshot_dirty {
            t(
                "Disk changed · Refresh snapshot (F5)",
                "磁盘内容已变更 · 刷新快照 (F5)",
            )
        } else {
            t("Refresh snapshot (F5)", "刷新快照 (F5)")
        };
        let location_label = div()
            .min_w_0()
            .truncate()
            .font_weight(FontWeight::MEDIUM)
            .child(display_location.to_owned());
        let p = self.palette();
        div()
            .flex()
            .flex_none()
            .min_w_0()
            .h(px(30.))
            .items_center()
            .gap_1()
            .child(if self.cancel.is_none() {
                self.command("change-location", false, cx)
                    .flex_shrink()
                    .min_w_0()
                    .max_w(px((width * 0.25).min(330.)))
                    .child(icon("folder", 14., p.text_secondary))
                    .child(location_label)
                    .child(icon("chevron-down", 12., p.muted))
                    .tooltip(move |w, cx| Tooltip::new(hint.clone()).build(w, cx))
                    .into_any_element()
            } else {
                location_label.max_w(px(width * 0.25)).into_any_element()
            })
            .child(
                div()
                    .flex_none()
                    .w(px(1.))
                    .h(px(16.))
                    .mx_1()
                    .bg(self.divider()),
            )
            .child(self.summary(width, cx))
            .child(div().flex_1())
            .map(|d| {
                if self.cancel.is_some() {
                    return d.when(self.scan_items.is_none(), |d| {
                        d.child(self.button("cancel", t("Cancel export", "取消导出"), false, cx))
                    });
                }
                let handle = cx.weak_entity();
                let focus = self.focus.clone();
                let zh = self.zh;
                d.when(self.snapshot_dirty && width >= 740., |d| {
                    d.child(
                        div()
                            .flex_none()
                            .text_xs()
                            .text_color(self.palette().warn)
                            .child(t("Disk changed", "磁盘已变更")),
                    )
                })
                .child(
                    self.command("refresh", self.snapshot_dirty, cx)
                        .child(icon("refresh", 14., p.text_secondary))
                        .when(width >= 710., |d| d.child(t("Refresh", "刷新")))
                        .tooltip(move |w, cx| Tooltip::new(refresh_hint).build(w, cx)),
                )
                .child(
                    self.menu_command("export-menu", true, cx)
                        .child(
                            div()
                                .flex()
                                .items_center()
                                .gap(px(6.))
                                .text_color(p.text)
                                .child(icon("upload", 14., p.text_secondary))
                                .child(self.menu_label(t("Export", "导出"))),
                        )
                        .dropdown_menu_with_anchor(Corner::TopRight, move |mut menu, _, cx| {
                            menu = menu.action_context(focus.clone());
                            let Some(dashboard) = handle.upgrade() else {
                                return menu;
                            };
                            let s = dashboard.read(cx);
                            let Some(tree) = &s.tree else { return menu };
                            let full = tree.nodes[0].files;
                            let current_types = if s.types {
                                s.type_rows().iter().map(|(_, totals)| totals.files).sum()
                            } else if s.pane == 1 {
                                s.view.result_totals.files
                            } else {
                                s.view.totals(tree, 0).files
                            };
                            let (files, dirs) =
                                if s.export_opts.depth == 0 && !s.export_opts.selected {
                                    let total = if s.pane == 1 {
                                        s.view.result_totals
                                    } else {
                                        s.view.totals(tree, 0)
                                    };
                                    (
                                        if s.export_opts.files { total.files } else { 0 },
                                        if s.export_opts.folders {
                                            total.folders
                                        } else {
                                            0
                                        },
                                    )
                                } else {
                                    s.csv_rows(tree).fold((0u64, 0u64), |(f, d), id| {
                                        if tree.nodes[id].dir {
                                            (f, d + 1)
                                        } else {
                                            (f + 1, d)
                                        }
                                    })
                                };
                            for (id, en, cn) in [
                                ("csv", "Current results CSV", "当前结果 CSV"),
                                ("export-panel", "CSV options…", "CSV 导出设置…"),
                                ("full", "Full snapshot CSV", "完整快照 CSV"),
                                ("types-csv", "Current file types CSV", "当前文件类型 CSV"),
                                ("full-types", "All file types CSV", "完整扫描文件类型 CSV"),
                                ("png", "Treemap PNG…", "空间图 PNG…"),
                                ("mft", "Volume MFT…", "卷 MFT…"),
                            ] {
                                let h = handle.clone();
                                let label = if zh { cn } else { en };
                                let label = match id {
                                    "csv" if zh => {
                                        format!("{label} · {files} 个文件 / {dirs} 个目录…")
                                    }
                                    "csv" => format!("{label} · {files} files / {dirs} folders…"),
                                    "types-csv" | "full-types" | "full" => {
                                        let count = if id == "types-csv" {
                                            current_types
                                        } else {
                                            full
                                        };
                                        format!(
                                            "{label} · {count} {}…",
                                            if zh { "个文件" } else { "files" }
                                        )
                                    }
                                    _ => label.to_owned(),
                                };
                                menu = menu.item(PopupMenuItem::new(label).on_click(
                                    move |_, w, cx| {
                                        let _ = h.update(cx, |s, cx| s.action(id, w, cx));
                                    },
                                ));
                            }
                            menu
                        }),
                )
            })
    }

    fn summary(&self, width: f32, cx: &App) -> Div {
        let t = |en, zh| self.text(en, zh);
        let root = &self.tree.as_ref().expect("loaded scan").nodes[0];
        let metric = if self.options.allocated {
            t("Space on disk", "磁盘占用")
        } else {
            t("File size", "文件大小")
        };
        let totals = div()
            .flex_none()
            .flex()
            .items_center()
            .gap_2()
            .child(
                div()
                    .text_color(self.muted())
                    .child(if self.options.allocated {
                        t("Scan on disk", "扫描占用")
                    } else {
                        t("Scan size", "扫描大小")
                    }),
            )
            .child(
                div()
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_size(px(17.))
                    .child(format_size(root.weight(self.options.allocated), self.unit)),
            )
            .id("scan-totals")
            .tooltip({
                let info = format!(
                    "{} · {} {} · {} {}",
                    metric,
                    root.files,
                    t("files", "个文件"),
                    root.folders,
                    t("folders", "个目录")
                );
                move |w, cx| Tooltip::new(info.clone()).build(w, cx)
            })
            .into_any_element();
        let mut row = div()
            .flex_none()
            .flex()
            .items_center()
            .gap_2()
            .child(totals);
        if let Some((total, free)) = self.volume {
            let free_label = format!(
                "{} {}",
                t("Volume free", "卷可用"),
                format_size(free, self.unit)
            );
            let used_label = format!(
                "{} {}",
                t("Used", "已用"),
                format_size(total.saturating_sub(free), self.unit)
            );
            let total_label = format!(
                "{} {}",
                t("Capacity", "总容量"),
                format_size(total, self.unit)
            );
            row = row.child(
                self.menu_command("volume-details", false, cx)
                    .child(self.menu_label(if width < 710. {
                        format_size(free, self.unit)
                    } else {
                        free_label.clone()
                    }))
                    .dropdown_menu_with_anchor(Corner::TopRight, move |menu, _, _| {
                        menu.label(free_label.clone())
                            .label(used_label.clone())
                            .label(total_label.clone())
                    }),
            );
        }
        row.when(width >= 920., |d| {
            d.child(div().text_xs().text_color(self.muted()).child(format!(
                "{} {}  ·  {} {}",
                root.files,
                t("files", "个文件"),
                root.folders,
                t("folders", "个目录")
            )))
        })
    }

    fn location_input(&self, loaded: bool, cx: &Context<Self>) -> Div {
        let t = |en, zh| self.text(en, zh);
        let paths = locations(&self.target.read(cx).value());
        let action = match paths.as_slice() {
            [] => t("Scan", "扫描").to_owned(),
            [p] if p.has_root()
                && p.parent().is_none()
                && p.to_string_lossy().as_bytes().get(1) == Some(&b':') =>
            {
                let drive = p
                    .to_string_lossy()
                    .chars()
                    .next()
                    .unwrap()
                    .to_ascii_uppercase();
                if self.zh {
                    format!("扫描 {drive} 盘")
                } else {
                    format!("Scan {drive}:")
                }
            }
            [p] if p.extension().is_some_and(|ext| {
                ["csv", "tsv", "mft", "bin"]
                    .iter()
                    .any(|e| ext.eq_ignore_ascii_case(e))
            }) =>
            {
                t("Open location", "打开此位置").to_owned()
            }
            [_] => t("Scan this folder", "扫描此文件夹").to_owned(),
            paths => {
                if self.zh {
                    format!("扫描 {} 个位置", paths.len())
                } else {
                    format!("Scan {} locations", paths.len())
                }
            }
        };
        div()
            .flex()
            .flex_1()
            .min_w_0()
            .gap_2()
            .items_center()
            .child(
                div()
                    .flex_1()
                    .min_w_0()
                    .child(Input::new(&self.target).border_color(self.palette().input_border)),
            )
            .when(loaded, |d| {
                d.child(
                    self.button("folder", t("Browse…", "浏览…"), false, cx)
                        .h(px(30.)),
                )
                .child(
                    self.button("import", t("Import…", "导入…"), false, cx)
                        .h(px(30.)),
                )
            })
            .child(self.button("scan", action, true, cx).h(px(30.)))
            .when(loaded, |d| {
                d.child(
                    self.button("close-location", t("Cancel", "取消"), false, cx)
                        .h(px(30.)),
                )
            })
    }

    pub(super) fn location_row(&self, cx: &Context<Self>) -> Div {
        let t = |en, zh| self.text(en, zh);
        let p = self.palette();
        let row = self.panel().flex_none().p_2().flex().items_center().gap_2();
        if let Some(count) = self.scan_items {
            let canceling = self
                .cancel
                .as_ref()
                .is_some_and(|c| c.load(Ordering::Relaxed));
            let path = self.target.read(cx).value();
            return row
                .child(icon("folder", 15., p.muted))
                .child(
                    div()
                        .id("scan-location")
                        .flex_1()
                        .min_w_0()
                        .truncate()
                        .child(path.clone())
                        .tooltip(move |w, cx| Tooltip::new(path.clone()).build(w, cx)),
                )
                .child(format!(
                    "{} · {count} {}",
                    if canceling {
                        t("Canceling…", "正在取消…")
                    } else {
                        t("Scanning…", "正在扫描…")
                    },
                    t("items found", "项已发现")
                ))
                .child(self.button("cancel", t("Cancel", "取消扫描"), false, cx));
        }
        if self.cancel.is_some() {
            return row
                .child(div().flex_1().child(t("Exporting…", "正在导出…")))
                .child(self.button("cancel", t("Cancel export", "取消导出"), false, cx));
        }
        row
    }

    pub(super) fn welcome(&self, cx: &Context<Self>) -> Div {
        let t = |en, zh| self.text(en, zh);
        let p = self.palette();
        let busy = self.cancel.is_some();
        self.panel()
            .flex_1()
            .min_h_0()
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap_3()
            .px_4()
            .child(
                div()
                    .size(px(60.))
                    .rounded(px(16.))
                    .flex()
                    .items_center()
                    .justify_center()
                    .bg(p.accent_soft)
                    .child(icon("folder", 28., p.accent)),
            )
            .child(div().text_xl().font_weight(FontWeight::SEMIBOLD).child(
                if self.scan_items.is_some() {
                    t("Scanning location…", "正在扫描位置…")
                } else if busy {
                    t("Exporting…", "正在导出…")
                } else {
                    t("Scan a folder or drive", "扫描文件夹或磁盘")
                },
            ))
            .when(!busy, |d| {
                d.child(div().text_color(self.muted()).text_center().child(t(
                    "Paste a path, choose a folder, or drop it here.",
                    "粘贴路径、选择文件夹，或将文件夹拖到这里。",
                )))
            })
            .when(!busy, |d| {
                d.child(
                    div()
                        .w_full()
                        .max_w(px(560.))
                        .flex()
                        .child(self.location_input(false, cx)),
                )
                .child(
                    self.line()
                        .justify_center()
                        .child(self.button("folder", t("Choose folder…", "选择文件夹…"), false, cx))
                        .child(self.button(
                            "import",
                            t("Import snapshot…", "导入快照…"),
                            false,
                            cx,
                        )),
                )
            })
    }

    pub(super) fn about_page(&self) -> Div {
        self.panel()
            .flex_1()
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap_2()
            .child(img(self.brand.clone()).size(px(70.)))
            .child(
                div()
                    .text_2xl()
                    .font_weight(FontWeight::SEMIBOLD)
                    .child("DiskCleaner"),
            )
            .child(self.text("Windows disk space analyzer", "Windows 磁盘空间分析器"))
            .child(
                div()
                    .text_color(self.muted())
                    .child(format!("{}  ·  Rust + GPUI", env!("CARGO_PKG_VERSION"))),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(self.muted())
                    .child("Fonts · MiSans (Xiaomi)"),
            )
    }

    pub(super) fn settings_page(&self, cx: &Context<Self>) -> Stateful<Div> {
        let t = |en, zh| self.text(en, zh);
        let section = |heading| {
            self.panel()
                .flex_none()
                .px_4()
                .py_3()
                .flex()
                .flex_col()
                .child(
                    div()
                        .mb_2()
                        .text_size(px(16.))
                        .font_weight(FontWeight::SEMIBOLD)
                        .child(heading),
                )
        };
        let row = |label, id, value: String| {
            div()
                .flex()
                .items_center()
                .gap_2()
                .py_1()
                .child(div().flex_1().child(label))
                .child(self.button(id, value, false, cx))
        };
        let toggle = |label: &'static str, id: &'static str, checked| {
            div()
                .flex()
                .h(px(38.))
                .items_center()
                .gap_2()
                .child(div().flex_1().child(label))
                .child(
                    Switch::new(id)
                        .checked(checked)
                        .tooltip(label)
                        .on_click(cx.listener(move |s, _, w, cx| s.action(id, w, cx))),
                )
        };
        div()
            .id("settings-scroll")
            .flex_1()
            .min_h_0()
            .overflow_y_scroll()
            .flex()
            .flex_col()
            .gap_2()
            .child(
                section(t("Appearance & language", "外观与语言"))
                    .child(row(
                        t("Theme", "主题"),
                        "theme",
                        if self.system_theme {
                            t("System", "跟随系统")
                        } else if self.dark {
                            t("Dark", "深色")
                        } else {
                            t("Light", "浅色")
                        }
                        .into(),
                    ))
                    .child(row(
                        t("Language", "界面语言"),
                        "language",
                        if self.zh { "简体中文" } else { "English" }.into(),
                    ))
                    .child(row(
                        t("Size units", "容量单位"),
                        "unit",
                        [t("Automatic", "自动"), "B", "KiB", "MiB", "GiB", "TiB"][self.unit].into(),
                    )),
            )
            .child(
                section(t("Treemap", "空间图"))
                    .child(row(
                        t("Folder depth", "显示层级"),
                        "depth",
                        self.depth.to_string(),
                    ))
                    .child(toggle(
                        t("Color by file type", "按文件类型着色"),
                        "colors",
                        self.by_type,
                    ))
                    .child(toggle(t("Show names", "显示名称"), "labels", self.labels))
                    .child(row(
                        t("Minimum cell size", "最小色块尺寸"),
                        "cell",
                        format!("{} px", self.min_cell),
                    ))
                    .child(toggle(
                        t("Include free space", "显示可用空间"),
                        "free",
                        self.free,
                    ))
                    .child(toggle(
                        t("Include unknown space", "显示未知空间"),
                        "unknown",
                        self.unknown,
                    ))
                    .child(row(
                        t("PNG dimensions", "PNG 导出尺寸"),
                        "image-size",
                        format!("{} × {}", self.image_size.0, self.image_size.1),
                    ))
                    .child(toggle(t("Grayscale PNG", "灰度 PNG"), "gray", self.gray))
                    .child(row(
                        t("Tags in current view", "当前视图的标记"),
                        "clear-tags",
                        t("Clear tags", "清除标记").into(),
                    )),
            )
            .child(
                section(t("Windows integration", "Windows 集成")).child(
                    div().flex().gap_2().flex_wrap().children(
                        [
                            ("shell-on", t("Add Explorer menu", "启用右键菜单")),
                            ("shell-off", t("Remove Explorer menu", "移除右键菜单")),
                            ("admin", t("Restart as administrator", "以管理员身份重启")),
                            ("mft", t("Export volume MFT…", "导出卷 MFT…")),
                            ("update", t("Check for updates…", "检查更新…")),
                        ]
                        .into_iter()
                        .map(|(id, v)| self.button(id, v, false, cx)),
                    ),
                ),
            )
    }
}
