// mnpdf — a minimal local-only PDF reader/annotator.
// The webview owns all logic (pdf.js + pdf-lib); Rust only does local IO:
// read/write files, a tiny JSON key-value store, pass-through of the file
// path given on the command line, and window geometry persistence.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use std::fs;
use std::sync::atomic::{AtomicBool, Ordering};
use tauri::{Manager, PhysicalPosition, PhysicalSize, WindowEvent};

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

// ---- window geometry (win.json, Rust-owned) ----

static WIN_DIRTY: AtomicBool = AtomicBool::new(false);

#[derive(serde::Serialize, serde::Deserialize)]
struct WinState {
    x: i32,
    y: i32,
    w: i32,
    h: i32,
}

fn win_file(app: &tauri::AppHandle) -> Option<std::path::PathBuf> {
    let dir = app.path().app_data_dir().ok()?;
    fs::create_dir_all(&dir).ok()?;
    Some(dir.join("win.json"))
}

fn save_win(app: &tauri::AppHandle) {
    let Some(win) = app.get_webview_window("main") else {
        return;
    };
    if win.is_minimized().unwrap_or(false) {
        return; // never persist the minimized -32000 position
    }
    let (Ok(pos), Ok(size)) = (win.outer_position(), win.outer_size()) else {
        return;
    };
    let Some(path) = win_file(app) else { return };
    let st = WinState {
        x: pos.x,
        y: pos.y,
        w: size.width as i32,
        h: size.height as i32,
    };
    if let Ok(json) = serde_json::to_string(&st) {
        let _ = fs::write(path, json);
    }
}

fn apply_default_bounds(win: &tauri::WebviewWindow) {
    // right side of the primary screen, full height (~62% width)
    let Ok(Some(mon)) = win.primary_monitor() else {
        return;
    };
    let mw = mon.size().width as i32;
    let mh = mon.size().height as i32;
    let w = (mw * 62 / 100).max(520);
    let h = mh.max(400);
    let x = mon.position().x + mw - w;
    let y = mon.position().y;
    let _ = win.set_size(PhysicalSize::new(w.max(1) as u32, h.max(1) as u32));
    let _ = win.set_position(PhysicalPosition::new(x.max(0), y.max(0)));
}

fn setup_window(app: &tauri::AppHandle) {
    let Some(win) = app.get_webview_window("main") else {
        return;
    };
    let restored = win_file(app)
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str::<WinState>(&t).ok());
    if let Some(st) = restored {
        let _ = win.set_position(PhysicalPosition::new(st.x, st.y));
        let _ = win.set_size(PhysicalSize::new(st.w.max(1) as u32, st.h.max(1) as u32));
    } else {
        apply_default_bounds(&win);
    }

    // persist geometry: events just flag, a 1s saver thread writes the latest
    let saver_app = app.clone();
    std::thread::spawn(move || loop {
        std::thread::sleep(std::time::Duration::from_millis(1000));
        if WIN_DIRTY.swap(false, Ordering::Relaxed) {
            save_win(&saver_app);
        }
    });

    let save_app = app.clone();
    win.on_window_event(move |e| match e {
        WindowEvent::Moved(_) | WindowEvent::Resized(_) => {
            WIN_DIRTY.store(true, Ordering::Relaxed);
        }
        WindowEvent::CloseRequested { .. } => save_win(&save_app),
        _ => {}
    });
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            setup_window(app.handle());
            Ok(())
        })
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
