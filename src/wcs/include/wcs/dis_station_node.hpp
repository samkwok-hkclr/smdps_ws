#ifndef DIS_STATION_NODE__
#define DIS_STATION_NODE__

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <optional>

#include "rclcpp/rclcpp.hpp"

#include <open62541pp/open62541pp.hpp>

#include "smdps_msgs/msg/dispense_content.hpp"

#include "smdps_msgs/msg/dispenser_station_status.hpp"
#include "smdps_msgs/msg/dispenser_unit_status.hpp"
#include "smdps_msgs/srv/dispense_drug.hpp"

#define NO_OF_UNITS 12
#define MAX_HB_COUNT 5 * 4

using namespace std::chrono_literals;
using std::placeholders::_1;
using std::placeholders::_2;
using std::placeholders::_3;

class DispenserStationNode : public rclcpp::Node 
{
  using DispenseContent = smdps_msgs::msg::DispenseContent;
  using DispenserStationStatus = smdps_msgs::msg::DispenserStationStatus;
  using DispenserUnitStatus = smdps_msgs::msg::DispenserUnitStatus;
  using DispenseDrug = smdps_msgs::srv::DispenseDrug;

public:
  explicit DispenserStationNode(const rclcpp::NodeOptions& options);
  ~DispenserStationNode();

  inline const std::string form_opcua_url(void);
  bool init_opcua_cli(void);
  void start_opcua_cli(void); 
  void wait_for_opcua_connection(void);

  constexpr std::string_view get_enum_name(opcua::NodeClass node_class);
  constexpr std::string_view get_log_level_name(opcua::LogLevel level);
  constexpr std::string_view get_log_category_name(opcua::LogCategory category);
  void logger_wrapper(opcua::LogLevel level, opcua::LogCategory category, std::string_view msg);

  void disconnected_cb(void);
  void connected_cb(void);
  void session_activated_cb(void);
  void session_closed_cb(void);
  void inactive_cb(void);

  void create_sub_async(void);
  void create_monitored_item(const opcua::CreateSubscriptionResponse &response, const opcua::NodeId &id, const std::string &name, bool &ref);
  void sub_status_change_cb(uint32_t sub_id, opcua::StatusChangeNotification &notification);
  void sub_deleted_cb(uint32_t sub_id);
  void monitored_item_deleted_cb(uint32_t sub_id, uint32_t mon_id, std::string name);
  void monitored_item_created_cb(opcua::MonitoredItemCreateResult &result, std::string name);

  void heartbeat_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value);
  void general_bool_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value, const std::string name, bool &bool_ref);
  void alm_code_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value);

private:
  std::mutex mutex_;
  std::string ip_;
  std::string port_;

  uint32_t heartbeat_counter_ = 0;
  const uint32_t OPCUA_TIMEOUT = 1000; // 1000ms

  std::shared_ptr<DispenserStationStatus> status_;

  rclcpp::TimerBase::SharedPtr status_timer_;
  rclcpp::TimerBase::SharedPtr opcua_timer_;
  rclcpp::TimerBase::SharedPtr units_lack_timer_;
  rclcpp::Publisher<DispenserStationStatus>::SharedPtr status_pub_;

  rclcpp::Subscription<DispenseContent>::SharedPtr dis_ctx_sub_;

  rclcpp::Service<DispenseDrug>::SharedPtr dis_req_srv_;

  void dis_station_status_cb(void);
  void heartbeat_valid_cb(void);
  void units_lack_cb(void);

  void dis_req_handle(
    const std::shared_ptr<DispenseDrug::Request> req, 
    std::shared_ptr<DispenseDrug::Response> res);

protected:
  bool sim_;

  opcua::Client cli;
  std::atomic<bool> cli_started_;
  std::thread cli_thread_;

  const opcua::NamespaceIndex ns_ind = 4;
  const std::string send_prefix = "SEND|";
  const std::string rev_prefix = "REV|";

  const opcua::NodeId heartbeat_id = {ns_ind, send_prefix + "Heartbeat"};
  const opcua::NodeId running_id = {ns_ind, send_prefix + "Running"};
  const opcua::NodeId paused_id = {ns_ind, send_prefix + "Paused"};
  const opcua::NodeId error_id = {ns_ind, send_prefix + "Error"};
  const opcua::NodeId alm_code_id = {ns_ind, send_prefix + "ALMCode"};

  const opcua::NodeId cmd_valid_state_id = {ns_ind, send_prefix + "CmdValidState"};
  const opcua::NodeId cmd_amt_id = {ns_ind, send_prefix + "CmdAmount"};
  const opcua::NodeId dispensing_id = {ns_ind, send_prefix + "Dispensing"};
  const opcua::NodeId amt_dis_id = {ns_ind, send_prefix + "AmountDispensed"};
  const opcua::NodeId completed_id = {ns_ind, send_prefix + "Completed"};

  const opcua::NodeId cmd_req_id = {ns_ind, rev_prefix + "CmdRequest"};
  const opcua::NodeId cmd_exe_id = {ns_ind, rev_prefix + "CmdExecute"};

  std::array<opcua::NodeId, NO_OF_UNITS> unit_amt_id;

  std::array<opcua::NodeId, NO_OF_UNITS> unit_lack_id;

  std::array<opcua::NodeId, NO_OF_UNITS> bin_opening_id;
  std::array<opcua::NodeId, NO_OF_UNITS> bin_opened_id;
  std::array<opcua::NodeId, NO_OF_UNITS> bin_closing_id;
  std::array<opcua::NodeId, NO_OF_UNITS> bin_closed_id;
  std::array<opcua::NodeId, NO_OF_UNITS> baffle_opening_id;
  std::array<opcua::NodeId, NO_OF_UNITS> baffle_opened_id;
  std::array<opcua::NodeId, NO_OF_UNITS> baffle_closing_id;
  std::array<opcua::NodeId, NO_OF_UNITS> baffle_closed_id;
};
#endif