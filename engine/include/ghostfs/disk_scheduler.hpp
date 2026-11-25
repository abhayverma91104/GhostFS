#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ghostfs {

enum class SchedulingAlgorithm {
  FCFS,
  SSTF,
  SCAN,
  CSCAN,
  LOOK,
  CLOOK,
};

enum class SweepDirection {
  Left,
  Right,
};

struct IoRequest {
  std::string request_id;
  uint32_t original_index = 0;
  uint32_t cylinder = 0;
  uint32_t logical_block = 0;
  bool has_logical_block = false;
  std::string op_type = "read";  // "read" | "write"
  uint64_t arrival_time = 0;
  std::string label;
};

struct SimulationStep {
  uint32_t step_index = 0;
  uint32_t from_cylinder = 0;
  uint32_t to_cylinder = 0;
  uint64_t movement = 0;
  std::string direction;   // "left" | "right" | "none"
  std::string step_type;   // "service" | "boundary_visit" | "circular_jump"
  bool is_service = true;
  std::string request_id;
  uint32_t original_index = 0;
  uint32_t logical_block = 0;
  std::string op_type;
  uint64_t cumulative_movement = 0;
};

struct SchedulerConfig {
  SchedulingAlgorithm algorithm = SchedulingAlgorithm::FCFS;
  uint32_t min_cylinder = 0;
  uint32_t max_cylinder = 199;
  uint32_t initial_head = 53;
  SweepDirection initial_direction = SweepDirection::Right;
  std::string scheduling_mode = "static";  // "static" | "dynamic"
  double seek_time_per_cylinder_ms = 0.0;  // 0 = disabled unless configured
  uint32_t benchmark_repetitions = 1;
};

struct SimulationResult {
  std::string algorithm;
  std::string scheduling_mode;
  uint32_t min_cylinder = 0;
  uint32_t max_cylinder = 199;
  uint32_t initial_head = 53;
  std::string initial_direction;
  uint32_t serviced_count = 0;
  uint64_t total_head_movement = 0;
  double average_movement = 0.0;
  uint64_t max_single_step_movement = 0;
  uint64_t min_single_step_movement = 0;
  uint32_t direction_changes = 0;
  uint64_t circular_repositioning_distance = 0;
  double estimated_seek_time_ms = 0.0;
  double median_exec_duration_us = 0.0;
  uint32_t benchmark_repetitions = 1;
  std::vector<IoRequest> original_requests;
  std::vector<uint32_t> service_sequence;
  std::vector<uint32_t> head_path;
  std::vector<SimulationStep> steps;
};

struct AlgorithmComparisonRow {
  SimulationResult result;
  double reduction_vs_fcfs_pct = 0.0;
  uint32_t rank_by_movement = 1;
};

struct MultiAlgorithmComparisonResult {
  uint32_t min_cylinder = 0;
  uint32_t max_cylinder = 199;
  uint32_t initial_head = 53;
  std::string initial_direction;
  uint32_t request_count = 0;
  uint64_t fcfs_total_movement = 0;
  std::vector<AlgorithmComparisonRow> algorithms;
};

SchedulingAlgorithm parse_scheduling_algorithm(const std::string& name);
std::string scheduling_algorithm_to_string(SchedulingAlgorithm alg);
SweepDirection parse_sweep_direction(const std::string& dir);
std::string sweep_direction_to_string(SweepDirection dir);

class DiskScheduler {
 public:
  static SimulationResult simulate(
      const SchedulerConfig& config,
      const std::vector<IoRequest>& requests);

  static MultiAlgorithmComparisonResult compare_all(
      const SchedulerConfig& base_config,
      const std::vector<IoRequest>& requests);
};

}  // namespace ghostfs
