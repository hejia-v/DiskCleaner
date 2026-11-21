use crate::scan::Tree;
use anyhow::Result;
use std::path::Path;

#[derive(Clone, Copy, Debug)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}
#[derive(Clone, Copy)]
pub struct Cell {
    pub id: usize,
    pub rect: Rect,
    pub color: u32,
    pub weight: u64,
    pub header: f32,
}
pub const FREE: usize = usize::MAX;
pub const UNKNOWN: usize = usize::MAX - 1;
#[derive(Clone, PartialEq)]
pub struct Style {
    pub depth: u8,
    pub min: f32,
    pub types: bool,
    pub names: bool,
    pub gray: bool,
    pub dark: bool,
    pub unit: usize,
    pub free: u64,
    pub unknown: u64,
}
impl Default for Style {
    fn default() -> Self {
        Self {
            depth: 3,
            min: 2.,
            types: true,
            names: true,
            gray: false,
            dark: false,
            unit: 0,
            free: 0,
            unknown: 0,
        }
    }
}
pub fn label(tree: &Tree, id: usize, zh: bool) -> String {
    match id {
        FREE => if zh { "可用空间" } else { "Free space" }.into(),
        UNKNOWN => if zh { "未知空间" } else { "Unknown space" }.into(),
        _ => tree.nodes[id].display_name().to_string_lossy().into_owned(),
    }
}
fn rgb_to_hsl(color: u32) -> (f32, f32, f32) {
    let r = ((color >> 16) & 255) as f32 / 255.0;
    let g = ((color >> 8) & 255) as f32 / 255.0;
    let b = (color & 255) as f32 / 255.0;
    let max = r.max(g).max(b);
    let min = r.min(g).min(b);
    let l = (max + min) / 2.0;
    if (max - min).abs() < f32::EPSILON {
        return (0.0, 0.0, l);
    }
    let d = max - min;
    let s = if l > 0.5 {
        d / (2.0 - max - min)
    } else {
        d / (max + min)
    };
    let h = if max == r {
        ((g - b) / d + if g < b { 6.0 } else { 0.0 }) / 6.0
    } else if max == g {
        ((b - r) / d + 2.0) / 6.0
    } else {
        ((r - g) / d + 4.0) / 6.0
    };
    (h, s, l)
}

fn hsl_to_rgb(h: f32, s: f32, l: f32) -> u32 {
    if s <= 0.0 {
        let v = (l * 255.0).round() as u32;
        return (v << 16) | (v << 8) | v;
    }
    let q = if l < 0.5 {
        l * (1.0 + s)
    } else {
        l + s - l * s
    };
    let p = 2.0 * l - q;
    let channel = |t: f32| {
        let mut t = t;
        if t < 0.0 {
            t += 1.0;
        }
        if t > 1.0 {
            t -= 1.0;
        }
        if t < 1.0 / 6.0 {
            p + (q - p) * 6.0 * t
        } else if t < 0.5 {
            q
        } else if t < 2.0 / 3.0 {
            p + (q - p) * (2.0 / 3.0 - t) * 6.0
        } else {
            p
        }
    };
    let r = (channel(h + 1.0 / 3.0) * 255.0).round() as u32;
    let g = (channel(h) * 255.0).round() as u32;
    let b = (channel(h - 1.0 / 3.0) * 255.0).round() as u32;
    (r << 16) | (g << 8) | b
}

fn tuned(color: u32, lightness: f32, saturation: f32) -> u32 {
    let (h, s, _) = rgb_to_hsl(color);
    hsl_to_rgb(h, (s * saturation).clamp(0.0, 1.0), lightness)
}

// Keep folder headers in the branch hue, dark enough for white lettering.
pub fn folder_band(color: u32, dark: bool) -> u32 {
    let (h, s, l) = rgb_to_hsl(color);
    if dark {
        hsl_to_rgb(h, (s * 0.9).min(0.45), 0.20)
    } else {
        hsl_to_rgb(h, s, (l * 0.88).clamp(0.0, 0.74))
    }
}
pub fn label_colors(color: u32) -> (u32, u32) {
    let brightness = ((color >> 16) & 255) * 299 + ((color >> 8) & 255) * 587 + (color & 255) * 114;
    if brightness < 122_000 {
        (0xffffff, 0xf3f6fb)
    } else {
        (0x14243b, 0x35455b)
    }
}
impl Rect {
    pub fn fits_name(self) -> bool {
        self.w > 48. && self.h > 20.
    }
    pub fn fits_size(self) -> bool {
        self.w >= 88. && self.h >= 48.
    }
}
const COLORS: [u32; 12] = [
    0x82c4bf, 0x8eb8e8, 0xedbf80, 0xb1b5ea, 0x8fcdd6, 0xe9ae99, 0xa0cfb1, 0xd1b1d8, 0x96bdcf,
    0xe5bc9c, 0x94cdc2, 0x95afe4,
];
const COLORS_DARK: [u32; 12] = [
    0x2f5f5b, 0x33477a, 0x6b4e28, 0x454b7a, 0x2c5a63, 0x6e463c, 0x2f5a40, 0x5a4166, 0x2e5364,
    0x6b4c38, 0x2f5d55, 0x3a4c80,
];
fn color(tree: &Tree, root: usize, id: usize, by_type: bool, dark: bool) -> u32 {
    let type_color = |value: u32| {
        if dark { tuned(value, 0.38, 1.1) } else { value }
    };
    let ext = Path::new(&tree.nodes[id].name)
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .to_lowercase();
    if by_type && !tree.nodes[id].dir {
        match ext.as_str() {
            "qt" | "mov" | "mpeg" | "mpg" | "mpe" | "m1s" | "mp4" | "avi" | "mts" | "vob"
            | "wmv" | "rm" | "3gp" | "mkv" => return type_color(0xdfb9b1),
            "exe" | "dll" | "sys" | "ini" | "msi" | "log" => return type_color(0xe3d2a7),
            "zip" | "7z" | "7zip" | "rar" | "iso" => return type_color(0xbec8e2),
            "jpg" | "jpeg" | "gif" | "png" | "tif" | "tiff" | "nef" | "raw" | "rw2" | "cr2"
            | "bmp" | "psd" => return type_color(0xb3d1c5),
            "mp2" | "mp3" | "wav" | "ogg" | "aif" | "alp" | "au" | "ac3" | "amr" | "aac"
            | "m4a" | "wma" | "aiff" | "flac" | "voc" => return type_color(0xcfc2dd),
            _ => {}
        }
    }
    let mut branch = id;
    while branch != root && tree.nodes[branch].parent != root {
        branch = tree.nodes[branch].parent;
    }
    let key = tree.nodes[branch].name.to_string_lossy();
    let hash = key
        .bytes()
        .fold(2166136261u32, |h, b| (h ^ b as u32).wrapping_mul(16777619));
    let palette = if dark { &COLORS_DARK } else { &COLORS };
    palette[hash as usize % palette.len()]
}
pub fn layout(tree: &Tree, root: usize, weights: &[u64], bounds: Rect, style: &Style) -> Vec<Cell> {
    layout_with_selection(tree, root, weights, bounds, style, None)
}

pub fn layout_with_selection(
    tree: &Tree,
    root: usize,
    weights: &[u64],
    bounds: Rect,
    style: &Style,
    selected: Option<usize>,
) -> Vec<Cell> {
    let (min, types) = (style.min, style.types);
    let mut cells = Vec::new();
    let mut bounds = bounds;
    let total = weights[root] as f64 + style.free as f64 + style.unknown as f64;
    let width = bounds.w;
    if total > 0. {
        let (free_color, unknown_color) = if style.dark {
            (0x2e4a38, 0x3a3f46)
        } else {
            (0xb9d5ba, 0xd1d1c9)
        };
        for (id, bytes, color) in [
            (FREE, style.free, free_color),
            (UNKNOWN, style.unknown, unknown_color),
        ] {
            if bytes > 0 {
                let w = width * (bytes as f64 / total) as f32;
                bounds.w -= w;
                cells.push(Cell {
                    id,
                    rect: Rect {
                        x: bounds.x + bounds.w,
                        w,
                        ..bounds
                    },
                    color,
                    weight: bytes,
                    header: 0.,
                })
            }
        }
    }
    let mut stack = vec![(root, bounds, style.depth)];
    while let Some((id, rect, levels)) = stack.pop() {
        let small = rect.w < min || rect.h < min;
        if small && Some(id) != selected {
            continue;
        }
        if weights[id] == 0 {
            continue;
        }
        let node = &tree.nodes[id];
        let descend = !small && levels > 0 && !node.children.is_empty();
        let header = if style.names
            && node.dir
            && rect.fits_name()
            && (!descend || (rect.h > 42. && rect.w > 70.))
        {
            22_f32.min(rect.h - 1.)
        } else {
            0.
        };
        cells.push(Cell {
            id,
            rect,
            color: color(tree, root, id, types, style.dark),
            weight: weights[id],
            header,
        });
        if !descend {
            continue;
        }
        let rect = Rect {
            x: rect.x + 1.,
            y: rect.y + header + 1.,
            w: rect.w - 2.,
            h: rect.h - header - 2.,
        };
        let mut ids: Vec<_> = tree.nodes[id]
            .children
            .iter()
            .copied()
            .filter(|&c| weights[c] > 0)
            .collect();
        ids.sort_unstable_by_key(|&id| std::cmp::Reverse(weights[id]));
        if ids.is_empty() {
            continue;
        }
        let mut remaining = rect;
        let scale = f64::from(rect.w) * f64::from(rect.h) / weights[id] as f64;
        let mut start = 0;
        while start < ids.len() && remaining.w > 0. && remaining.h > 0. {
            let side = f64::from(remaining.w.min(remaining.h));
            let first = weights[ids[start]] as f64 * scale;
            let mut sum = first;
            let mut worst = (side * side / first).max(first / (side * side));
            let mut end = start + 1;
            while end < ids.len() {
                let next = weights[ids[end]] as f64 * scale;
                let square = (sum + next).powi(2);
                let ratio = (side * side * first / square).max(square / (side * side * next));
                if ratio > worst {
                    break;
                }
                sum += next;
                worst = ratio;
                end += 1;
            }
            let vertical = remaining.w >= remaining.h;
            let span = if vertical { remaining.w } else { remaining.h };
            let thickness = if end == ids.len() {
                span
            } else {
                (sum / side) as f32
            }
            .min(span);
            let mut along = 0.;
            for (idx, &child) in ids[start..end].iter().enumerate() {
                let left = (side as f32 - along).max(0.);
                let length = if start + idx + 1 == end {
                    left
                } else {
                    (side * weights[child] as f64 * scale / sum) as f32
                }
                .min(left);
                let cell = if vertical {
                    Rect {
                        x: remaining.x,
                        y: remaining.y + along,
                        w: thickness,
                        h: length,
                    }
                } else {
                    Rect {
                        x: remaining.x + along,
                        y: remaining.y,
                        w: length,
                        h: thickness,
                    }
                };
                stack.push((child, cell, levels - 1));
                along += length;
            }
            if vertical {
                remaining.x += thickness;
                remaining.w -= thickness;
            } else {
                remaining.y += thickness;
                remaining.h -= thickness;
            }
            start = end;
        }
    }
    cells
}
pub fn png(
    path: &Path,
    tree: &Tree,
    cells: &[Cell],
    dimensions: (u32, u32),
    style: &Style,
    zh: bool,
    cancel: &std::sync::atomic::AtomicBool,
) -> Result<()> {
    crate::scan::check(cancel)?;
    let (width, height) = dimensions;
    anyhow::ensure!(
        width > 0 && height > 0 && u64::from(width) * u64::from(height) <= 64_000_000,
        "Image exceeds 64 megapixels"
    );
    let fonts = if style.names {
        export_fonts()
    } else {
        Vec::new()
    };
    let mut img = image::RgbImage::from_pixel(width, height, image::Rgb([239, 239, 235]));
    for c in cells {
        let r = c.rect;
        let ink = |value: u32| {
            let [_, red, green, blue] = value.to_be_bytes();
            if style.gray {
                let gray =
                    (u32::from(red) * 30 + u32::from(green) * 59 + u32::from(blue) * 11) / 100;
                image::Rgb([gray as u8; 3])
            } else {
                image::Rgb([red, green, blue])
            }
        };
        let color = ink(c.color);
        let band = ink(folder_band(c.color, style.dark));
        let band_end = (r.y + c.header).round() as u32;
        for y in (r.y.round() as u32)..((r.y + r.h).round() as u32).saturating_sub(1).min(height) {
            crate::scan::check(cancel)?;
            let fill = if y < band_end { band } else { color };
            for x in (r.x.round() as u32)..((r.x + r.w).round() as u32).saturating_sub(1).min(width)
            {
                img.put_pixel(x, y, fill)
            }
        }
        let leaf = c.id >= UNKNOWN || !tree.nodes[c.id].dir;
        if style.names && r.fits_name() && (c.header > 0. || leaf) {
            let (primary, secondary) = label_colors(c.color);
            let name_color = if c.header > 0. {
                if style.dark { 0xffffff } else { 0x14243b }
            } else {
                primary
            };
            draw_label(
                &mut img,
                r,
                &label(tree, c.id, zh),
                &fonts,
                14.,
                ink(name_color),
            );
            if leaf && r.fits_size() {
                let second_line = Rect {
                    y: r.y + 17.,
                    h: r.h - 17.,
                    ..r
                };
                draw_label(
                    &mut img,
                    second_line,
                    &crate::view::format_size(c.weight, style.unit),
                    &fonts,
                    11.,
                    ink(secondary),
                );
            }
        }
    }
    crate::scan::save(path, |w| {
        use image::ImageEncoder;
        image::codecs::png::PngEncoder::new(w).write_image(
            img.as_raw(),
            width,
            height,
            image::ExtendedColorType::Rgb8,
        )?;
        crate::scan::check(cancel)
    })
}

fn export_fonts() -> Vec<fontdue::Font> {
    let dir =
        std::path::PathBuf::from(std::env::var_os("SystemRoot").unwrap_or_default()).join("Fonts");
    ["segoeui.ttf", "msyh.ttc"]
        .iter()
        .filter_map(|name| {
            fontdue::Font::from_bytes(
                std::fs::read(dir.join(name)).ok()?,
                fontdue::FontSettings::default(),
            )
            .ok()
        })
        .collect()
}
fn draw_label(
    image: &mut image::RgbImage,
    rect: Rect,
    label: &str,
    fonts: &[fontdue::Font],
    size: f32,
    ink: image::Rgb<u8>,
) {
    let mut x = rect.x as i32 + 4;
    let baseline = rect.y as i32 + 16;
    for ch in label.chars() {
        let Some(font) = fonts.iter().find(|font| font.has_glyph(ch)) else {
            continue;
        };
        let (metrics, alpha) = font.rasterize(ch, size);
        if x + metrics.width as i32 >= (rect.x + rect.w) as i32 - 2 {
            break;
        }
        for (i, a) in alpha.into_iter().enumerate().filter(|(_, a)| *a > 0) {
            let dx = x + metrics.xmin + (i % metrics.width) as i32;
            let dy = baseline - metrics.ymin - metrics.height as i32 + (i / metrics.width) as i32;
            if dx < 0 || dy < 0 || dx >= image.width() as i32 || dy >= image.height() as i32 {
                continue;
            }
            let pixel = image.get_pixel_mut(dx as u32, dy as u32);
            for (v, tint) in pixel.0.iter_mut().zip(ink.0) {
                *v = ((u16::from(*v) * (255 - u16::from(a)) + u16::from(tint) * u16::from(a)) / 255)
                    as u8
            }
        }
        x += metrics.advance_width.ceil() as i32;
    }
}
