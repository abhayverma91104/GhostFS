#include "ghostfs/disk_scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ghostfs {
namespace {

std::string to_lower_ascii(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

uint64_t abs_diff_u32(uint32_t a, uint32_t b) {
  return (a >= b) ? static_cast<uint64_t>(a - b) : static_cast<uint64_t>(b - a);
}

void validate_config_and_requests(
    const SchedulerConfig& cfg,
    const std::vector<IoRequest>& reqs) {
  if (cfg.min_cylinder > cfg.max_cylinder) {
    throw std::runtime_error("ERR_INVALID_GEOMETRY: min_cylinder must be <= max_cylinder");
  }
  if (cfg.initial_head < cfg.min_cylinder || cfg.initial_head > cfg.max_cylinder) {
    throw std::runtime_error("ERR_INVALID_HEAD: initial_head is outside [min_cylinder, max_cylinder]");
  }
  if (cfg.scheduling_mode != "static" && cfg.scheduling_mode != "dynamic") {
    throw std::runtime_error("ERR_INVALID_ARGUMENT: scheduling_mode must be 'static' or 'dynamic'");
  }
  if (reqs.size() > 10000U) {
    throw std::runtime_error("ERR_WORKLOAD_TOO_LARGE: Request queue exceeds maximum bound of 10000");
  }
  for (const auto& r : reqs) {
    if (r.cylinder < cfg.min_cylinder || r.cylinder > cfg.max_cylinder) {
      throw std::runtime_error(
          "ERR_OUT_OF_RANGE_REQUEST: Request cylinder " + std::to_string(r.cylinder) +
          " outside [" + std::to_string(cfg.min_cylinder) + ", " +
          std::to_string(cfg.max_cylinder) + "]");
    }
  }
}

struct PendingItem {
  IoRequest req;
  bool serviced = false;
};

SimulationResult run_once(
    const SchedulerConfig& cfg,
    const std::vector<IoRequest>& input_reqs) {
  SimulationResult res{};
  res.algorithm = scheduling_algorithm_to_string(cfg.algorithm);
  res.scheduling_mode = cfg.scheduling_mode;
  res.min_cylinder = cfg.min_cylinder;
  res.max_cylinder = cfg.max_cylinder;
  res.initial_head = cfg.initial_head;
  res.initial_direction = sweep_direction_to_string(cfg.initial_direction);
  res.benchmark_repetitions = std::max<uint32_t>(1U, cfg.benchmark_repetitions);

  res.original_requests.reserve(input_reqs.size());
  std::vector<PendingItem> items;
  items.reserve(input_reqs.size());
  for (uint32_t i = 0; i < input_reqs.size(); ++i) {
    IoRequest copy = input_reqs[i];
    copy.original_index = i;
    if (copy.request_id.empty()) {
      copy.request_id = "req_" + std::to_string(i + 1U);
    }
    res.original_requests.push_back(copy);
    items.push_back(PendingItem{copy, false});
  }

  res.head_path.push_back(cfg.initial_head);
  if (items.empty()) {
    return res;
  }

  uint32_t head = cfg.initial_head;
  SweepDirection cur_dir = cfg.initial_direction;
  std::string last_move_dir = "none";
  uint64_t cumulative = 0;
  uint64_t min_step = std::numeric_limits<uint64_t>::max();
  uint64_t max_step = 0;

  auto record_move = [&](uint32_t target, const std::string& step_type, PendingItem* item) {
    const uint64_t dist = abs_diff_u32(head, target);
    std::string move_dir = "none";
    if (target > head) move_dir = "right";
    else if (target < head) move_dir = "left";

    if (move_dir != "none") {
      if (last_move_dir != "none" && move_dir != last_move_dir) {
        res.direction_changes++;
      }
      last_move_dir = move_dir;
    }

    cumulative += dist;
    if (dist > max_step) max_step = dist;
    if (dist < min_step) min_step = dist;
    if (step_type == "circular_jump") {
      res.circular_repositioning_distance += dist;
    }

    SimulationStep step{};
    step.step_index = static_cast<uint32_t>(res.steps.size() + 1U);
    step.from_cylinder = head;
    step.to_cylinder = target;
    step.movement = dist;
    step.direction = move_dir;
    step.step_type = step_type;
    step.is_service = (item != nullptr);
    step.cumulative_movement = cumulative;
    if (item != nullptr) {
      step.request_id = item->req.request_id;
      step.original_index = item->req.original_index;
      step.logical_block = item->req.logical_block;
      step.op_type = item->req.op_type;
      res.service_sequence.push_back(item->req.cylinder);
      res.serviced_count++;
      item->serviced = true;
    }

    res.steps.push_back(std::move(step));
    head = target;
    res.head_path.push_back(head);
  };

  const size_t n = items.size();

  if (cfg.scheduling_mode == "dynamic") {
    // Dynamic arrival-time scheduling: only requests with arrival_time <= cur_time are eligible
    uint64_t cur_time = 0;
    size_t remaining = n;
    while (remaining > 0) {
      std::vector<PendingItem*> eligible;
      uint64_t next_arrival = std::numeric_limits<uint64_t>::max();
      for (auto& it : items) {
        if (it.serviced) continue;
        if (it.req.arrival_time <= cur_time) {
          eligible.push_back(&it);
        } else if (it.req.arrival_time < next_arrival) {
          next_arrival = it.req.arrival_time;
        }
      }
      if (eligible.empty()) {
        cur_time = next_arrival;
        continue;
      }

      PendingItem* chosen = nullptr;
      if (cfg.algorithm == SchedulingAlgorithm::FCFS) {
        chosen = eligible.front();
      } else if (cfg.algorithm == SchedulingAlgorithm::SSTF) {
        uint64_t best_d = std::numeric_limits<uint64_t>::max();
        for (auto* cand : eligible) {
          const uint64_t d = abs_diff_u32(head, cand->req.cylinder);
          if (d < best_d ||
              (d == best_d && chosen != nullptr && cand->req.original_index < chosen->req.original_index)) {
            best_d = d;
            chosen = cand;
          }
        }
      } else {
        // Directional selection among eligible requests (SCAN/C-SCAN/LOOK/C-LOOK)
        auto pick_dir = [&](SweepDirection d) -> PendingItem* {
          PendingItem* best = nullptr;
          for (auto* cand : eligible) {
            if (d == SweepDirection::Right && cand->req.cylinder >= head) {
              if (!best || cand->req.cylinder < best->req.cylinder) best = cand;
            } else if (d == SweepDirection::Left && cand->req.cylinder <= head) {
              if (!best || cand->req.cylinder > best->req.cylinder) best = cand;
            }
          }
          return best;
        };
        chosen = pick_dir(cur_dir);
        if (!chosen) {
          if (cfg.algorithm == SchedulingAlgorithm::SCAN) {
            const uint32_t b = (cur_dir == SweepDirection::Right) ? cfg.max_cylinder : cfg.min_cylinder;
            if (head != b) record_move(b, "boundary_visit", nullptr);
            cur_dir = (cur_dir == SweepDirection::Right) ? SweepDirection::Left : SweepDirection::Right;
            chosen = pick_dir(cur_dir);
          } else if (cfg.algorithm == SchedulingAlgorithm::LOOK) {
            cur_dir = (cur_dir == SweepDirection::Right) ? SweepDirection::Left : SweepDirection::Right;
            chosen = pick_dir(cur_dir);
          } else if (cfg.algorithm == SchedulingAlgorithm::CSCAN) {
            const uint32_t b_end = (cur_dir == SweepDirection::Right) ? cfg.max_cylinder : cfg.min_cylinder;
            const uint32_t b_start = (cur_dir == SweepDirection::Right) ? cfg.min_cylinder : cfg.max_cylinder;
            if (head != b_end) record_move(b_end, "boundary_visit", nullptr);
            record_move(b_start, "circular_jump", nullptr);
            chosen = pick_dir(cur_dir);
          } else {
            // C-LOOK: jump to extreme in opposite side
            PendingItem* extreme = nullptr;
            for (auto* cand : eligible) {
              if (cur_dir == SweepDirection::Right) {
                if (!extreme || cand->req.cylinder < extreme->req.cylinder) extreme = cand;
              } else {
                if (!extreme || cand->req.cylinder > extreme->req.cylinder) extreme = cand;
              }
            }
            if (extreme) {
              record_move(extreme->req.cylinder, "circular_jump", extreme);
              remaining--;
              cur_time += std::max<uint64_t>(1ULL, res.steps.back().movement);
              continue;
            }
          }
        }
      }
      if (chosen) {
        const uint64_t move = abs_diff_u32(head, chosen->req.cylinder);
        record_move(chosen->req.cylinder, "service", chosen);
        remaining--;
        cur_time += std::max<uint64_t>(1ULL, move);
      }
    }
  } else {
    // Static baseline scheduling
    if (cfg.algorithm == SchedulingAlgorithm::FCFS) {
      for (auto& it : items) {
        record_move(it.req.cylinder, "service", &it);
      }
    } else if (cfg.algorithm == SchedulingAlgorithm::SSTF) {
      for (size_t done = 0; done < n; ++done) {
        PendingItem* best = nullptr;
        uint64_t best_d = std::numeric_limits<uint64_t>::max();
        for (auto& it : items) {
          if (it.serviced) continue;
          const uint64_t d = abs_diff_u32(head, it.req.cylinder);
          if (d < best_d) {
            best_d = d;
            best = &it;
          } else if (d == best_d && best != nullptr) {
            // Deterministic tie-breaker: prefer current sweep direction, then lower cylinder, then queue index
            const bool cand_in_dir = (cur_dir == SweepDirection::Right) ? (it.req.cylinder >= head) : (it.req.cylinder <= head);
            const bool best_in_dir = (cur_dir == SweepDirection::Right) ? (best->req.cylinder >= head) : (best->req.cylinder <= head);
            if (cand_in_dir != best_in_dir) {
              if (cand_in_dir) best = &it;
            } else if (it.req.cylinder != best->req.cylinder) {
              if (it.req.cylinder < best->req.cylinder) best = &it;
            } else if (it.req.original_index < best->req.original_index) {
              best = &it;
            }
          }
        }
        if (best->req.cylinder > head) cur_dir = SweepDirection::Right;
        else if (best->req.cylinder < head) cur_dir = SweepDirection::Left;
        record_move(best->req.cylinder, "service", best);
      }
    } else {
      // Partition into left (< head), at_head (== head), and right (> head)
      std::vector<PendingItem*> right_side;
      std::vector<PendingItem*> left_side;
      std::vector<PendingItem*> at_head;
      for (auto& it : items) {
        if (it.req.cylinder > head) right_side.push_back(&it);
        else if (it.req.cylinder < head) left_side.push_back(&it);
        else at_head.push_back(&it);
      }

      auto asc_cmp = [](const PendingItem* a, const PendingItem* b) {
        if (a->req.cylinder != b->req.cylinder) return a->req.cylinder < b->req.cylinder;
        return a->req.original_index < b->req.original_index;
      };
      auto desc_cmp = [](const PendingItem* a, const PendingItem* b) {
        if (a->req.cylinder != b->req.cylinder) return a->req.cylinder > b->req.cylinder;
        return a->req.original_index < b->req.original_index;
      };

      // Service requests already at initial head first
      for (auto* it : at_head) {
        record_move(it->req.cylinder, "service", it);
      }

      if (cfg.algorithm == SchedulingAlgorithm::SCAN || cfg.algorithm == SchedulingAlgorithm::LOOK) {
        const bool is_scan = (cfg.algorithm == SchedulingAlgorithm::SCAN);
        if (cfg.initial_direction == SweepDirection::Right) {
          std::sort(right_side.begin(), right_side.end(), asc_cmp);
          std::sort(left_side.begin(), left_side.end(), desc_cmp);
          for (auto* it : right_side) record_move(it->req.cylinder, "service", it);
          if (!left_side.empty()) {
            if (is_scan && head != cfg.max_cylinder) {
              record_move(cfg.max_cylinder, "boundary_visit", nullptr);
            }
            for (auto* it : left_side) record_move(it->req.cylinder, "service", it);
          }
        } else {
          std::sort(left_side.begin(), left_side.end(), desc_cmp);
          std::sort(right_side.begin(), right_side.end(), asc_cmp);
          for (auto* it : left_side) record_move(it->req.cylinder, "service", it);
          if (!right_side.empty()) {
            if (is_scan && head != cfg.min_cylinder) {
              record_move(cfg.min_cylinder, "boundary_visit", nullptr);
            }
            for (auto* it : right_side) record_move(it->req.cylinder, "service", it);
          }
        }
      } else {
        // C-SCAN or C-LOOK
        const bool is_cscan = (cfg.algorithm == SchedulingAlgorithm::CSCAN);
        if (cfg.initial_direction == SweepDirection::Right) {
          std::sort(right_side.begin(), right_side.end(), asc_cmp);
          std::sort(left_side.begin(), left_side.end(), asc_cmp);
          for (auto* it : right_side) record_move(it->req.cylinder, "service", it);
          if (!left_side.empty()) {
            if (is_cscan) {
              if (head != cfg.max_cylinder) record_move(cfg.max_cylinder, "boundary_visit", nullptr);
              record_move(cfg.min_cylinder, "circular_jump", nullptr);
              for (auto* it : left_side) record_move(it->req.cylinder, "service", it);
            } else {
              // C-LOOK: circular jump directly to lowest pending request, servicing it
              record_move(left_side.front()->req.cylinder, "circular_jump", left_side.front());
              for (size_t k = 1; k < left_side.size(); ++k) {
                record_move(left_side[k]->req.cylinder, "service", left_side[k]);
              }
            }
          }
        } else {
          std::sort(left_side.begin(), left_side.end(), desc_cmp);
          std::sort(right_side.begin(), right_side.end(), desc_cmp);
          for (auto* it : left_side) record_move(it->req.cylinder, "service", it);
          if (!right_side.empty()) {
            if (is_cscan) {
              if (head != cfg.min_cylinder) record_move(cfg.min_cylinder, "boundary_visit", nullptr);
              record_move(cfg.max_cylinder, "circular_jump", nullptr);
              for (auto* it : right_side) record_move(it->req.cylinder, "service", it);
            } else {
              record_move(right_side.front()->req.cylinder, "circular_jump", right_side.front());
              for (size_t k = 1; k < right_side.size(); ++k) {
                record_move(right_side[k]->req.cylinder, "service", right_side[k]);
              }
            }
          }
        }
      }
    }
  }

  res.total_head_movement = cumulative;
  res.average_movement = (res.serviced_count > 0U)
                             ? static_cast<double>(res.total_head_movement) / static_cast<double>(res.serviced_count)
                             : 0.0;
  res.max_single_step_movement = max_step;
  res.min_single_step_movement = (min_step == std::numeric_limits<uint64_t>::max()) ? 0ULL : min_step;
  if (cfg.seek_time_per_cylinder_ms > 0.0) {
    res.estimated_seek_time_ms = static_cast<double>(res.total_head_movement) * cfg.seek_time_per_cylinder_ms;
  }
  return res;
}

}  // namespace

SchedulingAlgorithm parse_scheduling_algorithm(const std::string& name) {
  const std::string low = to_lower_ascii(name);
  if (low == "fcfs") return SchedulingAlgorithm::FCFS;
  if (low == "sstf") return SchedulingAlgorithm::SSTF;
  if (low == "scan") return SchedulingAlgorithm::SCAN;
  if (low == "c-scan" || low == "cscan" || low == "c_scan") return SchedulingAlgorithm::CSCAN;
  if (low == "look") return SchedulingAlgorithm::LOOK;
  if (low == "c-look" || low == "clook" || low == "c_look") return SchedulingAlgorithm::CLOOK;
  throw std::runtime_error("ERR_INVALID_ARGUMENT: Unknown scheduling algorithm: " + name);
}

std::string scheduling_algorithm_to_string(SchedulingAlgorithm alg) {
  switch (alg) {
    case SchedulingAlgorithm::FCFS: return "FCFS";
    case SchedulingAlgorithm::SSTF: return "SSTF";
    case SchedulingAlgorithm::SCAN: return "SCAN";
    case SchedulingAlgorithm::CSCAN: return "C-SCAN";
    case SchedulingAlgorithm::LOOK: return "LOOK";
    case SchedulingAlgorithm::CLOOK: return "C-LOOK";
  }
  return "FCFS";
}

SweepDirection parse_sweep_direction(const std::string& dir) {
  const std::string low = to_lower_ascii(dir);
  if (low == "left") return SweepDirection::Left;
  if (low == "right") return SweepDirection::Right;
  throw std::runtime_error("ERR_INVALID_ARGUMENT: Direction must be 'left' or 'right'");
}

std::string sweep_direction_to_string(SweepDirection dir) {
  return (dir == SweepDirection::Left) ? "left" : "right";
}

SimulationResult DiskScheduler::simulate(
    const SchedulerConfig& config,
    const std::vector<IoRequest>& requests) {
  validate_config_and_requests(config, requests);

  const uint32_t reps = std::max<uint32_t>(1U, std::min<uint32_t>(100U, config.benchmark_repetitions));
  std::vector<double> durations_us;
  durations_us.reserve(reps);
  SimulationResult final_res{};

  for (uint32_t r = 0; r < reps; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    SimulationResult cur = run_once(config, requests);
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    durations_us.push_back(us);
    if (r == 0) {
      final_res = std::move(cur);
    }
  }
  std::sort(durations_us.begin(), durations_us.end());
  final_res.median_exec_duration_us = durations_us[durations_us.size() / 2U];
  final_res.benchmark_repetitions = reps;
  return final_res;
}

MultiAlgorithmComparisonResult DiskScheduler::compare_all(
    const SchedulerConfig& base_config,
    const std::vector<IoRequest>& requests) {
  validate_config_and_requests(base_config, requests);

  const std::vector<SchedulingAlgorithm> algs = {
      SchedulingAlgorithm::FCFS,
      SchedulingAlgorithm::SSTF,
      SchedulingAlgorithm::SCAN,
      SchedulingAlgorithm::CSCAN,
      SchedulingAlgorithm::LOOK,
      SchedulingAlgorithm::CLOOK,
  };

  MultiAlgorithmComparisonResult out{};
  out.min_cylinder = base_config.min_cylinder;
  out.max_cylinder = base_config.max_cylinder;
  out.initial_head = base_config.initial_head;
  out.initial_direction = sweep_direction_to_string(base_config.initial_direction);
  out.request_count = static_cast<uint32_t>(requests.size());

  for (const auto alg : algs) {
    SchedulerConfig cfg = base_config;
    cfg.algorithm = alg;
    AlgorithmComparisonRow row{};
    row.result = simulate(cfg, requests);
    if (alg == SchedulingAlgorithm::FCFS) {
      out.fcfs_total_movement = row.result.total_head_movement;
    }
    out.algorithms.push_back(std::move(row));
  }

  for (auto& row : out.algorithms) {
    if (out.fcfs_total_movement > 0ULL) {
      row.reduction_vs_fcfs_pct =
          (static_cast<double>(out.fcfs_total_movement) -
           static_cast<double>(row.result.total_head_movement)) /
          static_cast<double>(out.fcfs_total_movement) * 100.0;
    } else {
      row.reduction_vs_fcfs_pct = 0.0;
    }
  }

  // Compute rank by total_head_movement (1 = lowest)
  std::vector<size_t> order(out.algorithms.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    return out.algorithms[a].result.total_head_movement < out.algorithms[b].result.total_head_movement;
  });
  for (size_t r = 0; r < order.size(); ++r) {
    if (r > 0 && out.algorithms[order[r]].result.total_head_movement ==
                     out.algorithms[order[r - 1]].result.total_head_movement) {
      out.algorithms[order[r]].rank_by_movement = out.algorithms[order[r - 1]].rank_by_movement;
    } else {
      out.algorithms[order[r]].rank_by_movement = static_cast<uint32_t>(r + 1U);
    }
  }

  return out;
}

}  // namespace ghostfs
