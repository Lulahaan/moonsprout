//! Token buckets and the per-IP limits (PROTOCOL.md, "Rate limits").

use std::collections::HashMap;
use std::net::IpAddr;
use std::time::{Duration, Instant};

/// A token bucket: `rate` tokens per second, at most `burst`.
#[derive(Debug, Clone)]
pub struct Bucket {
    tokens: f64,
    burst: f64,
    rate: f64,
    last: Instant,
}

impl Bucket {
    pub fn new(rate_per_sec: f64, burst: f64, now: Instant) -> Bucket {
        Bucket { tokens: burst, burst, rate: rate_per_sec, last: now }
    }

    fn refill(&mut self, now: Instant) {
        let dt = now.saturating_duration_since(self.last).as_secs_f64();
        self.tokens = (self.tokens + dt * self.rate).min(self.burst);
        self.last = now;
    }

    /// Takes `n` tokens if there are that many.
    pub fn take(&mut self, n: f64, now: Instant) -> bool {
        self.refill(now);
        if self.tokens >= n {
            self.tokens -= n;
            true
        } else {
            false
        }
    }

    pub fn is_full(&mut self, now: Instant) -> bool {
        self.refill(now);
        self.tokens >= self.burst
    }
}

/// The limits of 5.7, already multiplied by --limits-scale.
#[derive(Debug, Clone)]
pub struct Limits {
    pub create_rate: f64,
    pub create_burst: f64,
    pub join_rate: f64,
    pub join_burst: f64,
    pub fail_rate: f64,
    pub fail_burst: f64,
    pub lockout: Duration,
    pub upgrade_rate: f64,
    pub upgrade_burst: f64,
    pub reg_rate: f64,
    pub reg_burst: f64,
    pub live_ws: u32,
    pub live_hosted: u32,
}

impl Limits {
    pub fn scaled(scale: f64) -> Limits {
        let per_min = |n: f64| n * scale / 60.0;
        let count = |n: f64| (n * scale).ceil().max(1.0);
        Limits {
            create_rate: per_min(6.0),
            create_burst: count(3.0),
            join_rate: per_min(30.0),
            join_burst: count(10.0),
            fail_rate: per_min(10.0),
            fail_burst: count(5.0),
            lockout: Duration::from_secs(300),
            upgrade_rate: per_min(20.0),
            upgrade_burst: count(10.0),
            reg_rate: per_min(16.0),
            reg_burst: count(16.0),
            live_ws: count(16.0) as u32,
            live_hosted: count(4.0) as u32,
        }
    }
}

#[derive(Debug)]
struct IpState {
    create: Bucket,
    join: Bucket,
    fail: Bucket,
    upgrade: Bucket,
    reg: Bucket,
    locked_until: Option<Instant>,
    live_ws: u32,
    live_hosted: u32,
}

/// Per-IP state. Not thread-safe; the caller holds it under a mutex.
#[derive(Debug)]
pub struct IpLimiter {
    limits: Limits,
    ips: HashMap<IpAddr, IpState>,
}

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum What {
    Create,
    Join,
    Upgrade,
    Reg,
}

impl IpLimiter {
    pub fn new(limits: Limits) -> IpLimiter {
        IpLimiter { limits, ips: HashMap::new() }
    }

    fn state(&mut self, ip: IpAddr, now: Instant) -> &mut IpState {
        let l = &self.limits;
        self.ips.entry(ip).or_insert_with(|| IpState {
            create: Bucket::new(l.create_rate, l.create_burst, now),
            join: Bucket::new(l.join_rate, l.join_burst, now),
            fail: Bucket::new(l.fail_rate, l.fail_burst, now),
            upgrade: Bucket::new(l.upgrade_rate, l.upgrade_burst, now),
            reg: Bucket::new(l.reg_rate, l.reg_burst, now),
            locked_until: None,
            live_ws: 0,
            live_hosted: 0,
        })
    }

    /// One event of kind `what` from `ip`; false = over the limit (429 / rate_limited).
    pub fn allow(&mut self, ip: IpAddr, what: What, now: Instant) -> bool {
        let s = self.state(ip, now);
        match what {
            What::Create => s.create.take(1.0, now),
            What::Join => {
                if s.locked_until.is_some_and(|t| now < t) {
                    return false;
                }
                s.join.take(1.0, now)
            }
            What::Upgrade => s.upgrade.take(1.0, now),
            What::Reg => s.reg.take(1.0, now),
        }
    }

    /// A join failed (not_found, bad_password). false = the IP is now locked out for 5 minutes.
    pub fn join_failed(&mut self, ip: IpAddr, now: Instant) -> bool {
        let lockout = self.limits.lockout;
        let s = self.state(ip, now);
        if s.fail.take(1.0, now) {
            true
        } else {
            s.locked_until = Some(now + lockout);
            false
        }
    }

    /// Takes a live WebSocket slot; false = 16 (scaled) are already open from this IP.
    pub fn ws_open(&mut self, ip: IpAddr, now: Instant) -> bool {
        let cap = self.limits.live_ws;
        let s = self.state(ip, now);
        if s.live_ws >= cap {
            return false;
        }
        s.live_ws += 1;
        true
    }

    pub fn ws_close(&mut self, ip: IpAddr) {
        if let Some(s) = self.ips.get_mut(&ip) {
            s.live_ws = s.live_ws.saturating_sub(1);
        }
    }

    /// Takes a hosted-session slot; false = 4 (scaled) sessions are already hosted from this IP.
    pub fn host_open(&mut self, ip: IpAddr, now: Instant) -> bool {
        let cap = self.limits.live_hosted;
        let s = self.state(ip, now);
        if s.live_hosted >= cap {
            return false;
        }
        s.live_hosted += 1;
        true
    }

    pub fn host_close(&mut self, ip: IpAddr) {
        if let Some(s) = self.ips.get_mut(&ip) {
            s.live_hosted = s.live_hosted.saturating_sub(1);
        }
    }

    /// Forgets IPs with nothing live, no lockout and full buckets.
    pub fn sweep(&mut self, now: Instant) {
        self.ips.retain(|_, s| {
            let locked = s.locked_until.is_some_and(|t| now < t);
            s.live_ws > 0
                || s.live_hosted > 0
                || locked
                || !(s.create.is_full(now)
                    && s.join.is_full(now)
                    && s.fail.is_full(now)
                    && s.upgrade.is_full(now)
                    && s.reg.is_full(now))
        });
    }

    #[cfg(test)]
    pub fn tracked(&self) -> usize {
        self.ips.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bucket_burst_and_refill() {
        let t0 = Instant::now();
        let mut b = Bucket::new(2.0, 3.0, t0);
        assert!(b.take(1.0, t0) && b.take(1.0, t0) && b.take(1.0, t0));
        assert!(!b.take(1.0, t0));
        assert!(!b.take(1.0, t0 + Duration::from_millis(400)));
        assert!(b.take(1.0, t0 + Duration::from_millis(600)));
        assert!(!b.take(1.0, t0 + Duration::from_millis(700)));
        // Refill never exceeds the burst.
        let later = t0 + Duration::from_secs(100);
        assert!(b.is_full(later));
        assert!(b.take(3.0, later));
        assert!(!b.take(1.0, later));
    }

    #[test]
    fn joins_and_lockout() {
        let t0 = Instant::now();
        let ip: IpAddr = "127.0.0.1".parse().unwrap();
        let mut l = IpLimiter::new(Limits::scaled(1.0));
        for _ in 0..10 {
            assert!(l.allow(ip, What::Join, t0));
        }
        assert!(!l.allow(ip, What::Join, t0));
        let t1 = t0 + Duration::from_secs(600);
        for _ in 0..5 {
            assert!(l.join_failed(ip, t1));
        }
        assert!(!l.join_failed(ip, t1)); // sixth failure in a burst: locked out
        assert!(!l.allow(ip, What::Join, t1 + Duration::from_secs(10)));
        assert!(!l.allow(ip, What::Join, t1 + Duration::from_secs(299)));
        assert!(l.allow(ip, What::Join, t1 + Duration::from_secs(301)));
    }

    #[test]
    fn scaled_limits() {
        let l = Limits::scaled(20.0);
        assert_eq!(l.create_burst, 60.0);
        assert_eq!(l.live_ws, 320);
        assert!((l.join_rate - 10.0).abs() < 1e-9); // 30/min x 20 = 10/s
        let l = Limits::scaled(0.01);
        assert_eq!(l.live_hosted, 1);
    }

    #[test]
    fn live_counters_and_sweep() {
        let t0 = Instant::now();
        let ip: IpAddr = "10.0.0.1".parse().unwrap();
        let mut l = IpLimiter::new(Limits::scaled(1.0));
        for _ in 0..4 {
            assert!(l.host_open(ip, t0));
        }
        assert!(!l.host_open(ip, t0));
        l.host_close(ip);
        assert!(l.host_open(ip, t0));
        for _ in 0..16 {
            assert!(l.ws_open(ip, t0));
        }
        assert!(!l.ws_open(ip, t0));
        l.sweep(t0 + Duration::from_secs(3600));
        assert_eq!(l.tracked(), 1);
        for _ in 0..16 {
            l.ws_close(ip);
        }
        for _ in 0..4 {
            l.host_close(ip);
        }
        l.sweep(t0 + Duration::from_secs(3600));
        assert_eq!(l.tracked(), 0);
    }
}
