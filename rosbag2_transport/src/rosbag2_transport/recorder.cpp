// Copyright 2018 Open Source Robotics Foundation, Inc.
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

#include "rosbag2_transport/recorder.hpp"

#include <algorithm>
#include <future>
#include <memory>
#include <regex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rclcpp/logging.hpp"
#include "rclcpp/clock.hpp"

#include "rosbag2_cpp/writer.hpp"

#include "rosbag2_interfaces/srv/snapshot.hpp"

#include "qos.hpp"
#include "topic_filter.hpp"

#ifdef _WIN32
// This is necessary because of a bug in yaml-cpp's cmake
#define YAML_CPP_DLL
// This is necessary because yaml-cpp does not always use dllimport/dllexport consistently
# pragma warning(push)
# pragma warning(disable:4251)
# pragma warning(disable:4275)
#endif
#include "yaml-cpp/yaml.h"
#ifdef _WIN32
# pragma warning(pop)
#endif

namespace rosbag2_transport
{

Recorder::Recorder(
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: rclcpp::Node(node_name, node_options)
{
  // TODO(karsten1987): Use this constructor later with parameter parsing.
  // The reader, storage_options as well as record_options can be loaded via parameter.
  // That way, the recorder can be used as a simple component in a component manager.
}

Recorder::Recorder(
  std::shared_ptr<rosbag2_cpp::Writer> writer,
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::RecordOptions & record_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: rclcpp::Node(node_name, rclcpp::NodeOptions(node_options).start_parameter_event_publisher(false)),
  writer_(std::move(writer)),
  storage_options_(storage_options),
  record_options_(record_options),
  stop_discovery_(record_options_.is_discovery_disabled)
{
}

Recorder::~Recorder()
{
  stop_discovery_ = true;
  if (discovery_future_.valid()) {
    discovery_future_.wait();
  }

  subscriptions_.clear();
}

void Recorder::record()
{
  topic_qos_profile_overrides_ = record_options_.topic_qos_profile_overrides;
  if (record_options_.rmw_serialization_format.empty()) {
    throw std::runtime_error("No serialization format specified!");
  }

  writer_->open(
    storage_options_,
    {rmw_get_serialization_format(), record_options_.rmw_serialization_format});

  // Only expose snapshot service when mode is enabled
  if (storage_options_.snapshot_mode) {
    srv_snapshot_ = create_service<rosbag2_interfaces::srv::Snapshot>(
      "~/snapshot",
      [this](
        const std::shared_ptr<rmw_request_id_t>/* request_header */,
        const std::shared_ptr<rosbag2_interfaces::srv::Snapshot::Request>/* request */,
        const std::shared_ptr<rosbag2_interfaces::srv::Snapshot::Response> response)
      {
        response->success = writer_->take_snapshot();
      });
  }

  serialization_format_ = record_options_.rmw_serialization_format;
  RCLCPP_INFO(this->get_logger(), "Listening for topics...");
  subscribe_topics(get_requested_or_available_topics());

  if (!record_options_.is_discovery_disabled) {
    discovery_future_ =
      std::async(std::launch::async, std::bind(&Recorder::topics_discovery, this));
  }
}

const rosbag2_cpp::Writer & Recorder::get_writer_handle()
{
  return *writer_;
}

void Recorder::topics_discovery()
{
  while (rclcpp::ok() && stop_discovery_ == false) {
    auto topics_to_subscribe =
      get_requested_or_available_topics();
    for (const auto & topic_and_type : topics_to_subscribe) {
      warn_if_new_qos_for_subscribed_topic(topic_and_type.first);
    }
    auto missing_topics = get_missing_topics(topics_to_subscribe);
    subscribe_topics(missing_topics);

    if (!record_options_.topics.empty() && subscriptions_.size() == record_options_.topics.size()) {
      RCLCPP_INFO(
        this->get_logger(),
        "All requested topics are subscribed. Stopping discovery...");
      return;
    }
    std::this_thread::sleep_for(record_options_.topic_polling_interval);
  }
}

std::unordered_map<std::string, std::string>
Recorder::get_requested_or_available_topics()
{
  auto all_topics_and_types = this->get_topic_names_and_types();
  auto filtered_topics_and_types = topic_filter::filter_topics_with_more_than_one_type(
    all_topics_and_types, record_options_.include_hidden_topics);

  filtered_topics_and_types = topic_filter::filter_topics_with_known_type(
    filtered_topics_and_types, topic_unknown_types_);

  if (!record_options_.topics.empty()) {
    // expand specified topics
    std::vector<std::string> expanded_topics;
    expanded_topics.reserve(record_options_.topics.size());
    for (const auto & topic : record_options_.topics) {
      expanded_topics.push_back(
        rclcpp::expand_topic_or_service_name(
          topic, this->get_name(), this->get_namespace(), false));
    }
    filtered_topics_and_types = topic_filter::filter_topics(
      expanded_topics, filtered_topics_and_types);
  }

  if (record_options_.regex.empty() && record_options_.exclude.empty()) {
    return filtered_topics_and_types;
  }

  return topic_filter::filter_topics_using_regex(
    filtered_topics_and_types,
    record_options_.regex,
    record_options_.exclude,
    record_options_.all
  );
}

std::unordered_map<std::string, std::string>
Recorder::get_missing_topics(const std::unordered_map<std::string, std::string> & all_topics)
{
  std::unordered_map<std::string, std::string> missing_topics;
  for (const auto & i : all_topics) {
    if (subscriptions_.find(i.first) == subscriptions_.end()) {
      missing_topics.emplace(i.first, i.second);
    }
  }
  return missing_topics;
}


void Recorder::subscribe_topics(
  const std::unordered_map<std::string, std::string> & topics_and_types)
{
  for (const auto & topic_with_type : topics_and_types) {
    subscribe_topic(
      {
        topic_with_type.first,
        topic_with_type.second,
        serialization_format_,
        serialized_offered_qos_profiles_for_topic(topic_with_type.first)
      });
  }
}

void Recorder::subscribe_topic(const rosbag2_storage::TopicMetadata & topic)
{
  // Need to create topic in writer before we are trying to create subscription. Since in
  // callback for subscription we are calling writer_->write(bag_message); and it could happened
  // that callback called before we reached out the line: writer_->create_topic(topic)
  writer_->create_topic(topic);

  Rosbag2QoS subscription_qos{subscription_qos_for_topic(topic.name)};
  auto subscription = create_subscription(topic.name, topic.type, subscription_qos);
  if (subscription) {
    subscriptions_.insert({topic.name, subscription});
    RCLCPP_INFO_STREAM(
      this->get_logger(),
      "Subscribed to topic '" << topic.name << "'");
  } else {
    writer_->remove_topic(topic);
    subscriptions_.erase(topic.name);
  }
}

std::shared_ptr<rclcpp::GenericSubscription>
Recorder::create_subscription(
  const std::string & topic_name, const std::string & topic_type, const rclcpp::QoS & qos)
{
  auto subscription = this->create_generic_subscription(
    topic_name,
    topic_type,
    qos,
    [this, topic_name, topic_type](std::shared_ptr<rclcpp::SerializedMessage> message) {
      writer_->write(message, topic_name, topic_type, rclcpp::Clock(RCL_SYSTEM_TIME).now());
    });
  return subscription;
}

std::string Recorder::serialized_offered_qos_profiles_for_topic(const std::string & topic_name)
{
  YAML::Node offered_qos_profiles;
  auto endpoints = this->get_publishers_info_by_topic(topic_name);
  for (const auto & info : endpoints) {
    offered_qos_profiles.push_back(Rosbag2QoS(info.qos_profile()));
  }
  return YAML::Dump(offered_qos_profiles);
}

rclcpp::QoS Recorder::subscription_qos_for_topic(const std::string & topic_name) const
{
  if (topic_qos_profile_overrides_.count(topic_name)) {
    RCLCPP_INFO_STREAM(
      this->get_logger(),
      "Overriding subscription profile for " << topic_name);
    return topic_qos_profile_overrides_.at(topic_name);
  }
  return Rosbag2QoS::adapt_request_to_offers(
    topic_name, this->get_publishers_info_by_topic(topic_name));
}

void Recorder::warn_if_new_qos_for_subscribed_topic(const std::string & topic_name)
{
  auto existing_subscription = subscriptions_.find(topic_name);
  if (existing_subscription == subscriptions_.end()) {
    // Not subscribed yet
    return;
  }
  if (topics_warned_about_incompatibility_.count(topic_name) > 0) {
    // Already warned about this topic
    return;
  }
  const auto actual_qos = existing_subscription->second->get_actual_qos();
  const auto & used_profile = actual_qos.get_rmw_qos_profile();
  auto publishers_info = this->get_publishers_info_by_topic(topic_name);
  for (const auto & info : publishers_info) {
    auto new_profile = info.qos_profile().get_rmw_qos_profile();
    bool incompatible_reliability =
      new_profile.reliability == RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT &&
      used_profile.reliability != RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
    bool incompatible_durability =
      new_profile.durability == RMW_QOS_POLICY_DURABILITY_VOLATILE &&
      used_profile.durability != RMW_QOS_POLICY_DURABILITY_VOLATILE;

    if (incompatible_reliability) {
      RCLCPP_WARN_STREAM(
        this->get_logger(),
        "A new publisher for subscribed topic " << topic_name << " "
          "was found offering RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT, "
          "but rosbag already subscribed requesting RMW_QOS_POLICY_RELIABILITY_RELIABLE. "
          "Messages from this new publisher will not be recorded.");
      topics_warned_about_incompatibility_.insert(topic_name);
    } else if (incompatible_durability) {
      RCLCPP_WARN_STREAM(
        this->get_logger(),
        "A new publisher for subscribed topic " << topic_name << " "
          "was found offering RMW_QOS_POLICY_DURABILITY_VOLATILE, "
          "but rosbag2 already subscribed requesting RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL. "
          "Messages from this new publisher will not be recorded.");
      topics_warned_about_incompatibility_.insert(topic_name);
    }
  }
}

}  // namespace rosbag2_transport
