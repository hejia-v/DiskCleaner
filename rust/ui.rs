mod render;
mod row_icons;
use crate::{
    Args, locations,
    map::{self, Rect},
    platform,
    scan::{self, Tree},
    table,
    view::{self, Options, TypeRows, View, format_size},
};
use anyhow::{Context as _, Result, anyhow, ensure};
use gpui::{prelude::*, *};
use gpui_component::{
    Root, Theme, ThemeMode,
    input::{Input, InputEvent, InputState},
};
use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
    path::PathBuf,
    rc::Rc,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    time::Duration,
};

actions!(
    disk_cleaner,
    [
        Refresh, GoUp, Back, Forward, Home, CopyInfo, Trash, PickFolder, SaveCsv, SaveMap,
        ShowFilter
    ]
);
type TaskResult = std::result::Result<(), String>;
fn hwnd(window: &Window) -> Result<windows_sys::Win32::Foundation::HWND> {
    match HasWindowHandle::window_handle(window)
        .map_err(|e| anyhow!(e.to_string()))?
        .as_raw()
    {
        RawWindowHandle::Win32(h) => Ok(h.hwnd.get() as _),
        _ => Err(anyhow!("Expected a Windows window")),
    }
}
pub fn run(args: Args, paths: Vec<PathBuf>) -> Result<()> {
    let result = Rc::new(RefCell::new(Ok(())));
    let status = result.clone();
    Application::new().run(move |cx| {
        gpui_component::init(cx);
        cx.bind_keys([
            KeyBinding::new("f5", Refresh, None),
            KeyBinding::new("alt-up", GoUp, None),
            KeyBinding::new("alt-left", Back, None),
            KeyBinding::new("alt-right", Forward, None),
            KeyBinding::new("ctrl-home", Home, None),
            KeyBinding::new("ctrl-shift-c", CopyInfo, None),
            KeyBinding::new("f3", PickFolder, None),
            KeyBinding::new("ctrl-e", SaveCsv, None),
            KeyBinding::new("ctrl-s", SaveMap, None),
            KeyBinding::new("ctrl-shift-f", ShowFilter, None),
            KeyBinding::new("delete", Trash, Some("Snapshot")),
        ]);
        let (width, height) = args
            .geometry
            .as_deref()
            .and_then(|s| s.split_once('x'))
            .and_then(|(w, h)| Some((w.parse::<f32>().ok()?, h.parse::<f32>().ok()?)))
            .unwrap_or((1210., 795.));
        let bounds = Bounds::centered(None, size(px(width.max(600.)), px(height.max(420.))), cx);
        let mut initial = WindowBounds::Windowed(bounds);
        if args.geometry.is_none()
            && args.shot.is_none()
            && let Ok(p) = platform::settings()
            && let Ok(spec) = p.get_value::<String, _>("windowBounds")
        {
            let values = spec
                .split(',')
                .map(str::parse::<i32>)
                .collect::<Result<Vec<_>, _>>();
            if let Ok(v) = values
                && let [x, y, width, height, maximized] = v.as_slice()
                && *width >= 600
                && *height >= 420
            {
                let saved = Bounds::new(
                    point(px(*x as f32), px(*y as f32)),
                    size(px(*width as f32), px(*height as f32)),
                );
                if cx
                    .displays()
                    .iter()
                    .any(|d| d.bounds().contains(&saved.origin))
                {
                    initial = if *maximized != 0 {
                        WindowBounds::Maximized(saved)
                    } else {
                        WindowBounds::Windowed(saved)
                    };
                }
            }
        }
        let outcome = cx.open_window(
            WindowOptions {
                window_bounds: Some(initial),
                titlebar: Some(TitlebarOptions {
                    title: Some("DiskCleaner".into()),
                    appears_transparent: true,
                    ..Default::default()
                }),
                ..Default::default()
            },
            |window, cx| {
                let view = cx.new(|cx| Dashboard::new(args, paths, status.clone(), window, cx));
                cx.new(|cx| Root::new(view, window, cx))
            },
        );
        if let Err(e) = outcome {
            *status.borrow_mut() = Err(e.to_string());
            cx.quit();
        }
        cx.on_window_closed(|cx| {
            if cx.windows().is_empty() {
                cx.quit()
            }
        })
        .detach();
        cx.activate(true);
    });
    let outcome = result.replace(Ok(()));
    outcome.map_err(anyhow::Error::msg)
}

struct SnapshotWatch {
    watcher: RecommendedWatcher,
    events: mpsc::Receiver<notify::Result<notify::Event>>,
    overflow: Arc<AtomicBool>,
    partial: bool,
}
impl SnapshotWatch {
    fn register(paths: &[PathBuf]) -> Option<Self> {
        if !paths.iter().all(|p| p.is_dir()) {
            return None;
        }
        let (tx, events) = mpsc::sync_channel(1024);
        let overflow = Arc::new(AtomicBool::new(false));
        let dropped = overflow.clone();
        let mut watcher = notify::recommended_watcher(move |e| {
            if tx.try_send(e).is_err() {
                dropped.store(true, Ordering::Relaxed);
            }
        })
        .ok()?;
        let mut partial = false;
        for root in paths {
            if watcher.watch(root, RecursiveMode::Recursive).is_err() {
                partial = true;
            }
        }
        Some(Self {
            watcher,
            events,
            overflow,
            partial,
        })
    }
}
type ChartCache = (usize, u32, u32, map::Style, Rc<Vec<map::Cell>>);
enum ContentCheck {
    Running(Vec<usize>, Arc<AtomicBool>),
    Failed(String),
}
const TAG_COLORS: [u32; 4] = [0xdc2328, 0xf5d214, 0x14af41, 0x235fe6];
const RESULT_ROW_HEIGHT: f32 = 29.;
fn top_row(scroll: &UniformListScrollHandle) -> usize {
    let state = scroll.0.borrow();
    let height = state
        .last_item_size
        .as_ref()
        .map_or(RESULT_ROW_HEIGHT, |s| f32::from(s.item.height).max(1.));
    (-f32::from(state.base_handle.offset().y) / height).max(0.) as usize
}
fn restore_top_row(scroll: &UniformListScrollHandle, row: usize) {
    let state = scroll.0.borrow();
    let mut offset = state.base_handle.offset();
    let height = state
        .last_item_size
        .as_ref()
        .map_or(RESULT_ROW_HEIGHT, |s| f32::from(s.item.height).max(1.));
    let clipped_height = (-f32::from(offset.y)).max(0.) % height;
    offset.y = -px(row as f32 * height + clipped_height);
    state.base_handle.set_offset(offset);
}
struct Dashboard {
    brand: Arc<gpui::Image>,
    chart: RefCell<Option<ChartCache>>,
    chart_size: Size<Pixels>,
    list_viewports: [Size<Pixels>; 2],
    split: f32,
    split_drag: Option<(Pixels, f32)>,
    motion_enabled: bool,
    metric_slide: Option<(std::time::Instant, f32)>,
    // Previous scope and the target's old bounds, when zooming into a visible cell.
    map_transition: Option<(std::time::Instant, usize, Option<Rect>)>,
    drag_origin: Option<(usize, Point<Pixels>)>,
    dragging: bool,
    scoped_types: Option<(usize, TypeRows)>,
    args: Args,
    result: Rc<RefCell<TaskResult>>,
    tree: Option<Arc<Tree>>,
    sources: Vec<PathBuf>,
    target: Entity<InputState>,
    location_editor: bool,
    scan_items: Option<usize>,
    filter: Entity<InputState>,
    exclude: Entity<InputState>,
    search: Entity<InputState>,
    edit: Entity<InputState>,
    focus: FocusHandle,
    path_focus: FocusHandle,
    options: Options,
    view: View,
    expanded: HashSet<usize>,
    selected: usize,
    explicit_selection: bool,
    multi: HashSet<usize>,
    // Park the selection while paging so actions only target rows on screen.
    other_page_selection: HashSet<usize>,
    root: usize,
    history: Vec<usize>,
    future: Vec<usize>,
    columns: u16,
    columns_open: bool,
    tree_columns: u16,
    row_icons: row_icons::RowIcons,
    tree_columns_open: bool,
    update_url: String,
    system_theme: bool,
    pane: u8,
    compact_map: bool,
    types: bool,
    menu: bool,
    filters: bool,
    editing: &'static str,
    rename_source: Option<PathBuf>,
    status: String,
    snapshot_dirty: bool,
    zh: bool,
    dark: bool,
    unit: usize,
    depth: u8,
    by_type: bool,
    labels: bool,
    min_cell: f32,
    watch: Option<RecommendedWatcher>,
    events: Option<mpsc::Receiver<notify::Result<notify::Event>>>,
    removed: HashSet<PathBuf>,
    tags: HashMap<usize, u32>,
    volume: Option<(u64, u64)>,
    active_drive: Option<String>,
    drive_labels: HashMap<String, String>,
    free: bool,
    unknown: bool,
    gray: bool,
    image_size: (u32, u32),
    export_opts: table::ExportOptions,
    export_panel: bool,
    jobs: usize,
    closing: bool,
    cancel: Option<Arc<AtomicBool>>,
    serial: u64,
    list_scroll: UniformListScrollHandle,
    file_horizontal: ScrollHandle,
    tree_horizontal: ScrollHandle,
    content_checks: HashMap<usize, ContentCheck>,
    tree_scroll: UniformListScrollHandle,
}
impl Drop for Dashboard {
    fn drop(&mut self) {
        self.reset_checks();
        if let Some(c) = &self.cancel {
            c.store(true, Ordering::Relaxed)
        }
    }
}
impl Dashboard {
    fn text(&self, en: &'static str, zh: &'static str) -> &'static str {
        if self.zh { zh } else { en }
    }
    fn new(
        args: Args,
        paths: Vec<PathBuf>,
        result: Rc<RefCell<TaskResult>>,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let prefs = platform::preferences();
        let pref = winreg::RegKey::predef(winreg::enums::HKEY_CURRENT_USER)
            .open_subkey(r"Software\DiskCleaner\Preferences")
            .ok();
        let pref_text = |key: &str| {
            pref.as_ref()
                .and_then(|p| p.get_value::<String, _>(key).ok())
                .or_else(|| prefs.get(&key.to_lowercase()).cloned())
        };
        let pref_num = |key: &str, default: u32| {
            pref.as_ref()
                .and_then(|p| p.get_value::<u32, _>(key).ok())
                .or_else(|| {
                    pref_text(key).and_then(|s| match s.as_str() {
                        "true" => Some(1),
                        "false" => Some(0),
                        _ => s.parse().ok(),
                    })
                })
                .unwrap_or(default)
        };
        let lang = args
            .lang
            .clone()
            .or_else(|| pref_text("Language"))
            .unwrap_or_else(|| "en".into());
        let theme = args
            .theme
            .clone()
            .or_else(|| pref_text("Theme"))
            .unwrap_or_else(|| "light".into());
        let dark = theme == "dark"
            || (theme == "auto"
                && matches!(
                    w.appearance(),
                    WindowAppearance::Dark | WindowAppearance::VibrantDark
                ));
        Theme::change(
            if dark {
                ThemeMode::Dark
            } else {
                ThemeMode::Light
            },
            Some(w),
            cx,
        );
        let target_text = if paths.is_empty() {
            platform::drives().into_iter().next().unwrap_or_default()
        } else {
            paths
                .iter()
                .map(|p| p.display().to_string())
                .collect::<Vec<_>>()
                .join(";")
        };
        let mut input = |value: String, placeholder: &'static str| {
            cx.new(|cx| {
                let mut s = InputState::new(w, cx).placeholder(placeholder);
                s.set_value(value, w, cx);
                s
            })
        };
        let target = input(target_text, "C:\\");
        let filter = input(
            args.filter.clone(),
            if lang == "en" { "Include" } else { "包含" },
        );
        let exclude = input(String::new(), if lang == "en" { "Exclude" } else { "排除" });
        let search = input(
            String::new(),
            if lang == "en" {
                "Search file names"
            } else {
                "搜索文件名"
            },
        );
        let edit = input(String::new(), "");
        let options = Options {
            allocated: pref_num("treemapAllocated", 1) != 0,
            limit: [100, 1000, 10000, 50000, 0][pref_num("maxFilesIndex", 1).min(4) as usize],
            show_dirs: pref_num("showFolders", 0) != 0,
            dup: args.dup.unwrap_or(pref_num("dupMode", 0).min(2) as u8),
            only: args.dup_only || pref_num("dupOnly", 0) != 0,
            column: args.sort.unwrap_or(pref_num("sortCol", 2).min(8) as u8),
            desc: args.sort_desc || (args.sort.is_none() && pref_num("sortOrder", 1) != 0),
            include: args.filter.clone(),
            full_path: args.filter_path,
            ..Default::default()
        };
        let focus = cx.focus_handle();
        let path_focus = cx.focus_handle().tab_stop(true);
        cx.on_focus(&path_focus, w, |_, _, cx| cx.notify()).detach();
        cx.on_blur(&path_focus, w, |_, _, cx| cx.notify()).detach();
        focus.focus(w);
        let mut s = Self {
            brand: Arc::new(gpui::Image::from_bytes(
                gpui::ImageFormat::Png,
                include_bytes!("../resources/app.png").to_vec(),
            )),
            chart: Default::default(),
            chart_size: Default::default(),
            list_viewports: Default::default(),
            split: (pref_num("splitPercent", 48).clamp(20, 80) as f32) / 100.,
            split_drag: None,
            motion_enabled: platform::client_animations_enabled(),
            metric_slide: None,
            map_transition: None,
            drag_origin: None,
            dragging: false,
            scoped_types: None,
            columns: (pref_num("FileColumns", 0b010111011) as u16) | 1,
            columns_open: false,
            tree_columns: pref_num("TreeColumnsV2", pref_num("TreeColumns", 0b1011) | (1 << 7))
                as u16
                | 1,
            row_icons: Default::default(),
            tree_columns_open: false,
            update_url: pref_text("updateUrl").unwrap_or_default(),
            system_theme: theme == "auto",
            pane: args.tab,
            filters: !args.filter.trim().is_empty(),
            location_editor: false,
            scan_items: None,
            args,
            result,
            tree: None,
            sources: vec![],
            target,
            filter,
            exclude,
            search,
            edit,
            focus,
            path_focus,
            options,
            view: View {
                tree_sort: pref_num("treeSort", 1).min(8) as u8,
                tree_ascending: pref_num("treeAscending", 0) != 0,
                ..Default::default()
            },
            expanded: HashSet::from([0]),
            selected: 0,
            explicit_selection: false,
            multi: HashSet::new(),
            other_page_selection: HashSet::new(),
            root: 0,
            history: vec![],
            future: vec![],
            types: false,
            menu: false,
            compact_map: false,
            editing: "",
            rename_source: None,
            status: String::new(),
            zh: lang == "zh_CN",
            dark,
            unit: pref_num("unit", 0).min(5) as usize,
            depth: pref_num("treemapDetail", 3).clamp(1, 15) as u8,
            by_type: pref_num("treemapClasses", 1) != 0,
            labels: pref_num("treemapNames", 1) != 0,
            min_cell: pref_num("treemapMinPixels", 2).clamp(1, 40) as f32,
            watch: None,
            snapshot_dirty: false,
            events: None,
            removed: HashSet::new(),
            tags: HashMap::new(),
            volume: None,
            active_drive: None,
            drive_labels: HashMap::new(),
            free: pref_num("treemapFree", 0) != 0,
            unknown: pref_num("treemapUnknown", 0) != 0,
            gray: false,
            image_size: {
                let w = pref_num("treemapW", 1920);
                let h = pref_num("treemapH", 1080);
                if w > 0 && h > 0 && u64::from(w) * u64::from(h) <= 64_000_000 {
                    (w, h)
                } else {
                    (1920, 1080)
                }
            },
            export_opts: Default::default(),
            export_panel: false,
            jobs: 0,
            closing: false,
            cancel: None,
            serial: 0,
            list_scroll: UniformListScrollHandle::new(),
            file_horizontal: ScrollHandle::new(),
            tree_horizontal: ScrollHandle::new(),
            content_checks: HashMap::new(),
            tree_scroll: UniformListScrollHandle::new(),
        };
        cx.spawn(async move |this, cx| {
            loop {
                let roots = platform::drives();
                if this
                    .update(cx, |s, cx| {
                        s.drive_labels.retain(|root, _| roots.contains(root));
                        cx.notify();
                    })
                    .is_err()
                {
                    break;
                }
                let mut lookups = Vec::new();
                for root in roots {
                    let this = this.clone();
                    let lookup = cx.background_executor().spawn(async move {
                        let label = platform::volume_label(&root).unwrap_or_default();
                        (root, label)
                    });
                    lookups.push(cx.spawn(async move |cx| {
                        let (root, label) = lookup.await;
                        let _ = this.update(cx, |s, cx| {
                            if s.drive_labels.get(&root) != Some(&label) {
                                s.drive_labels.insert(root, label);
                                cx.notify();
                            }
                        });
                    }));
                }
                for lookup in lookups {
                    lookup.await;
                }
                cx.background_executor()
                    .timer(Duration::from_secs(30))
                    .await;
            }
        })
        .detach();
        cx.subscribe_in(&s.target, w, |this, _, e: &InputEvent, w, cx| {
            if matches!(e, InputEvent::PressEnter { .. }) {
                this.action("scan", w, cx)
            } else if matches!(e, InputEvent::Change) {
                cx.notify();
            } else if this.location_editor && matches!(e, InputEvent::Focus) {
                this.target.read(cx).focus_handle(cx).dispatch_action(
                    &gpui_component::input::SelectAll,
                    w,
                    cx,
                );
            }
        })
        .detach();
        for entity in [&s.filter, &s.exclude, &s.search] {
            cx.subscribe_in(entity, w, |this, _, evt: &InputEvent, _, cx| {
                if matches!(evt, InputEvent::Change) {
                    let include = this.filter.read(cx).value();
                    let exclude = this.exclude.read(cx).value();
                    let search = this.search.read(cx).value();
                    if include.as_ref() == this.options.include
                        && exclude.as_ref() == this.options.exclude
                        && search.as_ref() == this.options.search
                    {
                        return;
                    }
                    this.options.include = include.into();
                    this.options.exclude = exclude.into();
                    this.options.search = search.into();
                    this.rebuild();
                    cx.notify()
                }
            })
            .detach();
        }
        cx.subscribe_in(&s.edit, w, |s, _, evt: &InputEvent, w, cx| {
            if matches!(evt, InputEvent::PressEnter { .. }) && !s.editing.is_empty() {
                s.action("apply-edit", w, cx)
            }
        })
        .detach();
        cx.observe_window_appearance(w, |s, w, cx| {
            if s.system_theme {
                s.dark = matches!(
                    w.appearance(),
                    WindowAppearance::Dark | WindowAppearance::VibrantDark
                );
                Theme::change(
                    if s.dark {
                        ThemeMode::Dark
                    } else {
                        ThemeMode::Light
                    },
                    Some(w),
                    cx,
                );
                cx.notify();
            }
        })
        .detach();
        let weak = cx.weak_entity();
        w.on_window_should_close(cx, move |w, cx| {
            weak.update(cx, |s, cx| {
                if s.args.shot.is_none()
                    && s.args.geometry.is_none()
                    && let Ok(p) = platform::settings()
                {
                    let mode = w.window_bounds();
                    let b = mode.get_bounds();
                    let spec = format!(
                        "{},{},{},{},{}",
                        f32::from(b.origin.x) as i32,
                        f32::from(b.origin.y) as i32,
                        f32::from(b.size.width) as i32,
                        f32::from(b.size.height) as i32,
                        u8::from(matches!(mode, WindowBounds::Maximized(_)))
                    );
                    let _ = p.set_value("windowBounds", &spec);
                }
                if s.jobs == 0 {
                    return true;
                }
                s.closing = true;
                if let Some(c) = &s.cancel {
                    c.store(true, Ordering::Relaxed)
                }
                s.status = s.text("Canceling…", "正在取消…").into();
                cx.notify();
                false
            })
            .unwrap_or(true)
        });
        if let Err(e) = hwnd(w).and_then(platform::icon) {
            s.status = e.to_string()
        }
        if !paths.is_empty() {
            s.scan(paths, w, cx)
        } else if s.args.shot.is_some() {
            s.schedule_shot(w, cx)
        }
        s
    }
    fn rebuild(&mut self) {
        self.scoped_types = None;
        self.chart.get_mut().take();
        if let Some(t) = &self.tree {
            self.view.rebuild(t, &self.options, &self.expanded);
            if self.pane == 1 {
                self.sync_file_selection(false);
            } else if !self.view.included[self.selected] {
                self.selected = 0;
                self.explicit_selection = false;
            }
        }
    }
    fn reset_checks(&mut self) {
        for (_, check) in self.content_checks.drain() {
            if let ContentCheck::Running(_, token) = check {
                token.store(true, Ordering::Relaxed);
            }
        }
        self.view.verified_groups.clear();
        self.view.content_classes.clear();
    }
    fn verify_group(&mut self, group: usize, cx: &mut Context<Self>) {
        let Some(tree) = self.tree.as_ref().filter(|t| t.live).cloned() else {
            return;
        };
        if matches!(
            self.content_checks.get(&group),
            Some(ContentCheck::Running(..))
        ) {
            return;
        }
        let members: Vec<_> = self
            .view
            .group_ids
            .iter()
            .enumerate()
            .filter_map(|(i, &g)| (g == Some(group)).then_some(i))
            .collect();
        if members.len() < 2 {
            return;
        }
        let token = Arc::new(AtomicBool::new(false));
        self.content_checks
            .insert(group, ContentCheck::Running(members.clone(), token.clone()));
        let cancel = token.clone();
        let task = cx
            .background_executor()
            .spawn(async move { scan::content_groups(&tree, &members, &cancel) });
        cx.spawn(async move |this, cx| {
            let result = task.await;
            let _ = this.update(cx, |s, cx| {
                if !token.load(Ordering::Relaxed) {
                    s.content_checks.remove(&group);
                    match result {
                        Ok(classes) => {
                            s.view.verified_groups.insert(group, classes);
                        }
                        Err(e) => {
                            s.view.verified_groups.remove(&group);
                            s.content_checks
                                .insert(group, ContentCheck::Failed(format!("{e:#}")));
                        }
                    }
                    s.rebuild();
                    if s.pane == 1 {
                        s.sync_file_selection(true);
                    }
                    cx.notify();
                }
            });
        })
        .detach();
        cx.notify();
    }
    fn scan_location(&mut self, paths: Vec<PathBuf>, w: &mut Window, cx: &mut Context<Self>) {
        self.menu = false;
        if self.pane == 2 {
            self.pane = 0;
        }
        self.scan(paths, w, cx)
    }
    fn snapshot_location(&self) -> String {
        self.sources
            .iter()
            .map(|p| p.display().to_string())
            .collect::<Vec<_>>()
            .join(";")
    }
    fn scan(&mut self, paths: Vec<PathBuf>, w: &mut Window, cx: &mut Context<Self>) {
        let paths = paths
            .into_iter()
            .map(|p| std::path::absolute(&p).unwrap_or(p))
            .collect::<Vec<_>>();
        if paths.is_empty() {
            self.status = self
                .text("Enter a location to scan", "请输入要扫描的位置")
                .into();
            cx.notify();
            return;
        }
        let location = paths
            .iter()
            .map(|p| p.display().to_string())
            .collect::<Vec<_>>()
            .join(";");
        if self.target.read(cx).value().as_ref() != location {
            self.target
                .update(cx, |input, cx| input.set_value(location, w, cx));
        }
        if let Some(c) = self.cancel.take() {
            c.store(true, Ordering::Relaxed)
        }
        if self.location_editor || self.tree.is_none() {
            self.focus.focus(w);
        }
        self.location_editor = false;
        self.scan_items = Some(0);
        let cancel = Arc::new(AtomicBool::new(false));
        self.cancel = Some(cancel.clone());
        self.jobs += 1;
        self.serial += 1;
        let serial = self.serial;
        self.status = self.text("Scanning…", "正在扫描…").into();
        let roots = paths.clone();
        let progress = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let reported = progress.clone();
        let scan_token = Arc::downgrade(&cancel);
        let args = self.args.clone();
        let task = cx.background_executor().spawn(async move {
            let watch = SnapshotWatch::register(&roots);
            let tree =
                scan::load_progress(&roots, &cancel, &|n| reported.store(n, Ordering::Relaxed))?;
            scan::check(&cancel)?;
            let export = crate::export(&tree, &args);
            let volume = if tree.live && roots.len() == 1 {
                (
                    platform::volume_space(&roots[0]),
                    platform::volume_drive(&roots[0]),
                )
            } else {
                (None, None)
            };
            Ok((tree, watch, export, volume))
        });
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(Duration::from_millis(400))
                    .await;
                let active = this.update(cx, |s, cx| {
                    if s.serial != serial
                        || !s
                            .cancel
                            .as_ref()
                            .is_some_and(|c| Arc::as_ptr(c) == scan_token.as_ptr())
                        || s.closing
                    {
                        return false;
                    }
                    let count = progress.load(Ordering::Relaxed);
                    if s.scan_items != Some(count) {
                        s.scan_items = Some(count);
                        cx.notify();
                    }
                    true
                });
                if !matches!(active, Ok(true)) {
                    break;
                }
            }
        })
        .detach();
        cx.spawn_in(w, async move |this, cx| {
            let res = task.await;
            let _ = this.update_in(cx, |s, w, cx| {
                s.jobs -= 1;
                if s.closing {
                    if s.jobs == 0 {
                        cx.quit()
                    }
                    return;
                }
                if serial != s.serial {
                    return;
                }
                let canceled = s.cancel.take().is_some_and(|c| c.load(Ordering::Relaxed));
                s.scan_items = None;
                let res = if canceled {
                    Err(anyhow!("Scan canceled"))
                } else {
                    res
                };
                match res {
                    Ok((t, watch, export, volume)) => {
                        let keep = s.sources == paths;
                        let old = s.tree.as_ref().filter(|_| keep);
                        let expanded: HashSet<_> = old
                            .map(|o| s.expanded.iter().map(|&id| o.path(id)).collect())
                            .unwrap_or_default();
                        let selection = old.map(|o| o.path(s.selected));
                        let was_explicit = s.explicit_selection;
                        let previous_filter = s.options.type_filter.take();
                        let filter_path = old.and_then(|tree| {
                            previous_filter.as_ref().map(|(id, _)| tree.path(*id))
                        });
                        let zoom = old.map(|o| o.path(s.root));
                        let selected_paths: HashSet<_> = old
                            .map(|o| s.multi.iter().map(|&id| o.path(id)).collect())
                            .unwrap_or_default();
                        let tags: HashMap<_, _> = old
                            .map(|o| {
                                s.tags
                                    .iter()
                                    .map(|(&id, &color)| (o.path(id), color))
                                    .collect()
                            })
                            .unwrap_or_default();
                        let tree_top = old.and_then(|o| {
                            s.view
                                .rows
                                .get(top_row(&s.tree_scroll))
                                .map(|&(id, _)| o.path(id))
                        });
                        let file_top = old.and_then(|o| {
                            s.view
                                .file_rows()
                                .get(s.view.file_at_row(top_row(&s.list_scroll)).0)
                                .map(|&id| o.path(id))
                        });
                        let (mut tree_anchor, mut file_anchor) = (None, None);
                        s.expanded = if keep {
                            HashSet::new()
                        } else {
                            HashSet::from([0])
                        };
                        s.multi.clear();
                        s.other_page_selection.clear();
                        s.selected = 0;
                        s.explicit_selection = false;
                        s.root = 0;
                        s.map_transition = None;
                        s.history.clear();
                        s.future.clear();
                        s.tags.clear();
                        s.removed.clear();
                        if keep {
                            for id in 0..t.nodes.len() {
                                let p = t.path(id);
                                if Some(&p) == filter_path.as_ref()
                                    && let Some((_, ext)) = &previous_filter
                                {
                                    s.options.type_filter = Some((id, ext.clone()));
                                }
                                if selected_paths.contains(&p) {
                                    s.multi.insert(id);
                                }
                                if let Some(&color) = tags.get(&p) {
                                    s.tags.insert(id, color);
                                }
                                if Some(&p) == tree_top.as_ref() {
                                    tree_anchor = Some(id);
                                }
                                if Some(&p) == file_top.as_ref() {
                                    file_anchor = Some(id);
                                }
                                if expanded.contains(&p) {
                                    s.expanded.insert(id);
                                }
                                if Some(&p) == selection.as_ref() {
                                    s.selected = id;
                                    s.explicit_selection = was_explicit;
                                }
                                if Some(&p) == zoom.as_ref() {
                                    s.root = id
                                }
                            }
                        }
                        (s.volume, s.active_drive) = volume;
                        s.sources = paths;
                        s.status = format!(
                            "{}  ·  {} {}  ·  {} {}",
                            s.text("Snapshot ready", "快照已就绪"),
                            t.nodes.len(),
                            s.text("items", "项"),
                            t.skipped,
                            s.text("skipped", "项跳过")
                        );
                        if let Err(e) = export {
                            s.fail(e)
                        }
                        s.reset_checks();
                        s.tree = Some(Arc::new(t));
                        s.rebuild();
                        if let Some(row) = tree_anchor
                            .and_then(|id| s.view.rows.iter().position(|&(n, _)| n == id))
                        {
                            restore_top_row(&s.tree_scroll, row);
                        }
                        if let Some(row) = file_anchor
                            .and_then(|id| s.view.file_rows().iter().position(|&n| n == id))
                        {
                            restore_top_row(&s.list_scroll, s.view.visual_row(row));
                        }
                        s.start_watch(watch, w, cx);
                        if s.args.copy_info {
                            s.copy_info(cx)
                        }
                    }
                    Err(_) if canceled => {
                        s.fail(anyhow!(s.text("Scan canceled", "扫描已取消")));
                        if s.tree.is_some() {
                            let path = s.snapshot_location();
                            s.target
                                .update(cx, |input, cx| input.set_value(path, w, cx));
                        }
                    }
                    Err(e) => {
                        s.location_editor = s.tree.is_some();
                        s.fail(e);
                    }
                }
                if s.args.shot.is_some() {
                    s.schedule_shot(w, cx)
                }
                cx.notify()
            });
        })
        .detach();
        cx.notify()
    }
    fn fail(&mut self, e: anyhow::Error) {
        self.status = format!("{e:#}");
        if self.args.shot.is_some() {
            *self.result.borrow_mut() = Err(self.status.clone())
        }
    }
    fn schedule_shot(&self, w: &mut Window, cx: &mut Context<Self>) {
        let Some(path) = self.args.shot.clone() else {
            return;
        };
        let delay = self.args.delay;
        cx.spawn_in(w, async move |this, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(delay))
                .await;
            let _ = this.update_in(cx, |s, w, cx| {
                if let Err(e) = hwnd(w).and_then(|h| platform::capture(h, &path)) {
                    s.fail(e)
                }
                cx.quit()
            });
        })
        .detach()
    }
    fn start_watch(
        &mut self,
        pending: Option<SnapshotWatch>,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.watch = None;
        self.snapshot_dirty = false;
        self.events = None;
        if !self.tree.as_ref().is_some_and(|t| t.live) {
            return;
        }
        let Some(SnapshotWatch {
            watcher,
            events,
            overflow,
            partial,
        }) = pending
        else {
            self.status
                .push_str(self.text(" · Watch unavailable", " · 无法监听变化"));
            return;
        };
        if partial {
            self.status.push_str(self.text(
                " · Some locations cannot be watched",
                " · 部分位置无法监听变化",
            ));
        }
        self.watch = Some(watcher);
        self.events = Some(events);
        let generation = self.tree.as_ref().map(Arc::downgrade).unwrap();
        cx.spawn_in(w, async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(Duration::from_millis(350))
                    .await;
                if generation.strong_count() == 0 {
                    break;
                }
                let keep = this.update(cx, |s, cx| {
                    if !s
                        .tree
                        .as_ref()
                        .is_some_and(|t| std::ptr::eq(Arc::as_ptr(t), generation.as_ptr()))
                    {
                        return false;
                    }
                    let mut unknown = overflow.swap(false, Ordering::Relaxed);
                    let mut paths = HashSet::new();
                    if let Some(rx) = &s.events {
                        for e in rx.try_iter() {
                            match e {
                                Ok(e) if matches!(e.kind, notify::EventKind::Access(_)) => {}
                                Ok(e) => {
                                    unknown |= e.need_rescan() || e.paths.is_empty();
                                    if matches!(e.kind, notify::EventKind::Remove(_)) {
                                        s.removed.extend(e.paths.iter().cloned());
                                    }
                                    paths.extend(e.paths);
                                }
                                Err(_) => unknown = true,
                            }
                        }
                    }
                    if unknown || !paths.is_empty() {
                        let tree = s.tree.as_ref().unwrap();
                        let affected = |id: usize| {
                            unknown || tree.path(id).ancestors().any(|p| paths.contains(p))
                        };
                        let mut dirty = false;
                        s.view.verified_groups.retain(|_, classes| {
                            let keep = !classes.iter().flatten().any(|&id| affected(id));
                            dirty |= !keep;
                            keep
                        });
                        s.content_checks.retain(|_, check| {
                            if let ContentCheck::Running(ids, token) = check
                                && ids.iter().any(|&id| affected(id))
                            {
                                token.store(true, Ordering::Relaxed);
                                dirty = true;
                                return false;
                            }
                            true
                        });
                        if dirty {
                            s.rebuild();
                        }
                        s.snapshot_dirty = true;
                        cx.notify()
                    }
                    true
                });
                if !matches!(keep, Ok(true)) {
                    break;
                }
            }
        })
        .detach()
    }
    fn type_scope(&self) -> usize {
        if self.pane == 1
            && let Some((scope, _)) = &self.options.type_filter
        {
            *scope
        } else {
            self.selected
        }
    }
    fn update_type_scope(&mut self) {
        let scope = self.type_scope();
        let only_duplicates = self.options.only && self.options.dup != 0;
        if !self.types || (scope == 0 && self.options.search.is_empty() && !only_duplicates) {
            self.scoped_types = None;
            return;
        }
        if self
            .scoped_types
            .as_ref()
            .is_some_and(|(id, _)| *id == scope)
        {
            return;
        }
        let Some(tree) = &self.tree else { return };
        let mut entries = HashMap::new();
        let search = self.options.search.to_lowercase();
        let mut stack = vec![scope];
        while let Some(id) = stack.pop() {
            if !self.view.included[id] {
                continue;
            }
            let n = &tree.nodes[id];
            if n.dir {
                stack.extend(&n.children)
            } else if (!only_duplicates || self.view.counts[id] > 1)
                && view::name_matches(n, &search)
            {
                view::add_type(&mut entries, n);
            }
        }
        self.scoped_types = Some((scope, view::type_rows(entries, self.options.allocated)));
    }
    fn type_rows(&self) -> &TypeRows {
        self.scoped_types
            .as_ref()
            .map(|(_, rows)| rows)
            .unwrap_or(&self.view.types)
    }
    fn navigate(&mut self, id: usize) {
        self.other_page_selection.clear();
        if self.root != id {
            self.history.push(self.root);
            self.future.clear();
            self.change_map_root(id);
        }
        self.selected = id;
        self.explicit_selection = false;
    }
    fn change_map_root(&mut self, id: usize) {
        if self.motion_enabled {
            let from = self
                .chart
                .get_mut()
                .as_ref()
                .filter(|(root, _, _, _, _)| *root == self.root)
                .and_then(|(_, _, _, _, cells)| cells.iter().find(|c| c.id == id))
                .map(|c| c.rect);
            self.map_transition = Some((std::time::Instant::now(), self.root, from));
        }
        self.root = id;
    }
    fn sync_file_selection(&mut self, scroll: bool) {
        if !self.explicit_selection {
            self.multi.clear();
            self.other_page_selection.clear();
            return;
        }
        let previous_range = self.view.file_range.clone();
        let previously_visible = self.multi.contains(&self.selected);
        let row = self.view.reveal_file(self.selected, self.options.limit);
        if self.other_page_selection.contains(&self.selected) && row.is_some() {
            self.multi = std::mem::take(&mut self.other_page_selection);
        } else {
            self.other_page_selection.clear();
        }
        if !self.multi.is_empty() {
            self.multi = self
                .view
                .file_rows()
                .iter()
                .copied()
                .filter(|id| self.multi.contains(id))
                .collect();
        }
        if let Some(row) = row {
            self.multi.insert(self.selected);
            if scroll || !previously_visible || previous_range != self.view.file_range {
                self.list_scroll
                    .scroll_to_item(self.view.visual_row(row), ScrollStrategy::Center);
            }
        } else if let Some(&id) = self.multi.iter().min() {
            self.selected = id;
        }
    }
    fn switch_pane(&mut self, pane: u8) {
        self.pane = pane;
        if pane == 1 {
            self.sync_file_selection(true);
        } else if pane == 0 && self.explicit_selection {
            self.reveal_tree_row(self.selected);
        }
    }
    fn browse_row(&mut self, key: &str, cx: &mut Context<Self>) {
        let Some(tree) = &self.tree else { return };
        if self.pane == 1 {
            let Some(&first) = self.view.file_rows().first() else {
                return;
            };
            let target = if !self.multi.contains(&self.selected)
                || !self.view.file_rows().contains(&self.selected)
            {
                first
            } else {
                let Some(i) = self.view.files.iter().position(|&id| id == self.selected) else {
                    return;
                };
                self.view.files[if key == "up" {
                    i.saturating_sub(1)
                } else {
                    (i + 1).min(self.view.files.len() - 1)
                }]
            };
            self.multi.clear();
            self.select(target, false, cx);
            self.sync_file_selection(true);
            return;
        }
        let Some(i) = self
            .view
            .rows
            .iter()
            .position(|&(id, _)| id == self.selected)
        else {
            return;
        };
        let id = self.selected;
        let target = match key {
            "up" => self.view.rows[i.saturating_sub(1)].0,
            "down" => self.view.rows[(i + 1).min(self.view.rows.len() - 1)].0,
            "left" if self.expanded.remove(&id) => {
                self.view.tree_rows(tree, &self.expanded);
                id
            }
            "left" => tree.nodes[id].parent,
            "right" if tree.nodes[id].dir && self.expanded.insert(id) => {
                self.view.tree_rows(tree, &self.expanded);
                id
            }
            "right" => self
                .view
                .rows
                .get(i + 1)
                .filter(|&&(child, _)| tree.nodes[child].parent == id)
                .map_or(id, |r| r.0),
            _ => return,
        };
        self.select(target, false, cx);
        self.reveal_tree_row(target);
    }
    fn reveal_tree_row(&mut self, id: usize) {
        let Some(t) = &self.tree else { return };
        let mut ancestor = id;
        let mut changed = false;
        while ancestor != 0 {
            ancestor = t.nodes[ancestor].parent;
            changed |= self.expanded.insert(ancestor);
        }
        if changed {
            self.view.tree_rows(t, &self.expanded);
        }
        if let Some(row) = self.view.rows.iter().position(|&(n, _)| n == id) {
            let edge = if row <= top_row(&self.tree_scroll) {
                ScrollStrategy::Top
            } else {
                ScrollStrategy::Bottom
            };
            self.tree_scroll.scroll_to_item(row, edge);
        }
    }
    fn select_map_item(&mut self, id: usize, zoom: bool, cx: &mut Context<Self>) {
        self.other_page_selection.clear();
        self.reveal_tree_row(id);
        let Some(t) = &self.tree else { return };
        if zoom {
            self.navigate(if t.nodes[id].dir {
                id
            } else {
                t.nodes[id].parent
            });
        }
        self.multi.clear();
        self.selected = id;
        self.explicit_selection = true;
        cx.notify();
    }
    fn select(&mut self, id: usize, open: bool, cx: &mut Context<Self>) {
        self.other_page_selection.clear();
        let Some(t) = &self.tree else { return };
        let changed = !self.explicit_selection || self.selected != id;
        self.selected = id;
        let dest = if t.nodes[id].dir {
            id
        } else {
            t.nodes[id].parent
        };
        if open && t.nodes[id].dir {
            if !self.expanded.remove(&id) {
                self.expanded.insert(id);
            }
            self.view.tree_rows(t, &self.expanded);
        }
        // Keep neighboring blocks visible when selecting an item already drawn in this map.
        if (self.pane == 0 || changed)
            && ((open && self.pane == 0)
                || !t.within(id, self.root)
                || self.chart.borrow().as_ref().map_or(
                    id != self.root && t.nodes[id].parent != self.root,
                    |(root, _, _, _, cells)| {
                        *root != self.root || !cells.iter().any(|cell| cell.id == id)
                    },
                ))
        {
            self.navigate(dest);
            self.chart.get_mut().take();
            self.selected = id
        }
        self.explicit_selection = true;
        if self.pane == 0 {
            self.multi.clear();
        }
        cx.notify()
    }
    fn copy_info(&self, cx: &mut App) {
        if let Some(t) = &self.tree {
            use std::fmt::Write;
            let mut text = String::new();
            let mut stack = vec![(self.selected, 0)];
            while let Some((id, d)) = stack.pop() {
                if !self.view.included[id] {
                    continue;
                }
                let n = &t.nodes[id];
                let _ = writeln!(
                    text,
                    "{}{}\t{}",
                    "  ".repeat(d),
                    n.name.to_string_lossy(),
                    format_size(self.view.weights[id], self.unit)
                );
                stack.extend(n.children.iter().rev().map(|&i| (i, d + 1)))
            }
            cx.write_to_clipboard(ClipboardItem::new_string(text))
        }
    }
    fn pick(&self, files: bool, w: &mut Window, cx: &mut Context<Self>) {
        let rx = cx.prompt_for_paths(PathPromptOptions {
            files,
            directories: !files,
            multiple: true,
            prompt: None,
        });
        cx.spawn_in(w, async move |this, cx| {
            let res = rx.await;
            let _ = this.update_in(cx, |s, w, cx| match res {
                Ok(Ok(Some(paths))) => {
                    let v = paths
                        .iter()
                        .map(|p| p.display().to_string())
                        .collect::<Vec<_>>()
                        .join(";");
                    s.target.update(cx, |t, cx| t.set_value(v, w, cx));
                    if !files && s.tree.is_none() && s.cancel.is_none() {
                        s.menu = false;
                        s.pane = 0;
                        cx.notify();
                    } else {
                        s.scan_location(paths, w, cx)
                    }
                }
                Ok(Err(e)) => {
                    s.fail(e);
                    cx.notify()
                }
                _ => {}
            });
        })
        .detach()
    }
    fn writable(&self) -> Result<PathBuf> {
        ensure!(
            self.pane != 1 || self.multi.contains(&self.selected),
            "Select an item"
        );
        self.check_item(self.selected)
    }
    fn start_drag(&mut self, id: usize, w: &Window, cx: &mut Context<Self>) {
        if self.dragging {
            return;
        }
        if self.pane == 1 && !self.multi.contains(&id) {
            self.multi.clear();
            self.multi.insert(id);
        }
        self.selected = id;
        self.explicit_selection = true;
        let paths = self
            .selected_ids()
            .into_iter()
            .map(|id| self.check_item(id))
            .collect::<Result<Vec<_>>>();
        let Ok((handle, paths)) = hwnd(w).and_then(|h| Ok((h, paths?))) else {
            return;
        };
        self.dragging = true;
        cx.spawn(async move |this, cx| {
            let result = platform::drag_files(handle, &paths);
            let _ = this.update(cx, |s, cx| {
                s.dragging = false;
                if let Err(e) = result {
                    s.fail(e);
                }
                cx.notify();
            });
        })
        .detach();
    }
    fn selected_ids(&self) -> Vec<usize> {
        if self.pane == 1 {
            let mut ids: Vec<_> = self.multi.iter().copied().collect();
            ids.sort_unstable();
            ids
        } else {
            vec![self.selected]
        }
    }
    fn recycle_roots(&self, tree: &Tree) -> Vec<usize> {
        if !self.explicit_selection || self.pane == 0 && !self.view.included[self.selected] {
            return Vec::new();
        }
        let mut ids = self.selected_ids();
        if ids.len() > 1 && ids.iter().any(|&id| tree.nodes[id].dir) {
            ids.retain(|&id| {
                let mut ancestor = id;
                while ancestor != 0 {
                    ancestor = tree.nodes[ancestor].parent;
                    if self.multi.contains(&ancestor) {
                        return false;
                    }
                }
                true
            });
        }
        ids
    }
    fn filtered_folder_scope(&self, tree: &Tree, roots: &[usize]) -> Option<(String, String)> {
        if !(self.view.filtered() || self.pane == 1 && !self.options.search.is_empty())
            || !roots.iter().any(|&id| tree.nodes[id].dir)
        {
            return None;
        }
        let (mut matched, mut files, mut size, mut whole) = (0, 0, 0, 0);
        for &id in roots {
            let node = &tree.nodes[id];
            let totals = self.view.scoped_totals(tree, id, self.pane == 1);
            matched += if node.dir { totals.files } else { 1 };
            files += if node.dir { node.files } else { 1 };
            size += totals.weight(self.options.allocated);
            whole += node.weight(self.options.allocated);
        }
        let target = if roots.len() == 1 {
            self.text("Entire folder", "整个目录")
        } else {
            self.text("All selected targets", "全部操作对象")
        };
        let label = format!(
            "{} {matched} {} · {}  /  {target} {files} {} · {}",
            self.text("Matched", "匹配"),
            self.text("files", "个文件"),
            format_size(size, self.unit),
            self.text("files", "个文件"),
            format_size(whole, self.unit)
        );
        let hidden = files - matched;
        let detail = if self.zh {
            format!("按扫描快照，操作包含 {hidden} 个未匹配文件。")
        } else {
            format!("The selection includes {hidden} unmatched files in this snapshot.")
        };
        Some((label, detail))
    }
    fn recycle_caption(&self, scoped: bool, count: usize) -> &'static str {
        if !scoped {
            self.text("Recycle", "移至回收站")
        } else if count == 1 {
            self.text("Recycle entire folder", "回收整个目录")
        } else {
            self.text("Recycle all targets", "回收全部操作对象")
        }
    }
    fn check_item(&self, id: usize) -> Result<PathBuf> {
        let t = self.tree.as_ref().context("Scan a location first")?;
        ensure!(t.live && id != 0, "Select an item in a live scan");
        let p = t.path(id);
        ensure!(
            !p.ancestors().any(|a| self.removed.contains(a)),
            "Refresh the snapshot first"
        );
        let m = std::fs::symlink_metadata(&p)?;
        use std::os::windows::fs::MetadataExt;
        let n = &t.nodes[id];
        ensure!(
            n.dir == m.is_dir()
                && (n.modified == 0 || n.modified == m.last_write_time())
                && (n.dir || n.bytes == m.file_size()),
            "Item changed; refresh first"
        );
        Ok(p)
    }
    fn map_style(&self) -> map::Style {
        let mut style = map::Style {
            depth: self.depth,
            min: self.min_cell,
            types: self.by_type,
            names: self.labels,
            gray: self.gray,
            unit: self.unit,
            ..Default::default()
        };
        if self.root == 0
            && self.options.allocated
            && self.sources.len() == 1
            && self.sources[0].parent().is_none()
            && let (Some((total, free)), Some(t)) = (self.volume, &self.tree)
        {
            style.free = if self.free { free } else { 0 };
            style.unknown = if self.unknown {
                total.saturating_sub(free).saturating_sub(t.nodes[0].alloc)
            } else {
                0
            };
        }
        style
    }
    fn csv_rows<'a>(&'a self, tree: &'a Tree) -> impl Iterator<Item = usize> + 'a {
        let selected: HashSet<_> = if self.export_opts.selected && self.explicit_selection {
            self.selected_ids().into_iter().collect()
        } else {
            HashSet::new()
        };
        let flat = self.pane == 1;
        let len = if flat {
            self.view.files.len()
        } else {
            tree.nodes.len()
        };
        (0..len)
            .map(move |i| if flat { self.view.files[i] } else { i })
            .filter(move |&id| {
                self.view.included[id] && self.export_opts.allows(tree, id, &selected)
            })
    }
    fn export_type_rows(&self, tree: &Tree) -> TypeRows {
        if self.types {
            return self.type_rows().clone();
        }
        if self.pane != 1 {
            return self.view.types.clone();
        }
        let mut types = HashMap::new();
        for &id in &self.view.files {
            if !tree.nodes[id].dir {
                view::add_type(&mut types, &tree.nodes[id]);
            }
        }
        view::type_rows(types, self.options.allocated)
    }
    fn save_as(&mut self, kind: &'static str, w: &mut Window, cx: &mut Context<Self>) {
        if self.cancel.is_some() {
            self.status = self
                .text("Wait for the current task to finish", "请等待当前任务完成")
                .into();
            return;
        }
        let snapshot = self.tree.clone();
        if kind != "mft" && snapshot.is_none() {
            self.status = self
                .text("Scan a location first", "请先扫描一个位置")
                .into();
            return;
        }
        let root = self.root;
        let folder = std::env::current_dir().unwrap_or_default();
        let name = match kind {
            "png" => "treemap.png",
            "types-csv" | "full-types" => "file-types.csv",
            "mft" => "volume.mft",
            _ => "snapshot.csv",
        };
        let rx = cx.prompt_for_new_path(&folder, Some(name));
        let weights = if kind == "png" {
            self.view.weights.clone()
        } else {
            Vec::new()
        };
        let style = self.map_style();
        let (width, height) = self.image_size;
        let zh = self.zh;
        let tags = self.tags.clone();
        let options = self.export_opts.clone();
        let rows = snapshot
            .as_ref()
            .filter(|_| kind == "csv")
            .map(|tree| self.csv_rows(tree).collect::<Vec<_>>())
            .unwrap_or_default();
        let filtered = if kind == "csv" {
            self.view.filtered_totals.clone()
        } else {
            Vec::new()
        };
        let types = snapshot
            .as_ref()
            .filter(|_| kind == "types-csv")
            .map(|tree| self.export_type_rows(tree))
            .unwrap_or_default();
        let target = self.target.read(cx).value().to_string();
        let drive = locations(&target).into_iter().next().unwrap_or_default();
        cx.spawn_in(w, async move |this, cx| {
            let Ok(Ok(Some(p))) = rx.await else { return };
            let token = Arc::new(AtomicBool::new(false));
            let worker = token.clone();
            let started = this.update(cx, |s, cx| {
                if s.cancel.is_some() {
                    return false;
                }
                s.jobs += 1;
                s.cancel = Some(token.clone());
                s.status = s.text("Exporting…", "正在导出…").into();
                cx.notify();
                true
            });
            if !matches!(started, Ok(true)) {
                return;
            }
            let task = cx.background_executor().spawn(async move {
                if kind == "mft" {
                    let root = drive
                        .components()
                        .next()
                        .context("Choose a drive first")?
                        .as_os_str()
                        .to_string_lossy();
                    return crate::mft::dump(&crate::drive_root(&root)?, &p, &worker);
                }
                let t = snapshot.context("No snapshot")?;
                match kind {
                    "png" => {
                        let mut cells = map::layout(
                            &t,
                            root,
                            &weights,
                            Rect {
                                x: 0.,
                                y: 0.,
                                w: width as f32,
                                h: height as f32,
                            },
                            &style,
                        );
                        for cell in &mut cells {
                            if let Some(color) = tags.get(&cell.id) {
                                cell.color = *color
                            }
                        }
                        map::png(&p, &t, &cells, (width, height), &style, zh, &worker)
                    }
                    "types-csv" => table::export_types(&types, &p, &worker),
                    "full-types" => {
                        let rows = t
                            .types(&worker)?
                            .into_iter()
                            .map(|(ext, files, bytes, alloc)| {
                                (
                                    ext,
                                    view::Totals {
                                        files,
                                        bytes,
                                        alloc,
                                        folders: 0,
                                    },
                                )
                            })
                            .collect();
                        table::export_types(&rows, &p, &worker)
                    }
                    "full" => table::write(
                        &t,
                        &p,
                        &table::ExportOptions {
                            fields: [true; 6],
                            ..Default::default()
                        },
                        1..t.nodes.len(),
                        &[],
                        &worker,
                    ),
                    _ => table::write(&t, &p, &options, rows, &filtered, &worker),
                }
            });
            let res = task.await;
            let _ = this.update(cx, |s, cx| {
                s.jobs -= 1;
                if s.closing {
                    if s.jobs == 0 {
                        cx.quit()
                    }
                    return;
                }
                if !s.cancel.as_ref().is_some_and(|c| Arc::ptr_eq(c, &token)) {
                    return;
                }
                s.cancel = None;
                s.status = match res {
                    Ok(()) => s.text("Export complete", "导出完成").into(),
                    Err(e) => format!("{e:#}"),
                };
                cx.notify()
            });
        })
        .detach()
    }
    fn persist_view(&self) -> Result<()> {
        if self.args.shot.is_some() {
            return Ok(());
        }
        let prefs = platform::settings()?;
        let index = [100, 1000, 10000, 50000, 0]
            .iter()
            .position(|&n| n == self.options.limit)
            .unwrap_or(4);
        for (k, v) in [
            ("unit", self.unit as u32),
            ("treemapAllocated", self.options.allocated as u32),
            ("maxFilesIndex", index as u32),
            ("showFolders", self.options.show_dirs as u32),
            ("dupMode", self.options.dup as u32),
            ("dupOnly", self.options.only as u32),
            ("FileColumns", self.columns as u32),
            ("TreeColumnsV2", self.tree_columns as u32),
            ("treeSort", self.view.tree_sort as u32),
            ("treeAscending", self.view.tree_ascending as u32),
            ("sortCol", self.options.column as u32),
            ("sortOrder", self.options.desc as u32),
            ("treemapW", self.image_size.0),
            ("treemapH", self.image_size.1),
            ("treemapDetail", self.depth as u32),
            ("treemapClasses", self.by_type as u32),
            ("treemapNames", self.labels as u32),
            ("treemapMinPixels", self.min_cell as u32),
            ("treemapFree", self.free as u32),
            ("treemapUnknown", self.unknown as u32),
        ] {
            prefs.set_value(k, &v)?
        }
        Ok(())
    }
    fn edit_value(
        &mut self,
        kind: &'static str,
        value: String,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.editing = kind;
        self.edit.update(cx, |e, cx| {
            e.set_value(value, w, cx);
            e.focus(w, cx)
        });
    }
    fn action(&mut self, a: &'static str, w: &mut Window, cx: &mut Context<Self>) {
        let res = (|| -> Result<()> {
            match a {
                "scan" => self.scan(locations(&self.target.read(cx).value()), w, cx),
                "refresh" => self.scan(self.sources.clone(), w, cx),
                "folder" => self.pick(false, w, cx),
                "import" => self.pick(true, w, cx),
                "change-location" | "close-location" => {
                    self.location_editor = a == "change-location";
                    let path = self.snapshot_location();
                    self.target.update(cx, |input, cx| {
                        input.set_value(path, w, cx);
                        if self.location_editor {
                            input.focus(w, cx);
                        }
                    });
                    if !self.location_editor {
                        self.focus.focus(w);
                    }
                }
                "clear-filters" | "clear-include" | "clear-exclude" => {
                    if a != "clear-exclude" {
                        self.filter
                            .update(cx, |input, cx| input.set_value("", w, cx));
                    }
                    if a != "clear-include" {
                        self.exclude
                            .update(cx, |input, cx| input.set_value("", w, cx));
                    }
                    if !self.filters {
                        self.focus.focus(w);
                    }
                }
                "cancel" => {
                    if let Some(c) = &self.cancel {
                        c.store(true, Ordering::Relaxed)
                    }
                }
                "tree" | "files" | "about" => {
                    self.menu = false;
                    self.editing = "";
                    self.switch_pane(match a {
                        "files" => 1,
                        "about" => 2,
                        _ => 0,
                    });
                    self.focus.focus(w);
                }
                "types" => self.types = !self.types,
                "whole-type-scan" => {
                    if let Some((scope, _)) = &mut self.options.type_filter {
                        *scope = 0;
                    }
                    self.rebuild();
                }
                "clear-type" => {
                    self.options.type_filter = None;
                    self.rebuild();
                }
                "clear-search" => {
                    self.search.update(cx, |input, cx| {
                        input.set_value("", w, cx);
                        input.focus(w, cx);
                    });
                }
                "show-map" => self.compact_map = true,
                "show-tree" => self.compact_map = false,
                "menu" => {
                    self.editing = "";
                    self.menu = !self.menu;
                }
                "filter" => {
                    self.filters = !self.filters || self.menu || self.pane == 2;
                    self.menu = false;
                    if self.pane == 2 {
                        self.switch_pane(0);
                    }
                    if self.filters {
                        self.filter.update(cx, |input, cx| input.focus(w, cx));
                    }
                }
                "logical-size" | "allocated-size" => {
                    let allocated = a == "allocated-size";
                    if self.options.allocated == allocated {
                        return Ok(());
                    }
                    let from = self.metric_position().0;
                    self.options.allocated = allocated;
                    self.rebuild();
                    self.metric_slide = self
                        .motion_enabled
                        .then(|| (std::time::Instant::now(), from));
                }
                "fullpath" => {
                    self.options.full_path = !self.options.full_path;
                    self.rebuild()
                }
                "dup" => {
                    self.reset_checks();
                    self.options.dup = (self.options.dup + 1) % 3;
                    self.rebuild()
                }
                "only" => {
                    self.options.only = !self.options.only;
                    self.rebuild()
                }
                "show-dirs" => {
                    self.options.show_dirs = !self.options.show_dirs;
                    self.rebuild()
                }
                "columns" => self.columns_open = !self.columns_open,
                "prev-page" | "next-page" => {
                    let limit = self.options.limit;
                    let len = self.view.files.len();
                    let start = if a == "next-page" {
                        self.view.file_range.end
                    } else {
                        self.view.file_range.start.saturating_sub(limit)
                    };
                    if limit != 0 && start < len && start != self.view.file_range.start {
                        self.view
                            .set_file_range(start..start.saturating_add(limit).min(len));
                        if self.view.file_rows().contains(&self.selected) {
                            if !self.other_page_selection.is_empty() {
                                self.multi = std::mem::take(&mut self.other_page_selection);
                            }
                        } else if !self.multi.is_empty() {
                            self.other_page_selection = std::mem::take(&mut self.multi);
                        }
                        self.list_scroll.scroll_to_item(0, ScrollStrategy::Top);
                        self.focus.focus(w);
                    }
                }
                "selection-page" => self.sync_file_selection(true),
                "sort-name" | "sort-size" | "sort-time" => {
                    let col = match a {
                        "sort-name" => 0,
                        "sort-size" => 4,
                        _ => 5,
                    };
                    self.options.desc = if col == self.options.column {
                        !self.options.desc
                    } else {
                        col != 0
                    };
                    self.options.column = col;
                    self.rebuild()
                }
                "tree-columns" => self.tree_columns_open = !self.tree_columns_open,
                "up" => {
                    if self.root != 0
                        && let Some(t) = &self.tree
                    {
                        self.navigate(t.nodes[self.root].parent)
                    }
                }
                "zoom" => {
                    if let Some(t) = &self.tree {
                        self.navigate(if t.nodes[self.selected].dir {
                            self.selected
                        } else {
                            t.nodes[self.selected].parent
                        });
                    }
                }
                "home" => self.navigate(0),
                "back" => {
                    if let Some(id) = self.history.pop() {
                        self.future.push(self.root);
                        self.change_map_root(id);
                        self.reveal_tree_row(id);
                        self.selected = id;
                        self.explicit_selection = false;
                    }
                }
                "forward" => {
                    if let Some(id) = self.future.pop() {
                        self.history.push(self.root);
                        self.change_map_root(id);
                        self.reveal_tree_row(id);
                        self.selected = id;
                        self.explicit_selection = false;
                    }
                }
                "copy" => {
                    if let Some(t) = &self.tree {
                        let lines = self
                            .selected_ids()
                            .into_iter()
                            .map(|id| t.path(id).display().to_string())
                            .collect::<Vec<_>>();
                        cx.write_to_clipboard(ClipboardItem::new_string(lines.join("\r\n")))
                    }
                }
                "props" => platform::shell_verb(hwnd(w)?, &self.writable()?, "properties")?,
                "admin" => {
                    ensure!(self.jobs == 0, "Wait for the current task to finish");
                    platform::shell_verb(hwnd(w)?, &std::env::current_exe()?, "runas")?;
                    cx.quit()
                }
                "terminal" => platform::command_prompt(&self.writable()?)?,
                "file-copy" | "file-cut" => {
                    let paths = self
                        .selected_ids()
                        .into_iter()
                        .map(|id| self.check_item(id))
                        .collect::<Result<Vec<_>>>()?;
                    platform::clipboard_files(hwnd(w)?, &paths, a == "file-cut")?
                }
                "info" => self.copy_info(cx),
                "open" | "reveal" => {
                    let paths = self
                        .selected_ids()
                        .into_iter()
                        .map(|id| self.check_item(id))
                        .collect::<Result<Vec<_>>>()?;
                    for path in paths {
                        if a == "reveal" && !path.is_dir() {
                            cx.reveal_path(&path);
                        } else {
                            cx.open_with_system(&path);
                        }
                    }
                }
                "recycle" => {
                    let tree = self.tree.as_ref().context("Scan a location first")?;
                    let roots = self.recycle_roots(tree);
                    ensure!(!roots.is_empty(), "Select an item");
                    let paths = roots
                        .iter()
                        .map(|&id| self.check_item(id))
                        .collect::<Result<Vec<_>>>()?;
                    let mut description = paths
                        .iter()
                        .map(|p| p.display().to_string())
                        .collect::<Vec<_>>()
                        .join("\n");
                    let scope = self.filtered_folder_scope(tree, &roots);
                    let button = self.recycle_caption(scope.is_some(), roots.len());
                    if let Some((label, detail)) = scope {
                        description = format!("{label}\n{detail}\n\n{description}");
                    }
                    let rx = w.prompt(
                        PromptLevel::Warning,
                        self.text("Move to Recycle Bin?", "移至回收站？"),
                        Some(&description),
                        &[self.text("Cancel", "取消"), button],
                        cx,
                    );
                    cx.spawn_in(w, async move |this, cx| {
                        if rx.await == Ok(1) {
                            let _ = this.update(cx, |s, cx| {
                                let res = (|| -> Result<()> {
                                    let now = s
                                        .recycle_roots(s.tree.as_ref().context("Scan changed")?)
                                        .into_iter()
                                        .map(|id| s.check_item(id))
                                        .collect::<Result<Vec<_>>>()?;
                                    ensure!(now == paths, "Selection changed");
                                    for path in &paths {
                                        platform::recycle(path)?;
                                        s.removed.insert(path.clone());
                                    }
                                    Ok(())
                                })();
                                if let Err(e) = res {
                                    s.fail(e)
                                }
                                cx.notify()
                            });
                        }
                    })
                    .detach();
                }
                "rename" => {
                    let p = self.writable()?;
                    self.rename_source = Some(p.clone());
                    self.edit_value(
                        "rename",
                        p.file_name()
                            .unwrap_or_default()
                            .to_string_lossy()
                            .into_owned(),
                        w,
                        cx,
                    )
                }
                "export-panel" => {
                    self.export_panel = !self.export_panel;
                    self.focus.focus(w);
                }
                "ex-files" => self.export_opts.files = !self.export_opts.files,
                "ex-folders" => self.export_opts.folders = !self.export_opts.folders,
                "ex-selected" => self.export_opts.selected = !self.export_opts.selected,
                "ex-depth" => {
                    self.edit_value("ex-depth", self.export_opts.depth.to_string(), w, cx)
                }
                "ex-record" | "ex-dates" | "ex-folder-size" | "ex-split" | "ex-percent"
                | "ex-volume" => {
                    let i = [
                        "ex-record",
                        "ex-dates",
                        "ex-folder-size",
                        "ex-split",
                        "ex-percent",
                        "ex-volume",
                    ]
                    .iter()
                    .position(|s| *s == a)
                    .unwrap();
                    self.export_opts.fields[i] = !self.export_opts.fields[i]
                }
                "image-size" => self.edit_value(
                    "image-size",
                    format!("{}x{}", self.image_size.0, self.image_size.1),
                    w,
                    cx,
                ),
                "gray" => self.gray = !self.gray,
                "free" => self.free = !self.free,
                "unknown" => self.unknown = !self.unknown,
                "update" => self.edit_value("update", self.update_url.clone(), w, cx),
                "apply-edit" => {
                    let v = self.edit.read(cx).value().to_string();
                    if self.editing == "cell" {
                        let pixels = v.parse::<u32>()?;
                        ensure!((1..=40).contains(&pixels), "Expected 1–40 pixels");
                        self.min_cell = pixels as f32;
                        self.persist_view()?;
                    } else if self.editing == "ex-depth" {
                        self.export_opts.depth = v.parse()?
                    } else if self.editing == "image-size" {
                        let (w, h) = v.split_once('x').context("Expected WIDTHxHEIGHT")?;
                        let (w, h) = (w.parse::<u32>()?, h.parse::<u32>()?);
                        ensure!(
                            w > 0 && h > 0 && u64::from(w) * u64::from(h) <= 64_000_000,
                            "Invalid image dimensions"
                        );
                        self.image_size = (w, h);
                        self.persist_view()?;
                    } else if self.editing == "rename" {
                        let p = self.writable()?;
                        ensure!(
                            self.rename_source.as_ref() == Some(&p),
                            "Selection changed; choose Rename again"
                        );
                        platform::rename(&p, &v)?;
                        self.removed.insert(p);
                    } else if self.editing == "update" {
                        self.update_url = v.trim().to_owned();
                        platform::settings()?.set_value("updateUrl", &self.update_url)?;
                        let task = cx
                            .background_executor()
                            .spawn(async move { platform::update(&v) });
                        cx.spawn(async move |this, cx| {
                            let r = task.await;
                            let _ = this.update(cx, |s, cx| {
                                s.status = r.unwrap_or_else(|e| e.to_string());
                                cx.notify()
                            });
                        })
                        .detach()
                    }
                    self.editing = "";
                    self.focus.focus(w);
                }
                "dismiss" => {
                    self.editing = "";
                    self.focus.focus(w);
                }
                "csv" | "full" | "png" | "types-csv" | "full-types" | "mft" => {
                    self.save_as(a, w, cx)
                }
                "unit" => self.unit = (self.unit + 1) % 6,
                "depth" => self.depth = self.depth % 15 + 1,
                "colors" => self.by_type = !self.by_type,
                "labels" => self.labels = !self.labels,
                "cell" => self.edit_value("cell", self.min_cell.to_string(), w, cx),
                "tag" => {
                    let tags = TAG_COLORS;
                    let next = self
                        .tags
                        .get(&self.selected)
                        .and_then(|v| tags.iter().position(|c| c == v))
                        .map_or(0, |i| i + 1);
                    if let Some(c) = tags.get(next) {
                        self.tags.insert(self.selected, *c);
                    } else {
                        self.tags.remove(&self.selected);
                    }
                }
                "tag1" | "tag2" | "tag3" | "tag4" => {
                    let color = TAG_COLORS[usize::from(a.as_bytes()[3] - b'1')];
                    if self.tags.get(&self.selected) == Some(&color) {
                        self.tags.remove(&self.selected);
                    } else {
                        self.tags.insert(self.selected, color);
                    }
                }
                "clear-tags" => {
                    if let Some(tree) = &self.tree {
                        self.tags.retain(|&id, _| !tree.within(id, self.root));
                    }
                }
                "deeper" => self.depth = (self.depth + 1).min(15),
                "shallower" => self.depth = self.depth.saturating_sub(1).max(1),
                "language" => {
                    self.zh = !self.zh;
                    for (entity, en, cn) in [
                        (&self.filter, "Include", "包含"),
                        (&self.exclude, "Exclude", "排除"),
                        (&self.search, "Search file names", "搜索文件名"),
                    ] {
                        entity.update(cx, |v, cx| v.set_placeholder(self.text(en, cn), w, cx));
                    }
                    platform::settings()?
                        .set_value("Language", &if self.zh { "zh_CN" } else { "en" })?
                }
                "theme" => {
                    let mode = if self.system_theme {
                        "light"
                    } else if self.dark {
                        "auto"
                    } else {
                        "dark"
                    };
                    self.system_theme = mode == "auto";
                    self.dark = mode == "dark"
                        || (self.system_theme
                            && matches!(
                                w.appearance(),
                                WindowAppearance::Dark | WindowAppearance::VibrantDark
                            ));
                    Theme::change(
                        if self.dark {
                            ThemeMode::Dark
                        } else {
                            ThemeMode::Light
                        },
                        Some(w),
                        cx,
                    );
                    platform::settings()?.set_value("Theme", &mode)?
                }
                "shell-on" | "shell-off" => {
                    platform::shell(a == "shell-on")?;
                    self.status = self
                        .text("Explorer menu updated", "资源管理器菜单已更新")
                        .into()
                }
                _ => {}
            }
            Ok(())
        })();
        if let Err(e) = res {
            self.fail(e)
        } else if matches!(
            a,
            "unit"
                | "logical-size"
                | "allocated-size"
                | "show-dirs"
                | "dup"
                | "only"
                | "depth"
                | "deeper"
                | "shallower"
                | "colors"
                | "labels"
                | "cell"
                | "free"
                | "unknown"
        ) && let Err(e) = self.persist_view()
        {
            self.fail(e)
        }
        cx.notify()
    }
}
