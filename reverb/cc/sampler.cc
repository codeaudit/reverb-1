// Copyright 2019 DeepMind Technologies Limited.
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

#include "reverb/cc/sampler.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "grpcpp/impl/codegen/client_context.h"
#include "grpcpp/impl/codegen/sync_stream.h"
#include "absl/memory/memory.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "reverb/cc/chunk_store.h"
#include "reverb/cc/platform/hash_map.h"
#include "reverb/cc/platform/hash_set.h"
#include "reverb/cc/platform/logging.h"
#include "reverb/cc/platform/status_macros.h"
#include "reverb/cc/platform/thread.h"
#include "reverb/cc/rate_limiter.h"
#include "reverb/cc/reverb_service.pb.h"
#include "reverb/cc/schema.pb.h"
#include "reverb/cc/support/grpc_util.h"
#include "reverb/cc/support/tf_util.h"
#include "reverb/cc/support/trajectory_util.h"
#include "reverb/cc/table.h"
#include "reverb/cc/tensor_compression.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_util.h"

namespace deepmind {
namespace reverb {
namespace {

inline bool SampleIsDone(const std::vector<SampleStreamResponse>& sample) {
  if (sample.empty()) return false;

  internal::flat_hash_set<uint64_t> received_chunks;
  for (const auto& response : sample) {
    if (response.has_data()) {
      received_chunks.insert(response.data().chunk_key());
    }
  }

  for (uint64_t key :
       internal::GetChunkKeys(sample.front().info().item().flat_trajectory())) {
    if (!received_chunks.contains(key)) {
      return false;
    }
  }
  return true;
}

template <typename T>
tensorflow::Tensor InitializeTensor(T value, int64_t length) {
  tensorflow::Tensor tensor(tensorflow::DataTypeToEnum<T>::v(),
                            tensorflow::TensorShape({length}));
  auto tensor_t = tensor.flat<T>();
  std::fill(tensor_t.data(), tensor_t.data() + length, value);
  return tensor;
}

// Special of trajectory unpacking which slightly lower memory overhead as
// chunks can be dropped incrementally instead of after all has been unpacked.
//
// TODO(b/177655981): Remove once the general case have been improved.
absl::Status TimestepTrajectoryAsSample(
    std::vector<SampleStreamResponse> responses,
    std::unique_ptr<Sample>* sample) {
  const auto& info = responses.front().info();

  // Extract all chunks belonging to this sample.
  std::deque<std::vector<tensorflow::Tensor>> chunks;

  // The chunks are not required to be aligned perfectly with the data so a
  // part of the first chunk is potentially stripped. The same applies to the
  // last part of the final chunk.
  int64_t offset =
      internal::TimestepTrajectoryOffset(info.item().flat_trajectory());
  int64_t remaining =
      internal::TimestepTrajectoryLength(info.item().flat_trajectory());

  for (auto& response : responses) {
    REVERB_CHECK_GT(remaining, 0);

    std::vector<tensorflow::Tensor> batches;
    batches.resize(response.data().data().tensors_size());

    int64_t batch_size = -1;

    // Convert each chunk tensor and release the chunk memory afterwards.
    int64_t insert_index = response.data().data().tensors_size() - 1;
    while (!response.data().data().tensors().empty()) {
      tensorflow::Tensor batch;

      {
        // This ensures we release the response proto after converting the
        // result to a tensor.
        auto chunk = absl::WrapUnique(response.mutable_data()
                                          ->mutable_data()
                                          ->mutable_tensors()
                                          ->ReleaseLast());
        batch = DecompressTensorFromProto(*chunk);
      }

      if (response.data().delta_encoded()) {
        batch = DeltaEncode(batch, /*encode=*/false);
      }

      if (batch_size < 0) {
        batch_size = batch.dim_size(0);
      } else {
        if (batch_size != batch.dim_size(0)) {
          return absl::InternalError(absl::StrCat(
              "Chunks of the same response must have identical batch size, but "
              "first chunk has batch size ",
              batch_size, " while the current chunk has batch size ",
              batch.dim_size(0)));
        }
      }

      batch =
          batch.Slice(offset, std::min<int64_t>(offset + remaining, batch_size));
      if (!batch.IsAligned()) {
        batch = tensorflow::tensor::DeepCopy(batch);
      }

      batches[insert_index--] = std::move(batch);
    }

    chunks.push_back(std::move(batches));

    remaining -= std::min<int64_t>(remaining, batch_size - offset);
    offset = 0;
  }

  REVERB_CHECK_EQ(remaining, 0);

  std::vector<bool> squeeze_columns;
  for (const auto& col : info.item().flat_trajectory().columns()) {
    squeeze_columns.push_back(col.squeeze());
  }

  *sample = absl::make_unique<Sample>(
      info.item().key(), info.probability(), info.table_size(),
      info.item().priority(), std::move(chunks), std::move(squeeze_columns));
  return absl::OkStatus();
}

absl::Status AsSample(std::vector<SampleStreamResponse> responses,
                      std::unique_ptr<Sample>* sample) {
  const auto& info = responses.front().info();

  // TODO(b/177655981): Remove this branch once the general case has been
  // improved.
  if (internal::IsTimestepTrajectory(info.item().flat_trajectory())) {
    return TimestepTrajectoryAsSample(std::move(responses), sample);
  }

  internal::flat_hash_map<uint64_t, std::unique_ptr<ChunkData>> chunks;
  for (auto& response : responses) {
    auto key = response.data().chunk_key();
    chunks[key] = absl::WrapUnique<ChunkData>(response.release_data());
  }

  // Extract all chunks belonging to this sample.
  std::vector<tensorflow::Tensor> unpacked_columns;
  for (const auto& column : info.item().flat_trajectory().columns()) {
    std::vector<tensorflow::Tensor> unpacked_chunks;
    for (const auto& slice : column.chunk_slices()) {
      auto it = chunks.find(slice.chunk_key());
      if (it == chunks.end()) {
        return absl::InternalError(
            absl::StrCat("Chunk ", slice.chunk_key(),
                         " could not be found when unpacking item ",
                         info.item().key(), "."));
      }
      unpacked_chunks.emplace_back();
      REVERB_RETURN_IF_ERROR(internal::UnpackChunkColumnAndSlice(
          *it->second, slice, &unpacked_chunks.back()));
    }

    // TODO(b/177655596): Avoid this concat when timesteps are emitted.
    unpacked_columns.emplace_back();
    REVERB_RETURN_IF_ERROR(FromTensorflowStatus(
        tensorflow::tensor::Concat(unpacked_chunks, &unpacked_columns.back())));
  }

  std::vector<bool> squeeze_columns;
  for (const auto& col : info.item().flat_trajectory().columns()) {
    squeeze_columns.push_back(col.squeeze());
  }

  *sample =
      absl::make_unique<Sample>(info.item().key(), info.probability(),
                                info.table_size(), info.item().priority(),
                                std::deque<std::vector<tensorflow::Tensor>>(
                                    {std::move(unpacked_columns)}),
                                std::move(squeeze_columns));

  return absl::OkStatus();
}

absl::Status AsSample(const Table::SampledItem& sampled_item,
                      std::unique_ptr<Sample>* sample) {
  internal::flat_hash_map<uint64_t, std::shared_ptr<ChunkStore::Chunk>> chunks(
      sampled_item.chunks.size());
  for (auto& chunk : sampled_item.chunks) {
    chunks[chunk->key()] = chunk;
  }

  std::vector<tensorflow::Tensor> flat_trajectory;
  flat_trajectory.reserve(sampled_item.item.flat_trajectory().columns_size());

  for (const auto& column : sampled_item.item.flat_trajectory().columns()) {
    std::vector<tensorflow::Tensor> unpacked_chunks;
    unpacked_chunks.reserve(column.chunk_slices_size());

    for (const auto& slice : column.chunk_slices()) {
      unpacked_chunks.emplace_back();
      REVERB_RETURN_IF_ERROR(internal::UnpackChunkColumnAndSlice(
          chunks[slice.chunk_key()]->data(), slice, &unpacked_chunks.back()));
    }

    flat_trajectory.emplace_back();
    REVERB_RETURN_IF_ERROR(FromTensorflowStatus(
        tensorflow::tensor::Concat(unpacked_chunks, &flat_trajectory.back())));
  }

  std::vector<bool> squeeze_columns;
  for (const auto& col : sampled_item.item.flat_trajectory().columns()) {
    squeeze_columns.push_back(col.squeeze());
  }

  *sample = absl::make_unique<deepmind::reverb::Sample>(
      sampled_item.item.key(), sampled_item.probability,
      sampled_item.table_size, sampled_item.item.priority(),
      std::deque<std::vector<tensorflow::Tensor>>({std::move(flat_trajectory)}),
      std::move(squeeze_columns));

  return absl::OkStatus();
}

class GrpcSamplerWorker : public SamplerWorker {
 public:
  // Constructs a new worker without creating a stream to a server.
  GrpcSamplerWorker(
      std::shared_ptr</* grpc_gen:: */ReverbService::StubInterface> stub,
      std::string table_name, int64_t samples_per_request,
      int flexible_batch_size)
      : stub_(std::move(stub)),
        table_name_(std::move(table_name)),
        samples_per_request_(samples_per_request),
        flexible_batch_size_(flexible_batch_size) {}

  // Cancels the stream and marks the worker as closed. Active and future
  // calls to `OpenStreamAndFetch` will return status `CANCELLED`.
  void Cancel() override {
    absl::MutexLock lock(&mu_);
    closed_ = true;
    if (context_ != nullptr) context_->TryCancel();
  }

  // Opens a new `SampleStream` to a server and requests `num_samples` samples
  // in batches with maximum size `samples_per_request`, with a timeout to
  // pass to the `Table::Sample` call. Once complete (either
  // done, from a non transient error, or from timing out), the stream is
  // closed and the number of samples pushed to `queue` is returned together
  // with the status of the stream.  A timeout will cause the Status type
  // DeadlineExceeded to be returned.
  std::pair<int64_t, absl::Status> FetchSamples(
      internal::Queue<std::unique_ptr<Sample>>* queue, int64_t num_samples,
      absl::Duration rate_limiter_timeout) override {
    std::unique_ptr<grpc::ClientReaderWriterInterface<SampleStreamRequest,
                                                      SampleStreamResponse>>
        stream;
    {
      absl::MutexLock lock(&mu_);
      if (closed_) {
        return {0, absl::CancelledError("`Close` called on Sampler.")};
      }
      context_ = absl::make_unique<grpc::ClientContext>();
      context_->set_wait_for_ready(false);
      stream = stub_->SampleStream(context_.get());
    }

    int64_t num_samples_returned = 0;
    while (num_samples_returned < num_samples) {
      SampleStreamRequest request;
      request.set_table(table_name_);
      request.set_num_samples(
          std::min(samples_per_request_, num_samples - num_samples_returned));
      request.mutable_rate_limiter_timeout()->set_milliseconds(
          NonnegativeDurationToInt64Millis(rate_limiter_timeout));
      request.set_flexible_batch_size(flexible_batch_size_);

      if (!stream->Write(request)) {
        return {num_samples_returned, FromGrpcStatus(stream->Finish())};
      }

      for (int64_t i = 0; i < request.num_samples(); i++) {
        std::vector<SampleStreamResponse> responses;
        while (!SampleIsDone(responses)) {
          SampleStreamResponse response;
          if (!stream->Read(&response)) {
            return {num_samples_returned, FromGrpcStatus(stream->Finish())};
          }
          responses.push_back(std::move(response));
        }

        std::unique_ptr<Sample> sample;
        auto status = AsSample(std::move(responses), &sample);
        if (!status.ok()) {
          return {num_samples_returned, status};
        }
        if (!queue->Push(std::move(sample))) {
          return {num_samples_returned,
                  absl::CancelledError("`Close` called on Sampler")};
        }
        ++num_samples_returned;
      }
    }

    if (num_samples_returned != num_samples) {
      return {num_samples_returned,
              absl::InternalError(
                  absl::StrCat("num_samples_returned != num_samples (",
                               num_samples_returned, " vs. ", num_samples))};
    }
    return {num_samples_returned, absl::OkStatus()};
  }

 private:
  // Stub used to open `SampleStream`-streams to a server.
  std::shared_ptr</* grpc_gen:: */ReverbService::StubInterface> stub_;

  // Name of the `Table` to sample from.
  const std::string table_name_;

  // The maximum number of samples to request in a "batch".
  const int64_t samples_per_request_;

  // Upper limit of the number of items that may be sampled in a single call
  // to
  // `Table::SampleFlexibleBatch` (lock not released between samples).
  const int flexible_batch_size_;

  // Context of the active stream.
  std::unique_ptr<grpc::ClientContext> context_ ABSL_GUARDED_BY(mu_);

  // True if `Cancel` has been called.
  bool closed_ ABSL_GUARDED_BY(mu_) = false;

  absl::Mutex mu_;
};

class LocalSamplerWorker : public SamplerWorker {
 public:
  // Constructs a new worker without creating a stream to a server.
  LocalSamplerWorker(std::shared_ptr<Table> table, int flexible_batch_size)
      : table_(table), flexible_batch_size_(flexible_batch_size) {
    REVERB_CHECK_GE(flexible_batch_size_, 1);
  }

  void Cancel() override {
    absl::MutexLock lock(&mu_);
    closed_ = true;
  }

  std::pair<int64_t, absl::Status> FetchSamples(
      internal::Queue<std::unique_ptr<Sample>>* queue, int64_t num_samples,
      absl::Duration rate_limiter_timeout) override {
    static const auto kWakeupTimeout = absl::Seconds(3);
    auto final_deadline = absl::Now() + rate_limiter_timeout;

    int64_t num_samples_returned = 0;
    while (num_samples_returned < num_samples) {
      {
        absl::MutexLock lock(&mu_);
        if (closed_) {
          return {0, absl::CancelledError("`Close` called on Sampler.")};
        }
      }

      // If the rate limiter deadline is long into the future then we set the
      // deadline `kWakeupTimeout` from now instead. Periodically waking up
      // allows us to check that the Sampler haven't been cancelled while we
      // were waiting.
      auto timeout =
          std::min(final_deadline, absl::Now() + kWakeupTimeout) - absl::Now();

      // Select the biggest batch size constrained by`flexible_batch_size_` and
      // the number of samples remaining.
      auto batch_size = std::min<int>(flexible_batch_size_,
                                      num_samples - num_samples_returned);

      std::vector<Table::SampledItem> items;
      auto status = table_->SampleFlexibleBatch(&items, batch_size, timeout);

      // If the deadline is exceeded but the "real deadline" is still in the
      // future then we are only waking up to check for cancellation.
      if (absl::IsDeadlineExceeded(status) && absl::Now() < final_deadline) {
        continue;
      }

      // All other errors are "real" and thus should be returned to the caller.
      if (!status.ok()) {
        return {num_samples_returned, status};
      }

      // Push sampled items to queue.
      for (const auto& item : items) {
        std::unique_ptr<Sample> sample;
        if (status = AsSample(item, &sample); !status.ok()) {
          return {num_samples_returned, status};
        }
        if (!queue->Push(std::move(sample))) {
          return {num_samples_returned,
                  absl::CancelledError("`Close` called on Sampler")};
        }
        ++num_samples_returned;
      }
    }

    if (num_samples_returned != num_samples) {
      return {num_samples_returned,
              absl::InternalError(
                  absl::StrCat("num_samples_returned != num_samples (",
                               num_samples_returned, " vs. ", num_samples))};
    }
    return {num_samples_returned, absl::OkStatus()};
  }

 private:
  std::shared_ptr<Table> table_;
  const int flexible_batch_size_;
  bool closed_ ABSL_GUARDED_BY(mu_) = false;
  absl::Mutex mu_;
};

int64_t GetNumWorkers(const Sampler::Options& options) {
  int64_t max_samples = options.max_samples == Sampler::kUnlimitedMaxSamples
                          ? INT64_MAX
                          : options.max_samples;
  int64_t num_workers = options.num_workers == Sampler::kAutoSelectValue
                          ? Sampler::kDefaultNumWorkers
                          : options.num_workers;

  // If a subset of the workers are able to fetch all of `max_samples` in the
  // first batch then there is no point in creating all of them.
  return std::min<int64_t>(
      num_workers,
      std::max<int64_t>(1,
                      max_samples / options.max_in_flight_samples_per_worker));
}

std::vector<std::unique_ptr<SamplerWorker>> MakeGrpcWorkers(
    std::shared_ptr</* grpc_gen:: */ReverbService::StubInterface> stub,
    const std::string& table_name, const Sampler::Options& options) {
  int64_t num_workers = GetNumWorkers(options);
  REVERB_CHECK_GE(num_workers, 1);
  std::vector<std::unique_ptr<SamplerWorker>> workers;
  workers.reserve(num_workers);
  for (int i = 0; i < num_workers; i++) {
    workers.push_back(absl::make_unique<GrpcSamplerWorker>(
        stub, table_name, options.max_in_flight_samples_per_worker,
        options.flexible_batch_size));
  }

  return workers;
}

std::vector<std::unique_ptr<SamplerWorker>> MakeLocalWorkers(
    std::shared_ptr<Table> table, const Sampler::Options& options) {
  int64_t num_workers = GetNumWorkers(options);
  REVERB_CHECK_GE(num_workers, 1);
  int flexible_batch_size =
      options.flexible_batch_size == Sampler::kAutoSelectValue
          ? table->DefaultFlexibleBatchSize()
          : options.flexible_batch_size;

  // Local workers do not send `SampleStreamRequest` and thus will not make
  // explicit use of `max_in_flight_samples_per_worker`. It would however be
  // incorrect to allow more than `max_in_flight_samples_per_worker` to be
  // sampled in a simple call so we limit `flexible_batch_size` by it. This will
  // have the desired effect as workers fetch new samples once the previous
  // batch has been depleted effectively limiting the number of in flight items
  // to `flexible_batch_size` (and thus `max_in_flight_samples_per_worker`).
  flexible_batch_size =
      std::min(flexible_batch_size, options.max_in_flight_samples_per_worker);

  std::vector<std::unique_ptr<SamplerWorker>> workers;
  workers.reserve(num_workers);
  for (int i = 0; i < num_workers; ++i) {
    workers.push_back(
        absl::make_unique<LocalSamplerWorker>(table, flexible_batch_size));
  }
  return workers;
}

}  // namespace

Sampler::Sampler(std::shared_ptr</* grpc_gen:: */ReverbService::StubInterface> stub,
                 const std::string& table_name, const Options& options,
                 internal::DtypesAndShapes dtypes_and_shapes)
    : Sampler(MakeGrpcWorkers(std::move(stub), table_name, options), table_name,
              options, std::move(dtypes_and_shapes)) {}

Sampler::Sampler(std::vector<std::unique_ptr<SamplerWorker>> workers,
                 const std::string& table, const Options& options,
                 internal::DtypesAndShapes dtypes_and_shapes)
    : table_(table),
      max_samples_(options.max_samples == kUnlimitedMaxSamples
                       ? INT64_MAX
                       : options.max_samples),
      max_samples_per_stream_(options.max_samples_per_stream == kAutoSelectValue
                                  ? kDefaultMaxSamplesPerStream
                                  : options.max_samples_per_stream),
      rate_limiter_timeout_(options.rate_limiter_timeout),
      workers_(std::move(workers)),
      active_sample_(nullptr),
      samples_(std::max<int>(options.num_workers, 1)),
      dtypes_and_shapes_(std::move(dtypes_and_shapes)) {
  REVERB_CHECK_GT(max_samples_, 0);
  REVERB_CHECK_GT(options.max_in_flight_samples_per_worker, 0);
  REVERB_CHECK(options.num_workers == kAutoSelectValue ||
               options.num_workers > 0);
  REVERB_CHECK(options.flexible_batch_size == kAutoSelectValue ||
               options.flexible_batch_size > 0);

  for (int i = 0; i < workers_.size(); i++) {
    worker_threads_.push_back(internal::StartThread(
        absl::StrCat("SamplerWorker_", i),
        [this, worker = workers_[i].get()] { RunWorker(worker); }));
  }
}

Sampler::Sampler(std::shared_ptr<Table> table, const Options& options,
                 internal::DtypesAndShapes dtypes_and_shapes)
    : Sampler(MakeLocalWorkers(table, options), table->name(), options,
              std::move(dtypes_and_shapes)) {}

Sampler::~Sampler() { Close(); }

absl::Status Sampler::GetNextTimestep(
    std::vector<tensorflow::Tensor>* data, bool* end_of_sequence) {
  REVERB_RETURN_IF_ERROR(MaybeSampleNext());
  if (!active_sample_->is_composed_of_timesteps()) {
    return absl::InvalidArgumentError(
        "Sampled trajectory cannot be decomposed into timesteps.");
  }

  *data = active_sample_->GetNextTimestep();
  REVERB_RETURN_IF_ERROR(
      ValidateAgainstOutputSpec(*data, ValidationMode::kTimestep));

  if (end_of_sequence != nullptr) {
    *end_of_sequence = active_sample_->is_end_of_sample();
  }

  if (active_sample_->is_end_of_sample()) {
    absl::WriterMutexLock lock(&mu_);
    if (++returned_ == max_samples_) samples_.Close();
  }

  return absl::OkStatus();
}

absl::Status Sampler::GetNextSample(
    std::vector<tensorflow::Tensor>* data) {
  std::unique_ptr<Sample> sample;
  REVERB_RETURN_IF_ERROR(PopNextSample(&sample));
  REVERB_RETURN_IF_ERROR(sample->AsBatchedTimesteps(data));
  REVERB_RETURN_IF_ERROR(
      ValidateAgainstOutputSpec(*data, ValidationMode::kBatchedTimestep));

  absl::WriterMutexLock lock(&mu_);
  if (++returned_ == max_samples_) samples_.Close();
  return absl::OkStatus();
}

absl::Status Sampler::GetNextTrajectory(std::vector<tensorflow::Tensor>* data) {
  std::unique_ptr<Sample> sample;
  REVERB_RETURN_IF_ERROR(PopNextSample(&sample));
  REVERB_RETURN_IF_ERROR(sample->AsTrajectory(data));
  REVERB_RETURN_IF_ERROR(
      ValidateAgainstOutputSpec(*data, ValidationMode::kTrajectory));

  absl::WriterMutexLock lock(&mu_);
  if (++returned_ == max_samples_) samples_.Close();
  return absl::OkStatus();
}

absl::Status Sampler::ValidateAgainstOutputSpec(
    const std::vector<tensorflow::Tensor>& data, Sampler::ValidationMode mode) {
  if (!dtypes_and_shapes_) {
    return absl::OkStatus();
  }

  if (data.size() != dtypes_and_shapes_->size()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Inconsistent number of tensors received from table '", table_,
        "'.  Specification has ", dtypes_and_shapes_->size(),
        " tensors, but data coming from the table shows ", data.size(),
        " tensors.\nTable signature: ",
        internal::DtypesShapesString(*dtypes_and_shapes_),
        ".\nIncoming tensor signature: ",
        internal::DtypesShapesString(internal::SpecsFromTensors(data))));
  }

  for (int i = 4; i < data.size(); ++i) {
    tensorflow::TensorShape elem_shape;
    if (mode == ValidationMode::kBatchedTimestep) {
      // Remove the outer dimension from data[i].shape() so we can properly
      // compare against the spec (which doesn't have the sequence dimension).
      elem_shape = data[i].shape();
      if (elem_shape.dims() == 0) {
        return absl::InvalidArgumentError(
            absl::StrCat("Invalid tensor shape received from table '", table_,
                         "'.  "
                         "time_step is false but data[",
                         i,
                         "] has scalar shape "
                         "(no time dimension)."));
      }
      elem_shape.RemoveDim(0);
    }

    auto* shape_ptr =
        mode == ValidationMode::kTimestep || mode == ValidationMode::kTrajectory
            ? &(data[i].shape())
            : &elem_shape;
    if (data[i].dtype() != dtypes_and_shapes_->at(i).dtype ||
        !dtypes_and_shapes_->at(i).shape.IsCompatibleWith(*shape_ptr)) {
      return absl::InvalidArgumentError(absl::StrCat(
          "Received incompatible tensor at flattened index ", i,
          " from table '", table_, "'.  Specification has (dtype, shape): (",
          tensorflow::DataTypeString(dtypes_and_shapes_->at(i).dtype), ", ",
          dtypes_and_shapes_->at(i).shape.DebugString(),
          ").  Tensor has (dtype, shape): (",
          tensorflow::DataTypeString(data[i].dtype()), ", ",
          shape_ptr->DebugString(), ").\nTable signature: ",
          internal::DtypesShapesString(*dtypes_and_shapes_)));
    }
  }
  return absl::OkStatus();
}

bool Sampler::should_stop_workers() const {
  return closed_ || returned_ == max_samples_ || !worker_status_.ok();
}

void Sampler::Close() {
  {
    absl::WriterMutexLock lock(&mu_);
    if (closed_) return;
    closed_ = true;
  }

  for (auto& worker : workers_) {
    worker->Cancel();
  }

  samples_.Close();
  worker_threads_.clear();  // Joins worker threads.
}

absl::Status Sampler::MaybeSampleNext() {
  if (active_sample_ != nullptr && !active_sample_->is_end_of_sample()) {
    return absl::OkStatus();
  }

  return PopNextSample(&active_sample_);
}

absl::Status Sampler::PopNextSample(std::unique_ptr<Sample>* sample) {
  if (samples_.Pop(sample)) return absl::OkStatus();

  absl::ReaderMutexLock lock(&mu_);
  if (returned_ == max_samples_) {
    return absl::OutOfRangeError("`max_samples` already returned.");
  }
  if (closed_) {
    return absl::CancelledError("Sampler has been cancelled.");
  }
  return worker_status_;
}

void Sampler::RunWorker(SamplerWorker* worker) {
  auto trigger = [this]() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
    return should_stop_workers() || requested_ < max_samples_;
  };

  while (true) {
    mu_.LockWhen(absl::Condition(&trigger));

    if (should_stop_workers()) {
      mu_.Unlock();
      return;
    }
    int64_t samples_to_stream =
        std::min<int64_t>(max_samples_per_stream_, max_samples_ - requested_);
    requested_ += samples_to_stream;
    mu_.Unlock();

    auto result = worker->FetchSamples(&samples_, samples_to_stream,
                                       rate_limiter_timeout_);
    {
      absl::WriterMutexLock lock(&mu_);

      // If the stream was closed prematurely then we need to reduce the number
      // of requested samples by the difference of the expected number and the
      // actual.
      requested_ -= samples_to_stream - result.first;

      // Overwrite the final status only if it wasn't already an error.
      if (worker_status_.ok() && !result.second.ok() &&
          !absl::IsUnavailable(result.second)) {
        worker_status_ = result.second;
        samples_.Close();  // Unblock any pending calls.
        return;
      }
    }
  }
}

Sample::Sample(tensorflow::uint64 key, double probability,
               tensorflow::int64 table_size, double priority,
               std::deque<std::vector<tensorflow::Tensor>> chunks,
               std::vector<bool> squeeze_columns)
    : key_(key),
      probability_(probability),
      table_size_(table_size),
      priority_(priority),
      num_timesteps_(0),
      num_data_tensors_(0),
      chunks_(std::move(chunks)),
      squeeze_columns_(std::move(squeeze_columns)),
      next_timestep_index_(0),
      next_timestep_called_(false) {
  REVERB_CHECK(!chunks_.empty()) << "Must provide at least one chunk.";
  REVERB_CHECK(!chunks_.front().empty())
      << "Chunks must hold at least one tensor.";

  num_data_tensors_ = chunks_.front().size();
  for (const auto& batches : chunks_) {
    num_timesteps_ += batches.front().dim_size(0);
  }
}

std::vector<tensorflow::Tensor> Sample::GetNextTimestep() {
  REVERB_CHECK(!is_end_of_sample());
  REVERB_CHECK(is_composed_of_timesteps());

  // Construct the output tensors.
  std::vector<tensorflow::Tensor> result;
  result.reserve(num_data_tensors_ + 4);
  result.push_back(tensorflow::Tensor(key_));
  result.push_back(tensorflow::Tensor(probability_));
  result.push_back(tensorflow::Tensor(table_size_));
  result.push_back(tensorflow::Tensor(priority_));

  for (const auto& t : chunks_.front()) {
    auto slice = t.SubSlice(next_timestep_index_);
    if (slice.IsAligned()) {
      result.push_back(std::move(slice));
    } else {
      result.push_back(tensorflow::tensor::DeepCopy(slice));
    }
  }

  // Advance the iterator.
  ++next_timestep_index_;
  if (next_timestep_index_ == chunks_.front().front().dim_size(0)) {
    // Go to the next chunk.
    chunks_.pop_front();
    next_timestep_index_ = 0;
  }
  next_timestep_called_ = true;

  return result;
}

bool Sample::is_end_of_sample() const { return chunks_.empty(); }

bool Sample::is_composed_of_timesteps() const {
  std::vector<int> column_lengths;
  for (int i = 0; i < chunks_.front().size(); i++) {
    int length = 0;
    for (const auto& unpacked_chunk : chunks_) {
      // Note that we can safely assume that the tensor is not a scalar since a
      // batch dimension is always added when building a chunk. A scalar would
      // thus be represented as a tensor of shape [1].
      length += unpacked_chunk[i].dim_size(0);
    }
    column_lengths.push_back(length);
  }

  return std::all_of(column_lengths.begin(), column_lengths.end(),
                     [&](int a) { return a == column_lengths[0]; });
}

absl::Status Sample::AsBatchedTimesteps(std::vector<tensorflow::Tensor>* data) {
  if (next_timestep_called_) {
    return absl::DataLossError(
        "Sample::AsBatchedTimesteps: Some time steps have been lost.");
  }
  if (!is_composed_of_timesteps()) {
    return absl::FailedPreconditionError(
        "Sample::AsBatchedTimesteps when trajectory cannot be decomposed into "
        "timesteps.");
  }

  std::vector<tensorflow::Tensor> sequences(num_data_tensors_ + 4);

  // Initialize the first three items with the key, probability and table size.
  sequences[0] = InitializeTensor(key_, num_timesteps_);
  sequences[1] = InitializeTensor(probability_, num_timesteps_);
  sequences[2] = InitializeTensor(table_size_, num_timesteps_);
  sequences[3] = InitializeTensor(priority_, num_timesteps_);

  // Prepare the data for concatenation.
  // data_tensors[i][j] is the j-th chunk of the i-th data tensor.
  std::vector<std::vector<tensorflow::Tensor>> data_tensors(num_data_tensors_);

  // Extract all chunks.
  while (!chunks_.empty()) {
    auto it_to = data_tensors.begin();
    for (auto& batch : chunks_.front()) {
      (it_to++)->push_back(std::move(batch));
    }
    chunks_.pop_front();
  }

  // Concatenate all chunks.
  int64_t i = 4;
  for (const auto& chunks : data_tensors) {
    REVERB_RETURN_IF_ERROR(FromTensorflowStatus(
        tensorflow::tensor::Concat(chunks, &sequences[i++])));
  }

  std::swap(sequences, *data);

  return absl::OkStatus();
}

absl::Status Sample::AsTrajectory(std::vector<tensorflow::Tensor>* data) {
  if (next_timestep_called_) {
    return absl::DataLossError(
        "Sample::AsBatchedTimesteps: Some time steps have been lost.");
  }
  std::vector<tensorflow::Tensor> sequences(num_data_tensors_ + 4);

  // Initialize the first four items with the key, probability, table size and
  // priority.
  sequences[0] = tensorflow::Tensor(key_);
  sequences[1] = tensorflow::Tensor(probability_);
  sequences[2] = tensorflow::Tensor(table_size_);
  sequences[3] = tensorflow::Tensor(priority_);

  // If trajectory cannot be decomposed into timesteps then `chunks_` only has
  // one item which is the entire (flattened) trajectory. There is therefore no
  // need to concat tensors in this case and we can simply move the content as
  // it is to data.
  if (chunks_.size() == 1) {
    for (int i = 0; i < chunks_.front().size(); i++) {
      sequences[i + 4] = std::move(chunks_.front()[i]);
    }
    chunks_.pop_front();
  } else {
    // Prepare the data for concatenation.
    // data_tensors[i][j] is the j-th chunk of the i-th data tensor.
    std::vector<std::vector<tensorflow::Tensor>> data_tensors(
        num_data_tensors_);

    // Extract all chunks.
    while (!chunks_.empty()) {
      auto it_to = data_tensors.begin();
      for (auto& batch : chunks_.front()) {
        (it_to++)->push_back(std::move(batch));
      }
      chunks_.pop_front();
    }

    // Concatenate all chunks.
    int64_t i = 4;
    for (const auto& chunks : data_tensors) {
      REVERB_RETURN_IF_ERROR(FromTensorflowStatus(
          tensorflow::tensor::Concat(chunks, &sequences[i++])));
    }
  }

  // Remove batch dimension from squeezed columns.
  for (int i = 0; i < squeeze_columns_.size(); i++) {
    if (!squeeze_columns_[i]) continue;
    if (int batch_dim = sequences[i + 4].shape().dim_size(0); batch_dim != 1) {
      return absl::InternalError(absl::StrCat(
          "Tried to squeeze column with batch size ", batch_dim, "."));
    }

    sequences[i + 4] = sequences[i + 4].SubSlice(0);
    if (!sequences[i + 4].IsAligned()) {
      sequences[i + 4] = tensorflow::tensor::DeepCopy(sequences[i + 4]);
    }
  }

  std::swap(sequences, *data);

  return absl::OkStatus();
}

absl::Status Sampler::Options::Validate() const {
  if (max_samples < 1 && max_samples != kUnlimitedMaxSamples) {
    return absl::InvalidArgumentError(
        absl::StrCat("max_samples (", max_samples, ") must be ",
                     kUnlimitedMaxSamples, " or >= 1"));
  }
  if (max_in_flight_samples_per_worker < 1) {
    return absl::InvalidArgumentError(
        absl::StrCat("max_in_flight_samples_per_worker (",
                     max_in_flight_samples_per_worker, ") has to be >= 1"));
  }
  if (num_workers < 1 && num_workers != kAutoSelectValue) {
    return absl::InvalidArgumentError(
        absl::StrCat("num_workers (", num_workers, ") must be ",
                     kAutoSelectValue, " or >= 1"));
  }
  if (max_samples_per_stream < 1 &&
      max_samples_per_stream != kUnlimitedMaxSamples) {
    return absl::InvalidArgumentError(
        absl::StrCat("max_samples_per_stream (", max_samples_per_stream,
                     ") must be ", kUnlimitedMaxSamples, " or >= 1"));
  }
  if (rate_limiter_timeout < absl::ZeroDuration()) {
    return absl::InvalidArgumentError(absl::StrCat(
        "rate_limiter_timeout (", absl::FormatDuration(rate_limiter_timeout),
        ") must not be negative."));
  }
  if (flexible_batch_size < 1 && flexible_batch_size != kAutoSelectValue) {
    return absl::InvalidArgumentError(
        absl::StrCat("flexible_batch_size (", flexible_batch_size, ") must be ",
                     kAutoSelectValue, " or >= 1"));
  }
  return absl::OkStatus();
}

}  // namespace reverb
}  // namespace deepmind
