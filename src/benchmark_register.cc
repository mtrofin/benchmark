// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "benchmark_register.h"

#ifndef BENCHMARK_OS_WINDOWS
#ifndef BENCHMARK_OS_FUCHSIA
#include <sys/resource.h>
#endif
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <thread>

#include "absl/flags/flag.h"
#include "benchmark/benchmark.h"
#include "benchmark_api_internal.h"
#include "check.h"
#include "complexity.h"
#include "internal_macros.h"
#include "log.h"
#include "mutex.h"
#include "re.h"
#include "statistics.h"
#include "string_util.h"
#include "timers.h"

namespace benchmark {

namespace {
// For non-dense Range, intermediate values are powers of kRangeMultiplier.
static const int kRangeMultiplier = 8;
// The size of a benchmark family determines is the number of inputs to repeat
// the benchmark on. If this is "large" then warn the user during configuration.
static const size_t kMaxFamilySize = 100;
}  // end namespace

namespace internal {

//=============================================================================//
//                         BenchmarkFamilies
//=============================================================================//

// Class for managing registered benchmarks.  Note that each registered
// benchmark identifies a family of related benchmarks to run.
class BenchmarkFamilies {
 public:
  static BenchmarkFamilies* GetInstance();

  // Registers a benchmark family and returns the index assigned to it.
  size_t AddBenchmark(std::unique_ptr<Benchmark> family);

  // Clear all registered benchmark families.
  void ClearBenchmarks();

  // Extract the list of benchmark instances that match the specified
  // regular expression.
  bool FindBenchmarks(std::string re,
                      std::vector<BenchmarkInstance>* benchmarks,
                      std::ostream* Err);

 private:
  BenchmarkFamilies() {}

  std::vector<std::unique_ptr<Benchmark>> families_;
  Mutex mutex_;
};

BenchmarkFamilies* BenchmarkFamilies::GetInstance() {
  static BenchmarkFamilies instance;
  return &instance;
}

size_t BenchmarkFamilies::AddBenchmark(std::unique_ptr<Benchmark> family) {
  MutexLock l(mutex_);
  size_t index = families_.size();
  families_.push_back(std::move(family));
  return index;
}

void BenchmarkFamilies::ClearBenchmarks() {
  MutexLock l(mutex_);
  families_.clear();
  families_.shrink_to_fit();
}

bool BenchmarkFamilies::FindBenchmarks(
    std::string spec, std::vector<BenchmarkInstance>* benchmarks,
    std::ostream* ErrStream) {
  CHECK(ErrStream);
  auto& Err = *ErrStream;
  // Make regular expression out of command-line flag
  std::string error_msg;
  Regex re;
  bool isNegativeFilter = false;
  if (spec[0] == '-') {
    spec.replace(0, 1, "");
    isNegativeFilter = true;
  }
  if (!re.Init(spec, &error_msg)) {
    Err << "Could not compile benchmark re: " << error_msg << std::endl;
    return false;
  }

  // Special list of thread counts to use when none are specified
  const std::vector<int> one_thread = {1};

  int next_family_index = 0;

  MutexLock l(mutex_);
  for (std::unique_ptr<Benchmark>& family : families_) {
    int family_index = next_family_index;
    int per_family_instance_index = 0;

    // Family was deleted or benchmark doesn't match
    if (!family) continue;

    if (family->ArgsCnt() == -1) {
      family->Args({});
    }
    const std::vector<int>* thread_counts =
        (family->thread_counts_.empty()
             ? &one_thread
             : &static_cast<const std::vector<int>&>(family->thread_counts_));
    const size_t family_size = family->args_.size() * thread_counts->size();
    // The benchmark will be run at least 'family_size' different inputs.
    // If 'family_size' is very large warn the user.
    if (family_size > kMaxFamilySize) {
      Err << "The number of inputs is very large. " << family->name_
          << " will be repeated at least " << family_size << " times.\n";
    }
    // reserve in the special case the regex ".", since we know the final
    // family size.
    if (spec == ".") benchmarks->reserve(benchmarks->size() + family_size);

    for (auto const& args : family->args_) {
      for (int num_threads : *thread_counts) {
        BenchmarkInstance instance(family.get(), family_index,
                                   per_family_instance_index, args,
                                   num_threads);

        const auto full_name = instance.name().str();
        if ((re.Match(full_name) && !isNegativeFilter) ||
            (!re.Match(full_name) && isNegativeFilter)) {
          benchmarks->push_back(std::move(instance));

          ++per_family_instance_index;

          // Only bump the next family index once we've estabilished that
          // at least one instance of this family will be run.
          if (next_family_index == family_index) ++next_family_index;
        }
      }
    }
  }
  return true;
}

Benchmark* RegisterBenchmarkInternal(Benchmark* bench) {
  std::unique_ptr<Benchmark> bench_ptr(bench);
  BenchmarkFamilies* families = BenchmarkFamilies::GetInstance();
  families->AddBenchmark(std::move(bench_ptr));
  return bench;
}

// FIXME: This function is a hack so that benchmark.cc can access
// `BenchmarkFamilies`
bool FindBenchmarksInternal(const std::string& re,
                            std::vector<BenchmarkInstance>* benchmarks,
                            std::ostream* Err) {
  return BenchmarkFamilies::GetInstance()->FindBenchmarks(re, benchmarks, Err);
}

//=============================================================================//
//                               Benchmark
//=============================================================================//

Benchmark::Benchmark(const char* name)
    : name_(name),
      aggregation_report_mode_(ARM_Unspecified),
      time_unit_(kNanosecond),
      range_multiplier_(kRangeMultiplier),
      min_time_(0),
      iterations_(0),
      repetitions_(0),
      measure_process_cpu_time_(false),
      use_real_time_(false),
      use_manual_time_(false),
      complexity_(oNone),
      complexity_lambda_(nullptr) {
  ComputeStatistics("mean", StatisticsMean);
  ComputeStatistics("median", StatisticsMedian);
  ComputeStatistics("stddev", StatisticsStdDev);
}

Benchmark::~Benchmark() {}

Benchmark* Benchmark::Name(const std::string& name) {
  SetName(name.c_str());
  return this;
}

Benchmark* Benchmark::Arg(int64_t x) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == 1);
  args_.push_back({x});
  return this;
}

Benchmark* Benchmark::Unit(TimeUnit unit) {
  time_unit_ = unit;
  return this;
}

Benchmark* Benchmark::Range(int64_t start, int64_t limit) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == 1);
  std::vector<int64_t> arglist;
  AddRange(&arglist, start, limit, range_multiplier_);

  for (int64_t i : arglist) {
    args_.push_back({i});
  }
  return this;
}

Benchmark* Benchmark::Ranges(
    const std::vector<std::pair<int64_t, int64_t>>& ranges) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == static_cast<int>(ranges.size()));
  std::vector<std::vector<int64_t>> arglists(ranges.size());
  for (std::size_t i = 0; i < ranges.size(); i++) {
    AddRange(&arglists[i], ranges[i].first, ranges[i].second,
             range_multiplier_);
  }

  ArgsProduct(arglists);

  return this;
}

Benchmark* Benchmark::ArgsProduct(
    const std::vector<std::vector<int64_t>>& arglists) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == static_cast<int>(arglists.size()));

  std::vector<std::size_t> indices(arglists.size());
  const std::size_t total = std::accumulate(
      std::begin(arglists), std::end(arglists), std::size_t{1},
      [](const std::size_t res, const std::vector<int64_t>& arglist) {
        return res * arglist.size();
      });
  std::vector<int64_t> args;
  args.reserve(arglists.size());
  for (std::size_t i = 0; i < total; i++) {
    for (std::size_t arg = 0; arg < arglists.size(); arg++) {
      args.push_back(arglists[arg][indices[arg]]);
    }
    args_.push_back(args);
    args.clear();

    std::size_t arg = 0;
    do {
      indices[arg] = (indices[arg] + 1) % arglists[arg].size();
    } while (indices[arg++] == 0 && arg < arglists.size());
  }

  return this;
}

Benchmark* Benchmark::ArgName(const std::string& name) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == 1);
  arg_names_ = {name};
  return this;
}

Benchmark* Benchmark::ArgNames(const std::vector<std::string>& names) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == static_cast<int>(names.size()));
  arg_names_ = names;
  return this;
}

Benchmark* Benchmark::DenseRange(int64_t start, int64_t limit, int step) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == 1);
  CHECK_LE(start, limit);
  for (int64_t arg = start; arg <= limit; arg += step) {
    args_.push_back({arg});
  }
  return this;
}

Benchmark* Benchmark::Args(const std::vector<int64_t>& args) {
  CHECK(ArgsCnt() == -1 || ArgsCnt() == static_cast<int>(args.size()));
  args_.push_back(args);
  return this;
}

Benchmark* Benchmark::Apply(void (*custom_arguments)(Benchmark* benchmark)) {
  custom_arguments(this);
  return this;
}

Benchmark* Benchmark::RangeMultiplier(int multiplier) {
  CHECK(multiplier > 1);
  range_multiplier_ = multiplier;
  return this;
}

Benchmark* Benchmark::MinTime(double t) {
  CHECK(t > 0.0);
  CHECK(iterations_ == 0);
  min_time_ = t;
  return this;
}

Benchmark* Benchmark::Iterations(IterationCount n) {
  CHECK(n > 0);
  CHECK(IsZero(min_time_));
  iterations_ = n;
  return this;
}

Benchmark* Benchmark::Repetitions(int n) {
  CHECK(n > 0);
  repetitions_ = n;
  return this;
}

Benchmark* Benchmark::ReportAggregatesOnly(bool value) {
  aggregation_report_mode_ = value ? ARM_ReportAggregatesOnly : ARM_Default;
  return this;
}

Benchmark* Benchmark::DisplayAggregatesOnly(bool value) {
  // If we were called, the report mode is no longer 'unspecified', in any case.
  aggregation_report_mode_ = static_cast<AggregationReportMode>(
      aggregation_report_mode_ | ARM_Default);

  if (value) {
    aggregation_report_mode_ = static_cast<AggregationReportMode>(
        aggregation_report_mode_ | ARM_DisplayReportAggregatesOnly);
  } else {
    aggregation_report_mode_ = static_cast<AggregationReportMode>(
        aggregation_report_mode_ & ~ARM_DisplayReportAggregatesOnly);
  }

  return this;
}

Benchmark* Benchmark::MeasureProcessCPUTime() {
  // Can be used together with UseRealTime() / UseManualTime().
  measure_process_cpu_time_ = true;
  return this;
}

Benchmark* Benchmark::UseRealTime() {
  CHECK(!use_manual_time_)
      << "Cannot set UseRealTime and UseManualTime simultaneously.";
  use_real_time_ = true;
  return this;
}

Benchmark* Benchmark::UseManualTime() {
  CHECK(!use_real_time_)
      << "Cannot set UseRealTime and UseManualTime simultaneously.";
  use_manual_time_ = true;
  return this;
}

Benchmark* Benchmark::Complexity(BigO complexity) {
  complexity_ = complexity;
  return this;
}

Benchmark* Benchmark::Complexity(BigOFunc* complexity) {
  complexity_lambda_ = complexity;
  complexity_ = oLambda;
  return this;
}

Benchmark* Benchmark::ComputeStatistics(std::string name,
                                        StatisticsFunc* statistics) {
  statistics_.emplace_back(name, statistics);
  return this;
}

Benchmark* Benchmark::Threads(int t) {
  CHECK_GT(t, 0);
  thread_counts_.push_back(t);
  return this;
}

Benchmark* Benchmark::ThreadRange(int min_threads, int max_threads) {
  CHECK_GT(min_threads, 0);
  CHECK_GE(max_threads, min_threads);

  AddRange(&thread_counts_, min_threads, max_threads, 2);
  return this;
}

Benchmark* Benchmark::DenseThreadRange(int min_threads, int max_threads,
                                       int stride) {
  CHECK_GT(min_threads, 0);
  CHECK_GE(max_threads, min_threads);
  CHECK_GE(stride, 1);

  for (auto i = min_threads; i < max_threads; i += stride) {
    thread_counts_.push_back(i);
  }
  thread_counts_.push_back(max_threads);
  return this;
}

Benchmark* Benchmark::ThreadPerCpu() {
  thread_counts_.push_back(CPUInfo::Get().num_cpus);
  return this;
}

void Benchmark::SetName(const char* name) { name_ = name; }

int Benchmark::ArgsCnt() const {
  if (args_.empty()) {
    if (arg_names_.empty()) return -1;
    return static_cast<int>(arg_names_.size());
  }
  return static_cast<int>(args_.front().size());
}

//=============================================================================//
//                            FunctionBenchmark
//=============================================================================//

void FunctionBenchmark::Run(State& st) { func_(st); }

}  // end namespace internal

void ClearRegisteredBenchmarks() {
  internal::BenchmarkFamilies::GetInstance()->ClearBenchmarks();
}

std::vector<int64_t> CreateRange(int64_t lo, int64_t hi, int multi) {
  std::vector<int64_t> args;
  internal::AddRange(&args, lo, hi, multi);
  return args;
}

std::vector<int64_t> CreateDenseRange(int64_t start, int64_t limit,
                                      int step) {
  CHECK_LE(start, limit);
  std::vector<int64_t> args;
  for (int64_t arg = start; arg <= limit; arg += step) {
    args.push_back(arg);
  }
  return args;
}

}  // end namespace benchmark
