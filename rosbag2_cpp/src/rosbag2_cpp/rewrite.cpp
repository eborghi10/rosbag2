#include "rosbag2_cpp/rewrite.hpp"

namespace
{

std::shared_ptr<rosbag2_storage::SerializedBagMessage> get_next(
  const std::vector<std::unique_ptr<rosbag2_cpp::Reader>> & input_bags,
  std::vector<std::shared_ptr<rosbag2_storage::SerializedBagMessage>> & next_messages)
{
  // refill queue
  for (size_t i = 0; i < next_messages.size(); i++) {
    if (next_messages[i] == nullptr && input_bags[i]->has_next()) {
      next_messages[i] = input_bags[i]->read_next();
    }
  }

  // find message with lowest timestamp

  std::shared_ptr<rosbag2_storage::SerializedBagMessage> earliest_msg = nullptr;
  size_t earliest_msg_index = -1;
  for (size_t i = 0; i < next_messages.size(); i++) {
    auto & msg = next_messages[i];
    if (msg == nullptr) {
      continue;
    }
    if (earliest_msg == nullptr || msg->time_stamp < earliest_msg->time_stamp) {
      earliest_msg = msg;
      earliest_msg_index = i;
    }
  }

  // clear returned message from queue before returning it, so it can be refilled next time
  if (earliest_msg != nullptr) {
    next_messages[earliest_msg_index].reset();
  }
  return earliest_msg;
}

}  // namespace

namespace rosbag2_cpp
{

void rewrite(
  const std::vector<std::unique_ptr<rosbag2_cpp::Reader>> & input_bags,
  const std::vector<std::unique_ptr<rosbag2_cpp::Writer>> & output_bags)
{
  if (input_bags.empty() || output_bags.empty()) {
    throw std::runtime_error("Must provide at least one input and one output bag to rewrite.");
  }

  std::vector<std::shared_ptr<rosbag2_storage::SerializedBagMessage>> next_messages;
  next_messages.resize(input_bags.size(), nullptr);

  std::shared_ptr<rosbag2_storage::SerializedBagMessage> next_msg;
  while (next_msg = get_next(input_bags, next_messages)) {
    for (auto & writer : output_bags) {
      writer->write(next_msg);
    }
  }
}

}  // namespace rosbag2_cpp
