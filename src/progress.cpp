// Provides cross-platform, console-based progress reporting and ETA estimation
// during compression and decompression stages.

#include "progress.h"
#include <cmath>
#include <iomanip>
#include <iostream>

namespace spring {

namespace {
log_level g_log_level = log_level::quiet;
ProgressBar *g_global_progress_bar = nullptr;
} // namespace

void Logger::set_level(log_level level) { g_log_level = level; }

log_level Logger::level() { return g_log_level; }

bool Logger::is_info_enabled() {
  return static_cast<int>(g_log_level) >= static_cast<int>(log_level::info);
}

bool Logger::is_debug_enabled() {
  return static_cast<int>(g_log_level) >= static_cast<int>(log_level::debug);
}

void Logger::set_verbose(bool verbose) {
  g_log_level = verbose ? log_level::info : log_level::quiet;
}

bool Logger::is_verbose() { return is_info_enabled(); }

ProgressBar *ProgressBar::GlobalInstance() { return g_global_progress_bar; }

void ProgressBar::SetGlobalInstance(ProgressBar *instance) {
  g_global_progress_bar = instance;
}

void Logger::log_info(const std::string &msg) {
  if (is_info_enabled()) {
    std::cout << msg << std::endl;
  }
}

void Logger::log_debug(const std::string &msg) {
  if (is_debug_enabled()) {
    std::cout << "[DEBUG] " << msg << std::endl;
  }
}

void Logger::log_warning(const std::string &msg) {
  // Warnings are printed regardless of verbosity usually, but let's keep it to
  // stdout
  std::cout << "WARNING: " << msg << std::endl;
}

void Logger::log_error(const std::string &msg) {
  // Errors are always logged to cerr
  std::cerr << msg << std::endl;
}

ProgressBar::ProgressBar(bool enabled) : enabled_(enabled) {
  if (enabled_ && !Logger::is_info_enabled()) {
    // Initial empty bar
    render(0.0F);
  }
}

ProgressBar::~ProgressBar() {
  if (enabled_ && !Logger::is_info_enabled()) {
    finalize();
  }
}

void ProgressBar::set_stage(const std::string &label, float start_pct,
                            float end_pct) {
  std::scoped_lock<std::mutex> lock(mutex_);
  current_label_ = label;
  stage_start_ = start_pct;
  stage_end_ = end_pct;
}

void ProgressBar::update(float stage_progress) {
  if (!enabled_ || Logger::is_info_enabled())
    return;

  std::scoped_lock<std::mutex> lock(mutex_);
  float global_pct =
      stage_start_ + (stage_progress * (stage_end_ - stage_start_));

  // Throttle rendering to 1% increments or significant changes
  if (std::abs(global_pct - last_rendered_pct_) >= 0.01F ||
      global_pct >= 1.0F) {
    render(global_pct);
    last_rendered_pct_ = global_pct;
  }
}

void ProgressBar::finalize() {
  if (!enabled_ || Logger::is_info_enabled())
    return;

  std::scoped_lock<std::mutex> lock(mutex_);
  if (finalized_)
    return;

  render(1.0F);
  std::cout << std::endl;
  finalized_ = true;
}

void ProgressBar::render(float global_progress) {
  const int bar_width = 40;
  int pos = static_cast<int>(bar_width * global_progress);

  std::cout << "\r" << std::left << std::setw(20) << current_label_ << " [";
  for (int i = 0; i < bar_width; ++i) {
    if (i < pos)
      std::cout << "=";
    else if (i == pos)
      std::cout << ">";
    else
      std::cout << " ";
  }
  std::cout << "] " << int(global_progress * 100.0) << "%" << std::flush;
}

} // namespace spring

