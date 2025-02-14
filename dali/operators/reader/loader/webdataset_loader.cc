// Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "dali/operators/reader/loader/webdataset_loader.h"
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include "dali/core/error_handling.h"
#include "dali/operators/reader/loader/webdataset/tar_utils.h"
#include "dali/pipeline/data/types.h"

namespace dali {

template <typename... Args>
inline std::string IndexFileErrMsg(const std::string& index_path, int64_t line,
                                   const Args&... details) {
  return make_string("Malformed index file at \"", index_path, "\" line ", line, " - ", details...);
}

namespace detail {
namespace wds {

inline MissingExtBehavior ParseMissingExtBehavior(std::string missing_component_behavior) {
  for (auto& c : missing_component_behavior)
    c = std::tolower(static_cast<unsigned char>(c));
  if (missing_component_behavior == "") {
    return MissingExtBehavior::Empty;
  } else if (missing_component_behavior == "skip") {
    return MissingExtBehavior::Skip;
  } else if (missing_component_behavior == "empty") {
    return MissingExtBehavior::Empty;
  } else if (missing_component_behavior == "error") {
    return MissingExtBehavior::Raise;
  } else {
    return MissingExtBehavior::Invalid;
  }
}


inline void ParseSampleDesc(std::vector<SampleDesc>& samples_container,
                            std::vector<ComponentDesc>& components_container,
                            std::ifstream& index_file, const std::string& index_path,
                            int64_t line) {
  // Preparing the SampleDesc
  samples_container.emplace_back();
  samples_container.back().components =
      VectorRange<ComponentDesc>(components_container, components_container.size());

  // Getting the components data
  std::string components_metadata;
  std::getline(index_file, components_metadata);
  std::stringstream extensions_stream(components_metadata);

  // Reading consecutive components
  ComponentDesc component;
  while (extensions_stream >> component.ext) {
    DALI_ENFORCE(extensions_stream >> component.offset >> component.size,
                 IndexFileErrMsg(index_path, line,
                                 "size or offset corresponding to the extension not found"));
    DALI_ENFORCE(
        component.offset % kBlockSize == 0,
        IndexFileErrMsg(index_path, line, "tar offset is not a multiple of tar block size (",
                        kBlockSize, "), perhaps the size value is exported before offset?"));
    components_container.emplace_back(std::move(component));
    samples_container.back().components.num++;
  }

  // Finishing up the SampleDesc
  DALI_ENFORCE(samples_container.back().components.num,
               IndexFileErrMsg(index_path, line, "no extensions provided for the sample"));
}

inline void ParseIndexFile(std::vector<SampleDesc>& samples_container,
                           std::vector<ComponentDesc>& components_container,
                           const std::string& index_path) {
  std::ifstream index_file(index_path);

  // Index Checking
  std::string global_meta;
  getline(index_file, global_meta);
  std::stringstream global_meta_stream(global_meta);
  std::string index_version;
  DALI_ENFORCE(global_meta_stream >> index_version,
               IndexFileErrMsg(index_path, 0, "no version signature found"));
  DALI_ENFORCE(kCurrentIndexVersion == index_version,
               IndexFileErrMsg(
                   index_path, 0,
                   "the version of the index file does not match the expected version (expected: ",
                   kCurrentIndexVersion, " actual: ", index_version, ")"));

  // Getting the number of samples in the index file
  int64_t sample_desc_num_signed;
  DALI_ENFORCE(global_meta_stream >> sample_desc_num_signed,
               IndexFileErrMsg(index_path, 0, "no sample count found"));
  DALI_ENFORCE(sample_desc_num_signed > 0,
               IndexFileErrMsg(index_path, 0, "sample count must be positive"));

  const size_t sample_desc_num = sample_desc_num_signed;
  samples_container.reserve(samples_container.size() + sample_desc_num);
  for (size_t sample_index = 0; sample_index < sample_desc_num; sample_index++) {
    ParseSampleDesc(samples_container, components_container, index_file, index_path,
                    sample_index + 1);
  }
}

}  // namespace wds
}  // namespace detail

inline std::string SupportedTypesListGen() {
  std::stringstream out;
  for (auto& dtype : detail::wds::kSupportedTypes) {
    out << dtype << ", ";
  }
  std::string out_str = out.str();
  return out_str.substr(0, out_str.size() - 2 * (detail::wds::kSupportedTypes.size() > 0));
}

WebdatasetLoader::WebdatasetLoader(const OpSpec& spec)
    : Loader(spec),
      paths_(spec.GetRepeatedArgument<std::string>("paths")),
      index_paths_(spec.GetRepeatedArgument<std::string>("index_paths")),
      missing_component_behavior_(detail::wds::ParseMissingExtBehavior(
          spec.GetArgument<std::string>("missing_component_behavior"))) {
  DALI_ENFORCE(paths_.size() == index_paths_.size(),
               "Number of webdataset archives does not match the number of index files");
  DALI_ENFORCE(paths_.size() > 0, "No webdataset archives provided");
  DALI_ENFORCE(missing_component_behavior_ != detail::wds::MissingExtBehavior::Invalid,
               make_string("Invalid value for missing_component_behavior '",
                           spec.GetArgument<std::string>("missing_component_behavior"),
                           "' possible values are: skip, error, empty"));

  std::vector<std::string> samples_exts = spec.GetRepeatedArgument<std::string>("ext");
  ext_.reserve(samples_exts.size());

  // splitting extension bundles by the delimiter
  for (size_t exts_idx = 0; exts_idx < samples_exts.size(); exts_idx++) {
    std::stringstream exts_stream(samples_exts[exts_idx]);
    std::string ext;
    ext_.emplace_back();
    while (std::getline(exts_stream, ext, detail::wds::kExtDelim)) {
      if (!ext_.back().count(ext)) {
        ext_.back().insert(ext);
      }
    }
  }

  dtypes_ = spec.HasArgument("dtypes") ? spec.GetRepeatedArgument<DALIDataType>("dtypes")
                                       : std::vector<DALIDataType>(ext_.size(), DALI_UINT8);

  for (DALIDataType dtype : dtypes_) {
    DALI_ENFORCE(detail::wds::kSupportedTypes.count(dtype),
                 make_string("Unsupported output dtype ", dtype,
                             ". Supported types are: ", SupportedTypesListGen()));
  }
  DALI_ENFORCE(ext_.size() == dtypes_.size(),
               "Number of extensions does not match the number of provided types");
}

WebdatasetLoader::~WebdatasetLoader() {}

void WebdatasetLoader::PrepareEmpty(vector<Tensor<CPUBackend>>& empty) {
  empty = std::vector<Tensor<CPUBackend>>(ext_.size());
  for (size_t output_index = 0; output_index < ext_.size(); output_index++) {
    empty[output_index].set_pinned(false);
    empty[output_index].reserve(tensor_init_bytes_);
    empty[output_index].set_type(dtypes_[output_index]);
  }
}

inline std::string GetExtension(const std::string& filepath) {
  const size_t dot_pos = filepath.find_first_of('.', filepath.find_last_of('/') + 1);
  return filepath.substr(dot_pos + 1);
}

void WebdatasetLoader::ReadSample(vector<Tensor<CPUBackend>>& sample) {
  MoveToNextShard(sample_index_);
  detail::wds::SampleDesc& current_sample = samples_[sample_index_];
  auto& current_wds_shard = wds_shards_[current_sample.wds_shard_index];

  for (auto& component : current_sample.components) {
    // Checking if the component data from the index file agrees with reality
    const auto& index_path = index_paths_[current_sample.wds_shard_index];
    DALI_ENFORCE(component.offset < static_cast<int64_t>(current_wds_shard->Size()),
                 IndexFileErrMsg(index_path, current_sample.line_number,
                                 "offset is outside of the archive file"));

    current_wds_shard->Seek(component.offset);

    // Skipping cached samples
    const std::string source_info =
        make_string("archive ", paths_[current_sample.wds_shard_index], "index file \"",
                    index_paths_[current_sample.wds_shard_index], "\" line ",
                    current_sample.line_number, "component offset ", component.offset);
    DALIMeta meta;
    meta.SetSourceInfo(source_info);
    if (ShouldSkipImage(source_info)) {
      meta.SetSkipSample(true);
      for (auto& output : component.outputs) {
        sample[output].Reset();
        sample[output].SetMeta(meta);
        sample[output].Resize({0}, dtypes_[output]);
      }
      continue;
    }
    // Reading Data
    if (copy_read_data_) {
      uint8_t* shared_tensor_data = nullptr;
      for (auto& output : component.outputs) {
        if (!shared_tensor_data) {
          if (sample[output].shares_data()) {
            sample[output].Reset();
          }
          sample[output].Resize(
              {static_cast<int64_t>(component.size / sample[output].type_info().size())},
              dtypes_[output]);
          shared_tensor_data = reinterpret_cast<uint8_t*>(sample[output].raw_mutable_data());
        } else {
          sample[output].ShareData(
              shared_tensor_data, component.size,
              {static_cast<int64_t>(component.size / sample[output].type_info().size())},
              sample[output].type());
        }
      }
      DALI_ENFORCE(current_wds_shard->Read(shared_tensor_data, component.size) == component.size,
                   "Error reading from a file " + paths_[current_sample.wds_shard_index]);
    } else {
      auto data = current_wds_shard->Get(component.size);
      for (auto& output : component.outputs) {
        sample[output].SetMeta(meta);
        sample[output].ShareData(
            data, component.size,
            {static_cast<int64_t>(component.size / sample[output].type_info().size())},
            sample[output].type());
      }
    }
  }

  // Setting non-filled outputs
  for (auto& empty_output : current_sample.empty_outputs) {
    sample[empty_output].Reset();
    sample[empty_output].Resize({0}, dtypes_[empty_output]);
  }
  sample_index_++;
}

Index WebdatasetLoader::SizeImpl() {
  return samples_.size();
}

void WebdatasetLoader::PrepareMetadataImpl() {
  if (!dont_use_mmap_) {
    mmap_reserver_ = FileStream::MappingReserver(static_cast<unsigned int>(paths_.size()));
  }
  copy_read_data_ = dont_use_mmap_ || !mmap_reserver_.CanShareMappedData();

  // initializing all the readers
  wds_shards_.reserve(paths_.size());
  for (auto& uri : paths_) {
    wds_shards_.emplace_back(FileStream::Open(uri, read_ahead_, !copy_read_data_));
  }

  // preparing the map from extensions to outputs
  std::unordered_map<std::string, std::vector<size_t>> ext_map;
  for (size_t output_index = 0; output_index < ext_.size(); output_index++) {
    for (auto& ext : ext_[output_index]) {
      ext_map[ext].push_back(output_index);
    }
  }

  // collecting and filtering the index files
  std::vector<detail::wds::SampleDesc> unfiltered_samples;
  std::vector<detail::wds::ComponentDesc> unfiltered_components;
  bitmask was_output_set;
  was_output_set.resize(ext_.size(), false);
  output_indicies_.reserve(ext_.size());

  std::vector<size_t> dtype_sizes_(dtypes_.size());
  for (size_t i = 0; i < dtypes_.size(); i++)
    dtype_sizes_[i] = TypeTable::GetTypeInfo(dtypes_[i]).size();

  for (size_t wds_shard_index = 0; wds_shard_index < index_paths_.size(); wds_shard_index++) {
    unfiltered_samples.resize(0);
    unfiltered_components.resize(0);
    detail::wds::ParseIndexFile(unfiltered_samples, unfiltered_components,
                                index_paths_[wds_shard_index]);

    for (auto& sample : unfiltered_samples) {
      detail::wds::SampleDesc new_sample{
          detail::wds::VectorRange<detail::wds::ComponentDesc>(components_, components_.size()),
          detail::wds::VectorRange<size_t>(empty_outputs_, empty_outputs_.size()), wds_shard_index,
          sample.line_number};

      size_t start_outputs_index = output_indicies_.size();

      for (auto& component : sample.components) {
        component.outputs =
            detail::wds::VectorRange<size_t>(output_indicies_, output_indicies_.size());
        for (auto& output : ext_map[component.ext]) {
          if (!was_output_set[output]) {
            DALI_ENFORCE(
                component.size % dtype_sizes_[output] == 0,
                make_string("Error in index file at \"", index_paths_[wds_shard_index], "\" line ",
                            sample.line_number, " - component size and dtype incompatible"));
            output_indicies_.push_back(output);
            component.outputs.num++;
            was_output_set[output] = true;
          } else {
            std::call_once(multiple_files_single_component, [&]() {
              DALI_WARN(make_string("Multiple components matching output ",
                                    output, " at line ", sample.line_number, " file \"",
                                    index_paths_[wds_shard_index], "\"."));
            });
          }
        }
        if (component.outputs.num) {
          components_.push_back(std::move(component));
          new_sample.components.num++;
        }
      }

      if (new_sample.components.num < ext_.size()) {
        switch (missing_component_behavior_) {
          case detail::wds::MissingExtBehavior::Empty:
            for (size_t output = 0; output < ext_.size(); output++) {
              if (!was_output_set[output]) {
                empty_outputs_.push_back(output);
                new_sample.empty_outputs.num++;
              }
            }
            samples_.push_back(new_sample);
            break;
          case detail::wds::MissingExtBehavior::Skip:
            components_.resize(new_sample.components.start);
            output_indicies_.resize(start_outputs_index);
            break;
          case detail::wds::MissingExtBehavior::Raise:
            DALI_FAIL(make_string("Underful sample detected at \"", index_paths_[wds_shard_index],
                                  "\" line ", sample.line_number));
            break;
          default:
            break;
        }
      } else {
        samples_.push_back(new_sample);
      }
      was_output_set.fill(false);
    }
  }
  sample_index_ = start_index(shard_id_, num_shards_, samples_.size());
}

void WebdatasetLoader::Reset(bool wrap_to_shard) {
  sample_index_ = wrap_to_shard ? start_index(shard_id_, num_shards_, samples_.size()) : 0;
}

}  // namespace dali
