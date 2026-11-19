use crate::scan::Tree;
use std::collections::{HashMap, HashSet};

#[derive(Default)]
pub struct Options {
    pub limit: usize,
    pub show_dirs: bool,
    pub allocated: bool,
    pub include: String,
    pub exclude: String,
    pub full_path: bool,
    pub search: String,
    pub type_filter: Option<(usize, String)>,
    pub dup: u8,
    pub only: bool,
    pub column: u8,
    pub desc: bool,
}
#[derive(Clone, Copy, Default)]
pub struct Totals {
    pub bytes: u64,
    pub alloc: u64,
    pub files: u64,
    pub folders: u64,
}
impl Totals {
    pub fn weight(self, allocated: bool) -> u64 {
        if allocated { self.alloc } else { self.bytes }
    }
}
pub type TypeRows = Vec<(String, Totals)>;

pub fn type_name(ext: &str, zh: bool) -> String {
    let (en, cn) = match ext {
        "" => ("File", "文件"),
        "txt" | "log" | "md" => ("Text document", "文本文档"),
        "ini" | "cfg" => ("Configuration", "配置文件"),
        "exe" | "com" => ("Application", "应用程序"),
        "dll" | "ocx" => ("Application extension", "应用程序扩展"),
        "sys" | "drv" => ("System file", "系统文件"),
        "jpg" | "jpeg" | "png" | "gif" | "bmp" | "svg" | "tif" | "webp" => ("Image", "图像"),
        "ico" => ("Icon", "图标"),
        "mp3" | "wav" | "flac" | "ogg" | "aac" => ("Audio", "音频"),
        "mp4" | "avi" | "mkv" | "mov" | "wmv" | "mpeg" => ("Video", "视频"),
        "zip" | "7z" | "rar" | "gz" | "cab" | "iso" => ("Archive", "归档文件"),
        "pdf" => ("PDF document", "PDF 文档"),
        "doc" | "docx" => ("Word document", "Word 文档"),
        "xls" | "xlsx" => ("Excel worksheet", "Excel 工作表"),
        "ppt" | "pptx" => ("PowerPoint", "演示文稿"),
        "html" | "htm" => ("HTML document", "HTML 文档"),
        "css" => ("Style sheet", "样式表"),
        "js" | "ps1" | "py" => ("Script", "脚本"),
        "bat" | "cmd" => ("Batch file", "批处理文件"),
        _ => {
            return format!(
                "{} {}",
                ext.to_uppercase(),
                if zh { "文件" } else { "file" }
            );
        }
    };
    if zh { cn.into() } else { en.into() }
}
pub fn add_type(types: &mut HashMap<String, Totals>, node: &crate::scan::Node) {
    let total = types.entry(extension(node)).or_default();
    total.files += 1;
    total.bytes += node.bytes;
    total.alloc += node.alloc;
}
pub fn extension(node: &crate::scan::Node) -> String {
    std::path::Path::new(&node.name)
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .to_lowercase()
}
pub fn name_matches(node: &crate::scan::Node, needle: &str) -> bool {
    needle.is_empty() || node.name.to_string_lossy().to_lowercase().contains(needle)
}
pub fn type_rows(types: HashMap<String, Totals>, allocated: bool) -> TypeRows {
    let mut rows: TypeRows = types.into_iter().collect();
    rows.sort_unstable_by(|a, b| {
        b.1.weight(allocated)
            .cmp(&a.1.weight(allocated))
            .then_with(|| a.0.cmp(&b.0))
    });
    rows
}
#[derive(Default)]
pub struct View {
    pub tree_sort: u8,
    pub tree_ascending: bool,
    pub before_search: usize,
    pub file_range: std::ops::Range<usize>,
    pub duplicate_groups: usize,
    pub group_ids: Vec<Option<usize>>,
    // (visual row, page-relative file index) for non-selectable group headings.
    pub group_headers: Vec<(usize, usize)>,
    pub result_totals: Totals,
    pub weights: Vec<u64>,
    pub included: Vec<bool>,
    pub files: Vec<usize>,
    pub counts: Vec<u64>,
    pub rows: Vec<(usize, usize)>,
    pub types: TypeRows,
    pub(crate) filtered_totals: Vec<Totals>,
    pub(crate) search_folder_totals: Vec<Totals>,
    pub content_classes: HashMap<usize, (usize, usize)>,
    pub verified_groups: HashMap<usize, Vec<Vec<usize>>>,
}
impl View {
    pub fn scoped_totals(&self, tree: &Tree, id: usize, file_list: bool) -> Totals {
        if file_list && let Some(total) = self.search_folder_totals.get(id) {
            return *total;
        }
        self.totals(tree, id)
    }
    pub fn filtered(&self) -> bool {
        !self.filtered_totals.is_empty()
    }
    pub fn display_group(&self, id: usize) -> Option<usize> {
        self.content_classes
            .get(&id)
            .map(|v| v.0)
            .or_else(|| self.group_ids.get(id).copied().flatten())
    }
    pub fn display_count(&self, id: usize) -> u64 {
        self.content_classes
            .get(&id)
            .map_or(self.counts[id], |&(_, count)| count as u64)
    }
    pub fn set_file_range(&mut self, range: std::ops::Range<usize>) {
        self.file_range = range;
        self.group_headers.clear();
        if self.group_ids.is_empty() {
            return;
        }
        let mut previous = None;
        for (index, &id) in self.files[self.file_range.clone()].iter().enumerate() {
            let group = self.display_group(id);
            if previous != group {
                self.group_headers
                    .push((index + self.group_headers.len(), index));
            }
            previous = group;
        }
    }
    pub fn visual_row(&self, file: usize) -> usize {
        file + self
            .group_headers
            .partition_point(|&(_, first)| first <= file)
    }
    pub fn file_at_row(&self, row: usize) -> (usize, bool) {
        match self.group_headers.binary_search_by_key(&row, |&(at, _)| at) {
            Ok(index) => (self.group_headers[index].1, true),
            Err(headings) => (row - headings, false),
        }
    }
    pub fn file_rows(&self) -> &[usize] {
        &self.files[self.file_range.clone()]
    }

    pub fn reveal_file(&mut self, id: usize, limit: usize) -> Option<usize> {
        if let Some(row) = self.file_rows().iter().position(|&item| item == id) {
            return Some(row);
        }
        if limit == 0 || self.file_range.len() == self.files.len() {
            return None;
        }
        let index = self.files.iter().position(|&item| item == id)?;
        let start = index / limit * limit;
        self.set_file_range(start..(start + limit).min(self.files.len()));
        Some(index - start)
    }
    pub fn totals(&self, t: &Tree, id: usize) -> Totals {
        self.filtered_totals.get(id).copied().unwrap_or_else(|| {
            let n = &t.nodes[id];
            Totals {
                bytes: n.bytes,
                alloc: n.alloc,
                files: n.files,
                folders: n.folders,
            }
        })
    }
    pub fn rebuild(&mut self, t: &Tree, opt: &Options, expanded: &HashSet<usize>) {
        let count = t.nodes.len();
        self.weights = vec![0; count];
        self.included = vec![false; count];
        self.counts = vec![0; count];
        self.files.clear();
        self.result_totals = Totals::default();
        self.search_folder_totals.clear();
        self.group_ids.clear();
        if opt.dup != 0 {
            self.group_ids.resize(count, None);
        }
        self.before_search = 0;
        let mut groups = HashMap::new();
        let mut types = HashMap::new();
        let inc = opt.include.trim().to_lowercase();
        let exc = opt.exclude.trim().to_lowercase();
        let search = opt.search.to_lowercase();
        let filtering = !inc.is_empty() || !exc.is_empty();
        self.filtered_totals.clear();
        if filtering {
            self.filtered_totals.resize(count, Totals::default());
        }
        let mut order = vec![0];
        let mut in_scope = opt
            .type_filter
            .as_ref()
            .filter(|(root, _)| *root != 0)
            .map(|(root, _)| (*root, vec![false; count]));
        let mut i = 0;
        while i < order.len() {
            let id = order[i];
            i += 1;
            let n = &t.nodes[id];
            if let Some((root, inside)) = &mut in_scope {
                inside[id] = id == *root || inside[n.parent];
            }
            let text = if inc.is_empty() && exc.is_empty() {
                String::new()
            } else if opt.full_path {
                t.path(id).to_string_lossy().to_lowercase()
            } else {
                n.name.to_string_lossy().to_lowercase()
            };
            order.extend_from_slice(&n.children);
            self.included[id] = id == 0
                || ((inc.is_empty() || text.contains(&inc))
                    && (exc.is_empty() || !text.contains(&exc)));
            // The representative is stable across filters; the count follows the filter.
            if !n.dir && opt.dup != 0 {
                groups
                    .entry((&n.name, n.bytes, if opt.dup == 2 { n.modified } else { 0 }))
                    .or_insert((id, 0u64))
                    .1 += u64::from(self.included[id]);
            }
            if !n.dir && self.included[id] {
                self.weights[id] = n.weight(opt.allocated);
                add_type(&mut types, n);
                if filtering {
                    self.filtered_totals[id].bytes = n.bytes;
                    self.filtered_totals[id].alloc = n.alloc;
                }
            }
        }
        for &id in order.iter().rev() {
            if id != 0 && self.included[id] {
                let p = t.nodes[id].parent;
                self.weights[p] += self.weights[id];
                if filtering {
                    let child = self.filtered_totals[id];
                    let parent = &mut self.filtered_totals[p];
                    parent.bytes += child.bytes;
                    parent.alloc += child.alloc;
                    parent.files += child.files + u64::from(!t.nodes[id].dir);
                    parent.folders += child.folders + u64::from(t.nodes[id].dir);
                }
                self.included[p] = true
            }
        }
        for (id, n) in t
            .nodes
            .iter()
            .enumerate()
            .filter(|(id, n)| *id != 0 && (!n.dir || opt.show_dirs) && self.included[*id])
        {
            let group = groups
                .get(&(&n.name, n.bytes, if opt.dup == 2 { n.modified } else { 0 }))
                .copied()
                .filter(|&(_, count)| !n.dir && count > 1);
            if let Some((group_id, copies)) = group {
                self.counts[id] = copies;
                self.group_ids[id] = Some(group_id);
            }
            if opt.only && opt.dup != 0 && group.is_none() {
                continue;
            }
            if let Some((_, ext)) = &opt.type_filter
                && (n.dir
                    || in_scope.as_ref().is_some_and(|(_, inside)| !inside[id])
                    || extension(n) != *ext)
            {
                continue;
            }
            self.before_search += 1;
            if name_matches(n, &search) {
                self.files.push(id);
                if n.dir {
                    self.result_totals.folders += 1;
                } else {
                    self.result_totals.files += 1;
                    self.result_totals.bytes += n.bytes;
                    self.result_totals.alloc += n.alloc;
                }
            }
        }
        self.content_classes.clear();
        for (&group, classes) in &self.verified_groups {
            let visible = |id: usize| self.group_ids.get(id) == Some(&Some(group));
            let mut members = classes.iter().flatten().copied().filter(|&id| visible(id));
            let Some(first) = members.next() else {
                continue;
            };
            // Restoring hidden candidates must never certify a file we did not read.
            if 1 + members.count() != self.counts[first] as usize {
                continue;
            }
            for class in classes {
                let copies = class.iter().filter(|&&id| visible(id)).count();
                for &id in class.iter().filter(|&&id| visible(id)) {
                    self.content_classes.insert(id, (class[0], copies));
                }
            }
        }
        // A matching folder represents its included subtree. Count nested matches once.
        if self.result_totals.folders != 0 {
            if !search.is_empty() {
                self.search_folder_totals.resize(count, Totals::default());
                for &id in &self.files {
                    let n = &t.nodes[id];
                    if !n.dir {
                        self.search_folder_totals[id] = Totals {
                            files: 1,
                            bytes: n.bytes,
                            alloc: n.alloc,
                            ..Default::default()
                        };
                    }
                }
                for &id in order.iter().skip(1).rev() {
                    let child = self.search_folder_totals[id];
                    let parent = &mut self.search_folder_totals[t.nodes[id].parent];
                    parent.files += child.files;
                    parent.bytes += child.bytes;
                    parent.alloc += child.alloc;
                }
            }
            let mut covered = vec![false; count];
            for &id in &self.files {
                covered[id] = true;
            }
            self.result_totals.bytes = 0;
            self.result_totals.alloc = 0;
            for &id in order.iter().skip(1) {
                if covered[t.nodes[id].parent] {
                    covered[id] = true;
                } else if covered[id] {
                    let totals = self.totals(t, id);
                    self.result_totals.bytes += totals.bytes;
                    self.result_totals.alloc += totals.alloc;
                }
            }
        }
        self.types = type_rows(types, opt.allocated);
        let mut files = std::mem::take(&mut self.files);
        files.sort_unstable_by(|&a, &b| {
            let (x, y) = (&t.nodes[a], &t.nodes[b]);
            let cmp = match opt.column {
                0 => x.name.cmp(&y.name),
                1 => t.path(x.parent).cmp(&t.path(y.parent)),
                2 => self.totals(t, a).bytes.cmp(&self.totals(t, b).bytes),
                3 => self.totals(t, a).alloc.cmp(&self.totals(t, b).alloc),
                4 => self.weights[a].cmp(&self.weights[b]),
                5 => x.modified.cmp(&y.modified),
                6 => x.attrs.cmp(&y.attrs),
                7 => self.display_count(a).cmp(&self.display_count(b)),
                8 => (self.display_count(a) * x.bytes).cmp(&(self.display_count(b) * y.bytes)),
                _ => std::cmp::Ordering::Equal,
            };
            let cmp = cmp.then_with(|| x.name.cmp(&y.name));
            if opt.desc { cmp.reverse() } else { cmp }
        });
        self.duplicate_groups = 0;
        if opt.dup != 0 {
            // Order groups by their first member under the selected column sort.
            let mut rank = HashMap::new();
            for &id in &files {
                if let Some(group) = self.group_ids[id] {
                    let next = rank.len();
                    rank.entry(group).or_insert(next);
                }
            }
            self.duplicate_groups = rank.len();
            files.sort_by_key(|&id| {
                let (class, copies) = self.content_classes.get(&id).copied().unwrap_or_default();
                (
                    self.group_ids[id].map_or(usize::MAX, |g| rank[&g]),
                    std::cmp::Reverse(copies),
                    class,
                )
            });
        }
        self.files = files;
        self.set_file_range(
            0..if opt.limit == 0 {
                self.files.len()
            } else {
                opt.limit.min(self.files.len())
            },
        );
        self.tree_rows(t, expanded);
    }
    pub fn tree_rows(&mut self, t: &Tree, expanded: &HashSet<usize>) {
        self.rows.clear();
        let mut stack = vec![(0, 0)];
        while let Some((id, depth)) = stack.pop() {
            if !self.included[id] && id != 0 {
                continue;
            }
            self.rows.push((id, depth));
            if expanded.contains(&id) {
                let mut kids = t.nodes[id].children.clone();
                kids.sort_unstable_by(|&a, &b| {
                    let cmp = match self.tree_sort {
                        0 => t.nodes[a].name.cmp(&t.nodes[b].name),
                        1 => self.weights[a].cmp(&self.weights[b]),
                        2 => self.totals(t, a).bytes.cmp(&self.totals(t, b).bytes),
                        3 => self.totals(t, a).alloc.cmp(&self.totals(t, b).alloc),
                        4 => {
                            let x = self.totals(t, a);
                            let y = self.totals(t, b);
                            (x.files + x.folders).cmp(&(y.files + y.folders))
                        }
                        5 => self.totals(t, a).files.cmp(&self.totals(t, b).files),
                        6 => self.totals(t, a).folders.cmp(&self.totals(t, b).folders),
                        7 => t.nodes[a].modified.cmp(&t.nodes[b].modified),
                        _ => t.nodes[a].attrs.cmp(&t.nodes[b].attrs),
                    }
                    .then_with(|| t.nodes[a].name.cmp(&t.nodes[b].name));
                    if self.tree_ascending {
                        cmp
                    } else {
                        cmp.reverse()
                    }
                });
                stack.extend(kids.into_iter().rev().map(|c| (c, depth + 1)))
            }
        }
    }
}
pub fn format_size(value: u64, unit: usize) -> String {
    let units = ["B", "KiB", "MiB", "GiB", "TiB"];
    let mut v = value as f64;
    let mut index = 0;
    while index < 4
        && (if unit == 0 {
            v >= 1024.
        } else {
            index + 1 < unit
        })
    {
        v /= 1024.;
        index += 1
    }
    if index == 0 {
        format!("{value} B")
    } else {
        format!("{v:.2} {}", units[index])
    }
}
