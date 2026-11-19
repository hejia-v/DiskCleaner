use crate::scan::{Node, Tree, check, save};
use crate::view::{Totals, TypeRows};
use anyhow::{Context, Result, ensure};
use chrono::{DateTime, Local, NaiveDateTime, TimeZone};
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    sync::atomic::AtomicBool,
};

const BASE: &[&str] = &[
    "File Name",
    "Path",
    "Size",
    "Allocated",
    "Modified",
    "Attributes",
    "Files",
    "Folders",
];
const EXTRA: &[&str] = &[
    "MFTRECNO",
    "CREATEDDATE",
    "LASTACCESSDATE",
    "FOLDERSIZE",
    "FOLDERALLOCATED",
    "ROOT",
    "FOLDERNAME",
    "FILENAME",
    "FILEEXT",
    "PERCENTOFPARENT",
    "DRIVECAPACITY",
    "USEDSPACE",
    "FREESPACE",
    "RESERVEDSPACE",
];
const ATTRS: &[(u32, char)] = &[
    (1, 'R'),
    (2, 'H'),
    (4, 'S'),
    (32, 'A'),
    (2048, 'C'),
    (16384, 'E'),
    (256, 'T'),
    (4096, 'O'),
    (16, 'D'),
];
pub fn date(ft: u64) -> String {
    if ft == 0 {
        return String::new();
    }
    DateTime::from_timestamp_millis((ft / 10_000) as i64 - 11644473600000)
        .map(|t| {
            t.with_timezone(&Local)
                .format("%Y-%m-%d %H:%M:%S")
                .to_string()
        })
        .unwrap_or_default()
}
fn timestamp(value: &str) -> u64 {
    NaiveDateTime::parse_from_str(value, "%Y-%m-%d %H:%M:%S")
        .ok()
        .and_then(|d| Local.from_local_datetime(&d).earliest())
        .and_then(|t| u64::try_from(t.timestamp_millis() + 11644473600000).ok())
        .unwrap_or(0)
        * 10_000
}
pub fn attrs(bits: u32) -> String {
    ATTRS
        .iter()
        .filter_map(|&(b, c)| (bits & b != 0).then_some(c))
        .collect()
}
fn delimiter(path: &Path) -> u8 {
    if path
        .extension()
        .is_some_and(|e| e.eq_ignore_ascii_case("tsv"))
    {
        b'\t'
    } else {
        b','
    }
}
#[derive(Clone)]
pub struct ExportOptions {
    pub files: bool,
    pub folders: bool,
    pub selected: bool,
    pub depth: usize,
    pub fields: [bool; 6],
}
impl Default for ExportOptions {
    fn default() -> Self {
        Self {
            files: true,
            folders: true,
            selected: false,
            depth: 0,
            fields: [false; 6],
        }
    }
}
const GROUPS: [usize; 14] = [0, 1, 1, 2, 2, 3, 3, 3, 3, 4, 5, 5, 5, 5];
pub fn export(tree: &Tree, path: &Path, full: bool) -> Result<()> {
    write(
        tree,
        path,
        &ExportOptions {
            fields: [full; 6],
            ..Default::default()
        },
        1..tree.nodes.len(),
        &[],
        &AtomicBool::new(false),
    )
}
impl ExportOptions {
    pub fn allows(
        &self,
        tree: &Tree,
        id: usize,
        selected: &std::collections::HashSet<usize>,
    ) -> bool {
        if id == 0 || (tree.nodes[id].dir && !self.folders) || (!tree.nodes[id].dir && !self.files)
        {
            return false;
        }
        if !self.selected && self.depth == 0 {
            return true;
        }
        let mut ancestor = id;
        let mut depth = 0;
        loop {
            if self.selected && selected.contains(&ancestor) || !self.selected && ancestor == 0 {
                return true;
            }
            if ancestor == 0 || self.depth != 0 && depth == self.depth {
                return false;
            }
            ancestor = tree.nodes[ancestor].parent;
            depth += 1;
        }
    }
}
pub fn write(
    tree: &Tree,
    path: &Path,
    opt: &ExportOptions,
    rows: impl IntoIterator<Item = usize>,
    filtered: &[Totals],
    cancel: &AtomicBool,
) -> Result<()> {
    ensure!(
        opt.files || opt.folders,
        "Choose files or folders to export"
    );
    check(cancel)?;
    let root_path = tree.path(0);
    let volume = (tree.live && root_path.is_absolute())
        .then(|| crate::platform::volume_space(&root_path))
        .flatten();
    save(path, |out| {
        let mut w = csv::WriterBuilder::new()
            .delimiter(delimiter(path))
            .from_writer(out);
        let mut headers = BASE.to_vec();
        headers.extend(
            EXTRA
                .iter()
                .enumerate()
                .filter(|(i, _)| opt.fields[GROUPS[*i]])
                .map(|(_, s)| *s),
        );
        w.write_record(headers)?;
        for id in rows {
            check(cancel)?;
            let n = &tree.nodes[id];
            let (bytes, alloc, files, folders) = filtered
                .get(id)
                .map(|v| (v.bytes, v.alloc, v.files, v.folders))
                .unwrap_or((n.bytes, n.alloc, n.files, n.folders));
            let parent = tree.path(n.parent);
            let name = n.name.to_string_lossy();
            let mut row = vec![
                name.to_string(),
                parent.display().to_string(),
                bytes.to_string(),
                alloc.to_string(),
                date(n.modified),
                attrs(n.attrs | if n.dir { 16 } else { 0 }),
                files.to_string(),
                folders.to_string(),
            ];
            if opt.fields.iter().any(|b| *b) {
                let p = Path::new(&n.name);
                let whole = filtered
                    .get(n.parent)
                    .map_or(tree.nodes[n.parent].bytes, |v| v.bytes);
                row.extend([
                    n.record.to_string(),
                    date(n.created),
                    date(n.accessed),
                    if n.dir {
                        bytes.to_string()
                    } else {
                        String::new()
                    },
                    if n.dir {
                        alloc.to_string()
                    } else {
                        String::new()
                    },
                    parent
                        .components()
                        .next()
                        .map(|p| p.as_os_str().to_string_lossy().into_owned())
                        .unwrap_or_default(),
                    parent.display().to_string(),
                    p.file_stem()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .into_owned(),
                    p.extension()
                        .unwrap_or_default()
                        .to_string_lossy()
                        .into_owned(),
                    format!(
                        "{:.3}",
                        if whole == 0 {
                            0.0
                        } else {
                            100. * bytes as f64 / whole as f64
                        }
                    ),
                ]);
                if let Some((total, free)) = volume {
                    row.extend([
                        total.to_string(),
                        (total - free).to_string(),
                        free.to_string(),
                        "0".into(),
                    ])
                } else {
                    row.extend([String::new(), String::new(), String::new(), String::new()])
                }
            }
            w.write_record(
                row.iter()
                    .enumerate()
                    .filter(|(i, _)| *i < BASE.len() || opt.fields[GROUPS[*i - BASE.len()]])
                    .map(|(_, s)| s),
            )?;
        }
        w.flush()?;
        check(cancel)
    })
}
pub fn export_types(rows: &TypeRows, path: &Path, cancel: &AtomicBool) -> Result<()> {
    save(path, |out| {
        let mut w = csv::Writer::from_writer(out);
        w.write_record(["Extension", "Files", "Size", "Allocated"])?;
        for (ext, total) in rows {
            check(cancel)?;
            w.write_record([
                ext,
                &total.files.to_string(),
                &total.bytes.to_string(),
                &total.alloc.to_string(),
            ])?
        }
        w.flush()?;
        check(cancel)
    })
}
fn key(p: &Path) -> String {
    p.to_string_lossy()
        .replace('/', "\\")
        .trim_end_matches('\\')
        .to_lowercase()
}
fn ensure_dir(tree: &mut Tree, dirs: &mut HashMap<String, usize>, path: &Path) -> usize {
    let mut missing = Vec::new();
    let mut p = path;
    let parent = loop {
        if let Some(&id) = dirs.get(&key(p)) {
            break id;
        }
        missing.push(p);
        if let Some(next) = p.parent() {
            p = next
        } else {
            break 0;
        }
    };
    let mut id = parent;
    for p in missing.into_iter().rev() {
        id = tree.add(
            id,
            Node {
                name: p.file_name().unwrap_or(p.as_os_str()).into(),
                dir: true,
                ..Default::default()
            },
        );
        dirs.insert(key(p), id);
    }
    id
}
pub fn load(path: &Path, cancel: &AtomicBool) -> Result<Tree> {
    load_progress(path, cancel, &|_| {})
}
pub fn load_progress(path: &Path, cancel: &AtomicBool, progress: &dyn Fn(usize)) -> Result<Tree> {
    let mut r = csv::ReaderBuilder::new()
        .delimiter(delimiter(path))
        .flexible(true)
        .from_path(path)?;
    let headers = r.headers()?.clone();
    let cols: HashMap<_, _> = headers
        .iter()
        .enumerate()
        .map(|(i, s)| (s.trim().to_ascii_lowercase(), i))
        .collect();
    ensure!(
        cols.contains_key("file name") && cols.contains_key("path"),
        "CSV needs File Name and Path columns"
    );
    let mut tree = Tree::new(path.file_stem().unwrap_or_default().into(), false);
    // Third-party exports may list descendants before siblings or their directory row.
    // Find the common parent without retaining a second copy of the input records.
    let data_start = r.position().clone();
    let mut base: Option<PathBuf> = None;
    let path_column = cols["path"];
    let name_column = cols["file name"];
    let mut found = 0usize;
    for record in r.records() {
        check(cancel)?;
        let row = record?;
        if row.get(name_column).is_some_and(|name| !name.is_empty()) {
            found += 1;
            if found.is_multiple_of(256) {
                progress(found);
            }
        }
        let parent = Path::new(row.get(path_column).unwrap_or(""));
        match &mut base {
            None => base = Some(parent.into()),
            Some(root) => {
                while !root.as_os_str().is_empty()
                    && !Path::new(&key(parent)).starts_with(Path::new(&key(root)))
                {
                    if !root.pop() {
                        root.clear();
                        break;
                    }
                }
            }
        }
    }
    progress(found);
    r.seek(data_start)?;
    let base = base.unwrap_or_else(|| PathBuf::from(&tree.nodes[0].name));
    tree.nodes[0].name = base.as_os_str().into();
    let mut dirs = HashMap::from([(key(&base), 0)]);
    for result in r.records() {
        check(cancel)?;
        let row = result?;
        let get = |name: &str| cols.get(name).and_then(|&i| row.get(i)).unwrap_or("");
        let number = |col: &str| -> Result<u64> {
            let s = get(col).trim();
            if s.is_empty() {
                Ok(0)
            } else {
                s.parse().with_context(|| format!("Invalid {col}: {s}"))
            }
        };
        let name = get("file name");
        if name.is_empty() {
            continue;
        }
        let parent = PathBuf::from(get("path"));
        let root = ensure_dir(&mut tree, &mut dirs, &parent);
        let full = parent.join(name);
        let attr = ATTRS
            .iter()
            .filter(|(_, c)| get("attributes").contains(*c))
            .map(|(b, _)| *b)
            .sum();
        let dir = attr & 16 != 0
            || number("files")? > 0
            || number("folders")? > 0
            || !get("foldersize").is_empty()
            || dirs.contains_key(&key(&full));
        let bytes = number("size")?;
        let alloc = if get("allocated").is_empty() {
            bytes
        } else {
            number("allocated")?
        };
        let mut n = Node {
            name: name.into(),
            dir,
            attrs: attr,
            bytes,
            alloc,
            modified: timestamp(get("modified")),
            created: timestamp(get("createddate")),
            accessed: timestamp(get("lastaccessdate")),
            record: number("mftrecno")?.try_into()?,
            ..Default::default()
        };
        let k = key(&full);
        if let Some(&id) = dirs.get(&k) {
            n.children = std::mem::take(&mut tree.nodes[id].children);
            n.parent = tree.nodes[id].parent;
            tree.nodes[id] = n
        } else {
            let id = tree.add(root, n);
            if dir {
                dirs.insert(k, id);
            }
        }
    }
    tree.finish(cancel)?;
    Ok(tree)
}
