// M5.3b endpoint (see endpoint.hpp).
#include "endpoint.hpp"

#include <algorithm>
#include <chrono>

namespace mppm::session {

Endpoint::Endpoint(net::Transport* transport, Hooks hooks, GameLink* link)
    : m_transport(transport), m_hooks(std::move(hooks)), m_link(link ? link : &m_ownLink)
{
}

Endpoint::~Endpoint() = default;

void Endpoint::make_host(const HostParams& p) { m_host = std::make_unique<HostCore>(*this, p); }

void Endpoint::make_client(const ClientParams& p) { m_client = std::make_unique<ClientCore>(*this, p); }

bool Endpoint::send(uint32_t peer, bool control, const std::vector<uint8_t>& data, bool flush)
{
	if (!m_transport || peer == 0) return false;
	if (control && data.size() > wire::kMaxChunked) {
		// A long control message (START with a long Synced or mod list, a full LOBBY): in parts on the
		// reliable, ordered channel; the receiving endpoint puts it back together.
		const size_t parts = (data.size() + wire::kChunkBytes - 1) / wire::kChunkBytes;
		if (parts > (size_t)wire::kMaxChunks) {
			++m_sendFailures;
			log("a control message of " + std::to_string(data.size()) + " bytes is too long to send");
			return false;
		}
		bool ok = true;
		for (size_t i = 0; i < parts; ++i) {
			std::vector<uint8_t> c;
			wire::put_header(c, wire::CHUNK, 0);
			c.push_back((uint8_t)i);
			c.push_back((uint8_t)parts);
			const size_t off = i * wire::kChunkBytes, len = std::min(wire::kChunkBytes, data.size() - off);
			c.insert(c.end(), data.begin() + (ptrdiff_t)off, data.begin() + (ptrdiff_t)(off + len));
			ok = m_transport->send(peer, net::Channel::Control, c.data(), c.size(), flush && i + 1 == parts) && ok;
		}
		if (!ok) ++m_sendFailures;
		return ok;
	}
	const bool ok = m_transport->send(peer, control ? net::Channel::Control : net::Channel::Input, data.data(), data.size(), flush);
	if (!ok) ++m_sendFailures;
	return ok;
}

void Endpoint::disconnect(uint32_t peer, uint32_t reason)
{
	if (m_transport && peer) m_transport->disconnect(peer, reason);
}

void Endpoint::flush_overflow()
{
	while (!m_overflow.empty()) {
		if (!m_link->n2g.push(m_overflow.front())) return;
		m_overflow.pop_front();
	}
}

void Endpoint::to_game(const N2G& m)
{
	flush_overflow();
	if (!m_overflow.empty() || !m_link->n2g.push(m)) m_overflow.push_back(m);
}

void Endpoint::note(const CoreNote& n)
{
	if (m_hooks.note) m_hooks.note(n);
}

int64_t Endpoint::slew_get() { return m_hooks.slewGet ? m_hooks.slewGet() : m_slewLocal; }

void Endpoint::slew_set(int64_t ns)
{
	if (m_hooks.slewSet) m_hooks.slewSet(ns);
	else m_slewLocal = ns;
}

bool Endpoint::tick_shift(int ticks)
{
	if (!m_hooks.tickShift) return false;
	m_hooks.tickShift(ticks);
	return true;
}

void Endpoint::log(const std::string& line)
{
	if (m_hooks.log) m_hooks.log(line);
}

void Endpoint::dispatch(const net::Event& e, int64_t now)
{
	if (m_hooks.transportEvent) m_hooks.transportEvent(e);
	switch (e.kind) {
	case net::Event::Kind::Connected:
		if (m_host) {
			net::PeerStats st;
			const std::string source = m_transport && m_transport->stats(e.peer, st) ? pending_source_key(st.remote) : std::string();
			m_host->on_connected(e.peer, e.connectData, now, source);
		}
		else if (m_client) m_client->on_connected(e.peer, now);
		break;
	case net::Event::Kind::Disconnected:
		m_chunks.erase(e.peer); // peer ids are not reused
		if (m_host) m_host->on_disconnected(e.peer, e.text, now);
		else if (m_client) m_client->on_disconnected(e.peer, e.text, now);
		break;
	case net::Event::Kind::Received: {
		const int64_t recv = e.recvNs ? (int64_t)e.recvNs : now;
		if (e.channel == net::Channel::Control && e.data.size() >= wire::kHeader && e.data[0] == wire::kMagic && e.data[2] == wire::CHUNK) {
			// A bad or out-of-order part goes to the core as it is: a CHUNK is no message the core reads,
			// so it counts as malformed (a bad header) or invalid traffic and ends the sender in the end.
			const auto bad = [&] {
				m_chunks.erase(e.peer);
				if (m_host) m_host->on_datagram(e.peer, e.data.data(), e.data.size(), recv, now);
				else if (m_client) m_client->on_datagram(e.peer, e.data.data(), e.data.size(), recv, now);
			};
			if (wire::decode_any(e.data.data(), e.data.size()) != wire::Result::Ok) {
				bad();
				break;
			}
			Reassembly& r   = m_chunks[e.peer];
			const int index = e.data[wire::kHeader], count = e.data[wire::kHeader + 1];
			if (index == 0) {
				r.data.clear();
				r.next = 0;
			}
			if (index != r.next) {
				bad();
				break;
			}
			r.data.insert(r.data.end(), e.data.begin() + wire::kHeader + 2, e.data.end());
			if (++r.next < count) break;
			std::vector<uint8_t> whole;
			whole.swap(r.data);
			m_chunks.erase(e.peer);
			if (m_host) m_host->on_datagram(e.peer, whole.data(), whole.size(), recv, now);
			else if (m_client) m_client->on_datagram(e.peer, whole.data(), whole.size(), recv, now);
			break;
		}
		if (m_host) m_host->on_datagram(e.peer, e.data.data(), e.data.size(), recv, now);
		else if (m_client) m_client->on_datagram(e.peer, e.data.data(), e.data.size(), recv, now);
		break;
	}
	default:
		break;
	}
}

int64_t Endpoint::now() const
{
	if (m_hooks.clock) return m_hooks.clock();
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Endpoint::step_game()
{
	const int64_t now = this->now();
	flush_overflow();
	G2N g;
	while (m_link->g2n.pop(g)) {
		if (m_host) m_host->on_game(g, now);
		else if (m_client) m_client->on_game(g, now);
	}
	if (const uint8_t bye = m_link->leaveReq.exchange(0)) {
		G2N l;
		l.kind  = G2N::Leave;
		l.count = bye;
		if (m_host) m_host->on_game(l, now);
		else if (m_client) m_client->on_game(l, now);
	}
	if (m_host) m_host->tick(now);
	else if (m_client) m_client->tick(now);
	flush_overflow();
}

void Endpoint::step(uint32_t waitUs)
{
	if (m_transport) {
		m_events.clear();
		m_transport->service(waitUs, m_events);
		for (const net::Event& e : m_events) dispatch(e, now());
	}
	step_game();
}

} // namespace mppm::session
