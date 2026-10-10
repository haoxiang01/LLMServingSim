/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimTopology.hh"

#include "config.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace HTSim {

simtime_picosec serialization_time(const uint64_t bytes, const linkspeed_bps speed) {
    return static_cast<simtime_picosec>(
        std::llround(static_cast<long double>(bytes) * 8.0L * 1e12L / static_cast<long double>(speed)));
}

HTSimTopology::HTSimTopology(const HTSimConfig& config, EventList& eventlist)
    : eventlist(eventlist), npus_count_per_dim(config.npus_count_per_dim) {
    const auto dims = npus_count_per_dim.size();
    if (dims == 0 || config.bandwidth_per_dim.size() != dims ||
        config.latency_per_dim.size() != dims) {
        throw std::invalid_argument("HTSim topology needs one bandwidth and latency per dimension");
    }
    npus_count = 1;
    for (std::size_t dim = 0; dim < dims; dim++) {
        const auto bw = config.bandwidth_per_dim[dim];
        const auto latency = config.latency_per_dim[dim];
        if (npus_count_per_dim[dim] <= 0 || !std::isfinite(bw) || bw <= 0 ||
            !std::isfinite(latency) || latency < 0) {
            throw std::invalid_argument("Invalid HTSim link dimension");
        }
        npus_count *= npus_count_per_dim[dim];
        // GiB/s, the unit the analytical backend reads (1 GB = 2^30 B there).
        speed_per_dim.push_back(static_cast<linkspeed_bps>(std::llround(bw * (1ull << 30) * 8)));
        latency_per_dim.push_back(timeFromNs(latency));
    }
    queue_bytes = config.queue_bytes == 0
        ? std::numeric_limits<mem_b>::max() / 2
        : static_cast<mem_b>(config.queue_bytes);
}

int HTSimTopology::link_dim(const int src, const int dst) const {
    // Same address translation as MultiDimTopology: dimension 0 innermost.
    auto s = src;
    auto d = dst;
    for (std::size_t dim = 0; dim < npus_count_per_dim.size(); dim++) {
        if (s % npus_count_per_dim[dim] != d % npus_count_per_dim[dim]) {
            return static_cast<int>(dim);
        }
        s /= npus_count_per_dim[dim];
        d /= npus_count_per_dim[dim];
    }
    throw std::invalid_argument("HTSim link between an NPU and itself");
}

const Route& HTSimTopology::route(const int src, const int dst) {
    assert(0 <= src && src < npus_count && 0 <= dst && dst < npus_count);
    auto it = links.find(key(src, dst));
    if (it == links.end()) {
        const auto dim = link_dim(src, dst);
        auto link = Link();
        link.entry = std::make_unique<Counter>("link_entry");
        link.queue = std::make_unique<LinkQueue>(speed_per_dim[dim], queue_bytes, eventlist);
        link.pipe = std::make_unique<Pipe>(latency_per_dim[dim], eventlist);
        link.exit = std::make_unique<Counter>("link_exit");
        link.route = std::make_unique<Route>();
        link.route->push_back(link.entry.get());
        link.route->push_back(link.queue.get());
        link.route->push_back(link.pipe.get());
        link.route->push_back(link.exit.get());
        it = links.emplace(key(src, dst), std::move(link)).first;
    }
    return *it->second.route;
}

linkspeed_bps HTSimTopology::speed(const int src, const int dst) const {
    return speed_per_dim[link_dim(src, dst)];
}

const HTSimTopology::Link& HTSimTopology::link(const int src, const int dst) const {
    const auto it = links.find(key(src, dst));
    assert(it != links.end());
    return it->second;
}

uint64_t HTSimTopology::entered(const int src, const int dst) const {
    return link(src, dst).entry->count;
}

uint64_t HTSimTopology::passed(const int src, const int dst) const {
    const auto& l = link(src, dst);
    // A drop happens on arrival at the queue, so it keeps the order too.
    return l.exit->count + static_cast<uint64_t>(l.queue->num_drops());
}

uint64_t HTSimTopology::packet_drops() const {
    auto drops = uint64_t(0);
    for (const auto& [key, link] : links) {
        drops += link.queue->num_drops();
    }
    return drops;
}

}  // namespace HTSim
