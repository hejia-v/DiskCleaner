use anyhow::{Context, Result, ensure};
use gpui::{App, Asset, Div, RenderImage, Window, div, img, prelude::*, px};
use std::{
    collections::HashSet,
    os::windows::ffi::OsStrExt,
    path::{Path, PathBuf},
    sync::Arc,
};
use windows::{
    Win32::{
        Foundation::RPC_E_CHANGED_MODE,
        Graphics::Imaging::{
            CLSID_WICImagingFactory, GUID_WICPixelFormat32bppBGRA, IWICImagingFactory,
            WICConvertBitmapSource,
        },
        Storage::FileSystem::{FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL},
        System::Com::{
            CLSCTX_INPROC_SERVER, COINIT_APARTMENTTHREADED, CoCreateInstance, CoInitializeEx,
            CoUninitialize,
        },
        UI::{Shell::*, WindowsAndMessaging::HICON},
    },
    core::{Owned, PCWSTR},
};

#[derive(Clone, Hash, PartialEq, Eq)]
struct IconRequest {
    path: PathBuf,
    directory: bool,
    actual: bool,
    modified: u64,
}

fn type_path(path: &Path, directory: bool) -> PathBuf {
    let mut name = PathBuf::from("item");
    if !directory && let Some(ext) = path.extension() {
        name.set_extension(ext);
    }
    name
}

fn shell_icon(path: &Path, directory: bool, actual: bool) -> Option<Owned<HICON>> {
    let name: Vec<_> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut info = SHFILEINFOW::default();
    let flags = SHGFI_ICON
        | SHGFI_SMALLICON
        | if actual {
            SHGFI_ADDOVERLAYS
        } else {
            SHGFI_USEFILEATTRIBUTES
        };
    let attributes = if directory {
        FILE_ATTRIBUTE_DIRECTORY
    } else {
        FILE_ATTRIBUTE_NORMAL
    };
    unsafe {
        let found = SHGetFileInfoW(
            PCWSTR(name.as_ptr()),
            attributes,
            Some(&mut info),
            size_of::<SHFILEINFOW>() as u32,
            flags,
        );
        (found != 0 && !info.hIcon.is_invalid()).then(|| Owned::new(info.hIcon))
    }
}

impl IconRequest {
    fn load_image(&self) -> Result<RenderImage> {
        // Shell extensions and WIC run on the asset worker that initialized COM.
        struct Apartment(bool);
        impl Drop for Apartment {
            fn drop(&mut self) {
                if self.0 {
                    unsafe { CoUninitialize() };
                }
            }
        }
        unsafe {
            let init = CoInitializeEx(None, COINIT_APARTMENTTHREADED);
            if init != RPC_E_CHANGED_MODE {
                init.ok()?;
            }
            let _apartment = Apartment(init.is_ok());
            let mut icon = shell_icon(&self.path, self.directory, self.actual);
            if icon.is_none() && self.actual {
                icon = shell_icon(
                    &type_path(&self.path, self.directory),
                    self.directory,
                    false,
                );
            }
            let icon = match icon {
                Some(icon) => icon,
                None => {
                    let mut stock = SHSTOCKICONINFO {
                        cbSize: size_of::<SHSTOCKICONINFO>() as u32,
                        ..Default::default()
                    };
                    let kind = if self.directory {
                        SIID_FOLDER
                    } else {
                        SIID_DOCNOASSOC
                    };
                    SHGetStockIconInfo(kind, SHGSI_ICON | SHGSI_SMALLICON, &mut stock)
                        .context("Windows stock icon unavailable")?;
                    Owned::new(stock.hIcon)
                }
            };
            let wic: IWICImagingFactory =
                CoCreateInstance(&CLSID_WICImagingFactory, None, CLSCTX_INPROC_SERVER)?;
            let bitmap = wic.CreateBitmapFromHICON(*icon)?;
            let bgra = WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, &bitmap)?;
            let (mut width, mut height) = (0, 0);
            bgra.GetSize(&mut width, &mut height)?;
            ensure!(
                (1..=256).contains(&width) && (1..=256).contains(&height),
                "Invalid Shell icon dimensions"
            );
            let mut pixels = image::RgbaImage::new(width, height);
            bgra.CopyPixels(std::ptr::null(), width * 4, pixels.as_mut())?;
            Ok(RenderImage::new([image::Frame::new(pixels)]))
        }
    }
}

struct ShellIcon;
impl Asset for ShellIcon {
    type Source = IconRequest;
    type Output = Option<Arc<RenderImage>>;
    #[expect(
        clippy::manual_async_fn,
        reason = "An async fn would capture the UI-thread App in the Send future"
    )]
    fn load(
        source: Self::Source,
        _: &mut App,
    ) -> impl Future<Output = Self::Output> + Send + 'static {
        async move { source.load_image().ok().map(Arc::new) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn loads_shell_icons_for_live_and_offline_rows() -> Result<()> {
        let fixture = tempfile::tempdir()?;
        let file = fixture.path().join("示例.txt");
        std::fs::write(&file, b"icon")?;
        for (path, directory, actual) in [
            (fixture.path().to_owned(), true, true),
            (file.clone(), false, true),
            (type_path(&file, false), false, false),
            (type_path(fixture.path(), true), true, false),
        ] {
            let icon = IconRequest {
                path,
                directory,
                actual,
                modified: 0,
            }
            .load_image()?;
            assert!(
                icon.as_bytes(0)
                    .unwrap()
                    .as_chunks::<4>()
                    .0
                    .iter()
                    .any(|p| p[3] > 0)
            );
        }
        Ok(())
    }
}

#[derive(Default)]
pub(super) struct RowIcons {
    previous: HashSet<IconRequest>,
    current: HashSet<IconRequest>,
}
impl RowIcons {
    pub(super) fn begin_frame(&mut self, window: &mut Window, cx: &mut App) {
        // A virtual list can measure one row separately from drawing its visible range.
        // Retain all requests from the last frame, including that measurement pass.
        for stale in self.previous.difference(&self.current) {
            if let Some(Some(image)) = window.get_asset::<ShellIcon>(stale, cx) {
                cx.drop_image(image, Some(window));
            }
            cx.remove_asset::<ShellIcon>(stale);
        }
        self.previous.clear();
        std::mem::swap(&mut self.previous, &mut self.current);
    }
    pub(super) fn element(
        &mut self,
        path: &Path,
        directory: bool,
        live: bool,
        modified: u64,
        window: &mut Window,
        cx: &mut App,
    ) -> Div {
        let actual = live && path.is_absolute();
        let key = IconRequest {
            path: if actual {
                path.to_owned()
            } else {
                type_path(path, directory)
            },
            directory,
            actual,
            modified: if actual { modified } else { 0 },
        };
        let image = window.use_asset::<ShellIcon>(&key, cx).flatten();
        self.current.insert(key);
        div()
            .flex_none()
            .size(px(16.))
            .when_some(image, |slot, image| slot.child(img(image).size_full()))
    }
}
