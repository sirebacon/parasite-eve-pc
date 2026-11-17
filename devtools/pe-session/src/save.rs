//! Save/tail export helpers matching the Python session's command schedule.
use crate::{
    io_at,
    process::GameSession,
    transport::{AttachedTransport, DebugTransport},
    SessionError,
};
use pe_protocol::{Command, ProtocolError, Reply, Status};
use pe_save::{CardName, PortableSave, BLOCK_SIZE, TAIL_SIZE};
use std::{
    fs::File,
    io::Read,
    path::{Path, PathBuf},
    time::Duration,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RawSaveBlock {
    pub name: CardName,
    pub bytes: Vec<u8>,
    pub companion: Option<String>,
}
impl RawSaveBlock {
    fn reply(reply: Reply, bytes: Vec<u8>) -> Result<Self, SessionError> {
        let mut words = reply.details.split_whitespace();
        let name = CardName::new(
            words
                .next()
                .ok_or_else(|| ProtocolError("save-block: missing filename".into()))?,
        )?;
        let mut companion = None;
        for word in words {
            if let Some(value) = word.strip_prefix("companion=") {
                if companion.is_some() || value.is_empty() || value.contains(['/', '\\', ':']) {
                    return Err(ProtocolError(
                        "save-block: invalid/duplicate companion filename".into(),
                    )
                    .into());
                }
                companion = Some(value.into());
            } else {
                return Err(
                    ProtocolError(format!("save-block: unknown reply field {word}")).into(),
                );
            }
        }
        if bytes.len() != BLOCK_SIZE {
            return Err(
                ProtocolError("save-block: expected an 8192-byte card block".into()).into(),
            );
        }
        Ok(Self {
            name,
            bytes,
            companion,
        })
    }
}
pub struct SaveClient<T> {
    transport: T,
    directory: PathBuf,
}
impl<'a> SaveClient<&'a mut GameSession> {
    pub fn headless(session: &'a mut GameSession) -> Self {
        let directory = session.directory().join("ctl");
        Self::new(session, directory)
    }
}
impl SaveClient<AttachedTransport> {
    pub fn attached(directory: PathBuf, timeout: Duration) -> Self {
        Self::new(
            AttachedTransport::new(directory.clone(), timeout),
            directory,
        )
    }
}
impl<T: DebugTransport> SaveClient<T> {
    pub fn new(transport: T, directory: PathBuf) -> Self {
        Self {
            transport,
            directory,
        }
    }
    fn request(&mut self, text: &str) -> Result<Reply, SessionError> {
        let command = Command::new(text)?;
        let reply = self.transport.execute(text)?;
        if reply.verb != command.verb() {
            return Err(ProtocolError(format!("{text}: mismatched reply {}", reply.verb)).into());
        }
        if reply.status == Status::Err {
            return Err(SessionError::Native {
                command: text.into(),
                reply: reply.line,
            });
        }
        Ok(reply)
    }
    pub fn mods_tail(&mut self) -> Result<Vec<u8>, SessionError> {
        self.request("mods-tail")?;
        read_exact(&self.directory.join("tail.bin"), TAIL_SIZE)
    }
    pub fn save_block(&mut self, slot: i32) -> Result<RawSaveBlock, SessionError> {
        let reply = self.request(&format!("save-block {slot}"))?;
        RawSaveBlock::reply(
            reply,
            read_exact(&self.directory.join("block.bin"), BLOCK_SIZE)?,
        )
    }
    pub fn save_files(&mut self, slot: i32) -> Result<PortableSave, SessionError> {
        let raw = self.save_block(slot)?;
        let card = pe_save::Card::parse(raw.bytes.clone())?;
        let link = card.companion_link()?;
        match (&link, &raw.companion) {
            (Some(link), Some(name)) if *name == link.filename(&raw.name) => {}
            (None, None) => {}
            _ => {
                return Err(ProtocolError(
                    "save-block: companion metadata does not match the card link".into(),
                )
                .into())
            }
        }
        let companion = if let Some(link) = link {
            Some(read_exact(
                &self.directory.join("companion.pex"),
                link.size as usize,
            )?)
        } else {
            None
        };
        Ok(PortableSave::new(raw.name.as_str(), raw.bytes, companion)?)
    }
    pub fn save_to(&mut self, directory: &Path, slot: i32) -> Result<PathBuf, SessionError> {
        Ok(self.save_files(slot)?.export_new(directory)?)
    }
}
fn read_exact(path: &Path, size: usize) -> Result<Vec<u8>, SessionError> {
    let file = io_at(path, File::open(path))?;
    let mut bytes = Vec::new();
    io_at(path, file.take((size + 1) as u64).read_to_end(&mut bytes))?;
    if bytes.len() != size {
        return Err(ProtocolError(format!(
            "{}: expected {size} bytes, found {}",
            path.display(),
            bytes.len()
        ))
        .into());
    }
    Ok(bytes)
}
