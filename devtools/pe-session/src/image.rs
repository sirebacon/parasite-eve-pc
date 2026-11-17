//! Lossless conversion of the native host's P6 RGB screenshots to PNG.
use crate::{io_at, SessionError};
use pe_protocol::ProtocolError;
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::Path,
};

const MAX_PIXELS: usize = 64 * 1024 * 1024;
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RgbFrame {
    width: u32,
    height: u32,
    pixels: Vec<u8>,
}
impl RgbFrame {
    pub fn width(&self) -> u32 {
        self.width
    }
    pub fn height(&self) -> u32 {
        self.height
    }
    pub fn pixels(&self) -> &[u8] {
        &self.pixels
    }
    pub fn from_ppm(bytes: &[u8]) -> Result<Self, ProtocolError> {
        let error = |s: &str| ProtocolError(format!("PPM: {s}"));
        let mut index = 0;
        if token(bytes, &mut index)? != b"P6" {
            return Err(error("expected binary P6 RGB"));
        }
        let dimension = |word: &[u8]| {
            std::str::from_utf8(word)
                .ok()
                .and_then(|s| s.parse::<u32>().ok())
                .filter(|n| *n > 0)
                .ok_or_else(|| error("dimensions must be positive u32 values"))
        };
        let width = dimension(token(bytes, &mut index)?)?;
        let height = dimension(token(bytes, &mut index)?)?;
        if token(bytes, &mut index)? != b"255" {
            return Err(error("only 8-bit RGB (max value 255) is supported"));
        }
        if !bytes.get(index).is_some_and(u8::is_ascii_whitespace) {
            return Err(error("missing raster separator"));
        }
        let count = (width as usize)
            .checked_mul(height as usize)
            .filter(|n| *n <= MAX_PIXELS)
            .and_then(|n| n.checked_mul(3))
            .ok_or_else(|| error("image exceeds the 64-megapixel limit"))?;
        // Use the exact raster length to distinguish CRLF from a CR separator
        // followed by a first pixel byte whose value happens to be LF.
        if bytes[index] == b'\r'
            && bytes.get(index + 1) == Some(&b'\n')
            && bytes.len() - index == count + 2
        {
            index += 2;
        } else {
            index += 1;
        }
        let pixels = &bytes[index..];
        if pixels.len() != count {
            return Err(error(&format!(
                "expected {count} raster bytes, found {}",
                pixels.len()
            )));
        }
        Ok(Self {
            width,
            height,
            pixels: pixels.to_vec(),
        })
    }
    pub fn to_png(&self) -> Result<Vec<u8>, ProtocolError> {
        let mut output = Vec::new();
        {
            let mut encoder = png::Encoder::new(&mut output, self.width, self.height);
            encoder.set_color(png::ColorType::Rgb);
            encoder.set_depth(png::BitDepth::Eight);
            encoder.set_filter(png::Filter::NoFilter);
            let mut writer = encoder
                .write_header()
                .map_err(|e| ProtocolError(format!("PNG header: {e}")))?;
            writer
                .write_image_data(&self.pixels)
                .map_err(|e| ProtocolError(format!("PNG pixels: {e}")))?;
            writer
                .finish()
                .map_err(|e| ProtocolError(format!("PNG finish: {e}")))?;
        }
        Ok(output)
    }
}

fn token<'a>(bytes: &'a [u8], index: &mut usize) -> Result<&'a [u8], ProtocolError> {
    loop {
        while bytes.get(*index).is_some_and(u8::is_ascii_whitespace) {
            *index += 1;
        }
        if bytes.get(*index) == Some(&b'#') {
            while bytes.get(*index).is_some_and(|b| *b != b'\n') {
                *index += 1;
            }
        } else {
            break;
        }
    }
    let start = *index;
    while bytes
        .get(*index)
        .is_some_and(|b| !b.is_ascii_whitespace() && *b != b'#')
    {
        *index += 1;
    }
    if start == *index {
        return Err(ProtocolError("PPM: incomplete header".into()));
    }
    Ok(&bytes[start..*index])
}

/// Writes a new file; existing screenshot evidence is never silently replaced.
pub fn ppm_to_png(ppm: &Path, png: &Path) -> Result<(), SessionError> {
    let bytes = io_at(ppm, fs::read(ppm))?;
    let frame =
        RgbFrame::from_ppm(&bytes).map_err(|e| ProtocolError(format!("{}: {e}", ppm.display())))?;
    let encoded = frame
        .to_png()
        .map_err(|e| ProtocolError(format!("{}: {e}", png.display())))?;
    let mut file = io_at(
        png,
        OpenOptions::new().write(true).create_new(true).open(png),
    )?;
    io_at(png, file.write_all(&encoded))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn frame(header: &[u8]) -> Vec<u8> {
        [header, &[10, 13, 32, 0, 128, 255]].concat()
    }
    #[test]
    fn native_header_and_whitespace_pixels_are_preserved() {
        let rgb = RgbFrame::from_ppm(&frame(b"P6\n2 1\n255\n")).unwrap();
        assert_eq!((rgb.width(), rgb.height()), (2, 1));
        assert_eq!(rgb.pixels(), [10, 13, 32, 0, 128, 255]);
    }
    #[test]
    fn header_comments_tabs_and_crlf_work_without_consuming_pixels() {
        let rgb = RgbFrame::from_ppm(&frame(b"P6\r\n# synthetic\n2\t1\r\n255\r\n")).unwrap();
        assert_eq!(rgb.pixels(), [10, 13, 32, 0, 128, 255]);
        let rgb = RgbFrame::from_ppm(&frame(b"P6\n2 1\n255\r")).unwrap();
        assert_eq!(rgb.pixels(), [10, 13, 32, 0, 128, 255]);
    }
    #[test]
    fn malformed_headers_sizes_and_payloads_are_rejected() {
        for bytes in [
            b"P3\n2 1\n255\n".as_slice(),
            b"P6\n0 1\n255\n",
            b"P6\n2 1\n65535\n",
            b"P6\n4294967295 4294967295\n255\n",
            b"P6\n2 1\n255",
            b"P6\n2 1\n255\nshort",
            b"P6\n# missing dims",
        ] {
            assert!(RgbFrame::from_ppm(bytes).is_err());
        }
        let mut extra = frame(b"P6\n2 1\n255\n");
        extra.push(0);
        assert!(RgbFrame::from_ppm(&extra).is_err());
    }
    #[test]
    fn png_roundtrip_retains_dimensions_depth_and_exact_rgb_bytes() {
        let rgb = RgbFrame::from_ppm(&frame(b"P6\n2 1\n255\n")).unwrap();
        let bytes = rgb.to_png().unwrap();
        let decoder = png::Decoder::new(std::io::Cursor::new(bytes));
        let mut reader = decoder.read_info().unwrap();
        let mut pixels = vec![0; reader.output_buffer_size().unwrap()];
        let info = reader.next_frame(&mut pixels).unwrap();
        assert_eq!((info.width, info.height), (2, 1));
        assert_eq!(info.color_type, png::ColorType::Rgb);
        assert_eq!(info.bit_depth, png::BitDepth::Eight);
        assert_eq!(&pixels[..info.buffer_size()], rgb.pixels());
    }
}
