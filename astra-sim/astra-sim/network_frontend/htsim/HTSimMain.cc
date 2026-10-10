/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "HTSimNetworkApi.hh"
#include "HTSimSession.hh"
#include "astra-sim/common/Logging.hh"
#include "common/CmdLineParser.hh"
#include <astra-network-analytical/common/NetworkParser.h>
#include <astra-network-analytical/congestion_unaware/Helper.h>
#include <memory_backend/analytical/AnalyticalMemory.hh>
#include <json/json.hpp>
#include <yaml-cpp/yaml.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AstraSim;
using namespace Analytical;
using namespace AstraSimAnalytical;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalCongestionUnaware;
using namespace HTSim;
using json = nlohmann::json;

namespace {

/// Swallows HTSim's console chatter when no --htsim-log is given.
class NullBuffer final : public std::streambuf {
  protected:
    int overflow(const int c) override {
        return traits_type::not_eof(c);
    }
    std::streamsize xsputn(const char*, const std::streamsize n) override {
        return n;
    }
};

std::string save_json_to_tmp(const json& j, const std::string& name) {
    const char* dir = "tmp__mem";
    if (::mkdir(dir, 0755) == -1 && errno != EEXIST) {
        std::perror("mkdir tmp__mem");
        std::exit(1);
    }
    // The pid keeps concurrent runs, which share this directory, apart.
    const auto path = std::string(dir) + "/" + std::to_string(::getpid()) + "_" + name + ".json";
    std::ofstream ofs(path);
    if (!ofs) {
        std::cerr << "Unable to write tmp file: " << path << "\n";
        std::exit(1);
    }
    ofs << j.dump(2);
    return path;
}

/// Same memory configuration handling as the analytical frontend.
std::vector<std::unique_ptr<AnalyticalMemory>> load_memory_levels(const std::string& memory_configuration) {
    json mem_json;
    std::ifstream ifs(memory_configuration);
    ifs >> mem_json;

    auto memory_levels = std::vector<std::unique_ptr<AnalyticalMemory>>();
    const bool is_single = mem_json.is_object() && mem_json.contains("memory-type") &&
                           mem_json.contains("mem-latency") && mem_json.contains("mem-bw");
    if (is_single) {
        std::cout << "Single Memory Configuration Detected" << std::endl;
        memory_levels.push_back(std::make_unique<AnalyticalMemory>(memory_configuration));
        return memory_levels;
    }
    const std::pair<const char*, const char*> levels[] = {
        {"local_mem", "LOCAL_MEMORY"}, {"remote_mem", "REMOTE_MEMORY"}, {"cxl_mem", "CXL_MEMORY"}};
    for (const auto& [key, location] : levels) {
        if (mem_json.contains(key) && mem_json[key].is_object()) {
            json j = mem_json[key];
            j["memory-location"] = location;
            const auto path = save_json_to_tmp(j, key);
            memory_levels.push_back(std::make_unique<AnalyticalMemory>(path));
            std::remove(path.c_str());
        }
    }
    ::rmdir("tmp__mem");  // fails harmlessly while another run still uses it
    return memory_levels;
}

/// Physical links of the run, read from the analytical network configuration.
HTSimConfig load_link_config(const std::string& network_configuration, const NetworkParser& parser) {
    if (YAML::LoadFile(network_configuration)["collective_networks"]) {
        throw std::invalid_argument(
            "collective_networks (collective_links) requires the analytical backend");
    }
    for (const auto topology : parser.get_topologies_per_dim()) {
        if (topology != TopologyBuildingBlock::FullyConnected) {
            throw std::invalid_argument("The HTSim backend models FullyConnected dimensions only");
        }
    }
    auto config = HTSimConfig();
    config.npus_count_per_dim = parser.get_npus_counts_per_dim();
    for (const auto bw : parser.get_bandwidths_per_dim()) {
        config.bandwidth_per_dim.push_back(bw);
    }
    for (const auto latency : parser.get_latencies_per_dim()) {
        config.latency_per_dim.push_back(latency);
    }
    return config;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Parse command line arguments
    auto cmd_line_parser = CmdLineParser(argv[0]);
    cmd_line_parser.get_options().add_options()(
        "htsim-transport", "tcp, or raw: packets at full link bandwidth without congestion control",
        cxxopts::value<std::string>()->default_value("tcp"))(
        "htsim-queue-bytes", "Output buffer of every link in bytes; 0 = unbounded (lossless)",
        cxxopts::value<uint64_t>()->default_value("0"))(
        "htsim-mtu", "Packet size in bytes",
        cxxopts::value<int>()->default_value("1500"))(
        "htsim-initial-cwnd", "TCP congestion window at the start of a message, in packets",
        cxxopts::value<uint32_t>()->default_value("10"))(
        "htsim-min-rto-us", "Minimum TCP retransmission timeout in microseconds",
        cxxopts::value<double>()->default_value("250000"))(
        "htsim-initial-rto-us", "TCP retransmission timeout before the first RTT sample, in microseconds",
        cxxopts::value<double>()->default_value("3000000"))(
        "htsim-tcp-handshake", "Open every message with a SYN round trip",
        cxxopts::value<bool>()->default_value("false"))(
        "htsim-log", "File for HTSim's console output (default: discarded)",
        cxxopts::value<std::string>()->default_value(""));
    cmd_line_parser.parse(argc, argv);

    const auto workload_configuration = cmd_line_parser.get<std::string>("workload-configuration");
    const auto comm_group_configuration =
        cmd_line_parser.get<std::string>("comm-group-configuration");
    const auto system_configuration = cmd_line_parser.get<std::string>("system-configuration");
    const auto memory_configuration = cmd_line_parser.get<std::string>("memory-configuration");
    const auto network_configuration = cmd_line_parser.get<std::string>("network-configuration");
    const auto logging_configuration = cmd_line_parser.get<std::string>("logging-configuration");
    const auto num_queues_per_dim = cmd_line_parser.get<int>("num-queues-per-dim");
    const auto comm_scale = cmd_line_parser.get<double>("comm-scale");
    const auto injection_scale = cmd_line_parser.get<double>("injection-scale");
    const auto rendezvous_protocol = cmd_line_parser.get<bool>("rendezvous-protocol");
    auto start_npu_ids = cmd_line_parser.get<std::vector<int>>("start-npu-ids");
    auto end_npu_ids = cmd_line_parser.get<std::vector<int>>("end-npu-ids");
    const auto htsim_log = cmd_line_parser.get<std::string>("htsim-log");

    // clear vector if default value is used
    if (start_npu_ids.size() == 1 && start_npu_ids[0] == -1) {
        start_npu_ids.clear();
    }
    if (end_npu_ids.size() == 1 && end_npu_ids[0] == -1) {
        end_npu_ids.clear();
    }

    AstraSim::LoggerFactory::init(logging_configuration);

    // Generate topology
    const auto network_parser = NetworkParser(network_configuration);
    const auto topology = construct_topology(network_parser);
    const auto npus_count = topology->get_npus_count();
    const auto npus_count_per_dim = topology->get_npus_count_per_dim();
    const auto dims_count = topology->get_dims_count();
    HTSimNetworkApi::set_topology(topology);

    auto htsim_config = load_link_config(network_configuration, network_parser);
    const auto transport = cmd_line_parser.get<std::string>("htsim-transport");
    if (transport == "tcp") {
        htsim_config.transport = HTSimTransport::Tcp;
    } else if (transport == "raw") {
        htsim_config.transport = HTSimTransport::Raw;
    } else {
        std::cerr << "Unknown --htsim-transport: " << transport << " (tcp or raw)" << std::endl;
        return 1;
    }
    htsim_config.queue_bytes = cmd_line_parser.get<uint64_t>("htsim-queue-bytes");
    htsim_config.mtu_bytes = cmd_line_parser.get<int>("htsim-mtu");
    htsim_config.initial_cwnd_packets = cmd_line_parser.get<uint32_t>("htsim-initial-cwnd");
    htsim_config.min_rto_us = cmd_line_parser.get<double>("htsim-min-rto-us");
    htsim_config.initial_rto_us = cmd_line_parser.get<double>("htsim-initial-rto-us");
    htsim_config.tcp_handshake = cmd_line_parser.get<bool>("htsim-tcp-handshake");

    // stdout is the frontend's protocol channel and nobody reads stderr, so
    // HTSim's own std::cout / std::cerr chatter must go elsewhere. ASTRA-sim's
    // reports and prompts are written by spdlog to the stdout FILE*, not
    // through std::cout, and are unaffected.
    auto* const console_out = std::cout.rdbuf();
    auto* const console_err = std::cerr.rdbuf();
    NullBuffer null_buffer;
    std::ofstream htsim_log_file;
    if (!htsim_log.empty()) {
        htsim_log_file.open(htsim_log);
        if (!htsim_log_file) {
            std::cerr << "Unable to open HTSim log: " << htsim_log << std::endl;
            return 1;
        }
    }
    auto* const chatter = htsim_log.empty() ? static_cast<std::streambuf*>(&null_buffer)
                                            : htsim_log_file.rdbuf();
    auto restore_console = [&] {
        std::cout.flush();
        std::cout.rdbuf(console_out);
        std::cerr.rdbuf(console_err);
    };

    auto memory_levels = load_memory_levels(memory_configuration);
    auto memory_apis = std::vector<AstraMemoryAPI*>();
    for (auto& mem_api : memory_levels) {
        memory_apis.push_back(mem_api.get());
    }

    std::cout.rdbuf(chatter);
    std::cerr.rdbuf(chatter);
    auto& session = HTSimSession::init(htsim_config);

    // Create ASTRA-sim related resources
    auto network_apis = std::vector<std::unique_ptr<HTSimNetworkApi>>();
    auto systems = std::vector<Sys*>();
    auto queues_per_dim = std::vector<int>(dims_count, num_queues_per_dim);
    for (int i = 0; i < npus_count; i++) {
        auto network_api = std::make_unique<HTSimNetworkApi>(i);
        auto* const system =
            new Sys(i, workload_configuration, comm_group_configuration, system_configuration,
                    memory_apis, network_api.get(), npus_count_per_dim, queues_per_dim,
                    injection_scale, comm_scale, rendezvous_protocol);
        network_apis.push_back(std::move(network_api));
        systems.push_back(system);
    }

    // Systems handled by each controller NPU, as in the analytical frontend.
    std::vector<std::vector<Sys*>> managed_systems(start_npu_ids.size());
    for (std::size_t idx = 0; idx < start_npu_ids.size(); ++idx) {
        const int npu_id = start_npu_ids[idx];
        const int upper_bound_id =
            (idx + 1 < start_npu_ids.size()) ? start_npu_ids[idx + 1] : npus_count;
        for (int sid = npu_id + 1; sid < upper_bound_id; ++sid) {
            if (std::find(end_npu_ids.begin(), end_npu_ids.end(), sid) != end_npu_ids.end()) {
                continue;
            }
            managed_systems[idx].push_back(systems[sid]);
        }
    }

    for (int i = 0; i < npus_count; i++) {
        systems[i]->workload->fire();
    }

    // ---- Frontend handshake ---------------------------------------------
    // The loop below is the analytical frontend's (congestion_unaware/
    // main.cc), including the idle-NPU re-ask suppression and its liveness
    // guarantees; see the comments there. Only the clock differs: one step
    // runs every HTSim event of the next timestamp, and an empty event list
    // advances in the same 1 ms quanta as an empty analytical EventQueue.
    // HTSim keeps no periodic event while nothing is in flight, so an empty
    // event list means the cluster is idle exactly as it does there.
    constexpr long long kSuppressionBackstop = 1000;
    constexpr double kIdleQuantumNs = 1000000;
    std::vector<long long> pass_gen(npus_count, -1);      // -1 = askable
    std::vector<long long> pass_deadline(npus_count, 0);  //  0 = no deadline
    std::vector<long long> last_reported_iter(npus_count, -1);
    long long state_gen = 0;
    long long unasked_rounds = 0;

    auto now_ns = [&] { return static_cast<long long>(session.get_time_ns()); };
    auto askable = [&](const int npu_id) {
        if (pass_gen[npu_id] < 0 || pass_gen[npu_id] != state_gen) {
            return true;
        }
        return pass_deadline[npu_id] > 0 && now_ns() >= pass_deadline[npu_id];
    };
    auto clear_suppression = [&] { std::fill(pass_gen.begin(), pass_gen.end(), -1); };

    bool exit = false;
    bool asked_any = false;
    // Report a finished iteration and act on the frontend's answer.
    auto serve = [&](const int npu_id, const std::vector<Sys*>& managed) {
        auto* const workload = systems[npu_id]->workload;
        if (workload->is_sleep || !workload->is_finished || !askable(npu_id)) {
            return;
        }
        asked_any = true;
        if (static_cast<long long>(workload->iteration) != last_reported_iter[npu_id]) {
            last_reported_iter[npu_id] = workload->iteration;
            ++state_gen;
        }
        workload->report();
        AstraSim::LoggerFactory::get_logger("workload")->info("Waiting");

        std::string new_filename;
        std::getline(std::cin, new_filename);

        if (new_filename.rfind("pass", 0) == 0) {
            const long long deadline = (new_filename.size() > 5)
                ? std::strtoll(new_filename.c_str() + 5, nullptr, 10)
                : 0;
            if (deadline < 0) {
                // "pass -1": the frontend changed scheduler state.
                ++state_gen;
                pass_gen[npu_id] = state_gen;
                pass_deadline[npu_id] = 0;
            } else {
                pass_gen[npu_id] = state_gen;
                pass_deadline[npu_id] = deadline;
            }
            return;
        }

        ++state_gen;
        pass_gen[npu_id] = -1;

        if (new_filename == "exit") {
            exit = true;
        } else if (new_filename == "done") {
            workload->is_sleep = true;
        } else {
            workload->add_workload(new_filename, managed);
        }
    };

    const auto no_managed = std::vector<Sys*>();
    while (!exit) {
        asked_any = false;
        if (session.has_pending_events()) {
            session.proceed();
        } else {
            session.advance_idle(kIdleQuantumNs);
            clear_suppression();
        }

        for (std::size_t idx = 0; idx < end_npu_ids.size() && !exit; ++idx) {
            serve(end_npu_ids[idx], no_managed);
        }
        for (std::size_t idx = 0; idx < start_npu_ids.size() && !exit; ++idx) {
            serve(start_npu_ids[idx], managed_systems[idx]);
        }
        if (exit) {
            break;
        }

        if (asked_any) {
            unasked_rounds = 0;
        } else if (++unasked_rounds >= kSuppressionBackstop) {
            unasked_rounds = 0;
            clear_suppression();
        }
    }

    std::cout << session.summary() << std::endl;

    // "Checking Non-Exited Systems ..." and the two verdicts are protocol:
    // Controller.read_wait and Controller.check_end match them exactly.
    restore_console();
    std::cout << "Checking Non-Exited Systems ..." << std::endl;
    bool done = true;
    for (int npu_id = 0; npu_id < npus_count; npu_id++) {
        if (systems[npu_id]->workload->is_finished == false) {
            std::cout << "sys[" << npu_id << "] " << std::endl;
            systems[npu_id]->workload->et_feeder->printGraph();
            done = false;
        }
    }
    std::cout << "---------------------------" << std::endl;
    std::cout << (done ? "All Request Has Been Exited" : "ERROR: Some Requests Remain") << std::endl;
    std::cout << "---------------------------" << std::endl;

    AstraSim::LoggerFactory::shutdown();
    return 0;
}
