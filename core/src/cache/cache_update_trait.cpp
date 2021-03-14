#include <cache/cache_update_trait.hpp>

#include <boost/filesystem/operations.hpp>

#include <logging/log.hpp>
#include <storages/secdist/component.hpp>
#include <testsuite/cache_control.hpp>
#include <tracing/tracer.hpp>
#include <utils/assert.hpp>
#include <utils/async.hpp>
#include <utils/atomic.hpp>
#include <utils/statistics/metadata.hpp>

#include <cache/dump/dump_manager.hpp>
#include <cache/dump/factory.hpp>

namespace cache {

EmptyCacheError::EmptyCacheError(const std::string& cache_name)
    : std::runtime_error("Cache " + cache_name + " is empty") {}

void ThrowDumpUnimplemented(const std::string& name) {
  const auto message = fmt::format(
      "IsDumpEnabled returns true for cache {}, but cache dump is "
      "unimplemented for it. See cache::dump::Read, cache::dump::Write",
      name);
  UASSERT_MSG(false, message);
  throw std::logic_error(message);
}

void CacheUpdateTrait::Update(UpdateType update_type) {
  auto update = update_.Lock();
  const auto config = GetConfig();

  if (config->allowed_update_types == AllowedUpdateTypes::kOnlyFull &&
      update_type == UpdateType::kIncremental) {
    update_type = UpdateType::kFull;
  }

  DoUpdate(update_type, *update);
}

void CacheUpdateTrait::DumpSyncDebug() {
  auto update = update_.Lock();
  const auto config = GetConfig();

  DumpAsyncIfNeeded(DumpType::kForced, *update, *config);
  if (update->dump_task.IsValid()) update->dump_task.Wait();
}

CacheUpdateTrait::CacheUpdateTrait(const CacheConfigStatic& config,
                                   testsuite::CacheControl& cache_control,
                                   std::string name,
                                   engine::TaskProcessor& fs_task_processor)
    : CacheUpdateTrait(config, dump::CreateDefaultOperationsFactory(config),
                       cache_control, std::move(name), fs_task_processor) {}

CacheUpdateTrait::CacheUpdateTrait(
    const CacheConfigStatic& config,
    std::unique_ptr<dump::OperationsFactory> dump_rw_factory,
    testsuite::CacheControl& cache_control, std::string name,
    engine::TaskProcessor& fs_task_processor)
    : static_config_(config),
      config_(static_config_),
      cache_control_(cache_control),
      name_(std::move(name)),
      fs_task_processor_(fs_task_processor),
      periodic_update_enabled_(
          cache_control.IsPeriodicUpdateEnabled(static_config_, name)),
      is_running_(false),
      force_next_update_full_(false),
      periodic_task_flags_{utils::PeriodicTask::Flags::kChaotic,
                           utils::PeriodicTask::Flags::kCritical},
      cache_modified_(false),
      last_dumped_update_(dump::TimePoint{}),
      dump_rw_factory_(std::move(dump_rw_factory)),
      dumper_(CacheConfigStatic{static_config_}, name_) {
  UASSERT(dump_rw_factory_);
}

CacheUpdateTrait::~CacheUpdateTrait() {
  if (is_running_.load()) {
    LOG_ERROR()
        << "CacheUpdateTrait is being destroyed while periodic update "
           "task is still running. "
           "Derived class has to call StopPeriodicUpdates() in destructor. "
        << "Component name '" << name_ << "'";
    // Don't crash in production
    UASSERT_MSG(false, "StopPeriodicUpdates() is not called");
  }
}

AllowedUpdateTypes CacheUpdateTrait::AllowedUpdateTypes() const {
  const auto config = config_.Read();
  return config->allowed_update_types;
}

void CacheUpdateTrait::StartPeriodicUpdates(utils::Flags<Flag> flags) {
  if (is_running_.exchange(true)) {
    return;
  }

  const auto config = GetConfig();

  // CacheInvalidatorHolder is created here to achieve that cache invalidators
  // are registered in the order of cache component dependency.
  // We exploit the fact that StartPeriodicUpdates is called at the end
  // of all concrete cache component constructors.
  cache_invalidator_holder_ =
      std::make_unique<testsuite::CacheInvalidatorHolder>(cache_control_,
                                                          *this);

  try {
    const bool dump_loaded = LoadFromDump(*config);

    if ((!dump_loaded || config->first_update_mode != FirstUpdateMode::kSkip) &&
        (!(flags & Flag::kNoFirstUpdate) || !periodic_update_enabled_)) {
      // ignore kNoFirstUpdate if !periodic_update_enabled_
      // because some components require caches to be updated at least once

      // Force first update, do it synchronously
      tracing::Span span("first-update/" + name_);
      try {
        DoPeriodicUpdate();
      } catch (const std::exception& e) {
        if (dump_loaded &&
            config->first_update_mode != FirstUpdateMode::kRequired) {
          LOG_ERROR() << "Failed to update cache " << name_
                      << " after loading a cache dump, going on with the "
                         "contents loaded from the dump";
        } else if (static_config_.allow_first_update_failure) {
          LOG_ERROR() << "Failed to update cache " << name_
                      << " for the first time, leaving it empty";
        } else {
          LOG_ERROR() << "Failed to update cache " << name_
                      << " for the first time";
          throw;
        }
      }
    }

    // Without this clause, after loading a cache dump, no full updates will
    // ever be performed with kOnlyIncremental. This can be problematic in case
    // the data in the cache has been corrupted in some way. Even restarting
    // the service won't help. Solution: perform a single asynchronous full
    // update.
    if (dump_loaded &&
        config->allowed_update_types == AllowedUpdateTypes::kOnlyIncremental &&
        config->force_full_second_update) {
      force_next_update_full_ = true;
      periodic_task_flags_ |= utils::PeriodicTask::Flags::kNow;
    }

    if (periodic_update_enabled_) {
      update_task_.Start("update-task/" + name_,
                         GetPeriodicTaskSettings(*config),
                         [this]() { DoPeriodicUpdate(); });
      cleanup_task_.Start(
          "cleanup-task/" + name_,
          utils::PeriodicTask::Settings(config->cleanup_interval), [this]() {
            tracing::Span::CurrentSpan().SetLocalLogLevel(
                logging::Level::kNone);
            config_.Cleanup();
            Cleanup();
          });
    }
  } catch (...) {
    is_running_ = false;  // update_task_ is not started, don't check it in dtr
    throw;
  }
}

void CacheUpdateTrait::StopPeriodicUpdates() {
  if (!is_running_.exchange(false)) {
    return;
  }

  try {
    update_task_.Stop();
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Exception in update task of cache " << name_
                << ". Reason: " << ex;
  }

  try {
    cleanup_task_.Stop();
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Exception in cleanup task of cache " << name_
                << ". Reason: " << ex;
  }

  auto update = update_.Lock();
  if (update->dump_task.IsValid() && !update->dump_task.IsFinished()) {
    LOG_WARNING() << "Stopping a dump task of cache " << name_;
    try {
      update->dump_task.RequestCancel();
      update->dump_task.Wait();
    } catch (const std::exception& ex) {
      LOG_ERROR() << "Exception in dump task of cache " << name_
                  << ". Reason: " << ex;
    }
  }
}

formats::json::Value CacheUpdateTrait::ExtendStatistics() {
  auto& full = GetStatistics().full_update;
  auto& incremental = GetStatistics().incremental_update;
  const auto any = cache::CombineStatistics(full, incremental);

  formats::json::ValueBuilder builder;
  utils::statistics::SolomonLabelValue(builder, "cache_name");
  builder[cache::kStatisticsNameFull] = cache::StatisticsToJson(full);
  builder[cache::kStatisticsNameIncremental] =
      cache::StatisticsToJson(incremental);
  builder[cache::kStatisticsNameAny] = cache::StatisticsToJson(any);

  builder[cache::kStatisticsNameCurrentDocumentsCount] =
      GetStatistics().documents_current_count.load();
  builder[cache::kStatisticsNameDump] =
      cache::StatisticsToJson(GetStatistics().dump);

  return builder.ExtractValue();
}

void CacheUpdateTrait::SetConfig(const std::optional<CacheConfig>& config) {
  config_.Assign(config ? static_config_.MergeWith(*config) : static_config_);
  const auto new_config = config_.Read();
  update_task_.SetSettings(GetPeriodicTaskSettings(*new_config));
  cleanup_task_.SetSettings({new_config->cleanup_interval});
  dumper_->SetConfig(*new_config);
}

void CacheUpdateTrait::DoPeriodicUpdate() {
  auto update = update_.Lock();
  const auto config = GetConfig();

  // The update is full regardless of `update_type`:
  // - if the cache is empty, or
  // - if the update is forced to be full (see `StartPeriodicUpdates`)
  const bool force_full_update =
      std::exchange(force_next_update_full_, false) ||
      update->last_update == std::chrono::system_clock::time_point{};

  auto update_type = UpdateType::kFull;
  if (!force_full_update) {
    switch (config->allowed_update_types) {
      case AllowedUpdateTypes::kOnlyFull:
        update_type = UpdateType::kFull;
        break;
      case AllowedUpdateTypes::kOnlyIncremental:
        update_type = UpdateType::kIncremental;
        break;
      case AllowedUpdateTypes::kFullAndIncremental:
        const auto steady_now = std::chrono::steady_clock::now();
        update_type =
            steady_now - update->last_full_update < config->full_update_interval
                ? UpdateType::kIncremental
                : UpdateType::kFull;
        break;
    }
  }

  try {
    DoUpdate(update_type, *update);
    DumpAsyncIfNeeded(DumpType::kHonorDumpInterval, *update, *config);
  } catch (const std::exception& ex) {
    LOG_WARNING() << "Error while updating cache " << name_
                  << ". Reason: " << ex;
    DumpAsyncIfNeeded(DumpType::kHonorDumpInterval, *update, *config);
    throw;
  }
}

void CacheUpdateTrait::AssertPeriodicUpdateStarted() {
  UASSERT_MSG(is_running_.load(), "Cache " + name_ +
                                      " has been constructed without calling "
                                      "StartPeriodicUpdates(), call it in ctr");
}

void CacheUpdateTrait::OnCacheModified() { cache_modified_ = true; }

void CacheUpdateTrait::GetAndWrite(dump::Writer&) const {
  ThrowDumpUnimplemented(name_);
}

void CacheUpdateTrait::ReadAndSet(dump::Reader&) {
  ThrowDumpUnimplemented(name_);
}

void CacheUpdateTrait::DoUpdate(UpdateType update_type, UpdateData& update) {
  const auto steady_now = std::chrono::steady_clock::now();
  const auto update_type_str =
      update_type == UpdateType::kFull ? "full" : "incremental";
  tracing::Span::CurrentSpan().AddTag("update_type", update_type_str);

  UpdateStatisticsScope stats(GetStatistics(), update_type);
  LOG_INFO() << "Updating cache update_type=" << update_type_str
             << " name=" << name_;

  const dump::TimePoint system_now =
      std::chrono::time_point_cast<dump::TimePoint::duration>(
          std::chrono::system_clock::now());
  Update(update_type, update.last_update, system_now, stats);
  LOG_INFO() << "Updated cache update_type=" << update_type_str
             << " name=" << name_;

  update.last_update = system_now;
  if (cache_modified_.exchange(false)) {
    update.last_modifying_update = system_now;
  }
  if (update_type == UpdateType::kFull) {
    update.last_full_update = steady_now;
  }
  statistics_.dump.is_current_from_dump = false;
}

bool CacheUpdateTrait::ShouldDump(DumpType type, UpdateData& update,
                                  const CacheConfigStatic& config) {
  if (!config.dumps_enabled) {
    LOG_DEBUG() << "Cache dump has not been performed, because cache dumps are "
                   "disabled for cache "
                << name_;
    return false;
  }

  if (update.last_update == dump::TimePoint{}) {
    LOG_DEBUG() << "Skipped cache dump for cache " << name_
                << ", because the cache has not loaded yet";
    return false;
  }

  if (type == DumpType::kHonorDumpInterval &&
      GetLastDumpedUpdate() > update.last_update - config.min_dump_interval) {
    LOG_DEBUG() << "Skipped cache dump for cache " << name_
                << ", because dump interval has not passed yet";
    return false;
  }

  // Prevent concurrent cache dumps from accumulating
  // and slowing everything down.
  if (update.dump_task.IsValid() && !update.dump_task.IsFinished()) {
    LOG_INFO() << "Skipped cache dump for cache " << name_
               << ", because a previous dump operation is in progress";
    return false;
  }

  return true;
}

bool CacheUpdateTrait::DoDump(dump::TimePoint update_time, ScopeTime& scope) {
  const auto dump_start = std::chrono::steady_clock::now();

  const auto config = config_.Read();

  std::uint64_t dump_size;
  try {
    auto dump_stats = dumper_->RegisterNewDump(update_time);
    const auto& dump_path = dump_stats.full_path;
    auto writer = dump_rw_factory_->CreateWriter(dump_path, scope);
    GetAndWrite(*writer);
    writer->Finish();
    dump_size = boost::filesystem::file_size(dump_path);
  } catch (const EmptyCacheError& ex) {
    // ShouldDump checks that a successful update has been performed,
    // but the cache could have been cleared forcefully
    LOG_WARNING() << "Could not dump cache " << name_
                  << ", because it is empty";
    return false;
  } catch (const std::exception& ex) {
    LOG_ERROR() << "Error while serializing a cache dump for cache " << name_
                << ". Reason: " << ex;
    return false;
  }

  dumper_->Cleanup();

  statistics_.dump.last_written_size = dump_size;
  statistics_.dump.last_nontrivial_write_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - dump_start);
  statistics_.dump.last_nontrivial_write_start_time = dump_start;
  return true;
}

void CacheUpdateTrait::DumpAsync(DumpOperation operation_type,
                                 UpdateData& update) {
  UASSERT_MSG(!update.dump_task.IsValid() || update.dump_task.IsFinished(),
              "Another cache dump task is already running");

  if (update.dump_task.IsValid()) {
    try {
      update.dump_task.Get();
    } catch (const std::exception& ex) {
      LOG_ERROR() << "Unexpected error from the previous cache dump for cache "
                  << name_ << ". Reason: " << ex;
    }
  }

  update.dump_task = utils::Async(
      fs_task_processor_, "cache-dump",
      [this, operation_type, old_update_time = GetLastDumpedUpdate(),
       new_update_time = update.last_modifying_update]() mutable {
        auto scope_time = tracing::Span::CurrentSpan().CreateScopeTime(
            "serialize-dump/" + name_);

        bool success = false;
        switch (operation_type) {
          case DumpOperation::kNewDump:
            success = DoDump(new_update_time, scope_time);
            break;
          case DumpOperation::kBumpTime:
            success = dumper_->BumpDumpTime(old_update_time, new_update_time);
            break;
        }

        if (success) {
          last_dumped_update_ = new_update_time;
        }
      });
}

void CacheUpdateTrait::DumpAsyncIfNeeded(DumpType type, UpdateData& update,
                                         const CacheConfigStatic& config) {
  if (!ShouldDump(type, update, config)) return;

  if (GetLastDumpedUpdate() == update.last_modifying_update) {
    // If nothing has been updated since the last time, skip the serialization
    // and dump processes by just renaming the dump file.
    LOG_DEBUG() << "Skipped cache dump for cache " << name_
                << ", because nothing has been updated";
    DumpAsync(DumpOperation::kBumpTime, update);
  } else {
    DumpAsync(DumpOperation::kNewDump, update);
  }
}

bool CacheUpdateTrait::LoadFromDump(const CacheConfigStatic& config) {
  auto update = update_.Lock();

  tracing::Span span("load-from-dump/" + name_);
  const auto load_start = std::chrono::steady_clock::now();

  if (!config.dumps_enabled) {
    LOG_DEBUG() << "Could not load a cache dump, because cache dumps are "
                   "disabled for cache "
                << name_;
    return false;
  }

  const std::optional<dump::TimePoint> update_time =
      utils::Async(fs_task_processor_, "cache-dump", [this] {
        try {
          auto dump_stats = dumper_->GetLatestDump();
          if (!dump_stats) return std::optional<dump::TimePoint>{};

          auto reader = dump_rw_factory_->CreateReader(dump_stats->full_path);
          ReadAndSet(*reader);
          reader->Finish();

          return std::optional{dump_stats->update_time};
        } catch (const std::exception& ex) {
          LOG_ERROR() << "Error while parsing a cache dump for cache " << name_
                      << ". Reason: " << ex;
          return std::optional<dump::TimePoint>{};
        }
      }).Get();

  if (!update_time) return false;

  LOG_INFO() << "Loaded a cache dump for cache " << name_;
  update->last_update = *update_time;
  update->last_modifying_update = *update_time;
  utils::AtomicMax(last_dumped_update_, *update_time);

  statistics_.dump.is_loaded = true;
  statistics_.dump.is_current_from_dump = true;
  statistics_.dump.load_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - load_start);
  return true;
}

dump::TimePoint CacheUpdateTrait::GetLastDumpedUpdate() {
  return last_dumped_update_.load();
}

utils::PeriodicTask::Settings CacheUpdateTrait::GetPeriodicTaskSettings(
    const CacheConfigStatic& config) {
  return utils::PeriodicTask::Settings{
      config.update_interval, config.update_jitter, periodic_task_flags_};
}

}  // namespace cache
