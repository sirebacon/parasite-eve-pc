use super::*;
#[test]
fn failed_write_cleanup_removes_owned_files_and_preserves_unrelated_files() {
    let root = std::env::temp_dir().join(format!(
        "pe-save-rollback-{}-{}",
        std::process::id(),
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    fs::create_dir(&root).unwrap();
    {
        let mut cleanup = Cleanup {
            files: vec![],
            dirs: vec![root.clone()],
            committed: false,
        };
        cleanup.write(&root.join("owned"), b"synthetic").unwrap();
        fs::write(root.join("unrelated"), b"keep").unwrap();
        assert!(cleanup
            .write(&root.join("missing-parent/file"), b"fail")
            .is_err());
    }
    assert!(!root.join("owned").exists());
    assert_eq!(fs::read(root.join("unrelated")).unwrap(), b"keep");
    fs::remove_file(root.join("unrelated")).unwrap();
    fs::remove_dir(root).unwrap();
}
