// Copyright 2021 Amazon.com Inc or its Affiliates
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

#ifndef ROSBAG2_CPP__REWRITE_HPP_
#define ROSBAG2_CPP__REWRITE_HPP_

#include <memory>
#include <vector>

#include "rosbag2_cpp/reader.hpp"
#include "rosbag2_cpp/writer.hpp"
#include "rosbag2_storage/storage_options.hpp"

namespace rosbag2_cpp
{

/// Given one or more existing rosbag2, write out one or more new bags with new settings.
/// This generic feature enables (but is not limited to) the following features:
/// - merge (multiple input bags, one output bag)
/// - split (one input bag, one output bag with some size or duration splitting values)
/// - filter (input bag(s) - output bag(s) accept different topics)
/// - compress
/// - serialization format conversion
///
/// \param input_bags - a vector of Readers for bag inputs. Must already be "open"ed
/// \param output_bags - full "recording" configuration of the bag(s) to output.
///   Must already be "open"ed
///   Each output bag will be passed every message from each input bag in timestamp order,
///   and is responsible for being configured to flter/process as needed.
// void rewrite(
//   const std::vector<std::string> & input_bags,
//   const std::vector<WriteOptions> & output_bags);

void rewrite(
  const std::vector<std::unique_ptr<rosbag2_cpp::Reader>> & input_bags,
  const std::vector<std::unique_ptr<rosbag2_cpp::Writer>> & output_bags);

}  // namespace rosbag2_cpp

#endif  // ROSBAG2_CPP__REWRITE_HPP_
