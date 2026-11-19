use crate::scan::{Node, Tree, check, save};
use anyhow::{Context, Result, bail, ensure};
use std::{
    collections::HashMap,
    ffi::OsString,
    fs::File,
    io::{BufReader, Read, Seek, SeekFrom, Write},
    os::windows::{ffi::OsStringExt, fs::OpenOptionsExt},
    path::Path,
    sync::atomic::AtomicBool,
};

const MAGIC: &[u8; 8] = b"WTMFT\0\0\x01";
fn field<const N: usize>(b: &[u8], i: usize) -> Result<[u8; N]> {
    Ok(b.get(i..)
        .and_then(|s| s.get(..N))
        .context("Truncated NTFS field")?
        .try_into()?)
}
fn u16at(b: &[u8], i: usize) -> Result<u16> {
    Ok(u16::from_le_bytes(field(b, i)?))
}
fn u32at(b: &[u8], i: usize) -> Result<u32> {
    Ok(u32::from_le_bytes(field(b, i)?))
}
fn u64at(b: &[u8], i: usize) -> Result<u64> {
    Ok(u64::from_le_bytes(field(b, i)?))
}

fn fixup(b: &mut [u8], sector: usize) -> Result<bool> {
    if !b.starts_with(b"FILE") {
        return Ok(false);
    }
    let off = u16at(b, 4)? as usize;
    let count = u16at(b, 6)? as usize;
    if count == 0 {
        return Ok(true);
    }
    if count < 2 || count - 1 != b.len() / sector || off + count * 2 > b.len() {
        return Ok(false);
    }
    let marker = field::<2>(b, off)?;
    for i in 1..count {
        let end = i * sector;
        if b[end - 2..end] != marker {
            return Ok(false);
        }
        let value = field::<2>(b, off + 2 * i)?;
        b[end - 2..end].copy_from_slice(&value);
    }
    Ok(true)
}
fn attributes(b: &[u8], mut visit: impl FnMut(u32, &[u8]) -> Result<()>) -> Result<()> {
    let used = u32at(b, 0x18)? as usize;
    let mut off = u16at(b, 0x14)? as usize;
    ensure!(
        off >= 0x30 && off <= used && used <= b.len(),
        "Invalid MFT record length"
    );
    while off + 4 <= used {
        let ty = u32at(b, off)?;
        if ty == u32::MAX {
            return Ok(());
        }
        let len = u32at(b, off + 4)? as usize;
        ensure!(
            len >= 24 && len <= used - off && (b[off + 8] == 0 || len >= 64),
            "Invalid MFT attribute length"
        );
        visit(ty, &b[off..off + len])?;
        off += len;
    }
    Ok(())
}
fn record(b: &mut [u8], sector: usize) -> Result<Option<(Node, u32)>> {
    if !fixup(b, sector)? || u16at(b, 0x16)? & 1 == 0 || u64at(b, 0x20)? != 0 {
        return Ok(None);
    }
    let mut n = Node {
        dir: u16at(b, 0x16)? & 2 != 0,
        record: u32at(b, 0x2c)?,
        ..Default::default()
    };
    let mut parent = 5;
    let mut priority = 0;
    let mut data = None;
    attributes(b, |ty, a| {
        if a[8] == 0 {
            let off = u16at(a, 20)? as usize;
            let len = u32at(a, 16)? as usize;
            ensure!(off >= 24, "Invalid resident attribute");
            let v = a
                .get(off..)
                .and_then(|s| s.get(..len))
                .context("Invalid MFT attribute value")?;
            match ty {
                0x10 if v.len() >= 36 => {
                    n.created = u64at(v, 0)?;
                    n.modified = u64at(v, 8)?;
                    n.accessed = u64at(v, 24)?;
                    n.attrs = u32at(v, 32)?;
                }
                0x30 if v.len() >= 66 => {
                    let rank = match v[65] {
                        1 => 4,
                        3 => 3,
                        0 => 2,
                        2 => 1,
                        _ => 0,
                    };
                    if rank > priority && v[64] > 0 {
                        let name = v
                            .get(66..66 + 2 * v[64] as usize)
                            .context("Invalid NTFS filename")?;
                        let chars: Vec<_> = name
                            .as_chunks::<2>()
                            .0
                            .iter()
                            .copied()
                            .map(u16::from_le_bytes)
                            .collect();
                        n.name = OsString::from_wide(&chars);
                        if n.attrs == 0 {
                            n.attrs = u32at(v, 56)?
                        }
                        if n.created == 0 {
                            n.created = u64at(v, 8)?
                        }
                        if n.modified == 0 {
                            n.modified = u64at(v, 16)?
                        }
                        if n.accessed == 0 {
                            n.accessed = u64at(v, 32)?
                        }
                        parent = u64at(v, 0)? as u32;
                        n.alloc = u64at(v, 40)?;
                        n.bytes = u64at(v, 48)?;
                        priority = rank;
                    }
                }
                0x80 if a[9] == 0 => data = Some((len as u64, len as u64)),
                _ => {}
            }
        } else if ty == 0x80 && a[9] == 0 && u64at(a, 16)? == 0 {
            data = Some((u64at(a, 48)?, u64at(a, 40)?))
        }
        Ok(())
    })?;
    if priority == 0 {
        return Ok(None);
    }
    if let Some((size, alloc)) = data {
        n.bytes = size;
        n.alloc = alloc
    }
    Ok(Some((n, parent)))
}
struct Records {
    tree: Tree,
    ids: HashMap<u32, usize>,
    parents: Vec<u32>,
    accepted: usize,
}
impl Records {
    fn new(name: OsString, live: bool) -> Self {
        Self {
            tree: Tree::new(name, live),
            ids: HashMap::from([(5, 0)]),
            parents: vec![5],
            accepted: 0,
        }
    }
    fn push(&mut self, r: &mut [u8], sector: usize) -> Result<()> {
        if let Some((n, p)) = record(r, sector)? {
            self.accepted += 1;
            if n.record == 5 {
                self.tree.nodes[0].record = 5;
                self.tree.nodes[0].modified = n.modified;
                return Ok(());
            }
            ensure!(
                Path::new(&n.name).file_name() == Some(n.name.as_os_str())
                    && !n.name.to_string_lossy().contains(':'),
                "Invalid NTFS path component"
            );
            if !self.ids.contains_key(&n.record) {
                self.ids.insert(n.record, self.tree.nodes.len());
                self.parents.push(p);
                self.tree.nodes.push(n);
            }
        }
        Ok(())
    }
    fn finish(mut self, cancel: &AtomicBool) -> Result<Tree> {
        for id in 1..self.tree.nodes.len() {
            check(cancel)?;
            let p = self.ids.get(&self.parents[id]).copied().unwrap_or(0);
            let p = if p != id && self.tree.nodes[p].dir {
                p
            } else {
                0
            };
            self.tree.nodes[id].parent = p;
            self.tree.nodes[p].children.push(id);
        }
        self.tree.mft_records = Some(self.accepted);
        self.tree.finish(cancel)?;
        Ok(self.tree)
    }
}
pub fn load(path: &Path, cancel: &AtomicBool) -> Result<Tree> {
    load_progress(path, cancel, &|_| {})
}
pub fn load_progress(path: &Path, cancel: &AtomicBool, progress: &dyn Fn(usize)) -> Result<Tree> {
    let f = File::open(path)?;
    let length = f.metadata()?.len();
    let mut input = BufReader::new(f);
    let mut magic = [0; 8];
    input.read_exact(&mut magic)?;
    let (size, count) = if &magic == MAGIC {
        let mut h = [0; 12];
        input.read_exact(&mut h)?;
        let size = u32at(&h, 0)? as u64;
        let count = u64at(&h, 4)?;
        ensure!(
            (512..=65536).contains(&size)
                && size.is_power_of_two()
                && count <= (length - 20) / size,
            "Invalid MFT header"
        );
        (size, count)
    } else {
        ensure!(
            length >= 1024 && length % 1024 == 0,
            "Invalid raw MFT length"
        );
        input.rewind()?;
        (1024, length / 1024)
    };
    ensure!(count <= 64 * 1024 * 1024, "Excessive MFT record count");
    let mut records = Records::new(path.file_stem().unwrap_or_default().into(), false);
    let mut buf = vec![0; size as usize];
    for i in 0..count {
        if i.is_multiple_of(256) {
            progress(records.accepted);
        }
        check(cancel)?;
        input.read_exact(&mut buf)?;
        let sector = sector_for_record(&buf);
        records.push(&mut buf, sector)?;
    }
    progress(records.accepted);
    records.finish(cancel)
}
fn sector_for_record(b: &[u8]) -> usize {
    let tails = u16::from_le_bytes([b[6], b[7]]).saturating_sub(1) as usize;
    if tails > 0 && b.len().is_multiple_of(tails) && (512..=4096).contains(&(b.len() / tails)) {
        b.len() / tails
    } else {
        512
    }
}

struct Volume {
    file: File,
    sector: usize,
    size: usize,
    count: u64,
    runs: Vec<(u64, u64)>,
}
impl Volume {
    fn open(path: &Path) -> Result<Self> {
        let letter = path
            .to_str()
            .and_then(|s| s.as_bytes().first())
            .copied()
            .context("Expected drive letter")?;
        ensure!(letter.is_ascii_alphabetic(), "Invalid drive");
        let file = File::options()
            .read(true)
            .share_mode(7)
            .open(format!(r"\\.\{}:", letter as char))?;
        Self::from_file(file)
    }
    fn from_file(mut file: File) -> Result<Self> {
        let mut boot = [0; 4096];
        file.read_exact(&mut boot)?;
        ensure!(&boot[3..11] == b"NTFS    ", "Volume is not NTFS");
        let sector = u16at(&boot, 11)? as usize;
        ensure!(
            [512, 1024, 2048, 4096].contains(&sector),
            "Invalid sector size"
        );
        let cluster = sector as u64 * boot[13] as u64;
        ensure!(cluster.is_power_of_two(), "Invalid cluster size");
        let factor = boot[64] as i8;
        let size = if factor < 0 {
            1u64.checked_shl(factor.unsigned_abs() as u32)
                .context("Invalid record exponent")?
        } else {
            cluster * factor as u64
        };
        ensure!(
            (512..=65536).contains(&size) && size % sector as u64 == 0,
            "Unsupported record size"
        );
        file.seek(SeekFrom::Start(
            u64at(&boot, 48)?
                .checked_mul(cluster)
                .context("Invalid MFT offset")?,
        ))?;
        let mut rec = vec![0; size as usize];
        file.read_exact(&mut rec)?;
        ensure!(fixup(&mut rec, sector)?, "Invalid MFT record 0");
        let mut runs = Vec::new();
        let mut length = 0;
        attributes(&rec, |ty, a| {
            if ty != 0x80 || a[8] == 0 || a[9] != 0 {
                return Ok(());
            }
            length = u64at(a, 48)?;
            let off = u16at(a, 32)? as usize;
            ensure!(off >= 64, "Invalid runlist offset");
            let mut r = a.get(off..).context("Invalid runlist offset")?;
            let mut lcn = 0i64;
            while let Some((&header, rest)) = r.split_first() {
                if header == 0 {
                    break;
                }
                let (n, d) = ((header & 15) as usize, (header >> 4) as usize);
                ensure!(
                    (1..=8).contains(&n) && (1..=8).contains(&d) && rest.len() >= n + d,
                    "Invalid MFT extent"
                );
                let mut len = [0; 8];
                len[..n].copy_from_slice(&rest[..n]);
                let mut delta = [if rest[n + d - 1] & 128 != 0 { 255 } else { 0 }; 8];
                delta[..d].copy_from_slice(&rest[n..n + d]);
                lcn = lcn
                    .checked_add(i64::from_le_bytes(delta))
                    .context("Invalid cluster offset")?;
                let bytes = u64::from_le_bytes(len)
                    .checked_mul(cluster)
                    .context("Invalid extent size")?;
                ensure!(lcn >= 0 && bytes > 0, "Invalid MFT extent");
                runs.push((
                    (lcn as u64)
                        .checked_mul(cluster)
                        .context("Invalid extent location")?,
                    bytes,
                ));
                r = &rest[n + d..];
            }
            Ok(())
        })?;
        ensure!(
            length > 0 && length / size <= 64 * 1024 * 1024,
            "Invalid MFT data size"
        );
        Ok(Self {
            file,
            sector,
            size: size as usize,
            count: length / size,
            runs,
        })
    }
    fn read(
        &mut self,
        cancel: &AtomicBool,
        mut consume: impl FnMut(&mut [u8]) -> Result<()>,
    ) -> Result<()> {
        let mut left = self.count * self.size as u64;
        let mut block = vec![0; 4 * 1024 * 1024];
        for &(offset, length) in &self.runs {
            if left == 0 {
                break;
            }
            self.file.seek(SeekFrom::Start(offset))?;
            let mut bytes = length.min(left);
            while bytes > 0 {
                check(cancel)?;
                let n = bytes.min(block.len() as u64) as usize;
                self.file.read_exact(&mut block[..n])?;
                consume(&mut block[..n])?;
                bytes -= n as u64;
                left -= n as u64;
            }
        }
        ensure!(left == 0, "Incomplete NTFS extent list");
        Ok(())
    }
}
pub fn drive(path: &Path, cancel: &AtomicBool, progress: &dyn Fn(usize)) -> Result<Tree> {
    let mut v = Volume::open(path)?;
    let (size, sector) = (v.size, v.sector);
    let mut records = Records::new(path.as_os_str().into(), true);
    let mut pending = Vec::new();
    v.read(cancel, |b| {
        pending.extend_from_slice(b);
        let end = pending.len() / size * size;
        for r in pending[..end].chunks_exact_mut(size) {
            check(cancel)?;
            records.push(r, sector)?;
        }
        progress(records.tree.nodes.len());
        pending.drain(..end);
        Ok(())
    })?;
    records.finish(cancel)
}
pub fn dump(drive: &Path, path: &Path, cancel: &AtomicBool) -> Result<()> {
    let mut volume = Volume::open(drive)?;
    save(path, |out| {
        header(out, volume.size as u32, volume.count)?;
        volume.read(cancel, |b| {
            out.write_all(b)?;
            Ok(())
        })?;
        check(cancel)
    })
}
fn header(w: &mut impl Write, size: u32, count: u64) -> Result<()> {
    w.write_all(MAGIC)?;
    w.write_all(&size.to_le_bytes())?;
    w.write_all(&count.to_le_bytes())?;
    Ok(())
}
fn synthetic(id: u32, parent: u32, name: &str, dir: bool, bytes: u64) -> Vec<u8> {
    let mut b = vec![0u8; 1024];
    let chars: Vec<_> = name.encode_utf16().collect();
    let att = 0x38;
    let v = att + 24;
    let len = (24 + 66 + chars.len() * 2).next_multiple_of(8);
    b[..4].copy_from_slice(b"FILE");
    for (off, val) in [
        (4, 0x30u16),
        (6, 3),
        (20, att as u16),
        (22, if dir { 3 } else { 1 }),
        (0x30, 1),
        (510, 1),
        (1022, 1),
        (att + 20, 24),
    ] {
        b[off..off + 2].copy_from_slice(&val.to_le_bytes())
    }
    for (off, val) in [
        (24, (att + len + 4) as u32),
        (28, 1024),
        (44, id),
        (att, 0x30),
        (att + 4, len as u32),
        (att + 16, (66 + chars.len() * 2) as u32),
        (att + len, u32::MAX),
    ] {
        b[off..off + 4].copy_from_slice(&val.to_le_bytes())
    }
    b[v + 56..v + 60].copy_from_slice(&(if dir { 16u32 } else { 32 }).to_le_bytes());
    let stamp = 133000000000000000u64;
    for (off, val) in [
        (v, parent as u64),
        (v + 8, stamp),
        (v + 16, stamp),
        (v + 24, stamp),
        (v + 32, stamp),
        (v + 40, bytes),
        (v + 48, bytes),
    ] {
        b[off..off + 8].copy_from_slice(&val.to_le_bytes())
    }
    b[v + 64] = chars.len() as u8;
    b[v + 65] = 1;
    for (i, c) in chars.into_iter().enumerate() {
        b[v + 66 + 2 * i..v + 68 + 2 * i].copy_from_slice(&c.to_le_bytes())
    }
    b
}
pub fn make(path: &Path, count: u32) -> Result<()> {
    if count > 64 * 1024 * 1024 - 11 {
        bail!("Excessive record count")
    }
    save(path, |w| {
        header(w, 1024, count as u64 + 11)?;
        for (id, (p, name)) in [
            (5, ""),
            (5, ""),
            (5, ""),
            (5, ""),
            (5, ""),
            (5, "ROOT"),
            (5, "Windows"),
            (5, "Users"),
            (6, "System32"),
            (6, "Temp"),
            (7, "alice"),
        ]
        .into_iter()
        .enumerate()
        {
            w.write_all(&synthetic(id as u32, p, name, true, 0))?
        }
        for i in 0..count {
            w.write_all(&synthetic(
                100 + i,
                8 + i % 3,
                &format!("file_{i}.dat"),
                false,
                1024 * (1 + (i as u64 * 37) % 4096),
            ))?
        }
        Ok(())
    })
}
pub fn selftest() -> Result<()> {
    let (n, p) =
        record(&mut synthetic(42, 8, "测试.txt", false, 1234), 512)?.context("Missing record")?;
    ensure!(
        n.bytes == 1234 && n.name == "测试.txt" && p == 8 && n.record == 42,
        "NTFS parser self-test failed"
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reads_fragmented_volume_and_cancels_before_export_commit() -> Result<()> {
        let mut file = tempfile::tempfile()?;
        let mut boot = [0u8; 4096];
        boot[3..11].copy_from_slice(b"NTFS    ");
        boot[11..13].copy_from_slice(&512u16.to_le_bytes());
        boot[13] = 1;
        boot[48..56].copy_from_slice(&8u64.to_le_bytes());
        boot[64] = (-10i8) as u8;
        file.write_all(&boot)?;
        let mut mft = synthetic(0, 5, "$MFT", false, 0);
        let off = u32at(&mft, 24)? as usize - 4;
        mft[off..off + 4].copy_from_slice(&0x80u32.to_le_bytes());
        mft[off + 4..off + 8].copy_from_slice(&72u32.to_le_bytes());
        mft[off + 8] = 1;
        mft[off + 32..off + 34].copy_from_slice(&64u16.to_le_bytes());
        mft[off + 48..off + 56].copy_from_slice(&2048u64.to_le_bytes());
        mft[off + 64..off + 71].copy_from_slice(&[0x11, 2, 8, 0x11, 2, 6, 0]);
        mft[off + 72..off + 76].copy_from_slice(&u32::MAX.to_le_bytes());
        mft[24..28].copy_from_slice(&((off + 76) as u32).to_le_bytes());
        file.write_all(&mft)?;
        file.seek(SeekFrom::Start(14 * 512))?;
        file.write_all(&synthetic(42, 5, "sample.bin", false, 321))?;
        file.rewind()?;
        let mut volume = Volume::from_file(file)?;
        let mut sizes = Vec::new();
        volume.read(&AtomicBool::new(false), |b| {
            let (node, _) = record(b, 512)?.unwrap();
            sizes.push(node.bytes);
            Ok(())
        })?;
        assert_eq!(sizes, [2048, 321]);
        let dir = tempfile::tempdir()?;
        let out = dir.path().join("out.mft");
        std::fs::write(&out, b"original")?;
        let cancel = AtomicBool::new(false);
        let outcome = save(&out, |w| {
            volume.read(&cancel, |b| {
                w.write_all(b)?;
                cancel.store(true, std::sync::atomic::Ordering::Relaxed);
                Ok(())
            })
        });
        assert!(outcome.is_err());
        assert_eq!(std::fs::read(out)?, b"original");
        Ok(())
    }
}
