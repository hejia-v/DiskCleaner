use super::*;
use crate::{
    scan::{Node, Tree},
    view::{Options, View},
};
use std::{collections::HashSet, fs, io::Write};
fn running() -> AtomicBool {
    AtomicBool::new(false)
}

#[test]
fn content_check_reads_past_first_block_and_rejects_stale_or_offline_files() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let prefix = vec![42; 128 * 1024];
    for (parent, last) in [("a", 1), ("b", 1), ("c", 2)] {
        let folder = dir.path().join(parent);
        fs::create_dir(&folder)?;
        let mut file = fs::File::create(folder.join("same.bin"))?;
        file.write_all(&prefix)?;
        file.write_all(&[last])?;
    }
    let mut tree = scan::folder(dir.path(), &running())?;
    let mut ids: Vec<_> = (0..tree.nodes.len())
        .filter(|&id| !tree.nodes[id].dir)
        .collect();
    ids.sort_by_key(|&id| tree.path(id));
    let mut view = View::default();
    view.rebuild(
        &tree,
        &Options {
            dup: 1,
            only: true,
            ..Default::default()
        },
        &HashSet::new(),
    );
    assert_eq!((view.duplicate_groups, view.files.len()), (1, 3));
    let classes = scan::content_groups(&tree, &ids, &running())?;
    assert_eq!(classes, [vec![ids[0], ids[1]], vec![ids[2]]]);
    let group = view.group_ids[ids[0]].unwrap();
    view.verified_groups.insert(group, classes);
    view.rebuild(
        &tree,
        &Options {
            dup: 1,
            only: true,
            ..Default::default()
        },
        &HashSet::new(),
    );
    assert_eq!(view.group_headers, [(0, 0), (3, 2)]);
    assert_eq!(view.display_count(ids[0]), 2);
    assert_eq!(view.display_count(ids[2]), 1);
    let mut filtered = Options {
        dup: 1,
        only: true,
        include: ".bin".into(),
        ..Default::default()
    };
    view.rebuild(&tree, &filtered, &HashSet::new());
    assert_eq!(view.group_headers, [(0, 0), (3, 2)]);
    filtered.exclude = tree.path(ids[0]).to_string_lossy().into_owned();
    filtered.full_path = true;
    view.rebuild(&tree, &filtered, &HashSet::new());
    assert_eq!(view.display_count(ids[1]), 1);
    assert_eq!(view.group_headers.len(), 2);
    view.verified_groups
        .insert(group, vec![vec![ids[1]], vec![ids[2]]]);
    filtered.exclude.clear();
    view.rebuild(&tree, &filtered, &HashSet::new());
    assert!(view.content_classes.is_empty());
    assert!(scan::content_groups(&tree, &ids, &AtomicBool::new(true)).is_err());
    fs::write(tree.path(ids[1]), b"changed")?;
    assert!(scan::content_groups(&tree, &ids[..2], &running()).is_err());
    tree.live = false;
    assert!(scan::content_groups(&tree, &ids, &running()).is_err());
    Ok(())
}

#[test]
fn filtered_folder_scope_keeps_the_whole_target_and_percentage_denominator() -> Result<()> {
    let mut tree = Tree::new("scan".into(), true);
    let mut revised = 0;
    for name in ["original", "backup", "revised"] {
        let dir = tree.add(
            0,
            Node {
                name: name.into(),
                dir: true,
                ..Default::default()
            },
        );
        if name == "revised" {
            revised = dir;
        }
        for (name, bytes, alloc) in [("record.bin", 65536, 65536), ("notes.txt", 4, 4096)] {
            tree.add(
                dir,
                Node {
                    name: name.into(),
                    bytes,
                    alloc,
                    ..Default::default()
                },
            );
        }
    }
    tree.finish(&running())?;
    let mut opt = Options {
        allocated: true,
        show_dirs: true,
        include: ".txt".into(),
        ..Default::default()
    };
    let mut view = View::default();
    view.rebuild(&tree, &opt, &HashSet::from([0]));
    let matched = view.scoped_totals(&tree, revised, false);
    assert_eq!((matched.files, matched.alloc, matched.bytes), (1, 4096, 4));
    assert_eq!(
        (tree.nodes[revised].files, tree.nodes[revised].alloc),
        (2, 68 * 1024)
    );
    assert_eq!(
        (view.weights[0], tree.nodes[0].alloc),
        (12 * 1024, 204 * 1024)
    );
    opt.search = "revised".into();
    view.rebuild(&tree, &opt, &HashSet::from([0]));
    assert_eq!(view.files, [revised]);
    assert_eq!(view.scoped_totals(&tree, revised, true).files, 0);
    assert_eq!(view.scoped_totals(&tree, revised, false).files, 1);
    opt.include.clear();
    opt.search.clear();
    view.rebuild(&tree, &opt, &HashSet::new());
    assert!(!view.filtered());
    assert_eq!(view.weights[0], tree.nodes[0].alloc);
    Ok(())
}
#[test]
fn grouped_pages_keep_selection_indices_and_count_each_matching_byte_once() -> Result<()> {
    let mut tree = Tree::new("snapshot".into(), false);
    for name in ["first", "second", "third"] {
        let folder = tree.add(
            0,
            Node {
                name: name.into(),
                dir: true,
                ..Default::default()
            },
        );
        for (name, bytes) in [("alpha.txt", 2), ("beta.bin", 3)] {
            tree.add(
                folder,
                Node {
                    name: name.into(),
                    bytes,
                    alloc: 8,
                    ..Default::default()
                },
            );
        }
    }
    tree.finish(&running())?;
    let mut view = View::default();
    let mut opt = Options {
        limit: 2,
        dup: 1,
        only: true,
        ..Default::default()
    };
    view.rebuild(&tree, &opt, &HashSet::new());
    assert_eq!(
        (
            view.duplicate_groups,
            view.result_totals.files,
            view.result_totals.bytes,
            view.result_totals.alloc
        ),
        (2, 6, 15, 48)
    );
    assert_eq!(view.group_headers, [(0, 0)]);
    let third = view.files[2];
    assert_eq!(view.reveal_file(third, 2), Some(0));
    assert_eq!(view.group_headers, [(0, 0), (2, 1)]);
    for index in 0..2 {
        assert_eq!(view.file_at_row(view.visual_row(index)), (index, false));
    }
    opt.search = "beta".into();
    view.rebuild(&tree, &opt, &HashSet::new());
    assert_eq!(
        (
            view.duplicate_groups,
            view.result_totals.files,
            view.result_totals.bytes
        ),
        (1, 3, 9)
    );
    opt.search.clear();
    opt.show_dirs = true;
    opt.only = false;
    view.rebuild(&tree, &opt, &HashSet::new());
    assert_eq!(
        (
            view.result_totals.folders,
            view.result_totals.bytes,
            view.result_totals.alloc
        ),
        (3, 15, 48)
    );
    opt.search = "first".into();
    view.rebuild(&tree, &opt, &HashSet::new());
    assert_eq!(
        (
            view.result_totals.files,
            view.result_totals.folders,
            view.result_totals.bytes
        ),
        (0, 1, 5)
    );
    Ok(())
}

#[test]
fn type_details_keep_the_scope_extension_and_independent_search() -> Result<()> {
    let mut tree = Tree::new("snapshot".into(), false);
    let folder = tree.add(
        0,
        Node {
            name: "src".into(),
            dir: true,
            ..Default::default()
        },
    );
    let mut file = |parent, name: &str| {
        tree.add(
            parent,
            Node {
                name: name.into(),
                bytes: 4,
                ..Default::default()
            },
        )
    };
    let src = file(folder, "sample.RS");
    let outside = file(0, "sample.RS");
    file(folder, "sample.rs.bak");
    let readme = file(folder, "README");
    tree.finish(&running())?;
    let mut options = Options {
        type_filter: Some((src, "rs".into())),
        show_dirs: true,
        ..Default::default()
    };
    let mut view = View::default();
    let expanded = HashSet::from([0, folder]);
    view.rebuild(&tree, &options, &expanded);
    assert_eq!(view.files, [src]);
    options.type_filter = Some((folder, "rs".into()));
    view.rebuild(&tree, &options, &expanded);
    assert_eq!(view.files, [src]);
    options.type_filter = Some((0, "rs".into()));
    options.only = true;
    options.dup = 1;
    options.search = "missing".into();
    view.rebuild(&tree, &options, &expanded);
    assert!(view.files.is_empty());
    assert_eq!(view.before_search, 2);
    options.search.clear();
    view.rebuild(&tree, &options, &expanded);
    assert_eq!(
        view.files.iter().copied().collect::<HashSet<_>>(),
        HashSet::from([src, outside])
    );
    options.only = false;
    options.type_filter = Some((folder, String::new()));
    view.rebuild(&tree, &options, &expanded);
    assert_eq!(view.files, [readme]);
    Ok(())
}

#[test]
fn tree_labels_keep_volume_roots_and_shorten_directory_paths() {
    for (source, label) in [
        (r"C:\scans\项目", "项目"),
        (r"C:\", r"C:\"),
        (r"\\server\share\", "share"),
        (r"\\server\share\folder", "folder"),
        ("Multiple selections", "Multiple selections"),
    ] {
        let tree = Tree::new(source.into(), false);
        assert_eq!(tree.nodes[0].display_name(), std::ffi::OsStr::new(label));
        assert_eq!(tree.path(0).as_os_str(), std::ffi::OsStr::new(source));
    }
}

#[test]
fn treemap_directory_headers_reserve_space_without_covering_files() {
    let mut tree = Tree::new("root".into(), false);
    let dir = tree.add(
        0,
        Node {
            name: "folder".into(),
            dir: true,
            ..Default::default()
        },
    );
    let file = tree.add(
        dir,
        Node {
            name: "file.bin".into(),
            ..Default::default()
        },
    );
    let bounds = map::Rect {
        x: 0.,
        y: 0.,
        w: 300.,
        h: 200.,
    };
    let weights = [4096; 3];
    let cells = map::layout(&tree, 0, &weights, bounds, &map::Style::default());
    assert_eq!(cells[2].id, file);
    assert_eq!(cells[2].header, 0.);
    assert_eq!(cells[2].weight, weights[file]);
    for pair in cells.windows(2) {
        assert!(pair[0].header > 0.);
        assert!(pair[1].rect.y >= pair[0].rect.y + pair[0].header);
    }
}

#[test]
fn mft_roundtrip_and_sparse_record_id() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let p = dir.path().join("sample.mft");
    mft::make(&p, 2000)?;
    let mut image = fs::read(&p)?;
    let id = 20 + 11 * 1024 + 44;
    image[id..id + 4].copy_from_slice(&0xfffffff0u32.to_le_bytes());
    fs::write(&p, image)?;
    let t = mft::load(&p, &running())?;
    assert_eq!(t.nodes.len(), 2006);
    assert_eq!(t.nodes[0].files, 2000);
    assert!(t.nodes.iter().any(|n| n.record == 0xfffffff0));
    assert!(!t.live);
    Ok(())
}

#[test]
fn wide_treemap_preserves_every_visible_file_and_area() -> Result<()> {
    let mut tree = Tree::new("root".into(), false);
    for index in 0..6000 {
        tree.add(
            0,
            Node {
                name: format!("file{index}").into(),
                bytes: 1,
                ..Default::default()
            },
        );
    }
    tree.finish(&running())?;
    let weights = tree.nodes.iter().map(|n| n.bytes).collect::<Vec<_>>();
    let cells = map::layout(
        &tree,
        0,
        &weights,
        map::Rect {
            x: 0.,
            y: 0.,
            w: 4002.,
            h: 3002.,
        },
        &map::Style {
            names: false,
            min: 1.,
            ..Default::default()
        },
    );
    assert_eq!(cells.len(), 6001);
    assert!(cells.iter().all(|cell| cell.header == 0.));
    let area: f64 = cells
        .iter()
        .filter(|c| c.id != 0)
        .map(|c| f64::from(c.rect.w) * f64::from(c.rect.h))
        .sum();
    assert!((area / 12_000_000. - 1.).abs() < 0.0001);
    Ok(())
}

#[test]
fn explorer_drag_contains_files_from_different_directories() -> Result<()> {
    use windows::Win32::{
        System::{
            Com::*,
            Ole::{CF_HDROP, ReleaseStgMedium},
        },
        UI::Shell::{DragQueryFileW, HDROP},
    };
    let dir = tempfile::tempdir()?;
    fs::create_dir(dir.path().join("nested"))?;
    let paths = [dir.path().join("a.txt"), dir.path().join("nested/第二.txt")];
    for path in &paths {
        fs::write(path, b"drag data")?;
    }
    unsafe {
        CoInitializeEx(None, COINIT_APARTMENTTHREADED).ok()?;
        struct Apartment;
        impl Drop for Apartment {
            fn drop(&mut self) {
                unsafe { CoUninitialize() }
            }
        }
        let _apartment = Apartment;
        let data = platform::drag_data(&paths)?;
        let mut medium = data.GetData(&FORMATETC {
            cfFormat: CF_HDROP.0,
            dwAspect: DVASPECT_CONTENT.0,
            lindex: -1,
            tymed: TYMED_HGLOBAL.0 as u32,
            ..Default::default()
        })?;
        let handle = HDROP(medium.u.hGlobal.0);
        let count = DragQueryFileW(handle, u32::MAX, None);
        let mut actual = Vec::new();
        for i in 0..count {
            let mut name = vec![0u16; DragQueryFileW(handle, i, None) as usize + 1];
            let length = DragQueryFileW(handle, i, Some(&mut name));
            actual.push(PathBuf::from(String::from_utf16_lossy(
                &name[..length as usize],
            )));
        }
        ReleaseStgMedium(&mut medium);
        assert_eq!(actual, paths);
    }
    Ok(())
}

#[test]
fn name_exclusion_keeps_matching_descendants_and_recomputes_both_sizes() -> Result<()> {
    let mut tree = Tree::new("root".into(), false);
    let folder = tree.add(
        0,
        Node {
            name: "omit".into(),
            dir: true,
            ..Default::default()
        },
    );
    tree.add(
        folder,
        Node {
            name: "keep.LOG".into(),
            bytes: 3,
            alloc: 64,
            ..Default::default()
        },
    );
    tree.add(
        folder,
        Node {
            name: "omit.log".into(),
            bytes: 5,
            alloc: 128,
            ..Default::default()
        },
    );
    tree.finish(&running())?;
    let mut view = View::default();
    let mut options = Options {
        exclude: "omit".into(),
        allocated: true,
        ..Default::default()
    };
    view.rebuild(&tree, &options, &HashSet::from([0, folder]));
    let totals = view.totals(&tree, 0);
    assert_eq!(
        (totals.bytes, totals.alloc, totals.files, totals.folders),
        (3, 64, 1, 1)
    );
    assert_eq!(view.rows.len(), 3);
    let (ext, stat) = &view.types[0];
    assert_eq!(
        (ext.as_str(), stat.bytes, stat.alloc, stat.files),
        ("log", 3, 64, 1)
    );
    options.show_dirs = true;
    options.limit = 1;
    view.rebuild(&tree, &options, &HashSet::from([0, folder]));
    assert_eq!((view.file_rows().len(), view.files.len()), (1, 2));
    let last = view.files[1];
    assert_eq!(view.reveal_file(last, 1), Some(0));
    assert_eq!(view.file_rows(), &[last]);
    options.full_path = true;
    view.rebuild(&tree, &options, &HashSet::from([0, folder]));
    assert!(view.files.is_empty());
    assert!(view.file_rows().is_empty());
    assert_eq!(view.rows, [(0, 0)]);
    Ok(())
}

#[test]
fn explorer_migration_preserves_other_applications() -> Result<()> {
    use winreg::{RegKey, enums::HKEY_CURRENT_USER};
    let scope = tempfile::tempdir()?;
    let path = format!(
        r"Software\DiskCleaner\Tests\{}",
        scope.path().file_name().unwrap().to_string_lossy()
    );
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let (classes, _) = hkcu.create_subkey(&path)?;
    let exe = scope.path().join("DiskCleaner.exe");
    let legacy = r"Directory\shell\WizTree";
    let foreign = r"Drive\shell\WizTree";
    for (verb, command) in [
        (legacy, format!("\"{}\" \"%1\"", exe.display())),
        (foreign, "other.exe".into()),
    ] {
        let entry = classes.create_subkey(verb)?.0;
        entry.set_value("", &"Scan with DiskCleaner")?;
        entry.create_subkey("command")?.0.set_value("", &command)?;
    }
    platform::configure_shell(&classes, &exe, true)?;
    assert!(classes.open_subkey(legacy).is_err());
    assert!(classes.open_subkey(foreign).is_ok());
    let cmd: String = classes
        .open_subkey(r"Directory\Background\shell\DiskCleaner\command")?
        .get_value("")?;
    assert_eq!(cmd, format!("\"{}\" --scan \"%V\"", exe.display()));
    platform::configure_shell(&classes, &exe, false)?;
    assert!(classes.open_subkey(r"Directory\shell\DiskCleaner").is_err());
    assert!(classes.open_subkey(foreign).is_ok());
    drop(classes);
    hkcu.delete_subkey_all(path)?;
    Ok(())
}

#[test]
fn class_colors_and_zoom_use_the_displayed_branch() -> Result<()> {
    let mut tree = Tree::new("root".into(), false);
    let folder = tree.add(
        0,
        Node {
            name: "photos".into(),
            dir: true,
            ..Default::default()
        },
    );
    for name in ["a.PNG", "b.jpeg", "c.mp4"] {
        tree.add(
            folder,
            Node {
                name: name.into(),
                bytes: 10,
                ..Default::default()
            },
        );
    }
    tree.finish(&running())?;
    let weights: Vec<_> = tree.nodes.iter().map(|n| n.bytes).collect();
    let cells = map::layout(
        &tree,
        folder,
        &weights,
        map::Rect {
            x: 0.,
            y: 0.,
            w: 500.,
            h: 400.,
        },
        &map::Style::default(),
    );
    let color = |id| cells.iter().find(|cell| cell.id == id).unwrap().color;
    assert_eq!(color(2), color(3));
    assert_ne!(color(2), color(4));
    assert!(tree.within(2, folder));
    assert!(!tree.within(0, folder));
    Ok(())
}
#[test]
fn malformed_mft_fails_and_unused_record_is_ignored() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let p = dir.path().join("bad.mft");
    mft::make(&p, 1)?;
    let original = fs::read(&p)?;
    for (offset, value) in [
        (12, u64::MAX.to_le_bytes().to_vec()),
        (20 + 60, u32::MAX.to_le_bytes().to_vec()),
        (20 + 56 + 20, u16::MAX.to_le_bytes().to_vec()),
    ] {
        let mut data = original.clone();
        data[offset..offset + value.len()].copy_from_slice(&value);
        fs::write(&p, data)?;
        assert!(mft::load(&p, &running()).is_err());
    }
    let mut data = original;
    data[20 + 22] = 0;
    data[20 + 60..20 + 64].fill(255);
    fs::write(&p, data)?;
    assert_eq!(mft::load(&p, &running())?.nodes.len(), 7);
    Ok(())
}
#[test]
fn folder_csv_multi_and_empty_directory_roundtrip() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let p = dir.path().join("data");
    fs::create_dir(&p)?;
    fs::create_dir(p.join("empty"))?;
    fs::write(p.join("逗号,名.txt"), b"abc")?;
    let t = scan::folder(&p, &running())?;
    assert_eq!(
        t.nodes[0].modified,
        std::os::windows::fs::MetadataExt::last_write_time(&fs::metadata(&p)?)
    );
    assert_eq!(t.nodes[0].bytes, 3);
    assert_eq!(t.nodes[0].folders, 1);
    for ext in ["csv", "tsv"] {
        let dest = dir.path().join(format!("result.{ext}"));
        table::export(&t, &dest, true)?;
        let t = table::load(&dest, &running())?;
        assert_eq!(
            (t.nodes[0].bytes, t.nodes[0].files, t.nodes[0].folders),
            (3, 1, 1)
        );
        assert!(!t.live)
    }
    let merged = scan::load(&[p.clone(), dir.path().join("missing"), p], &running())?;
    assert_eq!(merged.skipped, 1);
    assert_eq!(merged.nodes[0].bytes, 6);
    assert_eq!(merged.nodes[0].files, 2);
    Ok(())
}
#[test]
fn cancel_and_failed_export_preserve_previous_file() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let p = dir.path().join("out.csv");
    fs::write(&p, b"original")?;
    let err = scan::save(&p, |w| {
        w.write_all(b"partial")?;
        anyhow::bail!("canceled")
    });
    assert!(err.is_err());
    assert_eq!(fs::read(p)?, b"original");
    assert!(scan::folder(dir.path(), &AtomicBool::new(true)).is_err());
    Ok(())
}
#[test]
fn filtered_metric_and_duplicates_share_the_scan_denominator() -> Result<()> {
    let mut t = Tree::new("root".into(), false);
    let folder = t.add(
        0,
        Node {
            name: "sub".into(),
            dir: true,
            ..Default::default()
        },
    );
    for p in [0, folder] {
        t.add(
            p,
            Node {
                name: "same.txt".into(),
                bytes: 1,
                alloc: 100,
                ..Default::default()
            },
        );
    }
    t.add(
        0,
        Node {
            name: "big.bin".into(),
            bytes: 98,
            alloc: 200,
            ..Default::default()
        },
    );
    t.finish(&running())?;
    let mut v = View::default();
    let mut options = Options {
        only: true,
        dup: 1,
        ..Default::default()
    };
    v.rebuild(&t, &options, &HashSet::from([0]));
    assert_eq!(v.files.len(), 2);
    assert_eq!(v.weights[0], 100);
    options.allocated = true;
    v.rebuild(&t, &options, &HashSet::new());
    assert_eq!(v.weights[0], 400);
    assert_eq!(v.rows.len(), 1);
    options.include = ".txt".into();
    options.search = "absent".into();
    v.rebuild(&t, &options, &HashSet::new());
    assert_eq!(v.weights[0], 200);
    assert!(v.files.is_empty());
    Ok(())
}
#[test]
fn tiny_cells_keep_their_area_and_free_space_has_no_file_id() -> Result<()> {
    let mut t = Tree::new("root".into(), false);
    for b in [1, 99] {
        t.add(
            0,
            Node {
                name: b.to_string().into(),
                bytes: b,
                ..Default::default()
            },
        );
    }
    t.finish(&running())?;
    let weights = [100, 1, 99];
    let r = map::Rect {
        x: 0.,
        y: 0.,
        w: 1000.,
        h: 500.,
    };
    let mut style = map::Style {
        names: false,
        min: 1.,
        free: 100,
        ..Default::default()
    };
    let cells = map::layout(&t, 0, &weights, r, &style);
    let large = cells.iter().find(|c| c.id == 2).unwrap().rect;
    let free = cells.iter().find(|c| c.id == map::FREE).unwrap();
    assert_eq!(free.rect.w, 500.);
    style.min = 12.;
    let cells = map::layout(&t, 0, &weights, r, &style);
    assert!(cells.iter().all(|c| c.id != 1));
    assert_eq!(cells.iter().find(|c| c.id == 2).unwrap().rect.w, large.w);
    let selected = map::layout_with_selection(&t, 0, &weights, r, &style, Some(1));
    let tiny = selected.iter().find(|c| c.id == 1).unwrap();
    assert_eq!(tiny.weight, 1);
    assert!(tiny.rect.w.min(tiny.rect.h) < style.min);
    assert_eq!(selected.iter().find(|c| c.id == 2).unwrap().rect.w, large.w);
    Ok(())
}
#[test]
fn rename_never_overwrites_an_existing_item() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let a = dir.path().join("a");
    let b = dir.path().join("b");
    fs::write(&a, b"A")?;
    fs::write(&b, b"B")?;
    assert!(platform::rename(&a, "b").is_err());
    assert!(platform::rename(&a, "../escape").is_err());
    assert_eq!(fs::read(b)?, b"B");
    platform::rename(&a, "c")?;
    assert_eq!(fs::read(dir.path().join("c"))?, b"A");
    Ok(())
}

#[test]
fn csv_child_first_preserves_common_root_and_custom_fields() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let file = dir.path().join("input.csv");
    fs::write(
        &file,
        "Path,File Name,Size,Allocated,Attributes\r\nC:\\sample\\sub,one,10,12,A\r\nC:\\sample,sub,10,12,D\r\nC:\\sample,two,7,8,A\r\n",
    )?;
    let tree = table::load(&file, &running())?;
    assert_eq!(tree.path(0), PathBuf::from(r"C:\sample"));
    assert_eq!(tree.nodes[0].files, 2);
    assert_eq!(tree.nodes[0].folders, 1);
    let out = dir.path().join("selected.csv");
    let sub = tree.nodes.iter().position(|n| n.name == "sub").unwrap();
    let options = table::ExportOptions {
        folders: false,
        selected: true,
        depth: 1,
        fields: [true, false, false, false, true, false],
        ..Default::default()
    };
    let selected = HashSet::from([sub]);
    let rows = (1..tree.nodes.len()).filter(|&id| options.allows(&tree, id, &selected));
    table::write(&tree, &out, &options, rows, &[], &running())?;
    let mut csv = csv::Reader::from_path(out)?;
    assert_eq!(csv.headers()?.len(), 10);
    assert_eq!(&csv.headers()?[9], "PERCENTOFPARENT");
    let rows: Vec<_> = csv.records().collect::<Result<_, _>>()?;
    assert_eq!(rows.len(), 1);
    assert_eq!(&rows[0][0], "one");
    Ok(())
}
