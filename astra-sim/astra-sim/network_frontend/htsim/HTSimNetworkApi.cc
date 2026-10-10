/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimNetworkApi.hh"
#include "HTSimSession.hh"
#include <cassert>

using namespace AstraSim;
using namespace HTSim;

void HTSimNetworkApi::set_topology(
    std::shared_ptr<NetworkAnalyticalCongestionUnaware::Topology> topology) noexcept {
    assert(topology != nullptr);
    HTSimNetworkApi::dims_count = topology->get_dims_count();
    HTSimNetworkApi::bandwidth_per_dim = topology->get_bandwidth_per_dim();
}

HTSimNetworkApi::HTSimNetworkApi(const int rank) noexcept : CommonNetworkApi(rank) {
    assert(rank >= 0);
}

timespec_t HTSimNetworkApi::sim_get_time() {
    return {NS, HTSimSession::instance().get_time_ns()};
}

void HTSimNetworkApi::sim_schedule(const timespec_t delta,
                                   void (*fun_ptr)(void*),
                                   void* const fun_arg) {
    assert(delta.time_res == NS);
    assert(fun_ptr != nullptr);
    HTSimSession::instance().schedule(delta.time_val, [fun_ptr, fun_arg] { fun_ptr(fun_arg); });
}

int HTSimNetworkApi::sim_send(void* const buffer,
                              const uint64_t count,
                              const int type,
                              const int dst,
                              const int tag,
                              sim_request* const request,
                              void (*msg_handler)(void*),
                              void* const fun_arg) {
    HTSimSession::instance().send(sim_comm_get_rank(), dst, count, tag, msg_handler, fun_arg);
    return 0;
}

int HTSimNetworkApi::sim_recv(void* const buffer,
                              const uint64_t count,
                              const int type,
                              const int src,
                              const int tag,
                              sim_request* const request,
                              void (*msg_handler)(void*),
                              void* const fun_arg) {
    HTSimSession::instance().recv(src, sim_comm_get_rank(), count, tag, msg_handler, fun_arg);
    return 0;
}
