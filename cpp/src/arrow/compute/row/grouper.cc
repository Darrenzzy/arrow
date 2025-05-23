// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "arrow/compute/row/grouper.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <type_traits>

#include "arrow/array/builder_primitive.h"

#include "arrow/compute/api_vector.h"
#include "arrow/compute/function.h"
#include "arrow/compute/key_hash_internal.h"
#include "arrow/compute/light_array_internal.h"
#include "arrow/compute/registry.h"
#include "arrow/compute/row/compare_internal.h"
#include "arrow/compute/row/grouper_internal.h"
#include "arrow/compute/row/row_encoder_internal.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/bitmap_ops.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/cpu_info.h"
#include "arrow/util/logging_internal.h"
#include "arrow/util/task_group.h"

namespace arrow {

using internal::checked_cast;
using internal::PrimitiveScalarBase;

namespace compute {

namespace {

constexpr uint32_t kNoGroupId = std::numeric_limits<uint32_t>::max();

using group_id_t = std::remove_const<decltype(kNoGroupId)>::type;
using GroupIdType = CTypeTraits<group_id_t>::ArrowType;
auto g_group_id_type = std::make_shared<GroupIdType>();

Status CheckForGetSegments(const ExecSpan& batch,
                           const std::vector<TypeHolder>& key_types) {
  if (batch.values.size() != key_types.size()) {
    return Status::Invalid("expected batch size ", key_types.size(), " but got ",
                           batch.values.size());
  }
  for (size_t i = 0; i < key_types.size(); i++) {
    const auto& value = batch.values[i];
    const auto& key_type = key_types[i];
    if (*value.type() != *key_type.type) {
      return Status::Invalid("expected batch value ", i, " of type ", *key_type.type,
                             " but got ", *value.type());
    }
  }
  return Status::OK();
}

struct BaseRowSegmenter : public RowSegmenter {
  explicit BaseRowSegmenter(const std::vector<TypeHolder>& key_types)
      : key_types_(key_types) {}

  const std::vector<TypeHolder>& key_types() const override { return key_types_; }

  std::vector<TypeHolder> key_types_;
};

Segment MakeSegment(int64_t batch_length, int64_t offset, int64_t length, bool extends) {
  return Segment{offset, length, offset + length >= batch_length, extends};
}

using ExtendFunc = std::function<bool(const void*)>;
constexpr bool kDefaultExtends = true;  // by default, the first segment extends

struct NoKeysSegmenter : public BaseRowSegmenter {
  static std::unique_ptr<RowSegmenter> Make() {
    return std::make_unique<NoKeysSegmenter>();
  }

  NoKeysSegmenter() : BaseRowSegmenter({}) {}

  Status Reset() override { return Status::OK(); }

  Result<std::vector<Segment>> GetSegments(const ExecSpan& batch) override {
    RETURN_NOT_OK(CheckForGetSegments(batch, {}));

    if (batch.length == 0) {
      return std::vector<Segment>{};
    }
    return std::vector<Segment>{
        MakeSegment(batch.length, 0, batch.length - 0, kDefaultExtends)};
  }
};

struct SimpleKeySegmenter : public BaseRowSegmenter {
  static Result<std::unique_ptr<RowSegmenter>> Make(TypeHolder key_type) {
    return std::make_unique<SimpleKeySegmenter>(key_type);
  }

  explicit SimpleKeySegmenter(TypeHolder key_type)
      : BaseRowSegmenter({key_type}),
        key_type_(key_types_[0]),
        save_key_data_(static_cast<size_t>(key_type_.type->byte_width())),
        extend_was_called_(false) {}

  Status Reset() override {
    extend_was_called_ = false;
    return Status::OK();
  }

  Result<std::vector<Segment>> GetSegments(const ExecSpan& batch) override {
    RETURN_NOT_OK(CheckForGetSegments(batch, {key_type_}));

    if (batch.length == 0) {
      return std::vector<Segment>{};
    }

    const auto& value = batch.values[0];
    DCHECK(is_fixed_width(*value.type()));

    std::vector<Segment> segments;
    const void* key_data;
    if (value.is_scalar()) {
      const auto& scalar = *value.scalar;
      DCHECK(scalar.is_valid);
      key_data = checked_cast<const PrimitiveScalarBase&>(scalar).data();
      bool extends = Extend(key_data);
      segments.push_back(MakeSegment(batch.length, 0, batch.length, extends));
    } else {
      DCHECK(value.is_array());
      const auto& array = value.array;
      DCHECK_EQ(array.GetNullCount(), 0);
      auto data = GetValuesAsBytes(array);
      int64_t byte_width = array.type->byte_width();
      int64_t offset = 0;
      bool extends = Extend(data);
      while (offset < array.length) {
        int64_t match_length = GetMatchLength(data + offset * byte_width, byte_width,
                                              data, offset, array.length);
        segments.push_back(MakeSegment(array.length, offset, match_length,
                                       offset == 0 ? extends : false));
        offset += match_length;
      }
      key_data = data + (array.length - 1) * byte_width;
    }

    SaveKeyData(key_data);

    return segments;
  }

 private:
  static const uint8_t* GetValuesAsBytes(const ArraySpan& data, int64_t offset = 0) {
    DCHECK_GT(data.type->byte_width(), 0);
    int64_t absolute_byte_offset = (data.offset + offset) * data.type->byte_width();
    return data.GetValues<uint8_t>(1, absolute_byte_offset);
  }

  // Find the match-length of a value within a fixed-width buffer
  static int64_t GetMatchLength(const uint8_t* match_bytes, int64_t match_width,
                                const uint8_t* array_bytes, int64_t offset,
                                int64_t length) {
    int64_t cursor, byte_cursor;
    for (cursor = offset, byte_cursor = match_width * cursor; cursor < length;
         cursor++, byte_cursor += match_width) {
      if (memcmp(match_bytes, array_bytes + byte_cursor,
                 static_cast<size_t>(match_width)) != 0) {
        break;
      }
    }
    return std::min(cursor, length) - offset;
  }

  bool Extend(const void* data) {
    if (ARROW_PREDICT_FALSE(!extend_was_called_)) {
      extend_was_called_ = true;
      return kDefaultExtends;
    }
    return 0 == memcmp(save_key_data_.data(), data, save_key_data_.size());
  }

  void SaveKeyData(const void* data) {
    memcpy(save_key_data_.data(), data, save_key_data_.size());
  }

 private:
  TypeHolder key_type_;
  std::vector<uint8_t> save_key_data_;  // previously seen segment-key grouping data
  bool extend_was_called_;
};

struct AnyKeysSegmenter : public BaseRowSegmenter {
  static Result<std::unique_ptr<RowSegmenter>> Make(
      const std::vector<TypeHolder>& key_types, ExecContext* ctx) {
    ARROW_ASSIGN_OR_RAISE(auto grouper, Grouper::Make(key_types, ctx));  // check types
    return std::make_unique<AnyKeysSegmenter>(key_types, ctx, std::move(grouper));
  }

  AnyKeysSegmenter(const std::vector<TypeHolder>& key_types, ExecContext* ctx,
                   std::unique_ptr<Grouper> grouper)
      : BaseRowSegmenter(key_types),
        grouper_(std::move(grouper)),
        save_group_id_(kNoGroupId) {}

  Status Reset() override {
    ARROW_RETURN_NOT_OK(grouper_->Reset());
    save_group_id_ = kNoGroupId;
    return Status::OK();
  }

  Result<std::vector<Segment>> GetSegments(const ExecSpan& batch) override {
    RETURN_NOT_OK(CheckForGetSegments(batch, {key_types_}));

    if (batch.length == 0) {
      return std::vector<Segment>{};
    }

    // determine if the first segment in this batch extends the last segment in the
    // previous batch
    bool extends = kDefaultExtends;
    if (save_group_id_ != kNoGroupId) {
      // the group id must be computed prior to resetting the grouper, since it is
      // compared to save_group_id_, and after resetting the grouper produces incomparable
      // group ids
      ARROW_ASSIGN_OR_RAISE(auto group_id, MapGroupIdAt(batch));
      // it "extends" unless the group id differs from the last group id
      extends = (group_id == save_group_id_);
    }

    // resetting drops grouper's group-ids, freeing-up memory for the next segment
    RETURN_NOT_OK(grouper_->Reset());

    std::vector<Segment> segments;
    ARROW_ASSIGN_OR_RAISE(auto datum, grouper_->Consume(batch));
    DCHECK(datum.is_array());
    // `data` is an array whose index-0 corresponds to index `offset` of `batch`
    const std::shared_ptr<ArrayData>& data = datum.array();
    DCHECK_EQ(data->length, batch.length);
    DCHECK_EQ(data->GetNullCount(), 0);
    DCHECK_EQ(data->type->id(), GroupIdType::type_id);
    const group_id_t* group_ids = data->GetValues<group_id_t>(1);
    int64_t current_group_offset = 0;
    int64_t cursor;
    for (cursor = 1; cursor < data->length; ++cursor) {
      if (group_ids[cursor] != group_ids[current_group_offset]) {
        segments.push_back(MakeSegment(batch.length, current_group_offset,
                                       cursor - current_group_offset,
                                       current_group_offset == 0 ? extends : false));
        current_group_offset = cursor;
      }
    }
    segments.push_back(MakeSegment(batch.length, current_group_offset,
                                   cursor - current_group_offset,
                                   current_group_offset == 0 ? extends : false));

    // update the save_group_id_ to the last group id in this batch
    save_group_id_ = group_ids[batch.length - 1];

    return segments;
  }

 private:
  // Runs the grouper on a single row.  This is used to determine the group id of the
  // first row of a new segment to see if it extends the previous segment.
  template <typename Batch>
  Result<group_id_t> MapGroupIdAt(const Batch& batch, int64_t offset = 0) {
    ARROW_ASSIGN_OR_RAISE(auto datum, grouper_->Consume(batch, offset,
                                                        /*length=*/1));
    DCHECK(datum.is_array());
    const std::shared_ptr<ArrayData>& data = datum.array();
    DCHECK_EQ(data->GetNullCount(), 0);
    DCHECK_EQ(data->type->id(), GroupIdType::type_id);
    DCHECK_EQ(1, data->length);
    const group_id_t* values = data->GetValues<group_id_t>(1);
    return values[0];
  }

 private:
  std::unique_ptr<Grouper> grouper_;
  group_id_t save_group_id_;
};

}  // namespace

Result<std::unique_ptr<RowSegmenter>> MakeAnyKeysSegmenter(
    const std::vector<TypeHolder>& key_types, ExecContext* ctx) {
  return AnyKeysSegmenter::Make(key_types, ctx);
}

Result<std::unique_ptr<RowSegmenter>> RowSegmenter::Make(
    const std::vector<TypeHolder>& key_types, bool nullable_keys, ExecContext* ctx) {
  if (key_types.size() == 0) {
    return NoKeysSegmenter::Make();
  } else if (!nullable_keys && key_types.size() == 1) {
    const DataType* type = key_types[0].type;
    if (type != NULLPTR && is_fixed_width(*type)) {
      return SimpleKeySegmenter::Make(key_types[0]);
    }
  }
  return AnyKeysSegmenter::Make(key_types, ctx);
}

namespace {

Status CheckAndCapLengthForConsume(int64_t batch_length, int64_t consume_offset,
                                   int64_t* consume_length) {
  if (consume_offset < 0) {
    return Status::Invalid("invalid grouper consume offset: ", consume_offset);
  }
  if (*consume_length < 0) {
    *consume_length = batch_length - consume_offset;
  }
  return Status::OK();
}

enum class GrouperMode { kPopulate, kConsume, kLookup };

struct GrouperImpl : public Grouper {
  static Result<std::unique_ptr<GrouperImpl>> Make(
      const std::vector<TypeHolder>& key_types, ExecContext* ctx) {
    auto impl = std::make_unique<GrouperImpl>();

    impl->encoders_.resize(key_types.size());
    impl->ctx_ = ctx;

    for (size_t i = 0; i < key_types.size(); ++i) {
      // TODO(wesm): eliminate this probably unneeded shared_ptr copy
      std::shared_ptr<DataType> key = key_types[i].GetSharedPtr();

      if (key->id() == Type::BOOL) {
        impl->encoders_[i] = std::make_unique<internal::BooleanKeyEncoder>();
        continue;
      }

      if (key->id() == Type::DICTIONARY) {
        impl->encoders_[i] =
            std::make_unique<internal::DictionaryKeyEncoder>(key, ctx->memory_pool());
        continue;
      }

      if (is_fixed_width(key->id())) {
        impl->encoders_[i] = std::make_unique<internal::FixedWidthKeyEncoder>(key);
        continue;
      }

      if (is_binary_like(key->id())) {
        impl->encoders_[i] =
            std::make_unique<internal::VarLengthKeyEncoder<BinaryType>>(key);
        continue;
      }

      if (is_large_binary_like(key->id())) {
        impl->encoders_[i] =
            std::make_unique<internal::VarLengthKeyEncoder<LargeBinaryType>>(key);
        continue;
      }

      if (key->id() == Type::NA) {
        impl->encoders_[i] = std::make_unique<internal::NullKeyEncoder>();
        continue;
      }

      return Status::NotImplemented("Keys of type ", *key);
    }

    return impl;
  }

  Status Reset() override {
    map_.clear();
    offsets_.clear();
    key_bytes_.clear();
    num_groups_ = 0;
    return Status::OK();
  }

  Status Populate(const ExecSpan& batch, int64_t offset, int64_t length) override {
    return ConsumeImpl(batch, offset, length, GrouperMode::kPopulate).status();
  }

  Result<Datum> Consume(const ExecSpan& batch, int64_t offset, int64_t length) override {
    return ConsumeImpl(batch, offset, length, GrouperMode::kConsume);
  }

  Result<Datum> Lookup(const ExecSpan& batch, int64_t offset, int64_t length) override {
    return ConsumeImpl(batch, offset, length, GrouperMode::kLookup);
  }

  template <typename VisitGroupFunc, typename VisitUnknownGroupFunc>
  void VisitKeys(int64_t length, const int32_t* key_offsets, const uint8_t* key_data,
                 bool insert_new_keys, VisitGroupFunc&& visit_group,
                 VisitUnknownGroupFunc&& visit_unknown_group) {
    for (int64_t i = 0; i < length; ++i) {
      const int32_t key_length = key_offsets[i + 1] - key_offsets[i];
      const uint8_t* key_ptr = key_data + key_offsets[i];
      std::string key(reinterpret_cast<const char*>(key_ptr), key_length);

      uint32_t group_id;
      if (insert_new_keys) {
        const auto [it, inserted] = map_.emplace(std::move(key), num_groups_);
        if (inserted) {
          // New key: update offsets and key_bytes
          ++num_groups_;
          if (key_length > 0) {
            const auto next_key_offset = static_cast<int32_t>(key_bytes_.size());
            key_bytes_.resize(next_key_offset + key_length);
            offsets_.push_back(next_key_offset + key_length);
            memcpy(key_bytes_.data() + next_key_offset, key_ptr, key_length);
          }
        }
        group_id = it->second;
      } else {
        const auto it = map_.find(std::move(key));
        if (it == map_.end()) {
          // Key not found
          visit_unknown_group();
          continue;
        }
        group_id = it->second;
      }
      visit_group(group_id);
    }
  }

  Result<Datum> ConsumeImpl(const ExecSpan& batch, int64_t offset, int64_t length,
                            GrouperMode mode) {
    ARROW_RETURN_NOT_OK(CheckAndCapLengthForConsume(batch.length, offset, &length));
    if (offset != 0 || length != batch.length) {
      auto batch_slice = batch.ToExecBatch().Slice(offset, length);
      return ConsumeImpl(ExecSpan(batch_slice), 0, -1, mode);
    }
    std::vector<int32_t> offsets_batch(batch.length + 1);
    for (int i = 0; i < batch.num_values(); ++i) {
      encoders_[i]->AddLength(batch[i], batch.length, offsets_batch.data());
    }

    int32_t total_length = 0;
    for (int64_t i = 0; i < batch.length; ++i) {
      auto total_length_before = total_length;
      total_length += offsets_batch[i];
      offsets_batch[i] = total_length_before;
    }
    offsets_batch[batch.length] = total_length;

    std::vector<uint8_t> key_bytes_batch(total_length);
    std::vector<uint8_t*> key_buf_ptrs(batch.length);
    for (int64_t i = 0; i < batch.length; ++i) {
      key_buf_ptrs[i] = key_bytes_batch.data() + offsets_batch[i];
    }

    for (int i = 0; i < batch.num_values(); ++i) {
      RETURN_NOT_OK(encoders_[i]->Encode(batch[i], batch.length, key_buf_ptrs.data()));
    }

    if (mode == GrouperMode::kPopulate) {
      VisitKeys(
          batch.length, offsets_batch.data(), key_bytes_batch.data(),
          /*insert_new_keys=*/true,
          /*visit_group=*/[](...) {},
          /*visit_unknown_group=*/[] {});
      return Datum();
    }

    TypedBufferBuilder<uint32_t> group_ids_batch(ctx_->memory_pool());
    RETURN_NOT_OK(group_ids_batch.Resize(batch.length));
    std::shared_ptr<Buffer> null_bitmap;

    if (mode == GrouperMode::kConsume) {
      auto visit_group = [&](uint32_t group_id) {
        group_ids_batch.UnsafeAppend(group_id);
      };
      auto visit_unknown_group = [] {};

      VisitKeys(batch.length, offsets_batch.data(), key_bytes_batch.data(),
                /*insert_new_keys=*/true, visit_group, visit_unknown_group);
    } else {
      DCHECK_EQ(mode, GrouperMode::kLookup);

      // Create a null bitmap to indicate which keys were found.
      TypedBufferBuilder<bool> null_bitmap_builder(ctx_->memory_pool());
      RETURN_NOT_OK(null_bitmap_builder.Resize(batch.length));

      auto visit_group = [&](uint32_t group_id) {
        group_ids_batch.UnsafeAppend(group_id);
        null_bitmap_builder.UnsafeAppend(true);
      };
      auto visit_unknown_group = [&] {
        group_ids_batch.UnsafeAppend(0);  // any defined value really
        null_bitmap_builder.UnsafeAppend(false);
      };

      VisitKeys(batch.length, offsets_batch.data(), key_bytes_batch.data(),
                /*insert_new_keys=*/false, visit_group, visit_unknown_group);

      ARROW_ASSIGN_OR_RAISE(null_bitmap, null_bitmap_builder.Finish());
    }
    ARROW_ASSIGN_OR_RAISE(auto group_ids, group_ids_batch.Finish());
    return Datum(UInt32Array(batch.length, std::move(group_ids), std::move(null_bitmap)));
  }

  uint32_t num_groups() const override { return num_groups_; }

  Result<ExecBatch> GetUniques() override {
    ExecBatch out({}, num_groups_);

    std::vector<uint8_t*> key_buf_ptrs(num_groups_);
    for (int64_t i = 0; i < num_groups_; ++i) {
      key_buf_ptrs[i] = key_bytes_.data() + offsets_[i];
    }

    out.values.resize(encoders_.size());
    for (size_t i = 0; i < encoders_.size(); ++i) {
      ARROW_ASSIGN_OR_RAISE(
          out.values[i],
          encoders_[i]->Decode(key_buf_ptrs.data(), static_cast<int32_t>(num_groups_),
                               ctx_->memory_pool()));
    }

    return out;
  }

  ExecContext* ctx_;
  // TODO We could use std::string_view since the keys are copied in key_bytes_.
  std::unordered_map<std::string, uint32_t> map_;
  std::vector<int32_t> offsets_ = {0};
  std::vector<uint8_t> key_bytes_;
  uint32_t num_groups_ = 0;
  std::vector<std::unique_ptr<internal::KeyEncoder>> encoders_;
};

struct GrouperFastImpl : public Grouper {
  static constexpr int kBitmapPaddingForSIMD = 64;  // bits
  static constexpr int kPaddingForSIMD = 32;        // bytes

  static bool CanUse(const std::vector<TypeHolder>& key_types) {
    if (key_types.size() == 0) {
      return false;
    }
#if ARROW_LITTLE_ENDIAN
    for (size_t i = 0; i < key_types.size(); ++i) {
      if (is_large_binary_like(key_types[i].id())) {
        return false;
      }
    }
    return true;
#else
    return false;
#endif
  }

  static Result<std::unique_ptr<GrouperFastImpl>> Make(
      const std::vector<TypeHolder>& keys, ExecContext* ctx) {
    auto impl = std::make_unique<GrouperFastImpl>();
    impl->ctx_ = ctx;

    RETURN_NOT_OK(impl->temp_stack_.Init(ctx->memory_pool(), 64 * minibatch_size_max_));
    impl->encode_ctx_.hardware_flags =
        arrow::internal::CpuInfo::GetInstance()->hardware_flags();
    impl->encode_ctx_.stack = &impl->temp_stack_;

    auto num_columns = keys.size();
    impl->col_metadata_.resize(num_columns);
    impl->key_types_.resize(num_columns);
    impl->dictionaries_.resize(num_columns);
    for (size_t icol = 0; icol < num_columns; ++icol) {
      const TypeHolder& key = keys[icol];
      if (key.id() == Type::DICTIONARY) {
        auto bit_width = checked_cast<const FixedWidthType&>(*key).bit_width();
        DCHECK_EQ(bit_width % 8, 0);
        impl->col_metadata_[icol] = KeyColumnMetadata(true, bit_width / 8);
      } else if (key.id() == Type::BOOL) {
        impl->col_metadata_[icol] = KeyColumnMetadata(true, 0);
      } else if (is_fixed_width(key.id())) {
        impl->col_metadata_[icol] = KeyColumnMetadata(
            true, checked_cast<const FixedWidthType&>(*key).bit_width() / 8);
      } else if (is_binary_like(key.id())) {
        impl->col_metadata_[icol] = KeyColumnMetadata(false, sizeof(uint32_t));
      } else if (key.id() == Type::NA) {
        impl->col_metadata_[icol] = KeyColumnMetadata(true, 0, /*is_null_type_in=*/true);
      } else {
        return Status::NotImplemented("Keys of type ", *key);
      }
      impl->key_types_[icol] = key;
    }

    impl->encoder_.Init(impl->col_metadata_,
                        /* row_alignment = */ sizeof(uint64_t),
                        /* string_alignment = */ sizeof(uint64_t));
    RETURN_NOT_OK(impl->rows_.Init(ctx->memory_pool(), impl->encoder_.row_metadata()));
    RETURN_NOT_OK(
        impl->rows_minibatch_.Init(ctx->memory_pool(), impl->encoder_.row_metadata()));
    impl->minibatch_size_ = impl->minibatch_size_min_;
    GrouperFastImpl* impl_ptr = impl.get();
    impl->map_equal_impl_ =
        [impl_ptr](int num_keys_to_compare, const uint16_t* selection_may_be_null,
                   const uint32_t* group_ids, uint32_t* out_num_keys_mismatch,
                   uint16_t* out_selection_mismatch, void*) {
          KeyCompare::CompareColumnsToRows(
              num_keys_to_compare, selection_may_be_null, group_ids,
              &impl_ptr->encode_ctx_, out_num_keys_mismatch, out_selection_mismatch,
              impl_ptr->encoder_.batch_all_cols(), impl_ptr->rows_,
              /* are_cols_in_encoding_order=*/true);
        };
    impl->map_append_impl_ = [impl_ptr](int num_keys, const uint16_t* selection, void*) {
      RETURN_NOT_OK(impl_ptr->encoder_.EncodeSelected(&impl_ptr->rows_minibatch_,
                                                      num_keys, selection));
      return impl_ptr->rows_.AppendSelectionFrom(impl_ptr->rows_minibatch_, num_keys,
                                                 nullptr);
    };
    RETURN_NOT_OK(impl->map_.init(impl->encode_ctx_.hardware_flags, ctx->memory_pool()));
    impl->cols_.resize(num_columns);
    impl->minibatch_hashes_.resize(impl->minibatch_size_max_ +
                                   kPaddingForSIMD / sizeof(uint32_t));

    return impl;
  }

  Status Reset() override {
    ARROW_DCHECK_EQ(temp_stack_.AllocatedSize(), 0);
    rows_.Clean();
    rows_minibatch_.Clean();
    map_.cleanup();
    RETURN_NOT_OK(map_.init(encode_ctx_.hardware_flags, ctx_->memory_pool()));
    // TODO: It is now assumed that the dictionaries_ are identical to the first batch
    // throughout the grouper's lifespan so no resetting is needed. But if we want to
    // support different dictionaries for different batches, we need to reset the
    // dictionaries_ here.
    return Status::OK();
  }

  Status Populate(const ExecSpan& batch, int64_t offset, int64_t length) override {
    return ConsumeImpl(batch, offset, length, GrouperMode::kPopulate).status();
  }

  Result<Datum> Consume(const ExecSpan& batch, int64_t offset, int64_t length) override {
    return ConsumeImpl(batch, offset, length, GrouperMode::kConsume);
  }

  Result<Datum> Lookup(const ExecSpan& batch, int64_t offset, int64_t length) override {
    return ConsumeImpl(batch, offset, length, GrouperMode::kLookup);
  }

  Result<Datum> ConsumeImpl(const ExecSpan& batch, int64_t offset, int64_t length,
                            GrouperMode mode) {
    ARROW_RETURN_NOT_OK(CheckAndCapLengthForConsume(batch.length, offset, &length));
    if (offset != 0 || length != batch.length) {
      auto batch_slice = batch.ToExecBatch().Slice(offset, length);
      return ConsumeImpl(ExecSpan(batch_slice), 0, -1, mode);
    }
    // ARROW-14027: broadcast scalar arguments for now
    for (int i = 0; i < batch.num_values(); i++) {
      if (batch[i].is_scalar()) {
        ExecBatch expanded = batch.ToExecBatch();
        for (int j = i; j < expanded.num_values(); j++) {
          if (expanded.values[j].is_scalar()) {
            ARROW_ASSIGN_OR_RAISE(
                expanded.values[j],
                MakeArrayFromScalar(*expanded.values[j].scalar(), expanded.length,
                                    ctx_->memory_pool()));
          }
        }
        return ConsumeImpl(ExecSpan(expanded), mode);
      }
    }
    return ConsumeImpl(batch, mode);
  }

  Result<Datum> ConsumeImpl(const ExecSpan& batch, GrouperMode mode) {
    int64_t num_rows = batch.length;
    int num_columns = batch.num_values();
    // Process dictionaries
    for (int icol = 0; icol < num_columns; ++icol) {
      if (key_types_[icol].id() == Type::DICTIONARY) {
        const ArraySpan& data = batch[icol].array;
        auto dict = MakeArray(data.dictionary().ToArrayData());
        if (dictionaries_[icol]) {
          if (!dictionaries_[icol]->Equals(dict)) {
            // TODO(bkietz) unify if necessary. For now, just error if any batch's
            // dictionary differs from the first we saw for this key
            return Status::NotImplemented("Unifying differing dictionaries");
          }
        } else {
          dictionaries_[icol] = std::move(dict);
        }
      }
    }

    for (int icol = 0; icol < num_columns; ++icol) {
      const uint8_t* non_nulls = NULLPTR;
      const uint8_t* fixedlen = NULLPTR;
      const uint8_t* varlen = NULLPTR;

      // Skip if the key's type is NULL
      if (key_types_[icol].id() != Type::NA) {
        if (batch[icol].array.buffers[0].data != NULLPTR) {
          non_nulls = batch[icol].array.buffers[0].data;
        }
        fixedlen = batch[icol].array.buffers[1].data;
        if (!col_metadata_[icol].is_fixed_length) {
          varlen = batch[icol].array.buffers[2].data;
        }
      }

      int64_t offset = batch[icol].array.offset;

      auto col_base = KeyColumnArray(col_metadata_[icol], offset + num_rows, non_nulls,
                                     fixedlen, varlen);

      cols_[icol] = col_base.Slice(offset, num_rows);
    }

    std::shared_ptr<arrow::Buffer> group_ids, null_bitmap;
    // If we need to return the group ids, then allocate a buffer of group ids
    // for all rows, otherwise each minibatch will reuse the same buffer.
    const int64_t groups_ids_size =
        (mode == GrouperMode::kPopulate) ? minibatch_size_max_ : num_rows;
    ARROW_ASSIGN_OR_RAISE(group_ids, AllocateBuffer(sizeof(uint32_t) * groups_ids_size,
                                                    ctx_->memory_pool()));
    if (mode == GrouperMode::kLookup) {
      ARROW_ASSIGN_OR_RAISE(null_bitmap,
                            AllocateBitmap(groups_ids_size, ctx_->memory_pool()));
    }

    // Split into smaller mini-batches
    //
    for (uint32_t start_row = 0; start_row < num_rows;) {
      uint32_t batch_size_next = std::min(static_cast<uint32_t>(minibatch_size_),
                                          static_cast<uint32_t>(num_rows) - start_row);
      uint32_t* batch_group_ids = group_ids->mutable_data_as<uint32_t>() +
                                  ((mode == GrouperMode::kPopulate) ? 0 : start_row);
      if (mode == GrouperMode::kLookup) {
        // Zero-initialize each mini-batch just before it is partially populated
        // in map_.find() below.
        // This is potentially more cache-efficient than zeroing the entire buffer
        // at once before this loop.
        memset(batch_group_ids, 0, batch_size_next * sizeof(uint32_t));
      }

      // Encode
      rows_minibatch_.Clean();
      encoder_.PrepareEncodeSelected(start_row, batch_size_next, cols_);

      // Compute hash
      Hashing32::HashMultiColumn(encoder_.batch_all_cols(), &encode_ctx_,
                                 minibatch_hashes_.data());

      // Map
      auto match_bitvector =
          util::TempVectorHolder<uint8_t>(&temp_stack_, (batch_size_next + 7) / 8);
      {
        auto local_slots = util::TempVectorHolder<uint8_t>(&temp_stack_, batch_size_next);
        map_.early_filter(batch_size_next, minibatch_hashes_.data(),
                          match_bitvector.mutable_data(), local_slots.mutable_data());
        map_.find(batch_size_next, minibatch_hashes_.data(),
                  match_bitvector.mutable_data(), local_slots.mutable_data(),
                  batch_group_ids, &temp_stack_, map_equal_impl_, nullptr);
      }
      if (mode == GrouperMode::kLookup) {
        // Fill validity bitmap from match_bitvector
        ::arrow::internal::CopyBitmap(match_bitvector.mutable_data(), /*offset=*/0,
                                      /*length=*/batch_size_next,
                                      null_bitmap->mutable_data(),
                                      /*dest_offset=*/start_row);
      } else {
        // Insert new keys
        auto ids = util::TempVectorHolder<uint16_t>(&temp_stack_, batch_size_next);
        int num_ids;
        util::bit_util::bits_to_indexes(0, encode_ctx_.hardware_flags, batch_size_next,
                                        match_bitvector.mutable_data(), &num_ids,
                                        ids.mutable_data());

        RETURN_NOT_OK(map_.map_new_keys(
            num_ids, ids.mutable_data(), minibatch_hashes_.data(), batch_group_ids,
            &temp_stack_, map_equal_impl_, map_append_impl_, nullptr));
      }

      start_row += batch_size_next;
      // XXX why not use minibatch_size_max_ from the start?
      minibatch_size_ = std::min(minibatch_size_max_, 2 * minibatch_size_);
    }

    if (mode == GrouperMode::kPopulate) {
      return Datum{};
    } else {
      return Datum(
          UInt32Array(batch.length, std::move(group_ids), std::move(null_bitmap)));
    }
  }

  uint32_t num_groups() const override { return static_cast<uint32_t>(rows_.length()); }

  // Make sure padded buffers end up with the right logical size

  Result<std::shared_ptr<Buffer>> AllocatePaddedBitmap(int64_t length) {
    ARROW_ASSIGN_OR_RAISE(
        std::shared_ptr<Buffer> buf,
        AllocateBitmap(length + kBitmapPaddingForSIMD, ctx_->memory_pool()));
    return SliceMutableBuffer(std::move(buf), 0, bit_util::BytesForBits(length));
  }

  Result<std::shared_ptr<Buffer>> AllocatePaddedBuffer(int64_t size) {
    ARROW_ASSIGN_OR_RAISE(
        std::shared_ptr<Buffer> buf,
        AllocateBuffer(size + kBitmapPaddingForSIMD, ctx_->memory_pool()));
    return SliceMutableBuffer(std::move(buf), 0, size);
  }

  Result<ExecBatch> GetUniques() override {
    auto num_columns = static_cast<uint32_t>(col_metadata_.size());
    int64_t num_groups = rows_.length();

    std::vector<std::shared_ptr<Buffer>> non_null_bufs(num_columns);
    std::vector<std::shared_ptr<Buffer>> fixedlen_bufs(num_columns);
    std::vector<std::shared_ptr<Buffer>> varlen_bufs(num_columns);

    for (size_t i = 0; i < num_columns; ++i) {
      if (col_metadata_[i].is_null_type) {
        uint8_t* non_nulls = NULLPTR;
        uint8_t* fixedlen = NULLPTR;
        cols_[i] =
            KeyColumnArray(col_metadata_[i], num_groups, non_nulls, fixedlen, NULLPTR);
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(non_null_bufs[i], AllocatePaddedBitmap(num_groups));
      if (col_metadata_[i].is_fixed_length && !col_metadata_[i].is_null_type) {
        if (col_metadata_[i].fixed_length == 0) {
          ARROW_ASSIGN_OR_RAISE(fixedlen_bufs[i], AllocatePaddedBitmap(num_groups));
        } else {
          ARROW_ASSIGN_OR_RAISE(
              fixedlen_bufs[i],
              AllocatePaddedBuffer(num_groups * col_metadata_[i].fixed_length));
        }
      } else {
        ARROW_ASSIGN_OR_RAISE(fixedlen_bufs[i],
                              AllocatePaddedBuffer((num_groups + 1) * sizeof(uint32_t)));
        // Set offset[0] to 0 so the later allocation of varlen_bufs doesn't see an
        // uninitialized value when num_groups == 0.
        reinterpret_cast<uint32_t*>(fixedlen_bufs[i]->mutable_data())[0] = 0;
      }
      cols_[i] =
          KeyColumnArray(col_metadata_[i], num_groups, non_null_bufs[i]->mutable_data(),
                         fixedlen_bufs[i]->mutable_data(), nullptr);
    }

    for (int64_t start_row = 0; start_row < num_groups;) {
      int64_t batch_size_next =
          std::min(num_groups - start_row, static_cast<int64_t>(minibatch_size_max_));
      encoder_.DecodeFixedLengthBuffers(start_row, start_row, batch_size_next, rows_,
                                        &cols_, encode_ctx_.hardware_flags, &temp_stack_);
      start_row += batch_size_next;
    }

    if (!rows_.metadata().is_fixed_length) {
      for (size_t i = 0; i < num_columns; ++i) {
        if (!col_metadata_[i].is_fixed_length) {
          auto varlen_size =
              reinterpret_cast<const uint32_t*>(fixedlen_bufs[i]->data())[num_groups];
          ARROW_ASSIGN_OR_RAISE(varlen_bufs[i], AllocatePaddedBuffer(varlen_size));
          cols_[i] = KeyColumnArray(
              col_metadata_[i], num_groups, non_null_bufs[i]->mutable_data(),
              fixedlen_bufs[i]->mutable_data(), varlen_bufs[i]->mutable_data());
        }
      }

      for (int64_t start_row = 0; start_row < num_groups;) {
        int64_t batch_size_next =
            std::min(num_groups - start_row, static_cast<int64_t>(minibatch_size_max_));
        encoder_.DecodeVaryingLengthBuffers(start_row, start_row, batch_size_next, rows_,
                                            &cols_, encode_ctx_.hardware_flags,
                                            &temp_stack_);
        start_row += batch_size_next;
      }
    }

    ExecBatch out({}, num_groups);
    out.values.resize(num_columns);
    for (size_t i = 0; i < num_columns; ++i) {
      if (col_metadata_[i].is_null_type) {
        out.values[i] = ArrayData::Make(null(), num_groups, {nullptr}, num_groups);
        continue;
      }
      auto valid_count = arrow::internal::CountSetBits(
          non_null_bufs[i]->data(), /*offset=*/0, static_cast<int64_t>(num_groups));
      int null_count = static_cast<int>(num_groups) - static_cast<int>(valid_count);

      if (col_metadata_[i].is_fixed_length) {
        out.values[i] = ArrayData::Make(
            key_types_[i].GetSharedPtr(), num_groups,
            {std::move(non_null_bufs[i]), std::move(fixedlen_bufs[i])}, null_count);
      } else {
        out.values[i] =
            ArrayData::Make(key_types_[i].GetSharedPtr(), num_groups,
                            {std::move(non_null_bufs[i]), std::move(fixedlen_bufs[i]),
                             std::move(varlen_bufs[i])},
                            null_count);
      }
    }

    // Process dictionaries
    for (size_t icol = 0; icol < num_columns; ++icol) {
      if (key_types_[icol].id() == Type::DICTIONARY) {
        if (dictionaries_[icol]) {
          out.values[icol].array()->dictionary = dictionaries_[icol]->data();
        } else {
          ARROW_ASSIGN_OR_RAISE(auto dict,
                                MakeArrayOfNull(key_types_[icol].GetSharedPtr(), 0));
          out.values[icol].array()->dictionary = dict->data();
        }
      }
    }

    return out;
  }

  static constexpr int minibatch_size_max_ = arrow::util::MiniBatch::kMiniBatchLength;
  static constexpr int minibatch_size_min_ = 128;
  int minibatch_size_;

  ExecContext* ctx_;
  arrow::util::TempVectorStack temp_stack_;
  LightContext encode_ctx_;

  std::vector<TypeHolder> key_types_;
  std::vector<KeyColumnMetadata> col_metadata_;
  std::vector<KeyColumnArray> cols_;
  std::vector<uint32_t> minibatch_hashes_;

  std::vector<std::shared_ptr<Array>> dictionaries_;

  RowTableImpl rows_;
  RowTableImpl rows_minibatch_;
  RowTableEncoder encoder_;
  SwissTable map_;
  SwissTable::EqualImpl map_equal_impl_;
  SwissTable::AppendImpl map_append_impl_;
};

}  // namespace

Result<std::unique_ptr<Grouper>> Grouper::Make(const std::vector<TypeHolder>& key_types,
                                               ExecContext* ctx) {
  if (GrouperFastImpl::CanUse(key_types)) {
    return GrouperFastImpl::Make(key_types, ctx);
  }
  return GrouperImpl::Make(key_types, ctx);
}

Result<std::shared_ptr<ListArray>> Grouper::ApplyGroupings(const ListArray& groupings,
                                                           const Array& array,
                                                           ExecContext* ctx) {
  ARROW_ASSIGN_OR_RAISE(Datum sorted,
                        compute::Take(array, groupings.data()->child_data[0],
                                      TakeOptions::NoBoundsCheck(), ctx));

  return std::make_shared<ListArray>(list(array.type()), groupings.length(),
                                     groupings.value_offsets(), sorted.make_array());
}

Result<std::shared_ptr<ListArray>> Grouper::MakeGroupings(const UInt32Array& ids,
                                                          uint32_t num_groups,
                                                          ExecContext* ctx) {
  if (ids.null_count() != 0) {
    return Status::Invalid("MakeGroupings with null ids");
  }

  ARROW_ASSIGN_OR_RAISE(auto offsets, AllocateBuffer(sizeof(int32_t) * (num_groups + 1),
                                                     ctx->memory_pool()));
  auto raw_offsets = reinterpret_cast<int32_t*>(offsets->mutable_data());

  std::memset(raw_offsets, 0, offsets->size());
  for (int i = 0; i < ids.length(); ++i) {
    DCHECK_LT(ids.Value(i), num_groups);
    raw_offsets[ids.Value(i)] += 1;
  }
  int32_t length = 0;
  for (uint32_t id = 0; id < num_groups; ++id) {
    auto offset = raw_offsets[id];
    raw_offsets[id] = length;
    length += offset;
  }
  raw_offsets[num_groups] = length;
  DCHECK_EQ(ids.length(), length);

  ARROW_ASSIGN_OR_RAISE(auto offsets_copy,
                        offsets->CopySlice(0, offsets->size(), ctx->memory_pool()));
  raw_offsets = reinterpret_cast<int32_t*>(offsets_copy->mutable_data());

  ARROW_ASSIGN_OR_RAISE(auto sort_indices, AllocateBuffer(sizeof(int32_t) * ids.length(),
                                                          ctx->memory_pool()));
  auto raw_sort_indices = reinterpret_cast<int32_t*>(sort_indices->mutable_data());
  for (int i = 0; i < ids.length(); ++i) {
    raw_sort_indices[raw_offsets[ids.Value(i)]++] = i;
  }

  return std::make_shared<ListArray>(
      list(int32()), num_groups, std::move(offsets),
      std::make_shared<Int32Array>(ids.length(), std::move(sort_indices)));
}

}  // namespace compute
}  // namespace arrow
