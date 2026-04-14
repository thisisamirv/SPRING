#include "spring_reader.h"
#include "decompress.h"
#include "fs_utils.h"
#include "integrity_utils.h"
#include "params.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>

namespace spring {

namespace {

/**
 * @brief Sink implementation that buffers records into a thread-safe queue.
 */
class BufferDecompressionSink : public DecompressionSink {
public:
  using StepPair = std::pair<std::vector<ReadRecord>, std::vector<ReadRecord>>;

  BufferDecompressionSink(std::queue<StepPair> &queue, std::mutex &mutex,
                          std::condition_variable &cv, bool paired_end,
                          size_t max_queue_size)
      : queue_(queue), mutex_(mutex), cv_(cv), paired_end_(paired_end),
        max_queue_size_(max_queue_size) {}

  void consume_step(std::string *id_buffer, std::string *read_buffer,
                    const std::string *quality_buffer, uint32_t count,
                    int stream_index) override {

    std::unique_lock<std::mutex> lock(mutex_.get());
    // Throttle decompression if the queue is full
    cv_.get().wait(lock,
                   [this] { return queue_.get().size() < max_queue_size_; });

    for (uint32_t i = 0; i < count; ++i) {
      update_record_crc(sequence_crc_[stream_index], read_buffer[i]);
      if (quality_buffer) {
        update_record_crc(quality_crc_[stream_index], quality_buffer[i]);
      }
      update_record_crc(id_crc_[stream_index], id_buffer[i]);
    }

    if (stream_index == 0) {
      current_step_.first.clear();
      current_step_.first.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        ReadRecord rec;
        rec.id = id_buffer[i];
        rec.sequence = read_buffer[i];
        if (quality_buffer)
          rec.quality = quality_buffer[i];
        current_step_.first.push_back(std::move(rec));
      }
      if (!paired_end_) {
        queue_.get().push(std::move(current_step_));
        current_step_ = {}; // Reset
        lock.unlock();
        cv_.get().notify_all();
      }
    } else {
      // stream_index == 1
      current_step_.second.clear();
      current_step_.second.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        ReadRecord rec;
        rec.id = id_buffer[i];
        rec.sequence = read_buffer[i];
        if (quality_buffer)
          rec.quality = quality_buffer[i];
        current_step_.second.push_back(std::move(rec));
      }
      queue_.get().push(std::move(current_step_));
      current_step_ = {}; // Reset
      lock.unlock();
      cv_.get().notify_all();
    }
  }

private:
  std::reference_wrapper<std::queue<StepPair>> queue_;
  std::reference_wrapper<std::mutex> mutex_;
  std::reference_wrapper<std::condition_variable> cv_;
  bool paired_end_;
  size_t max_queue_size_;
  StepPair current_step_;
};

} // namespace

class SpringReader::Impl {
public:
  // Disallow copying and moving for safety of background thread and resources.
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  Impl(std::string archive_path, int num_thr, std::string work_dir)
      : archive_path_(std::move(archive_path)), user_num_thr_(num_thr) {

    // Setup temporary directory
    if (work_dir.empty()) {
      std::filesystem::path temp_base = std::filesystem::temp_directory_path();
      temp_dir_ =
          (temp_base /
           ("spring_reader_" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
              .string();
    } else {
      temp_dir_ = std::move(work_dir);
    }
    std::filesystem::create_directories(temp_dir_);

    // Extract metadata first
    extract_tar_archive(
        archive_path_,
        temp_dir_); // Currently extracts whole thing; could be optimized

    std::string cp_path = temp_dir_ + "/cp.bin";
    std::ifstream cp_in(cp_path, std::ios::binary);
    if (!cp_in)
      throw std::runtime_error("Failed to read archive metadata.");
    read_compression_params(cp_in, params_);
    cp_in.close();

    if (user_num_thr_ > 0)
      params_.encoding.num_thr = user_num_thr_;

    // Start worker thread
    worker_thread_ = std::thread([this]() {
      try {
        BufferDecompressionSink sink(queue_, mutex_, cv_,
                                     params_.encoding.paired_end, 2);
        if (params_.encoding.long_flag) {
          decompress_long(temp_dir_, sink, params_);
        } else {
          decompress_short(temp_dir_, sink, params_);
        }

        {
          std::scoped_lock<std::mutex> lock(mutex_);
          worker_done_ = true;
        }
        cv_.notify_all();
      } catch (...) {
        std::scoped_lock<std::mutex> lock(mutex_);
        worker_exception_ = std::current_exception();
        worker_done_ = true;
        cv_.notify_all();
      }
    });
  }

  ~Impl() {
    {
      // We don't have a clean way to "abort" decompress_short yet without
      // changing it. For now, we wait for it to finish or just let it leak if
      // it takes too long? Better: just join it.
    }
    if (worker_thread_.joinable())
      worker_thread_.join();

    // Cleanup temp dir
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }

  bool next(ReadRecord &mate1, ReadRecord *mate2) {
    if (!current_step_cache_.first.empty() &&
        cache_pos_ < current_step_cache_.first.size()) {
      mate1 = std::move(current_step_cache_.first[cache_pos_]);
      if (mate2)
        *mate2 = std::move(current_step_cache_.second[cache_pos_]);
      cache_pos_++;
      return true;
    }

    // Try to get next batch
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || worker_done_; });

    if (worker_exception_)
      std::rethrow_exception(worker_exception_);

    if (queue_.empty() && worker_done_)
      return false;

    current_step_cache_ = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    cv_.notify_all(); // Notify worker that space is available

    cache_pos_ = 0;
    if (current_step_cache_.first.empty())
      return false;

    mate1 = std::move(current_step_cache_.first[cache_pos_]);
    if (mate2)
      *mate2 = std::move(current_step_cache_.second[cache_pos_]);
    cache_pos_++;
    return true;
  }

  [[nodiscard]] const compression_params &params() const { return params_; }

private:
  std::string archive_path_;
  std::string temp_dir_;
  compression_params params_;
  int user_num_thr_;

  std::thread worker_thread_;
  std::queue<BufferDecompressionSink::StepPair> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool worker_done_ = false;
  std::exception_ptr worker_exception_ = nullptr;

  BufferDecompressionSink::StepPair current_step_cache_;
  size_t cache_pos_ = 0;
};

SpringReader::SpringReader(const std::string &archive_path, int num_thr,
                           const std::string &work_dir)
    : impl_(std::make_unique<Impl>(archive_path, num_thr, work_dir)) {}

SpringReader::~SpringReader() = default;

const compression_params &SpringReader::params() const {
  return impl_->params();
}

bool SpringReader::next(ReadRecord &record) {
  if (impl_->params().encoding.paired_end)
    throw std::runtime_error("Archive is paired-end, use next(mate1, mate2)");
  return impl_->next(record, nullptr);
}

bool SpringReader::next(ReadRecord &mate1, ReadRecord &mate2) {
  if (!impl_->params().encoding.paired_end)
    throw std::runtime_error("Archive is single-end, use next(record)");
  return impl_->next(mate1, &mate2);
}

void SpringReader::get_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                               uint32_t id_crc[2]) {
  for (int i = 0; i < 2; ++i) {
    seq_crc[i] = 0;
    qual_crc[i] = 0;
    id_crc[i] = 0;
  }
  // This is tricky because the worker might still be running.
  // We need to wait or only report after we are done.
  // Actually, for simplicity, we provide this if anyone needs to check
  // after reading all records.
  // But since the Sink is internal to Impl, we might need a better way.
  // For now, let's keep it unimplemented or revisit if we need it for Reader
  // tests.
}

} // namespace spring
