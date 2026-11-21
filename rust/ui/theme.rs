use std::{borrow::Cow, sync::Arc};

use gpui::{App, AssetSource, FontFeatures, Hsla, Result, Rgba, SharedString, Window, px, rgba};

pub const UI_FONT: &str = "MiSans VF";

#[derive(Clone, Copy)]
pub struct Palette {
    pub canvas: Rgba,
    pub panel: Rgba,
    pub panel_alt: Rgba,
    pub rail: Rgba,
    pub rail_active: Rgba,
    pub rail_hover: Rgba,
    pub rail_text: Rgba,
    pub rail_muted: Rgba,
    pub text: Rgba,
    pub text_secondary: Rgba,
    pub muted: Rgba,
    pub accent: Rgba,
    pub accent_soft: Rgba,
    pub accent_ink: Rgba,
    pub border: Rgba,
    pub border_strong: Rgba,
    pub hover: Rgba,
    pub pressed: Rgba,
    pub row_hover: Rgba,
    pub bar_track: Rgba,
    pub seg_track: Rgba,
    pub input_border: Rgba,
    pub danger: Rgba,
    pub warn: Rgba,
    pub primary: Rgba,
    pub primary_ink: Rgba,
    pub thumb: Rgba,
    pub thumb_border: Rgba,
}

pub fn of(dark: bool) -> Palette {
    if dark {
        Palette {
            canvas: rgba(0x0A0B0DFF),
            panel: rgba(0x14161AFF),
            panel_alt: rgba(0x1A1D22FF),
            rail: rgba(0x0D0E11FF),
            rail_active: rgba(0x1A1C22FF),
            rail_hover: rgba(0x16181DFF),
            rail_text: rgba(0xA2A7B0FF),
            rail_muted: rgba(0x6E737DFF),
            text: rgba(0xEDEEF1FF),
            text_secondary: rgba(0xA2A7B0FF),
            muted: rgba(0x8B909AFF),
            accent: rgba(0x7C8CFFFF),
            accent_soft: rgba(0x7C8CFF21),
            accent_ink: rgba(0xC9D0FFFF),
            border: rgba(0xFFFFFF17),
            border_strong: rgba(0xFFFFFF24),
            hover: rgba(0xFFFFFF0E),
            pressed: rgba(0xFFFFFF13),
            row_hover: rgba(0xFFFFFF0A),
            bar_track: rgba(0xFFFFFF1A),
            seg_track: rgba(0x0B0C0FFF),
            input_border: rgba(0xFFFFFF1A),
            danger: rgba(0xFF6B6BFF),
            warn: rgba(0xE8B45AFF),
            primary: rgba(0xF2F3F5FF),
            primary_ink: rgba(0x0A0B0DFF),
            thumb: rgba(0x1F2127FF),
            thumb_border: rgba(0xFFFFFF12),
        }
    } else {
        Palette {
            canvas: rgba(0xFAFAFBFF),
            panel: rgba(0xFFFFFFFF),
            panel_alt: rgba(0xF7F7F9FF),
            rail: rgba(0xF4F4F6FF),
            rail_active: rgba(0xECECEFFF),
            rail_hover: rgba(0xEFEFF2FF),
            rail_text: rgba(0x5B6069FF),
            rail_muted: rgba(0x8A8F98FF),
            text: rgba(0x131417FF),
            text_secondary: rgba(0x5B6069FF),
            muted: rgba(0x6B7079FF),
            accent: rgba(0x5A63E8FF),
            accent_soft: rgba(0x5A63E817),
            accent_ink: rgba(0x3A43B8FF),
            border: rgba(0x11121514),
            border_strong: rgba(0x11121524),
            hover: rgba(0x1112150B),
            pressed: rgba(0x11121513),
            row_hover: rgba(0x11121509),
            bar_track: rgba(0x11121517),
            seg_track: rgba(0xEFEFF2FF),
            input_border: rgba(0x1112151A),
            danger: rgba(0xD92D20FF),
            warn: rgba(0xA96A00FF),
            primary: rgba(0x16171AFF),
            primary_ink: rgba(0xFFFFFFFF),
            thumb: rgba(0xFFFFFFFF),
            thumb_border: rgba(0x11121514),
        }
    }
}

pub fn apply(dark: bool, window: Option<&mut Window>, cx: &mut App) {
    use gpui_component::{Theme, ThemeMode};

    Theme::change(
        if dark {
            ThemeMode::Dark
        } else {
            ThemeMode::Light
        },
        window,
        cx,
    );

    let p = of(dark);
    let theme = Theme::global_mut(cx);
    theme.font_family = UI_FONT.into();
    theme.font_size = px(15.);
    theme.radius = px(6.);
    theme.radius_lg = px(10.);
    theme.background = p.canvas.into();
    theme.foreground = p.text.into();
    theme.border = p.border.into();
    theme.muted = p.panel_alt.into();
    theme.muted_foreground = p.muted.into();
    theme.input = p.input_border.into();
    theme.ring = p.accent.into();
    theme.selection = p.accent_soft.into();
    theme.accent = p.hover.into();
    theme.accent_foreground = p.text.into();
    theme.primary = p.primary.into();
    theme.primary_foreground = p.primary_ink.into();
    theme.secondary = p.panel_alt.into();
    theme.secondary_foreground = p.text_secondary.into();
    theme.popover = p.panel.into();
    theme.popover_foreground = p.text.into();
    theme.list = p.panel.into();
    theme.list_hover = p.hover.into();
    theme.list_active = p.accent_soft.into();
    theme.list_active_border = p.accent_soft.into();
    theme.table = p.panel.into();
    theme.table_head = p.panel.into();
    theme.table_head_foreground = p.muted.into();
    theme.table_hover = p.row_hover.into();
    theme.table_active = p.accent_soft.into();
    theme.table_row_border = p.border.into();
    theme.sidebar = p.rail.into();
    theme.sidebar_foreground = p.rail_text.into();
    theme.sidebar_border = p.border.into();
    theme.sidebar_accent = p.rail_active.into();
    theme.sidebar_accent_foreground = p.text.into();
    theme.scrollbar = p.canvas.into();
    theme.scrollbar_thumb = p.border_strong.into();
    theme.scrollbar_thumb_hover = p.muted.into();
    theme.switch = p.panel_alt.into();
    theme.switch_thumb = p.panel.into();
    theme.title_bar = p.canvas.into();
    theme.title_bar_border = p.border.into();
    theme.danger = p.danger.into();
    theme.danger_foreground = Hsla::from(rgba(0xFFFFFFFF));
    theme.link = p.accent.into();
    theme.link_hover = p.accent_ink.into();
}

pub fn tnum() -> FontFeatures {
    FontFeatures(Arc::new(vec![("tnum".to_string(), 1)]))
}

pub struct Assets;

impl AssetSource for Assets {
    fn load(&self, path: &str) -> Result<Option<Cow<'static, [u8]>>> {
        let bytes: &'static [u8] = match path {
            "icons/arrow-left.svg" => include_bytes!("../../resources/icons/arrow-left.svg"),
            "icons/arrow-right.svg" => include_bytes!("../../resources/icons/arrow-right.svg"),
            "icons/arrow-up.svg" => include_bytes!("../../resources/icons/arrow-up.svg"),
            "icons/chevron-down.svg" => include_bytes!("../../resources/icons/chevron-down.svg"),
            "icons/chevron-right.svg" => include_bytes!("../../resources/icons/chevron-right.svg"),
            "icons/dashboard.svg" => include_bytes!("../../resources/icons/dashboard.svg"),
            "icons/folder.svg" => include_bytes!("../../resources/icons/folder.svg"),
            "icons/funnel.svg" => include_bytes!("../../resources/icons/funnel.svg"),
            "icons/hard-drive.svg" => include_bytes!("../../resources/icons/hard-drive.svg"),
            "icons/info.svg" => include_bytes!("../../resources/icons/info.svg"),
            "icons/list.svg" => include_bytes!("../../resources/icons/list.svg"),
            "icons/more.svg" => include_bytes!("../../resources/icons/more.svg"),
            "icons/refresh.svg" => include_bytes!("../../resources/icons/refresh.svg"),
            "icons/search.svg" => include_bytes!("../../resources/icons/search.svg"),
            "icons/settings.svg" => include_bytes!("../../resources/icons/settings.svg"),
            "icons/table.svg" => include_bytes!("../../resources/icons/table.svg"),
            "icons/trash.svg" => include_bytes!("../../resources/icons/trash.svg"),
            "icons/upload.svg" => include_bytes!("../../resources/icons/upload.svg"),
            "icons/x.svg" => include_bytes!("../../resources/icons/x.svg"),
            _ => return Ok(None),
        };
        Ok(Some(Cow::Borrowed(bytes)))
    }

    fn list(&self, _path: &str) -> Result<Vec<SharedString>> {
        Ok(Vec::new())
    }
}
