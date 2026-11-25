// M5.3b in-process transport (see mem_transport.hpp).
#include "mem_transport.hpp"

#include <chrono>
#include <climits>

namespace mppm::session {

namespace {
const char* kReq = "@mem-req";
const char* kAck = "@mem-ack";
} // namespace

MemHub::MemHub(std::function<int64_t()> clock) : m_clock(std::move(clock)) { }

MemHub::~MemHub() = default;

int64_t MemHub::now() const
{
	if (m_clock) return m_clock();
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::unique_ptr<MemTransport> MemHub::endpoint(const std::string& name, const net::Impair& impair)
{
	return std::make_unique<MemTransport>(*this, name, impair);
}

void MemHub::post(const std::string& to, int64_t release, net::Event ev)
{
	auto it = m_boxes.find(to);
	if (it == m_boxes.end() || it->second.dead) return;
	Packet p;
	p.release = release;
	p.seq     = ++m_seq;
	p.ev      = std::move(ev);
	it->second.inbox.push(std::move(p));
	m_cv.notify_all();
}

void MemHub::vanish(const std::string& name, int64_t afterNs)
{
	std::lock_guard<std::mutex> lk(m_mu);
	auto it = m_boxes.find(name);
	if (it == m_boxes.end()) return;
	it->second.dead = true;
	while (!it->second.inbox.empty()) it->second.inbox.pop();
	if (MemTransport* t = it->second.owner) {
		for (auto& [pid, peer] : t->m_peers) {
			if (!peer.remotePeer) continue;
			net::Event ev;
			ev.kind = net::Event::Kind::Disconnected;
			ev.peer = peer.remotePeer;
			ev.text = "timeout";
			post(peer.remote, now() + afterNs, ev);
		}
	}
}

int64_t MemHub::next_release()
{
	std::lock_guard<std::mutex> lk(m_mu);
	int64_t best = INT64_MAX;
	for (auto& [name, box] : m_boxes)
		if (!box.dead && !box.inbox.empty() && box.inbox.top().release < best) best = box.inbox.top().release;
	return best;
}

MemTransport::MemTransport(MemHub& hub, const std::string& name, const net::Impair& impair) : m_hub(hub), m_name(name), m_impair(impair)
{
	m_rng = impair.seed ? impair.seed : 1;
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	MemHub::Box& b = m_hub.m_boxes[name];
	b.owner        = this;
	b.dead         = false;
}

MemTransport::~MemTransport()
{
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	for (auto& [pid, peer] : m_peers) {
		if (!peer.remotePeer) continue;
		net::Event ev;
		ev.kind = net::Event::Kind::Disconnected;
		ev.peer = peer.remotePeer;
		ev.text = "remote closed";
		m_hub.post(peer.remote, m_hub.now(), ev);
	}
	auto it = m_hub.m_boxes.find(m_name);
	if (it != m_hub.m_boxes.end() && it->second.owner == this) m_hub.m_boxes.erase(it);
}

uint32_t MemTransport::rnd()
{
	uint32_t x = m_rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	m_rng = x;
	return x;
}

int64_t MemTransport::delay_draw()
{
	int64_t d = (int64_t)m_impair.delayUs * 1000;
	if (m_impair.jitterUs) d += (int64_t)(rnd() % (m_impair.jitterUs + 1)) * 1000;
	return d;
}

net::PeerId MemTransport::connect(const std::string& ipPort, uint32_t connectData)
{
	std::string to = ipPort.rfind("mem:", 0) == 0 ? ipPort.substr(4) : ipPort;
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	const net::PeerId pid = m_nextPeer++;
	Peer& p               = m_peers[pid];
	p.remote              = to;
	p.st.remote           = "mem:" + to;
	net::Event ev;
	ev.kind        = net::Event::Kind::Connected;
	ev.text        = kReq;
	ev.aux         = m_name;
	ev.peer        = pid;
	ev.connectData = connectData;
	m_hub.post(to, m_hub.now() + delay_draw(), ev);
	return pid;
}

bool MemTransport::send(net::PeerId peer, net::Channel channel, const uint8_t* data, size_t size, bool flush)
{
	(void)flush;
	if (size == 0 || size > net::kMaxDatagram) return false;
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	auto it = m_peers.find(peer);
	if (it == m_peers.end() || !it->second.connected || !it->second.remotePeer) return false;
	Peer& p = it->second;
	auto box = m_hub.m_boxes.find(m_name);
	if (box != m_hub.m_boxes.end() && box->second.dead) return true; // a vanished endpoint sends into the void
	p.st.bytesSent += size;
	p.st.datagramsSent++;
	net::Event ev;
	ev.kind    = net::Event::Kind::Received;
	ev.peer    = p.remotePeer;
	ev.channel = channel;
	ev.data.assign(data, data + size);
	const int64_t now = m_hub.now();
	if (channel == net::Channel::Control) {
		int64_t rel = now + delay_draw();
		if (rel < p.lastControlRelease) rel = p.lastControlRelease;
		p.lastControlRelease = rel;
		m_hub.post(p.remote, rel, std::move(ev));
		return true;
	}
	if (m_impair.lossPpm && rnd() % 1000000u < m_impair.lossPpm) {
		p.st.impairDropped++;
		return true;
	}
	const bool dup = m_impair.dupPpm && rnd() % 1000000u < m_impair.dupPpm;
	if (dup) m_hub.post(p.remote, now + delay_draw(), ev);
	m_hub.post(p.remote, now + delay_draw(), std::move(ev));
	return true;
}

void MemTransport::disconnect(net::PeerId peer, uint32_t reason)
{
	(void)reason;
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	auto it = m_peers.find(peer);
	if (it == m_peers.end()) return;
	const int64_t now = m_hub.now();
	if (it->second.remotePeer) {
		net::Event ev;
		ev.kind = net::Event::Kind::Disconnected;
		ev.peer = it->second.remotePeer;
		ev.text = "remote disconnect";
		int64_t rel = now + delay_draw();
		if (rel < it->second.lastControlRelease) rel = it->second.lastControlRelease; // after its reliable data
		m_hub.post(it->second.remote, rel, ev);
	}
	net::Event local;
	local.kind = net::Event::Kind::Disconnected;
	local.peer = peer;
	local.text = "local disconnect";
	m_hub.post(m_name, now, local);
	it->second.connected  = false;
	it->second.remotePeer = 0;
}

void MemTransport::reset(net::PeerId peer)
{
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	auto it = m_peers.find(peer);
	if (it == m_peers.end()) return;
	if (it->second.remotePeer) {
		net::Event ev;
		ev.kind = net::Event::Kind::Disconnected;
		ev.peer = it->second.remotePeer;
		ev.text = "timeout";
		m_hub.post(it->second.remote, m_hub.now() + m_hub.resetTimeoutNs, ev);
	}
	m_peers.erase(it);
}

void MemTransport::service(uint32_t timeoutUs, std::vector<net::Event>& out)
{
	std::unique_lock<std::mutex> lk(m_hub.m_mu);
	const int64_t deadline = m_hub.now() + (int64_t)timeoutUs * 1000;
	for (;;) {
		auto boxIt = m_hub.m_boxes.find(m_name);
		if (boxIt == m_hub.m_boxes.end() || boxIt->second.dead) return;
		MemHub::Box& box  = boxIt->second;
		const int64_t now = m_hub.now();
		bool any          = false;
		while (!box.inbox.empty() && box.inbox.top().release <= now) {
			MemHub::Packet pk = box.inbox.top();
			box.inbox.pop();
			net::Event& ev = pk.ev;
			const uint64_t recv = (uint64_t)((m_hub.virtual_clock() ? pk.release : now) + m_clockOffset);
			if (ev.kind == net::Event::Kind::Connected && ev.text == kReq) {
				const net::PeerId pid = m_nextPeer++;
				Peer& p               = m_peers[pid];
				p.remote              = ev.aux;
				p.remotePeer          = ev.peer;
				p.connected           = true;
				p.st.remote           = "mem:" + ev.aux;
				net::Event ack;
				ack.kind        = net::Event::Kind::Connected;
				ack.text        = kAck;
				ack.peer        = ev.peer;
				ack.aux         = std::to_string(pid);
				ack.connectData = ev.connectData;
				m_hub.post(ev.aux, now + delay_draw(), ack);
				net::Event c;
				c.kind        = net::Event::Kind::Connected;
				c.peer        = pid;
				c.connectData = ev.connectData;
				out.push_back(c);
				any = true;
				continue;
			}
			if (ev.kind == net::Event::Kind::Connected && ev.text == kAck) {
				auto it = m_peers.find(ev.peer);
				if (it == m_peers.end()) continue;
				it->second.remotePeer = (net::PeerId)std::stoul(ev.aux);
				it->second.connected  = true;
				net::Event c;
				c.kind        = net::Event::Kind::Connected;
				c.peer        = ev.peer;
				c.connectData = ev.connectData;
				out.push_back(c);
				any = true;
				continue;
			}
			auto it = m_peers.find(ev.peer);
			if (ev.kind == net::Event::Kind::Disconnected) {
				if (it != m_peers.end()) m_peers.erase(it);
				else if (ev.text != "local disconnect") continue;
				out.push_back(ev);
				any = true;
				continue;
			}
			if (it == m_peers.end() || !it->second.connected) continue;
			it->second.st.bytesReceived += ev.data.size();
			it->second.st.datagramsReceived++;
			ev.recvNs = recv;
			out.push_back(std::move(ev));
			any = true;
		}
		if (any || timeoutUs == 0 || m_hub.virtual_clock()) return;
		if (m_woken) {
			m_woken = false;
			return;
		}
		if (now >= deadline) return;
		int64_t until = deadline;
		if (!box.inbox.empty() && box.inbox.top().release < until) until = box.inbox.top().release;
		m_hub.m_cv.wait_for(lk, std::chrono::nanoseconds(until - now));
	}
}

void MemTransport::set_impair(const net::Impair& impair)
{
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	m_impair = impair;
}

void MemTransport::wake()
{
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	m_woken = true;
	m_hub.m_cv.notify_all();
}

bool MemTransport::stats(net::PeerId peer, net::PeerStats& out) const
{
	std::lock_guard<std::mutex> lk(m_hub.m_mu);
	auto it = m_peers.find(peer);
	if (it == m_peers.end()) return false;
	out      = it->second.st;
	out.link = net::Link::Direct;
	return true;
}

} // namespace mppm::session
