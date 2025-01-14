#include "wcs/dis_station_node.hpp"

DispenserStationNode::DispenserStationNode(const rclcpp::NodeOptions& options)
: Node("dis_station_node", options),
  status_(std::make_shared<DispenserStationStatus>()),
  cli_started_(std::atomic<bool>{false})
{
  this->declare_parameter<uint8_t>("id", 0);
  this->declare_parameter<std::string>("ip", "");
  this->declare_parameter<std::string>("port", "");
  this->declare_parameter<bool>("simulation", false);

  this->get_parameter("id", status_->id);
  this->get_parameter("ip", ip_);
  this->get_parameter("port", port_);
  this->get_parameter("simulation", sim_);

  for (size_t i = 0; i < status_->unit_status.size(); i++)
  {
    status_->unit_status[i].id = i + 1;
    status_->unit_status[i].enable = true;
  }

  for (size_t i = 0; i < NO_OF_UNITS; i++)
  {
    unit_amt_id[i] = {ns_ind, rev_prefix + "Unit" + std::to_string(i+1) + "Amount"};
    unit_lack_id[i] = {ns_ind, send_prefix + "Unit" + std::to_string(i+1) + "Lack"};
    bin_opening_id[i] = {ns_ind, send_prefix + "Bin" + std::to_string(i+1) + "Opening"};
    bin_opened_id[i] = {ns_ind, send_prefix + "Bin" + std::to_string(i+1) + "Opened"};
    bin_closing_id[i] = {ns_ind, send_prefix + "Bin" + std::to_string(i+1) + "Closing"};
    bin_closed_id[i] = {ns_ind, send_prefix + "Bin" + std::to_string(i+1) + "Closed"};
    baffle_opening_id[i] = {ns_ind, send_prefix + "Baffle" + std::to_string(i+1) + "Opening"};
    baffle_opened_id[i] = {ns_ind, send_prefix + "Baffle" + std::to_string(i+1) + "Opened"};
    baffle_closing_id[i] = {ns_ind, send_prefix + "Baffle" + std::to_string(i+1) + "Closing"};
    baffle_closed_id[i] = {ns_ind, send_prefix + "Baffle" + std::to_string(i+1) + "Closed"};
  }

  if (!init_opcua_cli())
  {
    RCLCPP_ERROR(this->get_logger(), "init opcua client error occurred");
    rclcpp::shutdown();
  }

  status_pub_ = this->create_publisher<DispenserStationStatus>("status", 10);

  cli_thread_ = std::thread(std::bind(&DispenserStationNode::start_opcua_cli, this)); 
  wait_for_opcua_connection();

  status_timer_ = this->create_wall_timer(1s, std::bind(&DispenserStationNode::dis_station_status_cb, this));
  opcua_timer_ = this->create_wall_timer(250ms, std::bind(&DispenserStationNode::heartbeat_valid_cb, this));

  dis_req_srv_ = this->create_service<DispenseDrug>("dispense_request", std::bind(&DispenserStationNode::dis_req_handle, this, _1, _2));

  RCLCPP_INFO(this->get_logger(), "OPCUA Server: %s", form_opcua_url().c_str());
  RCLCPP_INFO(this->get_logger(), "dispenser station node is up");
}

DispenserStationNode::~DispenserStationNode()
{
  if (cli_started_.load()) 
  {
    cli_started_.store(false);
    cli.stop();
  }
  if (cli_thread_.joinable()) 
  {
    cli_thread_.join();
  }
}

void DispenserStationNode::dis_station_status_cb(void)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  status_pub_->publish(*status_);
  RCLCPP_DEBUG(this->get_logger(), "%s is working", __FUNCTION__);
}

void DispenserStationNode::heartbeat_valid_cb(void)
{
  std::future<opcua::Result<opcua::Variant>> future = opcua::services::readValueAsync(cli, heartbeat_id, opcua::useFuture);
  future.wait();
  const opcua::Result<opcua::Variant> &result = future.get();

  if (result.code() != UA_STATUSCODE_GOOD) 
  {
    RCLCPP_ERROR(this->get_logger(), "Error reading heartbeat: %s", std::to_string(result.code()).c_str());
    return;
  }

  std::optional<bool> val = result.value().scalar<bool>();
  if (!val) 
  {
    RCLCPP_ERROR(this->get_logger(), "Heartbeat value is not available");
    return;
  }
  RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), *val ? "true" : "false");

  const std::lock_guard<std::mutex> lock(mutex_);
  if (*val == status_->heartbeat) 
  {
    heartbeat_counter_++;
  } 
  else 
  {
    status_->heartbeat = true;
    heartbeat_counter_ = 0;
  }

  if (heartbeat_counter_ > MAX_HB_COUNT)
  {
    status_->heartbeat = false;
    RCLCPP_ERROR(this->get_logger(), "lost connection to %s", ip_.c_str());
  }
}

void DispenserStationNode::dis_req_handle(
  const std::shared_ptr<DispenseDrug::Request> req, 
  std::shared_ptr<DispenseDrug::Response> res)
{
  std::chrono::milliseconds retry_freq = 100ms;
  rclcpp::Rate loop_rate(retry_freq); 
  while (rclcpp::ok() && !cli.isConnected())
  {
    RCLCPP_ERROR(this->get_logger(), "waiting for opcua connection (%ld ms to retry)...", retry_freq.count());
    loop_rate.sleep();
  }

  uint32_t amt = 0;
  for (size_t i = 0; i < req->content.size(); i++)
  {
    if (req->content[i].unit_id == 0 || req->content[i].unit_id > NO_OF_UNITS)
    {
      RCLCPP_ERROR(this->get_logger(), "incorrect unit_id in service request");
      continue;
    }

    opcua::Variant amt_var;
    amt += req->content[i].amount;
    amt_var = static_cast<int16_t>(req->content[i].amount);

    std::future<opcua::StatusCode> future = opcua::services::writeValueAsync(cli, unit_amt_id[req->content[i].unit_id - 1], amt_var, opcua::useFuture);
    future.wait();

    const opcua::StatusCode &code = future.get();
    if (code != UA_STATUSCODE_GOOD)
      RCLCPP_ERROR(this->get_logger(), "writeValueAsync error occur in %s", __FUNCTION__);
  }

  opcua::Variant req_var;
  req_var = true;
  std::future<opcua::StatusCode> future = opcua::services::writeValueAsync(cli, cmd_req_id, req_var, opcua::useFuture);
  future.wait();

  const opcua::StatusCode &code = future.get();
  if (code != UA_STATUSCODE_GOOD)
    RCLCPP_ERROR(this->get_logger(), "writeValueAsync error occur in %s", __FUNCTION__);

  uint16_t t = 0;
  const uint16_t MAX_SEC = 10;
  const uint16_t MAX_RETIES = 1000 / retry_freq.count() * MAX_SEC; // retries = 1000ms / 100ms * 10s = 100 times
  while (rclcpp::ok())
  {
    bool done = false;

    std::future<opcua::Result<opcua::Variant>> future = opcua::services::readValueAsync(cli, cmd_req_id, opcua::useFuture);
    future.wait();
    const opcua::Result<opcua::Variant> &result = future.get();

    if (result.code() == UA_STATUSCODE_GOOD)
    {
      std::optional<bool> val = result.value().scalar<bool>();
      done = (*val == false);

      if (done)
      {
        RCLCPP_INFO(this->get_logger(), "Submit dispense request success");
        res->success = true; // FIXME
        break;
      }
    }
    else
    {
      RCLCPP_ERROR(this->get_logger(), "Read result with status code: %s", std::to_string(result.code()).c_str());
    }

    if (t > MAX_RETIES)
    {
      const std::string msg = "too long to do the dispense";
      RCLCPP_ERROR(this->get_logger(), msg.c_str());
      res->success = false; // FIXME
      res->message = msg; // FIXME
      break;
    }

    t++;
    loop_rate.sleep();
  }
}

void DispenserStationNode::heartbeat_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value)
{
  std::optional<bool> val = value.value().scalar<bool>();
  // const opcua::MonitoredItem item(cli, sub_id, mon_id);

  // const std::lock_guard<std::mutex> lock(mutex_);

  RCLCPP_INFO(this->get_logger(), ">>>> Heartbeat data change notification, value: %s", val ? "true" : "false");
  RCLCPP_DEBUG(this->get_logger(), ">>>> - subscription id: %d", sub_id);
  RCLCPP_DEBUG(this->get_logger(), ">>>> - monitored item id: %d", mon_id);
}

void DispenserStationNode::general_bool_cb(uint32_t sub_id, uint32_t mon_id, const opcua::DataValue &value, const std::string name, bool &bool_ref)
{
  std::optional<bool> val = value.value().scalar<bool>();
  // const opcua::MonitoredItem item(cli, sub_id, mon_id);

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

void DispenserStationNode::create_sub_async()
{
  opcua::SubscriptionParameters sub_params{};
  // sub_params.publishingInterval = 200;

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

      opcua::MonitoringParametersEx heartbeat_monitoring_params{};
      heartbeat_monitoring_params.samplingInterval = 100.0;

      opcua::services::createMonitoredItemDataChangeAsync(
        cli,
        response.subscriptionId(),
        opcua::ReadValueId(heartbeat_id, opcua::AttributeId::Value),
        opcua::MonitoringMode::Reporting,
        heartbeat_monitoring_params, 
        std::bind(&DispenserStationNode::heartbeat_cb, this, _1, _2, _3),
        std::bind(&DispenserStationNode::monitored_item_deleted_cb, this, _1, _2, "Heartbeat"),
        std::bind(&DispenserStationNode::monitored_item_created_cb, this, _1, "Heartbeat")
      );

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

      create_monitored_item(response, running_id, "Running", status_->running);
      create_monitored_item(response, paused_id, "Paused", status_->paused);
      create_monitored_item(response, error_id, "Error", status_->error);

      for (size_t i = 0; i < NO_OF_UNITS; i++)
      {
        auto &unit_status = status_->unit_status[i];

        create_monitored_item(response, unit_lack_id[i], "Unit" + std::to_string(i+1) + "Lack", unit_status.lack);
        create_monitored_item(response, bin_opening_id[i], "Bin" + std::to_string(i+1) + "Opening", unit_status.bin_opening);
        create_monitored_item(response, bin_opened_id[i], "Bin" + std::to_string(i+1) + "Opened", unit_status.bin_opened);
        create_monitored_item(response, bin_closing_id[i], "Bin" + std::to_string(i+1) + "Closing", unit_status.bin_closing);
        create_monitored_item(response, bin_closed_id[i], "Bin" + std::to_string(i+1) + "Closed", unit_status.bin_closed);
        create_monitored_item(response, baffle_opening_id[i], "Baffle" + std::to_string(i+1) + "Opening", unit_status.baffle_opening);
        create_monitored_item(response, baffle_opened_id[i], "Baffle" + std::to_string(i+1) + "Opened", unit_status.baffle_opened);
        create_monitored_item(response, baffle_closing_id[i], "Baffle" + std::to_string(i+1) + "Closing", unit_status.baffle_closing);
        create_monitored_item(response, baffle_closed_id[i], "Baffle" + std::to_string(i+1) + "Closed", unit_status.baffle_closed);
      }
    }
  );

  // std::future<opcua::CreateSubscriptionResponse> future = opcua::services::createSubscriptionAsync(
  //   cli,
  //   sub_params,
  //   true,  // publishingEnabled
  //   {}, // std::bind(&DispenserStationNode::sub_status_change_cb, this, _1, _2),
  //   std::bind(&DispenserStationNode::sub_deleted_cb, this, _1),
  //   opcua::useFuture
  // );
    
  // future.wait();
  // const opcua::CreateSubscriptionResponse &response = future.get();

  // RCLCPP_INFO(this->get_logger(), ">>>> Subscription created:");
  // RCLCPP_INFO(this->get_logger(), ">>>> - status code: %s", std::to_string(response.responseHeader().serviceResult()).c_str());
  // RCLCPP_INFO(this->get_logger(), ">>>> - subscription id: %s", std::to_string(response.subscriptionId()).c_str());

  // opcua::MonitoringParametersEx heartbeat_monitoring_params{};
  // heartbeat_monitoring_params.samplingInterval = 100.0;

  // opcua::services::createMonitoredItemDataChangeAsync(
  //   cli,
  //   response.subscriptionId(),
  //   opcua::ReadValueId(heartbeat_id, opcua::AttributeId::Value),
  //   opcua::MonitoringMode::Reporting,
  //   opcua::MonitoringParametersEx{}, 
  //   std::bind(&DispenserStationNode::heartbeat_cb, this, _1, _2, _3),
  //   std::bind(&DispenserStationNode::monitored_item_deleted_cb, this, _1, _2, "Heartbeat"),
  //   std::bind(&DispenserStationNode::monitored_item_created_cb, this, _1, "Heartbeat")
  // );

  // opcua::services::createMonitoredItemDataChangeAsync(
  //   cli,
  //   response.subscriptionId(),
  //   opcua::ReadValueId(alm_code_id, opcua::AttributeId::Value),
  //   opcua::MonitoringMode::Reporting,
  //   opcua::MonitoringParametersEx{},
  //   std::bind(&DispenserStationNode::alm_code_cb, this, _1, _2, _3),
  //   std::bind(&DispenserStationNode::monitored_item_deleted_cb, this, _1, _2, "ALM Code"), 
  //   std::bind(&DispenserStationNode::monitored_item_created_cb, this, _1, "ALM Code")
  // );

  // create_monitored_item(response, running_id, "Running", status_->running);
  // create_monitored_item(response, paused_id, "Paused", status_->paused);
  // create_monitored_item(response, error_id, "Error", status_->error);

  // for (size_t i = 0; i < NO_OF_UNITS; i++)
  // {
  //   auto &unit_status = status_->unit_status[i];

  //   create_monitored_item(response, unit_lack_id[i], "Unit" + std::to_string(i+1) + "Lack", unit_status.lack);
  //   create_monitored_item(response, bin_opening_id[i], "Bin" + std::to_string(i+1) + "Opening", unit_status.bin_opening);
  //   create_monitored_item(response, bin_opened_id[i], "Bin" + std::to_string(i+1) + "Opened", unit_status.bin_opened);
  //   create_monitored_item(response, bin_closing_id[i], "Bin" + std::to_string(i+1) + "Closing", unit_status.bin_closing);
  //   create_monitored_item(response, bin_closed_id[i], "Bin" + std::to_string(i+1) + "Closed", unit_status.bin_closed);
  //   create_monitored_item(response, baffle_opening_id[i], "Baffle" + std::to_string(i+1) + "Opening", unit_status.baffle_opening);
  //   create_monitored_item(response, baffle_opened_id[i], "Baffle" + std::to_string(i+1) + "Opened", unit_status.baffle_opened);
  //   create_monitored_item(response, baffle_closing_id[i], "Baffle" + std::to_string(i+1) + "Closing", unit_status.baffle_closing);
  //   create_monitored_item(response, baffle_closed_id[i], "Baffle" + std::to_string(i+1) + "Closed", unit_status.baffle_closed);
  // }

  RCLCPP_DEBUG(this->get_logger(), "%s is working", __FUNCTION__);
}

void DispenserStationNode::create_monitored_item(
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

inline const std::string DispenserStationNode::form_opcua_url(void)
{
  if (!ip_.empty() && !port_.empty())
    return "opc.tcp://" + ip_ + ":" + port_;
  else
    throw std::runtime_error(std::string("%s failed", __FUNCTION__));
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
  // add_subscriptions();
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

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  auto exec = std::make_shared<rclcpp::executors::MultiThreadedExecutor>();
  auto options = rclcpp::NodeOptions();
  auto node = std::make_shared<DispenserStationNode>(options);

  exec->add_node(node->get_node_base_interface());
  exec->spin();

  rclcpp::shutdown();
}

// void DispenserStationNode::units_lack_cb(void)
// {
//   const std::lock_guard<std::mutex> lock(mutex_);
//   for (size_t i = 0; i < NO_OF_UNITS; i++)
//   {
//     opcua::services::readValueAsync(
//       cli,
//       unit_lack_id[i],
//       [this, i](opcua::Result<opcua::Variant> &result) {
//         const bool val = result.value().scalar<bool>();
//         status_->unit_status[i].lack = val;
//         RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
//       }
//     );
//   }
//   RCLCPP_DEBUG(this->get_logger(), "%s is working", __FUNCTION__);
// }
  // for (size_t i = 0; i < NO_OF_UNITS; i++)
  // {
  //   opcua::services::readValueAsync(
  //     cli,
  //     bin_opening_id[i],
  //     [this, i](opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].bin_opening = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     bin_opened_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].bin_opened = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     bin_closing_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].bin_closing = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     bin_closed_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].bin_closed = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     baffle_opening_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].baffle_opening = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     baffle_opened_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].baffle_opened = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     baffle_closing_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].baffle_closing = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  //   opcua::services::readValueAsync(
  //     cli,
  //     baffle_closed_id[i],
  //     [this, i] (opcua::Result<opcua::Variant> &result) {
  //       const bool val = result.value().scalar<bool>();
  //       const std::lock_guard<std::mutex> lock(mutex_);
  //       status_->unit_status[i].baffle_closed = val;
  //       RCLCPP_DEBUG(this->get_logger(), "Read result with status code: %s, Data: %s", std::to_string(result.code()).c_str(), val ? "true" : "false");
  //     }
  //   );
  // }
