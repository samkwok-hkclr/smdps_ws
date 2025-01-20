#include "wcs/dis_station_node.hpp"

bool DispenserStationNode::init_opcua_cli(void)
{
  opcua::ClientConfig config;
  cli.config().setTimeout(OPCUA_TIMEOUT);
  cli.config().setLogger(std::bind(&DispenserStationNode::logger_wrapper, this, _1, _2, _3));

  cli.onDisconnected(std::bind(&DispenserStationNode::disconnected_cb, this));
  cli.onConnected(std::bind(&DispenserStationNode::connected_cb, this));
  cli.onSessionActivated(std::bind(&DispenserStationNode::session_activated_cb, this));
  cli.onSessionClosed(std::bind(&DispenserStationNode::session_closed_cb, this));
  cli.onInactive(std::bind(&DispenserStationNode::inactive_cb, this));

  return true;
}

void DispenserStationNode::start_opcua_cli(void)
{
  cli_started_.store(true);

  while (cli_started_.load() && rclcpp::ok()) 
  {
    try 
    {
      cli.connectAsync(form_opcua_url());
      cli.run();
    }
    catch (const opcua::BadStatus& e) 
    {
      cli.disconnectAsync();
      RCLCPP_ERROR(this->get_logger(), "Error: %s, Retry to connect in 1 seconds", e.what());
    }
    catch (...)
    {
      RCLCPP_ERROR(this->get_logger(), "caught an unknown exception!!!");
      cli_started_.store(false);
      rclcpp::shutdown();
    }

    std::this_thread::sleep_for(1s);
  }
}

void DispenserStationNode::wait_for_opcua_connection(void)
{
  RCLCPP_INFO(this->get_logger(), "started to wait the opcua connection <<<<<<<<<<<<<");
  std::this_thread::sleep_for(1s);

  std::chrono::seconds retry = 1s;
  rclcpp::Rate loop_rate(retry); 
  while (rclcpp::ok() && !cli.isConnected())
  {
    RCLCPP_ERROR(this->get_logger(), "waiting for opcua connection (%ld sec to retry)...", retry.count());
    loop_rate.sleep();
  }
}

void DispenserStationNode::disconnected_cb(void)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  RCLCPP_INFO(this->get_logger(), ">>> disconnected to opcua server: %s:%s", ip_.c_str(), port_.c_str());
}

void DispenserStationNode::connected_cb(void)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  RCLCPP_INFO(this->get_logger(), ">>> connected to opcua server: %s:%s", ip_.c_str(), port_.c_str());
}

void DispenserStationNode::session_activated_cb(void)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  create_sub_async();
  RCLCPP_INFO(this->get_logger(), ">>> session activated: %s:%s", ip_.c_str(), port_.c_str());
}

void DispenserStationNode::session_closed_cb(void)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  RCLCPP_INFO(this->get_logger(), ">>> session closed: %s:%s", ip_.c_str(), port_.c_str());
}

void DispenserStationNode::inactive_cb(void)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  RCLCPP_INFO(this->get_logger(), ">>> inactive: %s:%s", ip_.c_str(), port_.c_str());
}


void DispenserStationNode::heartbeat_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value)
{
  std::optional<bool> val = value.value().scalar<bool>();
  // const opcua::MonitoredItem item(cli, sub_id, mon_id);

  RCLCPP_INFO(this->get_logger(), ">>>> Heartbeat data change notification, value: %s", val ? "true" : "false");
  RCLCPP_DEBUG(this->get_logger(), ">>>> - subscription id: %d", sub_id);
  RCLCPP_DEBUG(this->get_logger(), ">>>> - monitored item id: %d", mon_id);
}

void DispenserStationNode::general_bool_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value, const std::string name, bool &bool_ref)
{
  std::optional<bool> val = value.value().scalar<bool>();

  const std::lock_guard<std::mutex> lock(mutex_);
  bool_ref = *val;
  
  if (!name.empty())
  {
    RCLCPP_INFO(this->get_logger(), ">>>> %s data change notification, value: %s", name.c_str(), *val ? "true" : "false");
    RCLCPP_DEBUG(this->get_logger(), ">>>> - subscription id: %d", sub_id);
    RCLCPP_DEBUG(this->get_logger(), ">>>> - monitored item id: %d", mon_id);
  }
}

void DispenserStationNode::alm_code_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value)
{
  std::optional<int16_t> val = value.value().scalar<int16_t>();
  // const opcua::MonitoredItem item(cli, sub_id, mon_id);

  const std::lock_guard<std::mutex> lock(mutex_);
  status_->error_code = *val;

  if (val == 200)
    RCLCPP_INFO(this->get_logger(), ">>>> ALM Code data change notification, value: %d", *val);
  else
    RCLCPP_ERROR(this->get_logger(), ">>>> ALM Code data change notification, value: %d", *val);
  RCLCPP_DEBUG(this->get_logger(), ">>>> - subscription id: %d", sub_id);
  RCLCPP_DEBUG(this->get_logger(), ">>>> - monitored item id: %d", mon_id);
}

void DispenserStationNode::completed_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value)
{
  std::optional<bool> val = value.value().scalar<bool>();

  if (val && *val)
    std::thread(std::bind(&DispenserStationNode::initiate, this)).detach(); 
  
  RCLCPP_INFO(this->get_logger(), ">>>> Completed data change notification, value: %s", *val ? "true" : "false");
  RCLCPP_DEBUG(this->get_logger(), ">>>> - subscription id: %d", sub_id);
  RCLCPP_DEBUG(this->get_logger(), ">>>> - monitored item id: %d", mon_id);
}

void DispenserStationNode::initiate(void)
{
  opcua::Variant init_var;
  init_var = true;

  std::future<opcua::StatusCode> stop_future = opcua::services::writeValueAsync(cli, initiate_id, init_var, opcua::useFuture);
  stop_future.wait();

  const opcua::StatusCode &stop_code = stop_future.get();
  if (stop_code != UA_STATUSCODE_GOOD)
    RCLCPP_ERROR(this->get_logger(), "writeValueAsync error occur in %s", __FUNCTION__);

  std::chrono::milliseconds freq = 200ms;
  rclcpp::Rate loop_rate(freq); 
  while (rclcpp::ok())
  {
    std::future<opcua::Result<opcua::Variant>> future = opcua::services::readValueAsync(cli, initiate_id, opcua::useFuture);
    future.wait();
    const opcua::Result<opcua::Variant> &result = future.get();

    if (result.code() != UA_STATUSCODE_GOOD)
    {
      RCLCPP_ERROR(this->get_logger(), "Read result with status code: %s", std::to_string(result.code()).c_str());
      continue;
    }

    std::optional<bool> val = result.value().scalar<bool>();
    if (val && *val)
      break;
    
    loop_rate.sleep();
  }

  init_var = false;

  std::future<opcua::StatusCode> start_future = opcua::services::writeValueAsync(cli, initiate_id, init_var, opcua::useFuture);
  start_future.wait();

  const opcua::StatusCode &start_code = start_future.get();
  if (start_code != UA_STATUSCODE_GOOD)
    RCLCPP_ERROR(this->get_logger(), "writeValueAsync error occur in %s", __FUNCTION__);

  RCLCPP_INFO(this->get_logger(), "Initiated the Station, ID: %d", status_->id);
}

void DispenserStationNode::create_sub_async()
{
  opcua::SubscriptionParameters sub_params{};
  sub_params.publishingInterval = 100;

  opcua::services::createSubscriptionAsync(
    cli,
    sub_params,
    true,
    std::bind(&DispenserStationNode::sub_status_change_cb, this, _1, _2),
    std::bind(&DispenserStationNode::sub_deleted_cb, this, _1),
    [&] (opcua::CreateSubscriptionResponse& response) {
      RCLCPP_INFO(this->get_logger(), ">>>> Subscription created:");
      RCLCPP_INFO(this->get_logger(), ">>>> - status code: %s", std::to_string(response.responseHeader().serviceResult()).c_str());
      RCLCPP_INFO(this->get_logger(), ">>>> - subscription id: %s", std::to_string(response.subscriptionId()).c_str());

      opcua::MonitoringParametersEx monitoring_params{};
      monitoring_params.samplingInterval = 100.0;

      opcua::services::createMonitoredItemDataChangeAsync(
        cli,
        response.subscriptionId(),
        opcua::ReadValueId(alm_code_id, opcua::AttributeId::Value),
        opcua::MonitoringMode::Reporting,
        opcua::MonitoringParametersEx{},
        std::bind(&DispenserStationNode::alm_code_cb, this, _1, _2, _3),
        std::bind(&DispenserStationNode::monitored_item_deleted_cb, this, _1, _2, "ALM Code"), 
        std::bind(&DispenserStationNode::monitored_item_created_cb, this, _1, "ALM Code")
      );
            
      opcua::services::createMonitoredItemDataChangeAsync(
        cli,
        response.subscriptionId(),
        opcua::ReadValueId(completed_id, opcua::AttributeId::Value),
        opcua::MonitoringMode::Reporting,
        monitoring_params,
        std::bind(&DispenserStationNode::completed_cb, this, _1, _2, _3),
        std::bind(&DispenserStationNode::monitored_item_deleted_cb, this, _1, _2, "Completed"), 
        std::bind(&DispenserStationNode::monitored_item_created_cb, this, _1, "Completed")
      );

      create_monitored_item_async(response, running_id, "Running", status_->running);
      create_monitored_item_async(response, paused_id, "Paused", status_->paused);
      create_monitored_item_async(response, error_id, "Error", status_->error);


      for (size_t i = 0; i < NO_OF_UNITS; i++)
      {
        auto &unit_status = status_->unit_status[i];

        create_monitored_item_async(response, unit_lack_id[i], "Unit" + std::to_string(i+1) + "Lack", unit_status.lack);
        create_monitored_item_async(response, bin_opening_id[i], "Bin" + std::to_string(i+1) + "Opening", unit_status.bin_opening);
        create_monitored_item_async(response, bin_opened_id[i], "Bin" + std::to_string(i+1) + "Opened", unit_status.bin_opened);
        create_monitored_item_async(response, bin_closing_id[i], "Bin" + std::to_string(i+1) + "Closing", unit_status.bin_closing);
        create_monitored_item_async(response, bin_closed_id[i], "Bin" + std::to_string(i+1) + "Closed", unit_status.bin_closed);
        create_monitored_item_async(response, baffle_opening_id[i], "Baffle" + std::to_string(i+1) + "Opening", unit_status.baffle_opening);
        create_monitored_item_async(response, baffle_opened_id[i], "Baffle" + std::to_string(i+1) + "Opened", unit_status.baffle_opened);
        create_monitored_item_async(response, baffle_closing_id[i], "Baffle" + std::to_string(i+1) + "Closing", unit_status.baffle_closing);
        create_monitored_item_async(response, baffle_closed_id[i], "Baffle" + std::to_string(i+1) + "Closed", unit_status.baffle_closed);
      }
    }
  );

  RCLCPP_DEBUG(this->get_logger(), "%s is working", __FUNCTION__);
}

void DispenserStationNode::create_monitored_item_async(
  const opcua::CreateSubscriptionResponse &response, 
  const opcua::NodeId &id, 
  const std::string &name, 
  bool &ref) 
{
  opcua::services::createMonitoredItemDataChangeAsync(
    cli,
    response.subscriptionId(),
    opcua::ReadValueId(id, opcua::AttributeId::Value),
    opcua::MonitoringMode::Reporting,
    opcua::MonitoringParametersEx{},
    std::bind(&DispenserStationNode::general_bool_cb, this, _1, _2, _3, name, ref),
    std::bind(&DispenserStationNode::monitored_item_deleted_cb, this, _1, _2, name), 
    std::bind(&DispenserStationNode::monitored_item_created_cb, this, _1, name)
  );
}

void DispenserStationNode::sub_status_change_cb(uint32_t sub_id, opcua::StatusChangeNotification &notification)
{
  (void) notification;
  RCLCPP_INFO(this->get_logger(), ">>>> Subscription status change: %d", sub_id);
}

void DispenserStationNode::sub_deleted_cb(uint32_t sub_id)
{
  RCLCPP_INFO(this->get_logger(), ">>>> Subscription deleted: %d", sub_id);
}

void DispenserStationNode::monitored_item_deleted_cb(uint32_t sub_id, uint32_t mon_id, std::string name)
{
  RCLCPP_INFO(this->get_logger(), ">>>> %s Monitored Item deleted:", name.c_str());
  RCLCPP_INFO(this->get_logger(), ">>>> - subscription id: %d", sub_id);
  RCLCPP_INFO(this->get_logger(), ">>>> - monitored item id: %d", mon_id);
}

void DispenserStationNode::monitored_item_created_cb(opcua::MonitoredItemCreateResult& result, std::string name)
{
  RCLCPP_DEBUG(this->get_logger(), ">>>> %s Monitored Item created:", name.c_str());
  RCLCPP_DEBUG(this->get_logger(), ">>>> - status code: %s", std::to_string(result.statusCode()).c_str());
  RCLCPP_DEBUG(this->get_logger(), ">>>> - monitored item id: %s", std::to_string(result.monitoredItemId()).c_str());
}

const std::string DispenserStationNode::form_opcua_url(void)
{
  if (!ip_.empty() && !port_.empty())
    return "opc.tcp://" + ip_ + ":" + port_;
  else
    throw std::runtime_error(std::string("%s failed", __FUNCTION__));
}

constexpr std::string_view DispenserStationNode::get_enum_name(opcua::NodeClass node_class) 
{
  switch (node_class) 
  {
  case opcua::NodeClass::Object:
    return "Object";
  case opcua::NodeClass::Variable:
    return "Variable";
  case opcua::NodeClass::Method:
    return "Method";
  case opcua::NodeClass::ObjectType:
    return "ObjectType";
  case opcua::NodeClass::VariableType:
    return "VariableType";
  case opcua::NodeClass::ReferenceType:
    return "ReferenceType";
  case opcua::NodeClass::DataType:
    return "DataType";
  case opcua::NodeClass::View:
    return "View";
  default:
    return "Unknown";
  }
}

constexpr std::string_view DispenserStationNode::get_log_level_name(opcua::LogLevel level) 
{
  switch (level) 
  {
  case opcua::LogLevel::Trace:
    return "trace";
  case opcua::LogLevel::Debug:
    return "debug";
  case opcua::LogLevel::Info:
    return "info";
  case opcua::LogLevel::Warning:
    return "warning";
  case opcua::LogLevel::Error:
    return "error";
  case opcua::LogLevel::Fatal:
    return "fatal";
  default:
    return "unknown";
  }
}
 
constexpr std::string_view DispenserStationNode::get_log_category_name(opcua::LogCategory category) 
{
  switch (category) 
  {
  case opcua::LogCategory::Network:
    return "network";
  case opcua::LogCategory::SecureChannel:
    return "channel";
  case opcua::LogCategory::Session:
    return "session";
  case opcua::LogCategory::Server:
    return "server";
  case opcua::LogCategory::Client:
    return "client";
  case opcua::LogCategory::Userland:
    return "userland";
  case opcua::LogCategory::SecurityPolicy:
    return "securitypolicy";
  default:
    return "unknown";
  }
}

void DispenserStationNode::logger_wrapper(opcua::LogLevel level, opcua::LogCategory category, std::string_view msg)
{
  switch (level) 
  {
  case opcua::LogLevel::Trace:
  case opcua::LogLevel::Debug:
  case opcua::LogLevel::Info:
  case opcua::LogLevel::Warning:
    RCLCPP_INFO(this->get_logger(), "[%s] %s", std::string(get_log_category_name(category)).c_str(), std::string(msg).c_str());
    break;
  case opcua::LogLevel::Error:
  case opcua::LogLevel::Fatal:
  default:
    RCLCPP_ERROR(this->get_logger(), "[%s] %s", std::string(get_log_category_name(category)).c_str(), std::string(msg).c_str());
    break;
  }
}