/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimProtoTcp.hh"

#include "config.h"
#include "network.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace HTSim {

HTSimProtoTcp* HTSimProtoTcp::instance = nullptr;

HTSimProtoTcp::RtxScanner::RtxScanner(const simtime_picosec period, EventList& eventlist)
    : EventSource(eventlist, "tcp_rtx_scanner"), period(period) {}

// The scanner is pending exactly while a flow is in flight. A scan left
// pending after the last flow would keep the event list non-empty: the
// frontend would then jump straight to it instead of advancing an idle
// cluster in 1 ms quanta, and see request arrivals up to a period late.
void HTSimProtoTcp::RtxScanner::add(const flowid_t flow_id, TcpSrc* const tcp) {
    tcps.emplace(flow_id, tcp);
    if (!armed) {
        armed = true;
        handle = EventList::sourceIsPendingGetHandle(*this, EventList::now() + period);
    }
}

void HTSimProtoTcp::RtxScanner::remove(const flowid_t flow_id) {
    tcps.erase(flow_id);
    if (tcps.empty() && armed) {
        armed = false;
        EventList::cancelPendingSourceByHandle(*this, handle);
    }
}

void HTSimProtoTcp::RtxScanner::doNextEvent() {
    armed = false;
    const auto now = eventlist().now();
    for (const auto& [flow_id, tcp] : tcps) {
        tcp->rtx_timer_hook(now, period);
    }
    if (!tcps.empty()) {
        armed = true;
        handle = EventList::sourceIsPendingGetHandle(*this, now + period);
    }
}

HTSimProtoTcp::HTSimProtoTcp(const HTSimConfig& config, EventList& eventlist)
    : eventlist(eventlist),
      topology(config, eventlist),
      tcp_handshake(config.tcp_handshake),
      collect_threshold(1024) {
    if (instance != nullptr) {
        throw std::logic_error("HTSim TCP protocol created twice");
    }
    if (config.mtu_bytes <= TcpAck::ACKSIZE) {
        throw std::invalid_argument("HTSim MTU must exceed the 40-byte acknowledgement");
    }
    if (config.initial_cwnd_packets == 0) {
        throw std::invalid_argument("HTSim initial congestion window must be at least one packet");
    }
    if (!std::isfinite(config.min_rto_us) || config.min_rto_us <= 0 ||
        !std::isfinite(config.initial_rto_us) || config.initial_rto_us <= 0) {
        throw std::invalid_argument("HTSim retransmission timeouts must be positive");
    }
    instance = this;

    // Must precede every TcpSrc: each one reads its MSS at construction.
    Packet::set_packet_size(config.mtu_bytes);
    const uint64_t mss = config.mtu_bytes;
    initial_cwnd_bytes = static_cast<uint32_t>(std::min<uint64_t>(
        config.initial_cwnd_packets * mss, std::numeric_limits<uint32_t>::max() - mss));
    TcpSrc::_min_rto = timeFromUs(config.min_rto_us);
    initial_rto = timeFromUs(config.initial_rto_us);

    // Upstream scans every 10 ms against a 250 ms minimum RTO. Keep that
    // ratio when the minimum RTO is lowered, so a timeout is noticed soon.
    const auto period = std::clamp(TcpSrc::_min_rto / 25, timeFromUs(1.0), timeFromMs(10));
    rtx_scanner = std::make_unique<RtxScanner>(period, eventlist);
}

HTSimProtoTcp::~HTSimProtoTcp() {
    instance = nullptr;
}

void HTSimProtoTcp::start_flow(const flowid_t flow_id,
                               const int src,
                               const int dst,
                               const uint64_t size) {
    auto* const tcp_src = new TcpSrc(nullptr, nullptr, eventlist);
    auto* const tcp_sink = new TcpSink();
    tcp_src->astrasim_flow_finish_send_cb = &HTSimProtoTcp::on_flow_sent;
    tcp_sink->astrasim_flow_finish_recv_cb = &HTSimProtoTcp::on_flow_received;
    tcp_src->_astrasim_preestablished = !tcp_handshake;

    // Packets are fixed-size, so a message occupies n full packets. The
    // sender keeps transmitting while _highest_sent <= _flow_size + 1 and the
    // flow completes once the cumulative ACK reaches _flow_size; data
    // sequence numbers start after the 1-byte SYN when there is one. These
    // values send exactly n data packets and complete on the n-th, where
    // upstream's set_flowsize(size) would send one more.
    const uint64_t mss = tcp_src->_mss;
    const uint64_t packets = std::max<uint64_t>(1, (size + mss - 1) / mss);
    tcp_src->_flow_size = packets * mss - (tcp_handshake ? 1 : 2);

    // Upstream starts every flow at a fixed 3 s; a loss before the first RTT
    // sample waits this long, since timeouts do not back off.
    tcp_src->_rto = initial_rto;
    tcp_src->_cwnd = initial_cwnd_bytes;
    tcp_src->_ssthresh = std::max(tcp_src->_ssthresh, initial_cwnd_bytes);

    auto* const route_out = new Route(topology.route(src, dst), *tcp_sink);
    auto* const route_back = new Route(topology.route(dst, src), *tcp_src);
    tcp_src->connect(*route_out, *route_back, *tcp_sink, eventlist.now());
    // connect() assigns the flow's own id; the session's id goes after it.
    tcp_src->setFlowId(flow_id);

    rtx_scanner->add(flow_id, tcp_src);
    active.emplace(flow_id, Connection{src, dst, tcp_src, tcp_sink, route_out, route_back});
}

std::string HTSimProtoTcp::summary() const {
    auto out = std::ostringstream();
    out << "TCP: " << active.size() << " active, " << finished.size()
        << " finished awaiting release, " << freed << " released; "
        << topology.links_count() << " links, " << topology.packet_drops() << " packet drops";
    return out.str();
}

void HTSimProtoTcp::on_flow_received(const flowid_t flow_id) {
    HTSimSession::instance().flow_received(flow_id);
}

void HTSimProtoTcp::on_flow_sent(const flowid_t flow_id) {
    auto* const self = instance;
    auto it = self->active.find(flow_id);
    assert(it != self->active.end());
    self->rtx_scanner->remove(flow_id);
    auto* const tcp_src = it->second.src;
    if (tcp_src->_rtx_timeout_pending) {
        // A retransmission armed before the final ACK arrived would only
        // keep the event list busy: the patched TcpSrc ignores it.
        EventList::cancelPendingSource(*tcp_src);
        tcp_src->_rtx_timeout_pending = false;
    }
    // Collect before adding this connection: its source is still on the call
    // stack (this runs inside TcpSrc::receivePacket), and no other
    // connection's endpoint can be.
    if (self->finished.size() >= self->collect_threshold) {
        self->collect_garbage();
    }
    auto connection = it->second;
    // The patched TcpSrc sends nothing after completion, so every data packet
    // of this flow has already entered the forward link.
    connection.data_mark = self->topology.entered(connection.src_id, connection.dst_id);
    self->finished.push_back(connection);
    self->active.erase(it);
    HTSimSession::instance().flow_sent(flow_id);
}

void HTSimProtoTcp::collect_garbage() {
    auto kept = std::vector<Connection>();
    for (auto c : finished) {
        // Queues and pipes dereference a packet's flow, which lives inside
        // the TcpSrc, so nothing may be freed while a packet is in flight.
        // The sink acknowledges on arrival, so once the data has passed the
        // forward link every acknowledgement is on the reverse one.
        if (!c.data_passed && topology.passed(c.src_id, c.dst_id) >= c.data_mark) {
            c.data_passed = true;
            c.ack_mark = topology.entered(c.dst_id, c.src_id);
        }
        const auto reachable = c.src->_rtx_timeout_pending || !c.data_passed ||
                               topology.passed(c.dst_id, c.src_id) < c.ack_mark;
        if (reachable) {
            kept.push_back(c);
            continue;
        }
        ++freed;
        delete c.src;
        delete c.sink;
        delete c.route_out;
        delete c.route_back;
    }
    finished.swap(kept);
    // Amortized: sweep again once as many connections have finished.
    collect_threshold = std::max<size_t>(1024, 2 * finished.size());
}

}  // namespace HTSim
