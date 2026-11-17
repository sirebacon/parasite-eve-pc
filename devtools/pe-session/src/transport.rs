//! Common command abstraction used by battle and save clients.
use crate::{process::GameSession, DebugControl, SessionError};
use pe_protocol::Reply;
use std::{path::PathBuf, time::Duration};
pub trait DebugTransport {
    fn execute(&mut self, command: &str) -> Result<Reply, SessionError>;
}
impl DebugTransport for &mut GameSession {
    fn execute(&mut self, command: &str) -> Result<Reply, SessionError> {
        self.command(command, 1200)
    }
}
pub struct AttachedTransport {
    control: DebugControl,
    timeout: Duration,
}
impl AttachedTransport {
    pub fn new(directory: PathBuf, timeout: Duration) -> Self {
        Self {
            control: DebugControl::new(directory),
            timeout,
        }
    }
}
impl DebugTransport for AttachedTransport {
    fn execute(&mut self, command: &str) -> Result<Reply, SessionError> {
        self.control.ask(command, self.timeout)
    }
}
