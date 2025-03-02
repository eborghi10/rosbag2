// Copyright 2018, Bosch Software Innovations GmbH.
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

#include "rosbag2_transport/player.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rcl/graph.h"

#include "rclcpp/rclcpp.hpp"

#include "rcutils/time.h"

#include "rosbag2_cpp/clocks/time_controller_clock.hpp"
#include "rosbag2_cpp/reader.hpp"
#include "rosbag2_cpp/typesupport_helpers.hpp"

#include "rosbag2_storage/storage_filter.hpp"

#include "qos.hpp"

namespace
{
/**
 * Trivial std::unique_lock wrapper providing constructor that allows Clang Thread Safety Analysis.
 * The std::unique_lock does not have these annotations.
 */
class RCPPUTILS_TSA_SCOPED_CAPABILITY TSAUniqueLock : public std::unique_lock<std::mutex>
{
public:
  explicit TSAUniqueLock(std::mutex & mu) RCPPUTILS_TSA_ACQUIRE(mu)
  : std::unique_lock<std::mutex>(mu)
  {}

  ~TSAUniqueLock() RCPPUTILS_TSA_RELEASE() {}
};

/**
 * Determine which QoS to offer for a topic.
 * The priority of the profile selected is:
 *   1. The override specified in play_options (if one exists for the topic).
 *   2. A profile automatically adapted to the recorded QoS profiles of publishers on the topic.
 *
 * \param topic_name The full name of the topic, with namespace (ex. /arm/joint_status).
 * \param topic_qos_profile_overrides A map of topic to QoS profile overrides.
 * @return The QoS profile to be used for subscribing.
 */
rclcpp::QoS publisher_qos_for_topic(
  const rosbag2_storage::TopicMetadata & topic,
  const std::unordered_map<std::string, rclcpp::QoS> & topic_qos_profile_overrides,
  const rclcpp::Logger & logger)
{
  using rosbag2_transport::Rosbag2QoS;
  auto qos_it = topic_qos_profile_overrides.find(topic.name);
  if (qos_it != topic_qos_profile_overrides.end()) {
    RCLCPP_INFO_STREAM(
      logger,
      "Overriding QoS profile for topic " << topic.name);
    return Rosbag2QoS{qos_it->second};
  } else if (topic.offered_qos_profiles.empty()) {
    return Rosbag2QoS{};
  }

  const auto profiles_yaml = YAML::Load(topic.offered_qos_profiles);
  const auto offered_qos_profiles = profiles_yaml.as<std::vector<Rosbag2QoS>>();
  return Rosbag2QoS::adapt_offer_to_recorded_offers(topic.name, offered_qos_profiles);
}
}  // namespace

namespace rosbag2_transport
{

Player::Player(const std::string & node_name, const rclcpp::NodeOptions & node_options)
: rclcpp::Node(node_name, node_options)
{
  // TODO(karsten1987): Use this constructor later with parameter parsing.
  // The reader, storage_options as well as play_options can be loaded via parameter.
  // That way, the player can be used as a simple component in a component manager.
  throw rclcpp::exceptions::UnimplementedError();
}

Player::Player(
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::PlayOptions & play_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: Player(std::make_unique<rosbag2_cpp::Reader>(),
    storage_options, play_options,
    node_name, node_options)
{}

Player::Player(
  std::unique_ptr<rosbag2_cpp::Reader> reader,
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::PlayOptions & play_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: Player(std::move(reader),
    // only call KeyboardHandler when using default keyboard handler implementation
    std::shared_ptr<KeyboardHandler>(new KeyboardHandler()),
    storage_options, play_options,
    node_name, node_options)
{}

Player::Player(
  std::unique_ptr<rosbag2_cpp::Reader> reader,
  std::shared_ptr<KeyboardHandler> keyboard_handler,
  const rosbag2_storage::StorageOptions & storage_options,
  const rosbag2_transport::PlayOptions & play_options,
  const std::string & node_name,
  const rclcpp::NodeOptions & node_options)
: rclcpp::Node(
    node_name,
    rclcpp::NodeOptions(node_options).arguments(play_options.topic_remapping_options)),
  storage_options_(storage_options),
  play_options_(play_options),
  keyboard_handler_(keyboard_handler)
{
  {
    std::lock_guard<std::mutex> lk(reader_mutex_);
    reader_ = std::move(reader);
    // keep reader open until player is destroyed
    reader_->open(storage_options_, {"", rmw_get_serialization_format()});
    auto metadata = reader_->get_metadata();
    starting_time_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
      metadata.starting_time.time_since_epoch()).count();
    clock_ = std::make_unique<rosbag2_cpp::TimeControllerClock>(starting_time_);
    set_rate(play_options_.rate);
    topic_qos_profile_overrides_ = play_options_.topic_qos_profile_overrides;
    prepare_publishers();
  }
  create_control_services();
  add_keyboard_callbacks();
}

Player::~Player()
{
  // remove callbacks on key_codes to prevent race conditions
  // Note: keyboard_handler handles locks between removing & executing callbacks
  for (auto cb_handle : keyboard_callbacks_) {
    keyboard_handler_->delete_key_press_callback(cb_handle);
  }
  // closes reader
  std::lock_guard<std::mutex> lk(reader_mutex_);
  if (reader_) {
    reader_->close();
  }
}

const std::chrono::milliseconds
Player::queue_read_wait_period_ = std::chrono::milliseconds(100);

bool Player::is_storage_completely_loaded() const
{
  if (storage_loading_future_.valid() &&
    storage_loading_future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    storage_loading_future_.get();
  }
  return !storage_loading_future_.valid();
}

void Player::play()
{
  rclcpp::Duration delay(0, 0);
  if (play_options_.delay >= rclcpp::Duration(0, 0)) {
    delay = play_options_.delay;
  } else {
    RCLCPP_WARN_STREAM(
      get_logger(),
      "Invalid delay value: " << play_options_.delay.nanoseconds() << ". Delay is disabled.");
  }

  try {
    do {
      if (delay > rclcpp::Duration(0, 0)) {
        RCLCPP_INFO_STREAM(get_logger(), "Sleep " << delay.nanoseconds() << " ns");
        std::chrono::nanoseconds duration(delay.nanoseconds());
        std::this_thread::sleep_for(duration);
      }
      {
        std::lock_guard<std::mutex> lk(reader_mutex_);
        reader_->seek(starting_time_);
        clock_->jump(starting_time_);
      }
      storage_loading_future_ = std::async(std::launch::async, [this]() {load_storage_content();});
      wait_for_filled_queue();
      play_messages_from_queue();
      {
        std::lock_guard<std::mutex> lk(ready_to_play_from_queue_mutex_);
        is_ready_to_play_from_queue_ = false;
        ready_to_play_from_queue_cv_.notify_all();
      }
    } while (rclcpp::ok() && play_options_.loop);
  } catch (std::runtime_error & e) {
    RCLCPP_ERROR(get_logger(), "Failed to play: %s", e.what());
  }
  std::lock_guard<std::mutex> lk(ready_to_play_from_queue_mutex_);
  is_ready_to_play_from_queue_ = false;
  ready_to_play_from_queue_cv_.notify_all();
}

void Player::pause()
{
  clock_->pause();
  RCLCPP_INFO_STREAM(get_logger(), "Pausing play.");
}

void Player::resume()
{
  clock_->resume();
  RCLCPP_INFO_STREAM(get_logger(), "Resuming play.");
}

void Player::toggle_paused()
{
  is_paused() ? resume() : pause();
}

bool Player::is_paused() const
{
  return clock_->is_paused();
}

double Player::get_rate() const
{
  return clock_->get_rate();
}

bool Player::set_rate(double rate)
{
  bool ok = clock_->set_rate(rate);
  if (ok) {
    RCLCPP_INFO_STREAM(get_logger(), "Set rate to " << rate);
  } else {
    RCLCPP_WARN_STREAM(get_logger(), "Failed to set rate to invalid value " << rate);
  }
  return ok;
}

rosbag2_storage::SerializedBagMessageSharedPtr * Player::peek_next_message_from_queue()
{
  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = message_queue_.peek();
  if (message_ptr == nullptr && !is_storage_completely_loaded() && rclcpp::ok()) {
    RCLCPP_WARN(
      get_logger(),
      "Message queue starved. Messages will be delayed. Consider "
      "increasing the --read-ahead-queue-size option.");
    while (message_ptr == nullptr && !is_storage_completely_loaded() && rclcpp::ok()) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
      message_ptr = message_queue_.peek();
    }
  }
  // Workaround for race condition between peek and is_storage_completely_loaded()
  // Don't sync with mutex for the sake of the performance
  if (message_ptr == nullptr) {
    message_ptr = message_queue_.peek();
  }
  return message_ptr;
}

bool Player::play_next()
{
  if (!clock_->is_paused()) {
    RCLCPP_WARN_STREAM(get_logger(), "Called play next, but not in paused state.");
    return false;
  }

  RCLCPP_INFO_STREAM(get_logger(), "Playing next message.");

  // Temporary take over playback from play_messages_from_queue()
  std::lock_guard<std::mutex> main_play_loop_lk(skip_message_in_main_play_loop_mutex_);
  skip_message_in_main_play_loop_ = true;
  // Wait for player to be ready for playback messages from queue i.e. wait for Player:play() to
  // be called if not yet and queue to be filled with messages.
  {
    std::unique_lock<std::mutex> lk(ready_to_play_from_queue_mutex_);
    ready_to_play_from_queue_cv_.wait(lk, [this] {return is_ready_to_play_from_queue_;});
  }

  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = peek_next_message_from_queue();

  bool next_message_published = false;
  while (message_ptr != nullptr && !next_message_published) {
    {
      rosbag2_storage::SerializedBagMessageSharedPtr message = *message_ptr;
      next_message_published = publish_message(message);
      clock_->jump(message->time_stamp);
    }
    message_queue_.pop();
    message_ptr = peek_next_message_from_queue();
  }
  return next_message_published;
}

void Player::seek(rcutils_time_point_value_t time_point)
{
  // Temporary stop playback in play_messages_from_queue() and block play_next()
  std::lock_guard<std::mutex> main_play_loop_lk(skip_message_in_main_play_loop_mutex_);
  skip_message_in_main_play_loop_ = true;
  // Wait for player to be ready for playback messages from queue i.e. wait for Player:play() to
  // be called if not yet and queue to be filled with messages.
  {
    std::unique_lock<std::mutex> lk(ready_to_play_from_queue_mutex_);
    ready_to_play_from_queue_cv_.wait(lk, [this] {return is_ready_to_play_from_queue_;});
  }
  cancel_wait_for_next_message_ = true;
  // if given seek value is earlier than the beginning of the bag, then clamp
  // it to the beginning of the bag
  if (time_point < starting_time_) {
    time_point = starting_time_;
  }
  {
    std::lock_guard<std::mutex> lk(reader_mutex_);
    // Purge current messages in queue.
    while (message_queue_.pop()) {}
    reader_->seek(time_point);
    clock_->jump(time_point);
    // Restart queuing thread if it has finished running (previously reached end of bag),
    // otherwise, queueing should continue automatically after releasing mutex
    if (is_storage_completely_loaded() && rclcpp::ok()) {
      storage_loading_future_ =
        std::async(std::launch::async, [this]() {load_storage_content();});
    }
  }
}

void Player::wait_for_filled_queue() const
{
  while (
    message_queue_.size_approx() < play_options_.read_ahead_queue_size &&
    !is_storage_completely_loaded() && rclcpp::ok())
  {
    std::this_thread::sleep_for(queue_read_wait_period_);
  }
}

void Player::load_storage_content()
{
  auto queue_lower_boundary =
    static_cast<size_t>(play_options_.read_ahead_queue_size * read_ahead_lower_bound_percentage_);
  auto queue_upper_boundary = play_options_.read_ahead_queue_size;

  while (rclcpp::ok()) {
    TSAUniqueLock lk(reader_mutex_);
    if (!reader_->has_next()) {break;}

    if (message_queue_.size_approx() < queue_lower_boundary) {
      enqueue_up_to_boundary(queue_upper_boundary);
    } else {
      lk.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

void Player::enqueue_up_to_boundary(size_t boundary)
{
  rosbag2_storage::SerializedBagMessageSharedPtr message;
  for (size_t i = message_queue_.size_approx(); i < boundary; i++) {
    if (!reader_->has_next()) {
      break;
    }
    message = reader_->read_next();
    message_queue_.enqueue(message);
  }
}

void Player::play_messages_from_queue()
{
  // Note: We need to use message_queue_.peek() instead of message_queue_.try_dequeue(message)
  // to support play_next() API logic.
  rosbag2_storage::SerializedBagMessageSharedPtr * message_ptr = peek_next_message_from_queue();
  { // Notify play_next() that we are ready for playback
    // Note: We should do notification that we are ready for playback after peeking pointer to
    // the next message. message_queue_.peek() is not allowed to be called from more than one
    // thread concurrently.
    std::lock_guard<std::mutex> lk(ready_to_play_from_queue_mutex_);
    is_ready_to_play_from_queue_ = true;
    ready_to_play_from_queue_cv_.notify_all();
  }
  while (message_ptr != nullptr && rclcpp::ok()) {
    rosbag2_storage::SerializedBagMessageSharedPtr message = *message_ptr;
    // Do not move on until sleep_until returns true
    // It will always sleep, so this is not a tight busy loop on pause
    while (rclcpp::ok() && !clock_->sleep_until(message->time_stamp)) {
      if (std::atomic_exchange(&cancel_wait_for_next_message_, false)) {
        break;
      }
    }
    std::lock_guard<std::mutex> lk(skip_message_in_main_play_loop_mutex_);
    if (rclcpp::ok()) {
      if (skip_message_in_main_play_loop_) {
        skip_message_in_main_play_loop_ = false;
        cancel_wait_for_next_message_ = false;
        message_ptr = peek_next_message_from_queue();
        continue;
      }
      publish_message(message);
    }
    message_queue_.pop();
    message_ptr = peek_next_message_from_queue();
  }
  // while we're in pause state, make sure we don't return
  // if we happen to be at the end of queue
  while (is_paused()) {
    clock_->sleep_until(clock_->now());
  }
}

void Player::prepare_publishers()
{
  rosbag2_storage::StorageFilter storage_filter;
  storage_filter.topics = play_options_.topics_to_filter;
  reader_->set_filter(storage_filter);

  // Create /clock publisher
  if (play_options_.clock_publish_frequency > 0.f) {
    const auto publish_period = std::chrono::nanoseconds(
      static_cast<uint64_t>(RCUTILS_S_TO_NS(1) / play_options_.clock_publish_frequency));
    // NOTE: PlayerClock does not own this publisher because rosbag2_cpp
    // should not own transport-based functionality
    clock_publisher_ = create_publisher<rosgraph_msgs::msg::Clock>(
      "/clock", rclcpp::ClockQoS());
    clock_publish_timer_ = create_wall_timer(
      publish_period, [this]() {
        auto msg = rosgraph_msgs::msg::Clock();
        msg.clock = rclcpp::Time(clock_->now());
        clock_publisher_->publish(msg);
      });
  }

  // Create topic publishers
  auto topics = reader_->get_all_topics_and_types();
  for (const auto & topic : topics) {
    if (publishers_.find(topic.name) != publishers_.end()) {
      continue;
    }
    // filter topics to add publishers if necessary
    auto & filter_topics = storage_filter.topics;
    if (!filter_topics.empty()) {
      auto iter = std::find(filter_topics.begin(), filter_topics.end(), topic.name);
      if (iter == filter_topics.end()) {
        continue;
      }
    }

    auto topic_qos = publisher_qos_for_topic(
      topic, topic_qos_profile_overrides_,
      get_logger());
    try {
      publishers_.insert(
        std::make_pair(
          topic.name, create_generic_publisher(topic.name, topic.type, topic_qos)));
    } catch (const std::runtime_error & e) {
      // using a warning log seems better than adding a new option
      // to ignore some unknown message type library
      RCLCPP_WARN(
        get_logger(),
        "Ignoring a topic '%s', reason: %s.", topic.name.c_str(), e.what());
    }
  }
}

bool Player::publish_message(rosbag2_storage::SerializedBagMessageSharedPtr message)
{
  bool message_published = false;
  auto publisher_iter = publishers_.find(message->topic_name);
  if (publisher_iter != publishers_.end()) {
    publisher_iter->second->publish(rclcpp::SerializedMessage(*message->serialized_data));
    message_published = true;
  }
  return message_published;
}

void Player::add_key_callback(
  KeyboardHandler::KeyCode key,
  const std::function<void()> & cb,
  const std::string & op_name)
{
  std::string key_str = enum_key_code_to_str(key);
  if (key == KeyboardHandler::KeyCode::UNKNOWN) {
    RCLCPP_ERROR_STREAM(
      get_logger(),
      "Invalid key binding " << key_str << " for " << op_name);
    throw std::invalid_argument("Invalid key binding.");
  }
  keyboard_callbacks_.push_back(
    keyboard_handler_->add_key_press_callback(
      [cb](KeyboardHandler::KeyCode /*key_code*/,
      KeyboardHandler::KeyModifiers /*key_modifiers*/) {cb();},
      key));
  // show instructions
  RCLCPP_INFO_STREAM(
    get_logger(),
    "Press " << key_str << " for " << op_name);
}

void Player::add_keyboard_callbacks()
{
  // skip if disabled
  if (play_options_.disable_keyboard_controls) {
    return;
  }
  RCLCPP_INFO_STREAM(get_logger(), "Adding keyboard callbacks.");
  // check keybindings
  add_key_callback(
    play_options_.pause_resume_toggle_key,
    [this]() {toggle_paused();},
    "Pause/Resume"
  );
  add_key_callback(
    play_options_.play_next_key,
    [this]() {play_next();},
    "Play Next Message"
  );
  add_key_callback(
    play_options_.increase_rate_key,
    [this]() {set_rate(get_rate() * 1.1);},
    "Increase Rate 10%"
  );
  add_key_callback(
    play_options_.decrease_rate_key,
    [this]() {set_rate(get_rate() * 0.9);},
    "Decrease Rate 10%"
  );
}

void Player::create_control_services()
{
  srv_pause_ = create_service<rosbag2_interfaces::srv::Pause>(
    "~/pause",
    [this](
      rosbag2_interfaces::srv::Pause::Request::ConstSharedPtr,
      rosbag2_interfaces::srv::Pause::Response::SharedPtr)
    {
      pause();
    });
  srv_resume_ = create_service<rosbag2_interfaces::srv::Resume>(
    "~/resume",
    [this](
      rosbag2_interfaces::srv::Resume::Request::ConstSharedPtr,
      rosbag2_interfaces::srv::Resume::Response::SharedPtr)
    {
      resume();
    });
  srv_toggle_paused_ = create_service<rosbag2_interfaces::srv::TogglePaused>(
    "~/toggle_paused",
    [this](
      rosbag2_interfaces::srv::TogglePaused::Request::ConstSharedPtr,
      rosbag2_interfaces::srv::TogglePaused::Response::SharedPtr)
    {
      toggle_paused();
    });
  srv_is_paused_ = create_service<rosbag2_interfaces::srv::IsPaused>(
    "~/is_paused",
    [this](
      rosbag2_interfaces::srv::IsPaused::Request::ConstSharedPtr,
      rosbag2_interfaces::srv::IsPaused::Response::SharedPtr response)
    {
      response->paused = is_paused();
    });
  srv_get_rate_ = create_service<rosbag2_interfaces::srv::GetRate>(
    "~/get_rate",
    [this](
      rosbag2_interfaces::srv::GetRate::Request::ConstSharedPtr,
      rosbag2_interfaces::srv::GetRate::Response::SharedPtr response)
    {
      response->rate = get_rate();
    });
  srv_set_rate_ = create_service<rosbag2_interfaces::srv::SetRate>(
    "~/set_rate",
    [this](
      rosbag2_interfaces::srv::SetRate::Request::ConstSharedPtr request,
      rosbag2_interfaces::srv::SetRate::Response::SharedPtr response)
    {
      response->success = set_rate(request->rate);
    });
  srv_play_next_ = create_service<rosbag2_interfaces::srv::PlayNext>(
    "~/play_next",
    [this](
      rosbag2_interfaces::srv::PlayNext::Request::ConstSharedPtr,
      rosbag2_interfaces::srv::PlayNext::Response::SharedPtr response)
    {
      response->success = play_next();
    });
  srv_seek_ = create_service<rosbag2_interfaces::srv::Seek>(
    "~/seek",
    [this](
      rosbag2_interfaces::srv::Seek::Request::ConstSharedPtr request,
      rosbag2_interfaces::srv::Seek::Response::SharedPtr response)
    {
      seek(rclcpp::Time(request->time).nanoseconds());
      response->success = true;
    });
}

}  // namespace rosbag2_transport
