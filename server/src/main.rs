//! mppm-server: lobby (join codes, membership), WebSocket signalling and the builtin UDP relay for
//! mppm online play. See README.md (running, flags, self-hosting) and PROTOCOL.md (the wire protocol).

mod codes;
mod config;
mod ice;
mod proto;
mod ratelimit;
mod relay;
mod rest;
mod session;
mod ws;

use std::fs::{File, OpenOptions};
use std::io::Write;
use std::net::SocketAddr;
use std::path::Path;
use std::process::ExitCode;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use clap::Parser;

use crate::config::{Args, RelayMode};
use crate::relay::{CloseReason, Relay};
use crate::session::App;

// ---- logging: stderr, plus --log-file ----

static LOG_FILE: OnceLock<Mutex<File>> = OnceLock::new();

/// Writes one line to stderr and the log file (the ready line, relay mapping lines).
pub fn emit(line: &str) {
    eprintln!("{line}");
    if let Some(f) = LOG_FILE.get() {
        let mut f = f.lock().unwrap_or_else(|e| e.into_inner());
        let _ = writeln!(f, "{line}");
    }
}

#[derive(Clone, Copy)]
struct Tee;

impl Write for Tee {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        std::io::stderr().write_all(buf)?;
        if let Some(f) = LOG_FILE.get() {
            let mut f = f.lock().unwrap_or_else(|e| e.into_inner());
            let _ = f.write_all(buf);
        }
        Ok(buf.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        std::io::stderr().flush()
    }
}

fn init_logging(log_file: Option<&Path>) -> anyhow::Result<()> {
    if let Some(p) = log_file {
        let f = OpenOptions::new()
            .create(true)
            .append(true)
            .open(p)
            .map_err(|e| anyhow::anyhow!("cannot open --log-file {}: {e}", p.display()))?;
        let _ = LOG_FILE.set(Mutex::new(f));
    }
    // RUST_LOG overrides; the default keeps stderr quiet until the ready line.
    let filter = tracing_subscriber::EnvFilter::try_from_default_env()
        .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("mppm_server=info,warn"));
    tracing_subscriber::fmt().with_env_filter(filter).with_ansi(false).with_writer(|| Tee).init();
    Ok(())
}

fn write_port_file(path: &Path, http: u16, relay: Option<u16>) -> anyhow::Result<()> {
    let mut text = format!("{http}\n");
    if let Some(r) = relay {
        text.push_str(&format!("relay={r}\n"));
    }
    let tmp = path.with_extension("tmp");
    std::fs::write(&tmp, text)?;
    std::fs::rename(&tmp, path)?;
    Ok(())
}

async fn shutdown_signal() {
    let ctrl_c = async {
        let _ = tokio::signal::ctrl_c().await;
    };
    #[cfg(unix)]
    let term = async {
        match tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) {
            Ok(mut s) => {
                s.recv().await;
            }
            Err(_) => std::future::pending::<()>().await,
        }
    };
    #[cfg(not(unix))]
    let term = std::future::pending::<()>();
    tokio::select! {
        _ = ctrl_c => {},
        _ = term => {},
    }
}

async fn run(args: Args) -> anyhow::Result<()> {
    let listener = tokio::net::TcpListener::bind(args.bind)
        .await
        .map_err(|e| anyhow::anyhow!("cannot bind {}: {e}", args.bind))?;
    let http_addr = listener.local_addr()?;

    let relay = if args.mode() == RelayMode::Builtin {
        let r = Relay::bind(
            args.relay_bind,
            args.relay_port,
            args.relay_ports,
            args.relay_kbps,
            Duration::from_secs(args.relay_idle_secs),
        )
        .await?;
        Some(Arc::new(r))
    } else {
        None
    };
    let relay_port = relay.as_ref().map(|r| r.local_port());
    let announce = relay_port.map(|p| format!("{}:{p}", args.relay_public.unwrap_or(args.relay_bind)));
    let relay_part = match relay_port {
        Some(rp) => format!("relay={}:{rp} ports={}-{}", args.relay_bind, args.relay_ports.0, args.relay_ports.1),
        None => "relay=- ports=-".to_string(),
    };
    let mode = args.mode();
    let port_file = args.port_file.clone();
    let app = App::new(args, relay.clone(), announce);

    if let Some(r) = &relay {
        tokio::spawn(r.clone().run_control(app.clone()));
    }
    {
        let app = app.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(Duration::from_millis(250));
            let mut last_sweep = Instant::now();
            loop {
                tick.tick().await;
                let now = Instant::now();
                app.reap(now);
                if now.duration_since(last_sweep) >= Duration::from_secs(60) {
                    last_sweep = now;
                    app.limits().sweep(now);
                }
            }
        });
    }

    if let Some(p) = &port_file {
        write_port_file(p, http_addr.port(), relay_port)
            .map_err(|e| anyhow::anyhow!("cannot write --port-file {}: {e}", p.display()))?;
    }
    emit(&format!("mppm-server: listening http={http_addr} {relay_part} mode={}", mode.as_str()));

    let router = rest::router(app.clone());
    let server = axum::serve(listener, router.into_make_service_with_connect_info::<SocketAddr>());
    let (stop_tx, stop_rx) = tokio::sync::oneshot::channel::<()>();
    let server_task = tokio::spawn(async move {
        let _ = server
            .with_graceful_shutdown(async move {
                let _ = stop_rx.await;
            })
            .await;
    });

    shutdown_signal().await;
    tracing::info!("shutting down");
    // Every member gets closed {reason:"server_shutdown"}; sockets that have not joined close too.
    app.end_all();
    let _ = app.shutdown.send(true);
    let _ = stop_tx.send(());
    let deadline = Instant::now() + Duration::from_secs(2);
    while app.live_ws.load(Ordering::SeqCst) > 0 && Instant::now() < deadline {
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
    if let Some(r) = &relay {
        r.close_all(CloseReason::Shutdown);
    }
    let _ = tokio::time::timeout(Duration::from_millis(500), server_task).await;
    Ok(())
}

fn main() -> ExitCode {
    let args = Args::parse();
    if let Err(msg) = args.validate() {
        eprintln!("mppm-server: {msg}");
        return ExitCode::from(2);
    }
    if let Err(e) = init_logging(args.log_file.as_deref()) {
        eprintln!("mppm-server: {e}");
        return ExitCode::from(2);
    }
    let rt = match tokio::runtime::Builder::new_multi_thread().enable_all().build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("mppm-server: cannot start the runtime: {e}");
            return ExitCode::FAILURE;
        }
    };
    match rt.block_on(run(args)) {
        Ok(()) => {
            // Detached socket tasks may still be running; do not wait for them.
            rt.shutdown_timeout(Duration::from_millis(200));
            ExitCode::SUCCESS
        }
        Err(e) => {
            emit(&format!("mppm-server: {e}"));
            ExitCode::FAILURE
        }
    }
}
