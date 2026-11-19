#![windows_subsystem = "windows"]
mod map;
mod mft;
mod platform;
mod scan;
mod table;
#[cfg(test)]
mod tests;
mod ui;
mod view;

use anyhow::{Result, ensure};
use clap::Parser;
use std::{
    ffi::OsString,
    path::{Path, PathBuf},
    sync::atomic::AtomicBool,
};

#[derive(Parser, Clone, Debug, Default)]
#[command(
    name = "DiskCleaner",
    version,
    about = "Windows disk usage analyzer · Rust + GPUI"
)]
pub struct Args {
    /// Scan a folder, drive, MFT dump, CSV or semicolon-separated locations.
    #[arg(long)]
    scan: Option<String>,
    #[arg(value_name = "LOCATION")]
    location: Option<PathBuf>,
    #[arg(long)]
    scan_mft: Option<PathBuf>,
    #[arg(long)]
    scan_csv: Option<PathBuf>,
    #[arg(long)]
    scan_multi: Option<String>,
    #[arg(long)]
    scan_drive: Option<String>,
    #[arg(long)]
    dump: Option<PathBuf>,
    #[arg(long)]
    check_sort: Option<PathBuf>,
    #[arg(long,num_args=2,value_names=["FILE","COUNT"])]
    make_mft: Vec<OsString>,
    #[arg(long,num_args=2,value_names=["DRIVE","FILE"])]
    dump_mft: Vec<PathBuf>,
    #[arg(long)]
    export: Option<PathBuf>,
    #[arg(long)]
    export_full: Option<PathBuf>,
    #[arg(long)]
    treemap_png: Option<PathBuf>,
    #[arg(long)]
    recycle: Option<PathBuf>,
    #[arg(long,num_args=2,value_names=["PATH","NAME"])]
    rename: Vec<OsString>,
    #[arg(long)]
    copy_info: bool,
    #[arg(long,value_parser=clap::value_parser!(u8).range(0..=2),default_value_t=0)]
    tab: u8,
    #[arg(long,value_parser=clap::value_parser!(u8).range(0..=2))]
    dup: Option<u8>,
    #[arg(long)]
    dup_only: bool,
    #[arg(long, default_value = "")]
    filter: String,
    #[arg(long)]
    filter_path: bool,
    #[arg(long,value_parser=clap::value_parser!(u8).range(0..=8))]
    sort: Option<u8>,
    #[arg(long)]
    sort_desc: bool,
    #[arg(long,value_parser=["auto","light","dark"])]
    theme: Option<String>,
    #[arg(long,value_parser=["en","zh_CN"])]
    lang: Option<String>,
    #[arg(long)]
    check_update: Option<String>,
    #[arg(long)]
    shell_install: bool,
    #[arg(long)]
    shell_uninstall: bool,
    #[arg(long)]
    selftest: bool,
    #[arg(long)]
    shot: Option<PathBuf>,
    #[arg(long, default_value_t = 2500)]
    delay: u64,
    /// Initial window size in logical pixels (WIDTHxHEIGHT).
    #[arg(long)]
    geometry: Option<String>,
}
fn locations(s: &str) -> Vec<PathBuf> {
    s.split(';')
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .collect()
}
fn drive_root(s: &str) -> Result<PathBuf> {
    let letter = s.trim_end_matches([':', '\\', '/']);
    ensure!(
        letter.len() == 1 && letter.as_bytes()[0].is_ascii_alphabetic(),
        "Invalid drive letter"
    );
    Ok(PathBuf::from(format!("{}:\\", letter.to_ascii_uppercase())))
}
fn run(args: Args) -> Result<()> {
    let cancel = AtomicBool::new(false);
    if args.selftest {
        mft::selftest()?;
        println!("selftest PASS");
        return Ok(());
    }
    if args.shell_install || args.shell_uninstall {
        return platform::shell(args.shell_install);
    }
    if let Some(url) = &args.check_update {
        println!("update-check {}", platform::update(url)?);
        return Ok(());
    }
    if let [file, count] = args.make_mft.as_slice() {
        return mft::make(Path::new(file), count.to_string_lossy().parse()?);
    }
    if let [drive, path] = args.dump_mft.as_slice() {
        return mft::dump(&drive_root(&drive.to_string_lossy())?, path, &cancel);
    }
    if let [path, name] = args.rename.as_slice() {
        return platform::rename(Path::new(path), &name.to_string_lossy());
    }
    if let Some(path) = &args.recycle {
        return platform::recycle(path);
    }
    let mut roots = args.scan.as_deref().map(locations).unwrap_or_default();
    if let Some(path) = &args.location {
        roots.push(path.clone())
    }
    if let Some(path) = &args.scan_mft {
        let t = mft::load(path, &cancel)?;
        println!(
            "scan-mft nodes= {} records= {}",
            t.nodes.len(),
            t.mft_records.unwrap_or(0)
        );
        return Ok(());
    }
    let headless = if let Some(path) = &args.scan_csv {
        Some(("scan-csv", vec![path.clone()]))
    } else if let Some(s) = &args.scan_multi {
        Some(("scan-multi", locations(s)))
    } else if let Some(s) = &args.scan_drive {
        Some(("scan-drive", vec![drive_root(s)?]))
    } else if let Some(p) = &args.check_sort {
        Some(("check-sort", vec![p.clone()]))
    } else {
        args.dump.as_ref().map(|p| ("dump", vec![p.clone()]))
    };
    if let Some((cmd, paths)) = headless {
        let t = if cmd == "scan-csv" {
            table::load(&paths[0], &cancel)?
        } else if matches!(cmd, "check-sort" | "dump") {
            scan::folder(&paths[0], &cancel)?
        } else {
            scan::load(&paths, &cancel)?
        };
        let n = &t.nodes[0];
        if cmd == "scan-drive" {
            println!(
                "mft= {} records= {}",
                t.mft_records.is_some(),
                t.mft_records.unwrap_or(0)
            )
        }
        if cmd == "check-sort" {
            ensure!(
                t.nodes.iter().all(|n| n
                    .children
                    .windows(2)
                    .all(|w| t.nodes[w[0]].bytes >= t.nodes[w[1]].bytes)),
                "Sort violations"
            );
            println!("violations= 0");
        }
        if cmd == "dump" {
            for (id, node) in t.nodes.iter().take(12).enumerate() {
                println!(
                    "{id} {:?} size {} children {}",
                    node.name,
                    node.bytes,
                    node.children.len()
                );
            }
        }
        let parts = if paths.len() > 1 { n.children.len() } else { 1 };
        println!(
            "{cmd} parts= {} nodes= {} size {} allocated {} files {} folders {} skipped {}",
            parts,
            t.nodes.len(),
            n.bytes,
            n.alloc,
            n.files,
            n.folders,
            t.skipped
        );
        return Ok(());
    }
    if args.shot.is_none()
        && (args.export.is_some() || args.export_full.is_some() || args.treemap_png.is_some())
    {
        let t = scan::load(&roots, &cancel)?;
        return export(&t, &args);
    }
    ui::run(args, roots)
}
fn export(t: &scan::Tree, a: &Args) -> Result<()> {
    if let Some(p) = &a.export {
        table::export(t, p, false)?
    }
    if let Some(p) = &a.export_full {
        table::export(t, p, true)?
    }
    if let Some(p) = &a.treemap_png {
        let style = map::Style::default();
        let weights = if a.filter.trim().is_empty() {
            t.nodes.iter().map(|n| n.alloc).collect()
        } else {
            let mut filtered = view::View::default();
            filtered.rebuild(
                t,
                &view::Options {
                    include: a.filter.clone(),
                    full_path: a.filter_path,
                    allocated: true,
                    ..Default::default()
                },
                &Default::default(),
            );
            filtered.weights
        };
        let cells = map::layout(
            t,
            0,
            &weights,
            map::Rect {
                x: 0.,
                y: 0.,
                w: 1920.,
                h: 1080.,
            },
            &style,
        );
        map::png(
            p,
            t,
            &cells,
            (1920, 1080),
            &style,
            a.lang.as_deref() == Some("zh_CN"),
            &AtomicBool::new(false),
        )?;
    }
    Ok(())
}
fn main() {
    // GUI subsystem keeps Explorer launches quiet. Redirected CLI handles are inherited.
    unsafe {
        windows_sys::Win32::System::Console::AttachConsole(
            windows_sys::Win32::System::Console::ATTACH_PARENT_PROCESS,
        );
    }
    let args = Args::parse();
    if let Err(e) = run(args) {
        eprintln!("DiskCleaner: {e:#}");
        std::process::exit(1)
    }
}
