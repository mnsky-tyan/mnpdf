// mnpdf — a minimal local-only PDF reader/annotator.
// The webview owns all logic (pdf.js + pdf-lib); Rust only does local IO:
// read/write files, a tiny JSON key-value store, and pass-through of the
// file path given on the command line.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use std::fs;
use tauri::Manager;

#[tauri::command]
fn read_file_b64(path: String) -> Result<String, String> {
    let bytes = fs::read(&path).map_err(|e| format!("cannot read {path}: {e}"))?;
    Ok(B64.encode(bytes))
}

#[tauri::command]
fn write_file_b64(path: String, data: String) -> Result<(), String> {
    let bytes = B64
        .decode(data.as_bytes())
        .map_err(|e| format!("bad payload: {e}"))?;
    fs::write(&path, &bytes).map_err(|e| format!("cannot write {path}: {e}"))
}

#[tauri::command]
fn initial_path() -> Option<String> {
    std::env::args()
        .skip(1)
        .find(|a| !a.starts_with('-') && a.to_lowercase().ends_with(".pdf"))
}

fn kv_file(app: &tauri::AppHandle) -> Result<std::path::PathBuf, String> {
    let dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("no app data dir: {e}"))?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir.join("kv.json"))
}

#[tauri::command]
fn kv_get(app: tauri::AppHandle, key: String) -> Result<Option<String>, String> {
    let path = kv_file(&app)?;
    let Ok(text) = fs::read_to_string(&path) else {
        return Ok(None);
    };
    let map: serde_json::Map<String, serde_json::Value> =
        serde_json::from_str(&text).map_err(|e| e.to_string())?;
    Ok(map
        .get(&key)
        .and_then(|v| v.as_str())
        .map(|s| s.to_string()))
}

#[tauri::command]
fn kv_set(app: tauri::AppHandle, key: String, value: String) -> Result<(), String> {
    let path = kv_file(&app)?;
    let mut map: serde_json::Map<String, serde_json::Value> = fs::read_to_string(&path)
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default();
    map.insert(key, serde_json::Value::String(value));
    fs::write(&path, serde_json::to_string(&map).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            read_file_b64,
            write_file_b64,
            initial_path,
            kv_get,
            kv_set
        ])
        .run(tauri::generate_context!())
        .expect("error while running mnpdf");
}
