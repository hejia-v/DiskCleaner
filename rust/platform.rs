use anyhow::{Context, Result, ensure};
use std::{
    ffi::OsStr,
    os::windows::{ffi::OsStrExt, process::CommandExt},
    path::Path,
    process::Command,
    ptr::null_mut,
};
use windows_sys::Win32::{
    Foundation::*, Graphics::Gdi::*, Storage::FileSystem::*, UI::WindowsAndMessaging::*,
};
use winreg::{RegKey, enums::*};

fn wide(s: &OsStr) -> Vec<u16> {
    s.encode_wide().chain(Some(0)).collect()
}
fn volume_root(path: &Path) -> Option<Vec<u16>> {
    let mut root = vec![0; 32768];
    (unsafe {
        GetVolumePathNameW(
            wide(path.as_os_str()).as_ptr(),
            root.as_mut_ptr(),
            root.len() as u32,
        )
    } != 0)
        .then_some(root)
}
pub fn cluster_size(path: &Path) -> u64 {
    let Some(root) = volume_root(path) else {
        return 4096;
    };
    let (mut spc, mut bps, mut free, mut total) = (0, 0, 0, 0);
    if unsafe { GetDiskFreeSpaceW(root.as_ptr(), &mut spc, &mut bps, &mut free, &mut total) } != 0
        && spc > 0
        && bps > 0
    {
        spc as u64 * bps as u64
    } else {
        4096
    }
}
pub fn volume_space(path: &Path) -> Option<(u64, u64)> {
    let root = volume_root(path)?;
    let (mut total, mut free) = (0, 0);
    (unsafe { GetDiskFreeSpaceExW(root.as_ptr(), null_mut(), &mut total, &mut free) } != 0)
        .then_some((total, free))
}
pub fn volume_label(root: &str) -> Option<String> {
    use windows_sys::Win32::System::Diagnostics::Debug::{
        SEM_FAILCRITICALERRORS, SetThreadErrorMode,
    };
    let mut previous = 0;
    let changed = unsafe { SetThreadErrorMode(SEM_FAILCRITICALERRORS, &mut previous) } != 0;
    let mut label = [0u16; 261];
    let ok = unsafe {
        GetVolumeInformationW(
            wide(OsStr::new(root)).as_ptr(),
            label.as_mut_ptr(),
            label.len() as u32,
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            0,
        )
    } != 0;
    if changed {
        unsafe {
            SetThreadErrorMode(previous, null_mut());
        }
    }
    ok.then(|| String::from_utf16_lossy(label.split(|&c| c == 0).next().unwrap_or_default()))
}

pub fn volume_drive(path: &Path) -> Option<String> {
    fn letter(path: &[u16]) -> Option<String> {
        let path = String::from_utf16_lossy(path);
        let path = path.strip_prefix(r"\\?\").unwrap_or(&path);
        let bytes = path.as_bytes();
        (bytes.len() == 3 && bytes[0].is_ascii_alphabetic() && &bytes[1..] == b":\\")
            .then(|| path.to_ascii_uppercase())
    }
    let root = volume_root(path)?;
    if let Some(drive) = letter(root.split(|&v| v == 0).next()?) {
        return Some(drive);
    }
    // A folder mount can share a volume with a drive letter elsewhere.
    let mut guid = [0; 64];
    if unsafe { GetVolumeNameForVolumeMountPointW(root.as_ptr(), guid.as_mut_ptr(), 64) } == 0 {
        return None;
    }
    let mut length = 0;
    unsafe {
        GetVolumePathNamesForVolumeNameW(guid.as_ptr(), null_mut(), 0, &mut length);
    }
    if length == 0 {
        return None;
    }
    let mut mounts = vec![0; length as usize];
    if unsafe {
        GetVolumePathNamesForVolumeNameW(guid.as_ptr(), mounts.as_mut_ptr(), length, &mut length)
    } == 0
    {
        return None;
    }
    mounts.split(|&v| v == 0).find_map(letter)
}
fn item(path: &Path) -> Result<std::path::PathBuf> {
    let p = std::path::absolute(path)?;
    ensure!(
        p.parent().is_some() && p.file_name().is_some(),
        "Select a file or subfolder"
    );
    std::fs::symlink_metadata(&p)?;
    Ok(p)
}
pub fn rename(path: &Path, name: &str) -> Result<()> {
    let path = item(path)?;
    ensure!(
        !name.is_empty()
            && !name.ends_with(['.', ' '])
            && !name.contains(['\\', '/', ':', '*', '?', '"', '<', '>', '|'])
            && !name.chars().any(char::is_control),
        "Invalid file name"
    );
    let target = path.with_file_name(name);
    ensure!(
        unsafe {
            MoveFileW(
                wide(path.as_os_str()).as_ptr(),
                wide(target.as_os_str()).as_ptr(),
            )
        } != 0,
        "{}",
        std::io::Error::last_os_error()
    );
    Ok(())
}
pub fn recycle(path: &Path) -> Result<()> {
    use windows::{
        Win32::{System::Com::*, UI::Shell::*},
        core::PCWSTR,
    };
    let path = wide(item(path)?.as_os_str());
    unsafe {
        let init = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
        // The UI thread already owns its apartment. Balance successful initializations only.
        let result = (|| -> Result<()> {
            let operation: IFileOperation =
                CoCreateInstance(&FileOperation, None, CLSCTX_INPROC_SERVER)?;
            operation.SetOperationFlags(
                FOFX_RECYCLEONDELETE | FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOFX_EARLYFAILURE,
            )?;
            let entry: IShellItem = SHCreateItemFromParsingName(PCWSTR(path.as_ptr()), None)?;
            operation.DeleteItem(&entry, None)?;
            operation.PerformOperations()?;
            ensure!(
                !operation.GetAnyOperationsAborted()?.as_bool(),
                "Recycle canceled"
            );
            Ok(())
        })();
        if init.is_ok() {
            CoUninitialize()
        }
        result
    }
}
pub(crate) fn drag_data(
    paths: &[std::path::PathBuf],
) -> Result<windows::Win32::System::Com::IDataObject> {
    use windows::Win32::{
        System::{Com::*, Ole::CF_HDROP},
        UI::Shell::SHCreateDataObject,
    };
    let bytes = drop_bytes(paths)?;
    unsafe {
        let data: IDataObject = SHCreateDataObject(None, None, None)?;
        let format = FORMATETC {
            cfFormat: CF_HDROP.0,
            dwAspect: DVASPECT_CONTENT.0,
            lindex: -1,
            tymed: TYMED_HGLOBAL.0 as u32,
            ..Default::default()
        };
        let mem = global_bytes(&bytes)?;
        let medium = STGMEDIUM {
            tymed: format.tymed,
            u: STGMEDIUM_0 {
                hGlobal: windows::Win32::Foundation::HGLOBAL(mem),
            },
            ..Default::default()
        };
        // Ownership passes to the data object only after SetData succeeds.
        if let Err(e) = data.SetData(&format, &medium, true) {
            GlobalFree(mem);
            return Err(e.into());
        }
        Ok(data)
    }
}
pub fn drag_files(hwnd: HWND, paths: &[std::path::PathBuf]) -> Result<()> {
    use windows::Win32::{
        System::Ole::{DROPEFFECT_COPY, DROPEFFECT_MOVE},
        UI::Shell::SHDoDragDrop,
    };
    let data = drag_data(paths)?;
    unsafe {
        SHDoDragDrop(
            Some(windows::Win32::Foundation::HWND(hwnd)),
            &data,
            None,
            DROPEFFECT_COPY | DROPEFFECT_MOVE,
        )?;
    }
    Ok(())
}
pub fn shell(install: bool) -> Result<()> {
    let exe = std::env::current_exe()?;
    let (classes, _) = RegKey::predef(HKEY_CURRENT_USER).create_subkey(r"Software\Classes")?;
    configure_shell(&classes, &exe, install)
}
pub(crate) fn configure_shell(classes: &RegKey, exe: &Path, install: bool) -> Result<()> {
    for (class, arg) in [
        ("Directory", "%1"),
        ("Folder", "%1"),
        ("Drive", "%1"),
        (r"Directory\Background", "%V"),
    ] {
        let key = format!(r"{class}\shell\DiskCleaner");
        if install {
            let (entry, _) = classes.create_subkey(&key)?;
            entry.set_value("", &"Scan with DiskCleaner")?;
            entry.set_value("Icon", &exe.as_os_str())?;
            entry
                .create_subkey("command")?
                .0
                .set_value("", &format!("\"{}\" --scan \"{}\"", exe.display(), arg))?;
        } else if let Err(e) = classes.delete_subkey_all(key)
            && e.kind() != std::io::ErrorKind::NotFound
        {
            return Err(e.into());
        }
        let legacy = format!(r"{class}\shell\WizTree");
        let owned = classes.open_subkey(&legacy).is_ok_and(|verb| {
            let title = verb.get_value::<String, _>("");
            let command = verb
                .open_subkey("command")
                .and_then(|key| key.get_value::<String, _>(""));
            title.as_deref().ok() == Some("Scan with DiskCleaner")
                && command.is_ok_and(|s| {
                    s.eq_ignore_ascii_case(&format!("\"{}\" \"{}\"", exe.display(), arg))
                })
        });
        if owned {
            classes.delete_subkey_all(legacy)?;
        }
    }
    Ok(())
}
pub fn update(endpoint: &str) -> Result<String> {
    use std::{ffi::c_void, ptr::null};
    use windows_sys::Win32::Networking::WinHttp::*;
    struct Handle(*mut c_void);
    impl Handle {
        fn new(p: *mut c_void) -> Result<Self> {
            ensure!(!p.is_null(), "{}", std::io::Error::last_os_error());
            Ok(Self(p))
        }
    }
    impl Drop for Handle {
        fn drop(&mut self) {
            unsafe {
                WinHttpCloseHandle(self.0);
            }
        }
    }
    fn check(ok: i32) -> Result<()> {
        ensure!(ok != 0, "{}", std::io::Error::last_os_error());
        Ok(())
    }
    let url = url::Url::parse(endpoint)?;
    ensure!(
        matches!(url.scheme(), "http" | "https"),
        "Expected an HTTP or HTTPS endpoint"
    );
    let host = wide(OsStr::new(url.host_str().context("Missing hostname")?));
    let object = wide(OsStr::new(
        &url[url::Position::BeforePath..url::Position::AfterQuery],
    ));
    let mut bytes = Vec::new();
    unsafe {
        let session = Handle::new(WinHttpOpen(
            wide(OsStr::new("DiskCleaner/0.1")).as_ptr(),
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            null(),
            null(),
            0,
        ))?;
        check(WinHttpSetTimeouts(session.0, 10000, 10000, 10000, 10000))?;
        let server = Handle::new(WinHttpConnect(
            session.0,
            host.as_ptr(),
            url.port_or_known_default().context("Missing port")?,
            0,
        ))?;
        let request = Handle::new(WinHttpOpenRequest(
            server.0,
            null(),
            object.as_ptr(),
            null(),
            null(),
            null(),
            if url.scheme() == "https" {
                WINHTTP_FLAG_SECURE
            } else {
                0
            },
        ))?;
        check(WinHttpSendRequest(request.0, null(), 0, null(), 0, 0, 0))?;
        check(WinHttpReceiveResponse(request.0, null_mut()))?;
        let mut status = 0u32;
        let mut size = 4;
        check(WinHttpQueryHeaders(
            request.0,
            WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
            null(),
            (&mut status as *mut u32).cast(),
            &mut size,
            null_mut(),
        ))?;
        ensure!((200..300).contains(&status), "HTTP {status}");
        let mut chunk = [0u8; 4096];
        let started = std::time::Instant::now();
        loop {
            let mut got = 0;
            check(WinHttpReadData(
                request.0,
                chunk.as_mut_ptr().cast(),
                chunk.len() as u32,
                &mut got,
            ))?;
            if got == 0 {
                break;
            }
            ensure!(
                bytes.len() + got as usize <= 65536,
                "Update response too large"
            );
            ensure!(started.elapsed().as_secs() < 10, "Update request timed out");
            bytes.extend_from_slice(&chunk[..got as usize]);
        }
    }
    let body = String::from_utf8(bytes)?;
    let field = |key: &str| {
        body.lines()
            .filter_map(|s| s.split_once('|'))
            .find(|(k, _)| k.trim().eq_ignore_ascii_case(key))
            .map(|(_, v)| v.trim())
            .unwrap_or("")
    };
    let version = field("VERSION");
    ensure!(!version.is_empty(), "Missing VERSION");
    let mut nums: Vec<u64> = version
        .split('.')
        .map(str::parse)
        .collect::<Result<_, _>>()?;
    nums.resize(nums.len().max(3), 0);
    let newer = nums.as_slice() > [0, 1, 0].as_slice()
        && (nums[..3] != [0, 1, 0] || nums[3..].iter().any(|n| *n > 0));
    Ok(format!(
        "version= {version:?} newer= {newer} infourl= {}",
        field("INFOURL")
    ))
}
pub fn icon(hwnd: HWND) -> Result<()> {
    unsafe {
        let module = windows_sys::Win32::System::LibraryLoader::GetModuleHandleW(std::ptr::null());
        for (kind, x, y) in [(1, SM_CXICON, SM_CYICON), (0, SM_CXSMICON, SM_CYSMICON)] {
            let icon = LoadImageW(
                module,
                std::ptr::without_provenance(1),
                IMAGE_ICON,
                GetSystemMetrics(x),
                GetSystemMetrics(y),
                LR_SHARED,
            );
            ensure!(
                !icon.is_null(),
                "Could not load embedded application icon: {}",
                std::io::Error::last_os_error()
            );
            SendMessageW(hwnd, WM_SETICON, kind, icon as isize);
        }
    }
    Ok(())
}
pub fn capture(hwnd: HWND, path: &Path) -> Result<()> {
    unsafe {
        let mut r = RECT::default();
        ensure!(
            GetClientRect(hwnd, &mut r) != 0 && r.right > 0 && r.bottom > 0,
            "Window not visible"
        );
        let dc = GetDC(hwnd);
        ensure!(!dc.is_null(), "GetDC failed");
        let mem = CreateCompatibleDC(dc);
        let bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        let old = SelectObject(mem, bmp);
        #[link(name = "user32")]
        unsafe extern "system" {
            fn PrintWindow(hwnd: HWND, dc: HDC, flags: u32) -> i32;
        }
        let ok = PrintWindow(hwnd, mem, 3);
        SelectObject(mem, old);
        let mut info = BITMAPINFO::default();
        info.bmiHeader.biSize = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        info.bmiHeader.biWidth = r.right;
        info.bmiHeader.biHeight = -r.bottom;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        let mut buf = vec![0u8; r.right as usize * r.bottom as usize * 4];
        let rows = GetDIBits(
            mem,
            bmp,
            0,
            r.bottom as u32,
            buf.as_mut_ptr().cast(),
            &mut info,
            DIB_RGB_COLORS,
        );
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(hwnd, dc);
        ensure!(ok != 0 && rows == r.bottom, "Window capture failed");
        for p in buf.as_chunks_mut::<4>().0 {
            p.swap(0, 2);
            p[3] = 255
        }
        image::save_buffer_with_format(
            path,
            &buf,
            r.right as u32,
            r.bottom as u32,
            image::ColorType::Rgba8,
            image::ImageFormat::Png,
        )?;
    }
    Ok(())
}
pub fn preferences() -> std::collections::HashMap<String, String> {
    let mut values = std::collections::HashMap::new();
    if let Some(folder) = std::env::var_os("APPDATA")
        && let Ok(text) =
            std::fs::read_to_string(Path::new(&folder).join("DiskCleaner/DiskCleaner.ini"))
    {
        let mut general = false;
        for line in text.trim_start_matches('\u{feff}').lines() {
            let s = line.trim();
            if s.starts_with('[') {
                general = s == "[General]"
            } else if general && let Some((k, v)) = s.split_once('=') {
                values.insert(k.to_ascii_lowercase(), v.to_string());
            }
        }
    }
    values
}
pub fn settings() -> Result<RegKey> {
    Ok(RegKey::predef(HKEY_CURRENT_USER)
        .create_subkey(r"Software\DiskCleaner\Preferences")?
        .0)
}
pub fn drives() -> Vec<String> {
    let mask = unsafe { GetLogicalDrives() };
    (0..26)
        .filter(|i| mask & (1 << i) != 0)
        .map(|i| format!("{}:\\", (b'A' + i) as char))
        .collect()
}

pub fn shell_verb(hwnd: HWND, path: &Path, verb: &str) -> Result<()> {
    use windows_sys::Win32::UI::Shell::*;
    let path = wide(path.as_os_str());
    let verb = wide(OsStr::new(verb));
    let mut info = SHELLEXECUTEINFOW {
        cbSize: std::mem::size_of::<SHELLEXECUTEINFOW>() as u32,
        fMask: SEE_MASK_INVOKEIDLIST,
        hwnd,
        lpVerb: verb.as_ptr(),
        lpFile: path.as_ptr(),
        nShow: SW_SHOW,
        ..Default::default()
    };
    ensure!(
        unsafe { ShellExecuteExW(&mut info) } != 0,
        "{}",
        std::io::Error::last_os_error()
    );
    Ok(())
}
pub fn command_prompt(path: &Path) -> Result<()> {
    let dir = if path.is_dir() {
        path
    } else {
        path.parent().context("Missing parent directory")?
    };
    let system = std::env::var_os("SystemRoot").context("Missing Windows directory")?;
    Command::new(Path::new(&system).join("System32/cmd.exe"))
        .current_dir(dir)
        .creation_flags(0x10)
        .spawn()?;
    Ok(())
}
fn drop_bytes(paths: &[std::path::PathBuf]) -> Result<Vec<u8>> {
    use windows_sys::Win32::UI::Shell::DROPFILES;
    ensure!(!paths.is_empty(), "Select a file or folder");
    let mut bytes = vec![0u8; std::mem::size_of::<DROPFILES>()];
    let len = bytes.len() as u32;
    bytes[..4].copy_from_slice(&len.to_le_bytes());
    bytes[16..20].copy_from_slice(&1u32.to_le_bytes()); // DROPFILES.fWide
    for path in paths {
        for c in wide(path.as_os_str()) {
            bytes.extend_from_slice(&c.to_le_bytes())
        }
    }
    bytes.extend_from_slice(&[0, 0]);
    Ok(bytes)
}
fn global_bytes(data: &[u8]) -> Result<HGLOBAL> {
    use windows_sys::Win32::System::Memory::*;
    unsafe {
        let mem = GlobalAlloc(GMEM_MOVEABLE, data.len());
        ensure!(!mem.is_null(), "Could not allocate file transfer memory");
        let ptr = GlobalLock(mem);
        if ptr.is_null() {
            GlobalFree(mem);
            anyhow::bail!("Could not lock file transfer memory");
        }
        std::ptr::copy_nonoverlapping(data.as_ptr(), ptr.cast(), data.len());
        GlobalUnlock(mem);
        Ok(mem)
    }
}
pub fn clipboard_files(hwnd: HWND, paths: &[std::path::PathBuf], cut: bool) -> Result<()> {
    use windows_sys::Win32::System::DataExchange::*;
    fn send(format: u32, data: &[u8]) -> Result<()> {
        let mem = global_bytes(data)?;
        if unsafe { SetClipboardData(format, mem) }.is_null() {
            unsafe {
                GlobalFree(mem);
            }
            anyhow::bail!("Could not set clipboard data");
        }
        Ok(())
    }
    let bytes = drop_bytes(paths)?;
    unsafe {
        let effect = RegisterClipboardFormatW(wide(OsStr::new("Preferred DropEffect")).as_ptr());
        ensure!(effect != 0 && OpenClipboard(hwnd) != 0, "Clipboard is busy");
        let result = (|| -> Result<()> {
            ensure!(EmptyClipboard() != 0, "Could not clear clipboard");
            send(15, &bytes)?;
            send(effect, &(if cut { 2u32 } else { 1 }).to_le_bytes())
        })();
        CloseClipboard();
        result
    }
}
pub fn client_animations_enabled() -> bool {
    use windows_sys::Win32::UI::WindowsAndMessaging::{
        SPI_GETCLIENTAREAANIMATION, SystemParametersInfoW,
    };
    let mut enabled = 0i32;
    unsafe {
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, (&raw mut enabled).cast(), 0) != 0
            && enabled != 0
    }
}
