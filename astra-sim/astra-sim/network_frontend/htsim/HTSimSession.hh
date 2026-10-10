/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HTSim {

typedef uint32_t flowid_t;
typedef void (*EventHandler)(void*);

/// How a message crosses a link.
enum class HTSimTransport {
    Tcp,  // one TCP flow per message: slow start, ACKs, loss recovery
    Raw,  // packets at the link's full bandwidth; no congestion control or ACKs
};

/**
 * Physical network and transport settings of one HTSim run.
 *
 * Links mirror the analytical backend's network configuration: every
 * dimension is FullyConnected, and a message between two NPUs crosses one
 * link of the lowest dimension in which their addresses differ.
 */
struct HTSimConfig {
    std::vector<int> npus_count_per_dim;
    std::vector<double> bandwidth_per_dim;  // GiB/s, as the analytical backend reads it
    std::vector<double> latency_per_dim;    // ns per link
    HTSimTransport transport = HTSimTransport::Tcp;
    uint64_t queue_bytes = 0;               // per-link output buffer; 0 = unbounded (lossless)
    int mtu_bytes = 1500;                   // packet size, headers included
    // TCP only
    uint32_t initial_cwnd_packets = 10;     // TCP congestion window at the start of a message
    double min_rto_us = 250000;             // lower bound of the TCP retransmission timeout
    double initial_rto_us = 3000000;        // TCP retransmission timeout before the first RTT sample
    bool tcp_handshake = false;             // false: connections already exist (no SYN round trip)
};

/**
 * Transport protocol behind an HTSimSession. A protocol starts flows and
 * reports their completion through HTSimSession::flow_received() and
 * HTSimSession::flow_sent().
 */
class HTSimProtocol {
  public:
    virtual ~HTSimProtocol() = default;
    virtual void start_flow(flowid_t flow_id, int src, int dst, uint64_t size) = 0;
    /// One-line summary for the HTSim log.
    virtual std::string summary() const = 0;
};

/**
 * HTSim simulation shared by every NPU's network API.
 *
 * Besides running the packet-level transport it provides what the
 * LLMServingSim frontend loop needs from a backend: advancing one timestamp
 * at a time, telling whether anything is pending, and moving the clock while
 * the cluster is idle.
 */
class HTSimSession {
  public:
    static HTSimSession& init(const HTSimConfig& config);
    static HTSimSession& instance();

    HTSimSession(const HTSimSession&) = delete;
    void operator=(const HTSimSession&) = delete;

    /// Whether any event (an ASTRA-sim callback or a packet) is pending.
    bool has_pending_events() const;

    /// Run every event scheduled at the next pending timestamp.
    void proceed();

    /// Move the clock forward by delta_ns while nothing is pending.
    void advance_idle(double delta_ns);

    double get_time_ns() const;

    /// Run fn after delta_ns. Callbacks due at one timestamp run in the order
    /// they were scheduled.
    void schedule(double delta_ns, std::function<void()> fn);

    /// sim_send: the handler runs once the receiver has acknowledged every byte.
    void send(int src, int dst, uint64_t size, int tag, EventHandler handler, void* arg);

    /// sim_recv: the handler runs once `size` bytes from src with this tag
    /// have arrived. Receives on one (tag, src, dst) are satisfied in order.
    void recv(int src, int dst, uint64_t size, int tag, EventHandler handler, void* arg);

    /// Called by the protocol when the last byte of a flow arrives.
    void flow_received(flowid_t flow_id);

    /// Called by the protocol when every byte of a flow has been acknowledged.
    void flow_sent(flowid_t flow_id);

    /// One-line summary of the run so far, for the HTSim log.
    std::string summary() const;

  private:
    explicit HTSimSession(const HTSimConfig& config);
    ~HTSimSession();

    struct Flow {
        int src;
        int dst;
        uint64_t size;
        int tag;
        EventHandler handler;
        void* arg;
        bool received;
    };

    struct PendingRecv {
        uint64_t size;
        EventHandler handler;
        void* arg;
    };

    struct RecvQueue {
        uint64_t arrived = 0;  // bytes arrived and not yet claimed by a receive
        std::deque<PendingRecv> pending;
    };

    // (tag, (src, dst))
    typedef std::pair<int, std::pair<int, int>> MsgKey;

    /// Credit arrived bytes and complete every receive they now cover.
    void deliver(int src, int dst, int tag, uint64_t size);
    /// Complete the receives at the front of key's queue that are covered.
    void drain(const MsgKey& key, bool immediate);

    class Dispatcher;
    std::unique_ptr<Dispatcher> dispatcher;
    std::unique_ptr<HTSimProtocol> protocol;

    flowid_t next_flow_id = 0;
    std::unordered_map<flowid_t, Flow> flows;
    std::map<MsgKey, RecvQueue> recv_queues;

    static HTSimSession* session;
};

}  // namespace HTSim
