use crate::{Card, CardName, Companion, SaveError};
use std::{
    collections::BTreeMap,
    error::Error,
    fmt,
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    path::{Path, PathBuf},
};
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PortableSave {
    name: CardName,
    card: Card,
    companion: Option<Companion>,
    companion_name: Option<String>,
}
impl PortableSave {
    pub fn new(name: &str, block: Vec<u8>, companion: Option<Vec<u8>>) -> Result<Self, SaveError> {
        let name = CardName::new(name)?;
        let card = Card::parse(block)?;
        let link = card.companion_link()?;
        let (companion, companion_name) = match (link, companion) {
            (Some(link), Some(bytes)) => (
                Some(Companion::parse(bytes, &name, &link)?),
                Some(link.filename(&name)),
            ),
            (None, None) => (None, None),
            (Some(_), None) => return Err(SaveError("required companion is missing".into())),
            (None, Some(_)) => return Err(SaveError("companion without a card link".into())),
        };
        Ok(Self {
            name,
            card,
            companion,
            companion_name,
        })
    }
    pub fn name(&self) -> &str {
        self.name.as_str()
    }
    pub fn card(&self) -> &Card {
        &self.card
    }
    pub fn companion(&self) -> Option<&Companion> {
        self.companion.as_ref()
    }
    pub fn files(&self) -> BTreeMap<PathBuf, &[u8]> {
        let mut files = BTreeMap::new();
        files.insert(PathBuf::from(self.name()), self.card.bytes());
        if let (Some(companion), Some(name)) = (&self.companion, &self.companion_name) {
            files.insert(Path::new(".pe-ext").join(name), companion.bytes());
        }
        files
    }
    /// New destination only. Validate before creation; write the companion before the card.
    /// On failure remove only files and empty directories created by this operation.
    pub fn export_new(&self, directory: &Path) -> Result<PathBuf, ExportError> {
        fs::create_dir(directory).map_err(|source| ExportError {
            path: directory.into(),
            source,
        })?;
        let mut cleanup = Cleanup {
            files: vec![],
            dirs: vec![directory.into()],
            committed: false,
        };
        if let (Some(companion), Some(name)) = (&self.companion, &self.companion_name) {
            let folder = directory.join(".pe-ext");
            fs::create_dir(&folder).map_err(|source| ExportError {
                path: folder.clone(),
                source,
            })?;
            cleanup.dirs.push(folder.clone());
            cleanup.write(&folder.join(name), companion.bytes())?;
        }
        let card = directory.join(self.name());
        cleanup.write(&card, self.card.bytes())?;
        cleanup.committed = true;
        Ok(card)
    }
    pub fn from_directory(card_path: &Path) -> Result<Self, ExportError> {
        let name = card_path
            .file_name()
            .and_then(|s| s.to_str())
            .ok_or_else(|| invalid(card_path, "invalid card filename"))?;
        let block = read_bounded(card_path, crate::BLOCK_SIZE)?;
        let card = Card::parse(block.clone()).map_err(|e| invalid(card_path, &e.to_string()))?;
        let link = card
            .companion_link()
            .map_err(|e| invalid(card_path, &e.to_string()))?;
        let companion = if let Some(link) = link {
            let name = CardName::new(name).map_err(|e| invalid(card_path, &e.to_string()))?;
            let path = card_path
                .parent()
                .unwrap_or(Path::new("."))
                .join(".pe-ext")
                .join(link.filename(&name));
            Some(read_bounded(&path, link.size as usize)?)
        } else {
            None
        };
        Self::new(name, block, companion).map_err(|e| invalid(card_path, &e.to_string()))
    }
}
fn read_bounded(path: &Path, size: usize) -> Result<Vec<u8>, ExportError> {
    let file = File::open(path).map_err(|source| ExportError {
        path: path.into(),
        source,
    })?;
    let mut bytes = vec![];
    file.take((size + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|source| ExportError {
            path: path.into(),
            source,
        })?;
    if bytes.len() != size {
        return Err(invalid(
            path,
            &format!("expected {size} bytes, found {}", bytes.len()),
        ));
    }
    Ok(bytes)
}
fn invalid(path: &Path, message: &str) -> ExportError {
    ExportError {
        path: path.into(),
        source: io::Error::new(io::ErrorKind::InvalidData, message.to_owned()),
    }
}
#[derive(Debug)]
pub struct ExportError {
    pub path: PathBuf,
    pub source: io::Error,
}
impl fmt::Display for ExportError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.path.display(), self.source)
    }
}
impl Error for ExportError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        Some(&self.source)
    }
}
struct Cleanup {
    files: Vec<PathBuf>,
    dirs: Vec<PathBuf>,
    committed: bool,
}
impl Cleanup {
    fn write(&mut self, path: &Path, bytes: &[u8]) -> Result<(), ExportError> {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .map_err(|source| ExportError {
                path: path.into(),
                source,
            })?;
        self.files.push(path.into());
        file.write_all(bytes)
            .and_then(|_| file.sync_all())
            .map_err(|source| ExportError {
                path: path.into(),
                source,
            })
    }
}
impl Drop for Cleanup {
    fn drop(&mut self) {
        if !self.committed {
            for file in self.files.iter().rev() {
                let _ = fs::remove_file(file);
            }
            for dir in self.dirs.iter().rev() {
                let _ = fs::remove_dir(dir);
            }
        }
    }
}
#[cfg(test)]
#[path = "portable_tests.rs"]
mod tests;
