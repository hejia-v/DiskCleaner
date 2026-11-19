use anyhow::{Context, Result, bail, ensure};
use std::{
    collections::HashMap,
    ffi::OsString,
    fs,
    os::windows::fs::MetadataExt,
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
};

#[derive(Default, Debug)]
pub struct Node {
    pub name: OsString,
    pub parent: usize,
    pub children: Vec<usize>,
    pub dir: bool,
    pub bytes: u64,
    pub alloc: u64,
    pub files: u64,
    pub folders: u64,
    pub attrs: u32,
    pub modified: u64,
    pub created: u64,
    pub accessed: u64,
    pub record: u32,
}

impl Node {
    pub fn display_name(&self) -> &std::ffi::OsStr {
        use std::path::{Component, Prefix};
        let path = Path::new(&self.name);
        if let Some(name) = path.file_name() {
            return name;
        }
        if let Some(Component::Prefix(prefix)) = path.components().next()
            && let Prefix::UNC(_, share) | Prefix::VerbatimUNC(_, share) = prefix.kind()
        {
            return share;
        }
        &self.name
    }

    pub fn weight(&self, allocated: bool) -> u64 {
        if allocated { self.alloc } else { self.bytes }
    }
}

#[derive(Debug)]
pub struct Tree {
    pub nodes: Vec<Node>,
    pub live: bool,
    pub skipped: usize,
    pub mft_records: Option<usize>,
}
pub fn check(cancel: &AtomicBool) -> Result<()> {
    ensure!(!cancel.load(Ordering::Relaxed), "Scan canceled");
    Ok(())
}

/// Fingerprint each stream once, then confirm equality within each bucket byte for byte.
pub fn content_groups(
    tree: &Tree,
    members: &[usize],
    cancel: &AtomicBool,
) -> Result<Vec<Vec<usize>>> {
    use std::hash::{DefaultHasher, Hasher};
    use std::io::{Read, Seek};
    use std::os::windows::fs::OpenOptionsExt;
    ensure!(tree.live && members.len() >= 2, "Live files required");
    let open = |id: usize| -> Result<fs::File> {
        let node = &tree.nodes[id];
        let path = tree.path(id);
        let file = fs::OpenOptions::new()
            .read(true)
            .share_mode(1)
            .open(&path)
            .with_context(|| path.display().to_string())?;
        let meta = file.metadata()?;
        ensure!(
            meta.is_file() && meta.len() == node.bytes && meta.last_write_time() == node.modified,
            "File changed: {}",
            path.display()
        );
        Ok(file)
    };
    let mut buckets: HashMap<(u64, u64), Vec<usize>> = HashMap::new();
    let mut groups: Vec<Vec<usize>> = Vec::new();
    let mut a = vec![0; 128 * 1024];
    let mut b = vec![0; a.len()];
    for &id in members {
        check(cancel)?;
        let mut other = open(id)?;
        let size = tree.nodes[id].bytes;
        let mut hash = DefaultHasher::new();
        let mut left = size;
        while left > 0 {
            check(cancel)?;
            let len = left.min(a.len() as u64) as usize;
            other.read_exact(&mut b[..len])?;
            hash.write(&b[..len]);
            left -= len as u64;
        }
        let candidates = buckets.entry((size, hash.finish())).or_default();
        let mut found = None;
        for &index in candidates.iter() {
            let mut reference = open(groups[index][0])?;
            other.rewind()?;
            let mut remaining = size;
            while remaining > 0 {
                check(cancel)?;
                let len = remaining.min(a.len() as u64) as usize;
                reference.read_exact(&mut a[..len])?;
                other.read_exact(&mut b[..len])?;
                if a[..len] != b[..len] {
                    break;
                }
                remaining -= len as u64;
            }
            if remaining == 0 {
                found = Some(index);
                break;
            }
        }
        if let Some(index) = found {
            groups[index].push(id);
        } else {
            candidates.push(groups.len());
            groups.push(vec![id]);
        }
    }
    Ok(groups)
}
impl Tree {
    pub fn new(name: OsString, live: bool) -> Self {
        Self {
            nodes: vec![Node {
                name,
                dir: true,
                ..Default::default()
            }],
            live,
            skipped: 0,
            mft_records: None,
        }
    }
    pub fn add(&mut self, parent: usize, mut node: Node) -> usize {
        let id = self.nodes.len();
        node.parent = parent;
        self.nodes.push(node);
        self.nodes[parent].children.push(id);
        id
    }
    pub fn within(&self, mut id: usize, root: usize) -> bool {
        while id != root && id != 0 {
            id = self.nodes[id].parent;
        }
        id == root
    }
    pub fn path(&self, mut id: usize) -> PathBuf {
        let mut parts = Vec::new();
        while id != 0 {
            parts.push(&self.nodes[id].name);
            id = self.nodes[id].parent;
        }
        let mut result = PathBuf::from(&self.nodes[0].name);
        for part in parts.into_iter().rev() {
            result.push(part);
        }
        result
    }
    pub fn finish(&mut self, cancel: &AtomicBool) -> Result<()> {
        let mut order = vec![0];
        let mut index = 0;
        while index < order.len() {
            check(cancel)?;
            order.extend_from_slice(&self.nodes[order[index]].children);
            index += 1;
        }
        ensure!(
            order.len() == self.nodes.len(),
            "Invalid snapshot hierarchy"
        );
        for id in order.into_iter().rev() {
            check(cancel)?;
            if !self.nodes[id].dir {
                continue;
            }
            let mut children = std::mem::take(&mut self.nodes[id].children);
            children.sort_unstable_by(|&a, &b| {
                self.nodes[b]
                    .bytes
                    .cmp(&self.nodes[a].bytes)
                    .then_with(|| self.nodes[a].name.cmp(&self.nodes[b].name))
            });
            let (mut bytes, mut alloc, mut files, mut folders) = (0u64, 0u64, 0u64, 0u64);
            for &c in &children {
                let n = &self.nodes[c];
                bytes = bytes.checked_add(n.bytes).context("Size overflow")?;
                alloc = alloc.checked_add(n.alloc).context("Allocation overflow")?;
                files += n.files + u64::from(!n.dir);
                folders += n.folders + u64::from(n.dir);
            }
            let n = &mut self.nodes[id];
            (n.bytes, n.alloc, n.files, n.folders, n.children) =
                (bytes, alloc, files, folders, children);
        }
        Ok(())
    }
    pub fn types(&self, cancel: &AtomicBool) -> Result<Vec<(String, u64, u64, u64)>> {
        let mut map = HashMap::new();
        for n in self.nodes.iter().filter(|n| !n.dir) {
            check(cancel)?;
            let ext = Path::new(&n.name)
                .extension()
                .unwrap_or_default()
                .to_string_lossy()
                .to_lowercase();
            let (count, size, alloc) = map.entry(ext).or_insert((0, 0, 0));
            *count += 1;
            *size += n.bytes;
            *alloc += n.alloc;
        }
        let mut rows: Vec<_> = map.into_iter().map(|(s, (n, b, a))| (s, n, b, a)).collect();
        rows.sort_by_key(|r| std::cmp::Reverse(r.2));
        Ok(rows)
    }
}
pub fn folder(path: &Path, cancel: &AtomicBool) -> Result<Tree> {
    enumerate(path, cancel, &|_| {})
}
fn enumerate(path: &Path, cancel: &AtomicBool, progress: &dyn Fn(usize)) -> Result<Tree> {
    let path = std::path::absolute(path)?;
    let meta = fs::symlink_metadata(&path)?;
    ensure!(meta.is_dir(), "Expected a directory: {}", path.display());
    let cluster = crate::platform::cluster_size(&path);
    let mut tree = Tree::new(path.clone().into_os_string(), true);
    tree.nodes[0].modified = meta.last_write_time();
    let mut pending = vec![(0, path)];
    while let Some((id, path)) = pending.pop() {
        check(cancel)?;
        let entries = match fs::read_dir(&path) {
            Ok(e) => e,
            Err(e) if id == 0 => return Err(e.into()),
            Err(_) => {
                tree.skipped += 1;
                continue;
            }
        };
        for entry in entries {
            check(cancel)?;
            let Ok(entry) = entry else {
                tree.skipped += 1;
                continue;
            };
            let Ok(meta) = entry.metadata() else {
                tree.skipped += 1;
                continue;
            };
            let attrs = meta.file_attributes();
            let dir = attrs & 0x10 != 0;
            let bytes = if dir { 0 } else { meta.file_size() };
            let child = tree.add(
                id,
                Node {
                    name: entry.file_name(),
                    dir,
                    bytes,
                    alloc: bytes.div_ceil(cluster).saturating_mul(cluster),
                    modified: meta.last_write_time(),
                    accessed: meta.last_access_time(),
                    created: meta.creation_time(),
                    attrs,
                    ..Default::default()
                },
            );
            if child.is_multiple_of(256) {
                progress(child);
            }
            if dir && attrs & 0x400 == 0 {
                pending.push((child, entry.path()));
            }
        }
        progress(tree.nodes.len() - 1);
    }
    tree.finish(cancel)?;
    Ok(tree)
}

fn load_one(path: &Path, cancel: &AtomicBool, progress: &dyn Fn(usize)) -> Result<Tree> {
    check(cancel)?;
    let ext = path
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .to_lowercase();
    if path.is_dir() {
        if path.parent().is_none() && path.to_string_lossy().as_bytes().get(1) == Some(&b':') {
            if let Ok(tree) = crate::mft::drive(path, cancel, progress) {
                return Ok(tree);
            }
            check(cancel)?;
        }
        enumerate(path, cancel, progress)
    } else {
        match ext.as_str() {
            "mft" | "bin" => crate::mft::load_progress(path, cancel, progress),
            "csv" | "tsv" => crate::table::load_progress(path, cancel, progress),
            _ => bail!("Unsupported scan location: {}", path.display()),
        }
    }
}

pub fn load(paths: &[PathBuf], cancel: &AtomicBool) -> Result<Tree> {
    load_progress(paths, cancel, &|_| {})
}
pub fn load_progress(
    paths: &[PathBuf],
    cancel: &AtomicBool,
    progress: &dyn Fn(usize),
) -> Result<Tree> {
    ensure!(!paths.is_empty(), "Choose a location");
    if let [path] = paths {
        return load_one(path, cancel, progress);
    }
    let mut merged = Tree::new("Multiple selections".into(), true);
    for path in paths {
        let tree = match load_one(path, cancel, &|n| progress(merged.nodes.len() + n)) {
            Ok(tree) => tree,
            Err(_) => {
                check(cancel)?;
                merged.skipped += 1;
                continue;
            }
        };
        let base = merged.nodes.len();
        merged.live &= tree.live;
        merged.skipped += tree.skipped;
        merged.nodes[0].children.push(base);
        for (id, mut n) in tree.nodes.into_iter().enumerate() {
            n.parent = if id == 0 { 0 } else { base + n.parent };
            for c in &mut n.children {
                *c += base
            }
            merged.nodes.push(n);
        }
    }
    ensure!(
        !merged.nodes[0].children.is_empty(),
        "No locations could be scanned"
    );
    merged.finish(cancel)?;
    Ok(merged)
}

pub fn save(
    path: &Path,
    write: impl FnOnce(&mut std::io::BufWriter<&mut fs::File>) -> Result<()>,
) -> Result<()> {
    use std::io::Write;
    let dir = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let mut tmp = tempfile::NamedTempFile::new_in(dir)?;
    {
        let mut out = std::io::BufWriter::new(tmp.as_file_mut());
        write(&mut out)?;
        out.flush()?;
    }
    tmp.persist(path)?;
    Ok(())
}
