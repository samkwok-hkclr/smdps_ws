#include "wcs/prod_line_ctrl.hpp"

ProdLineCtrl::ProdLineCtrl(const rclcpp::NodeOptions& options)
: Node("prod_line_ctrl", options), 
  svr_started_(std::atomic<bool>{false})
{
  this->declare_parameter<std::string>("hkclr_ip", "");
  this->declare_parameter<int>("hkclr_port", 0);
  this->declare_parameter<std::string>("jinli_ip", "");
  this->declare_parameter<int>("jinli_port", 0);
  this->declare_parameter<int>("no_of_dispenser_stations", 0);
  this->declare_parameter<int>("no_of_packaging_machine", 0);

  this->get_parameter("hkclr_ip", httpsvr_ip_);
  this->get_parameter("hkclr_port", httpsvr_port_);
  this->get_parameter("jinli_ip", jinli_ip_);
  this->get_parameter("jinli_port", jinli_port_);
  this->get_parameter("no_of_dispenser_stations", no_of_dis_stations_);
  this->get_parameter("no_of_packaging_machine", no_of_pkg_mac_);

  RCLCPP_INFO(this->get_logger(), "jinli HTTP server: %s:%d", jinli_ip_.c_str(), jinli_port_);
  RCLCPP_INFO(this->get_logger(), "hkclr HTTP server: %s:%d", httpsvr_ip_.c_str(), httpsvr_port_);

  hc_pub_ = this->create_publisher<Heartbeat>("jinli_heartbeat", 10);
  dis_err_pub_ = this->create_publisher<DispensingError>("dispensing_error", 10);
  cleaning_mac_scan_pub_ = this->create_publisher<ScannerTrigger>("scanner_trigger", 10);
  order_compl_pub_ = this->create_publisher<OrderCompletion>("order_completion", 10);
  mtrl_box_amt_pub_ = this->create_publisher<ContainerInfo>("container_info", 10);
  mtrl_box_status_pub_ = this->create_publisher<MaterialBoxStatus>("material_box_status", 10);

  pkg_mac_status_sub_ = this->create_subscription<PackagingMachineStatus>(
    "packaging_machine_status", 
    10, 
    std::bind(&ProdLineCtrl::pkg_mac_status_cb, this, _1));

  srv_ser_cbg_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  srv_cli_cbg_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  action_ser_cbg_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  hc_timer_cbg_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  container_timer_cbg_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  mtrl_box_info_timer_cbg_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

  hc_timer_ = this->create_wall_timer(1s, std::bind(&ProdLineCtrl::hc_cb, this), hc_timer_cbg_);
  mtrl_box_amt_timer_ = this->create_wall_timer(5s, std::bind(&ProdLineCtrl::mtrl_box_amt_container_cb, this), container_timer_cbg_);
  mtrl_box_info_timer_ = this->create_wall_timer(5s, std::bind(&ProdLineCtrl::mtrl_box_info_cb, this), mtrl_box_info_timer_cbg_);

  printing_info_cli_ = this->create_client<PrintingOrder>(
    "printing_order",
    rmw_qos_profile_services_default,
    srv_cli_cbg_
  );

  pkg_order_cli_ = this->create_client<PackagingOrder>(
    "packaging_order",
    rmw_qos_profile_services_default,
    srv_cli_cbg_
  );

  for (size_t i = 0; i < no_of_dis_stations_; i++)
  {
    auto tmp_cli = this->create_client<Trigger>(
      "/packaging_machine_" + std::to_string(i + 1) + "/init_package_machine",
      rmw_qos_profile_services_default,
      srv_cli_cbg_
    );

    while (rclcpp::ok() && !tmp_cli->wait_for_service(std::chrono::seconds(1))) 
    {
      RCLCPP_ERROR(this->get_logger(), "Init Packaging Machine Service not available!");
    }

    init_pkg_mac_cli_.push_back(tmp_cli);
  }

  for (size_t i = 0; i < no_of_dis_stations_; i++)
  {
    auto tmp_cli = this->create_client<DispenseDrug>(
      "/dispenser_station_" + std::to_string(i + 1) + "/dispense_request",
      rmw_qos_profile_services_default,
      srv_cli_cbg_
    );
    
    while (rclcpp::ok() && !tmp_cli->wait_for_service(std::chrono::seconds(1))) 
    {
      RCLCPP_ERROR(this->get_logger(), "Dispense Request Service not available!");
    }

    dis_req_cli_.push_back(tmp_cli);
  }  

  this->action_server_ = rclcpp_action::create_server<NewOrder>(
    this,
    "new_order",
    std::bind(&ProdLineCtrl::handle_goal, this, _1, _2),
    std::bind(&ProdLineCtrl::handle_cancel, this, _1),
    std::bind(&ProdLineCtrl::handle_accepted, this, _1),
    rcl_action_server_get_default_options(),
    action_ser_cbg_);

  if (!init_httpsvr())
  {
    RCLCPP_ERROR(this->get_logger(), "init_httpsvr error occurred");
    rclcpp::shutdown();
  }

  if (!init_httpcli())
  {
    RCLCPP_ERROR(this->get_logger(), "init_httpcli error occurred");
    rclcpp::shutdown();
  }

  RCLCPP_INFO(this->get_logger(), "Production Line Control is up");
}

ProdLineCtrl::~ProdLineCtrl()
{
  if (svr_started_.load()) 
  {
    httpsvr_->wait_until_ready();
    httpsvr_->stop();
  }
  if (httpsvr_thread_.joinable()) 
  {
    httpsvr_thread_.join();
  }
}

void ProdLineCtrl::hc_cb(void)
{
  nlohmann::json res_json;
  Heartbeat msg;
  msg.state = health_check(res_json);
  msg.header.stamp = this->get_clock()->now();
  RCLCPP_DEBUG(this->get_logger(), "jinli server is %s", msg.state ? "OK" : "ERROR");
  hc_pub_->publish(msg);
}

void ProdLineCtrl::mtrl_box_amt_container_cb(void)
{
  nlohmann::json res_json;

  if (!get_mtrl_box_amt(res_json)) 
  {
    RCLCPP_ERROR(this->get_logger(), "%s error ocurred", __FUNCTION__);
    return;
  }

  RCLCPP_DEBUG(this->get_logger(), "\n%s", res_json.dump().c_str());
  ContainerInfo msg;
  msg.amount = static_cast<uint8_t>(res_json["amount"]);
  msg.header.stamp = this->get_clock()->now();
  mtrl_box_amt_pub_->publish(msg);
}

void ProdLineCtrl::mtrl_box_info_cb(void)
{
  nlohmann::json res_json;
  
  if (!get_mtrl_box_info(res_json)) 
  {
    RCLCPP_ERROR(this->get_logger(), "%s error ocurred", __FUNCTION__);
    return;
  }

  RCLCPP_DEBUG(this->get_logger(), "\n%s", res_json.dump().c_str());
  
  const std::string no_order = "0";
  for (const auto &mtrl_box : res_json["materialBoxs"])
  {
    if (!no_order.compare(mtrl_box["orderId"].get<std::string>()))
      continue;
    
    MaterialBoxStatus msg;
    for (size_t i = 0; i < msg.material_box.slots.size(); i++) 
    {
      nlohmann::json mtrl_box_cell_res_json;

      const httplib::Params params = {
        { "MaterialBoxId", std::to_string(mtrl_box["id"].get<int>()) },
        { "cellId", std::to_string(i) }
      };

      if (!get_cell_info_by_id_and_cell_id(params, mtrl_box_cell_res_json))
      {
        RCLCPP_ERROR(this->get_logger(), "%s had unknown error", __FUNCTION__);
        continue;
      }

      for (const auto &drug : mtrl_box_cell_res_json["cell"]["drugs"])
      {
        if (drug["isCompleted"].get<int>() != 0)
        {
          DispensingDetail dis_detail_msg;
          for (const auto &location : drug["locations"]) // The length of locations must be 1
          {
            dis_detail_msg.location.dispenser_station = location["dispenserStation"].get<int>();
            dis_detail_msg.location.dispenser_unit = location["dispenserUnit"].get<int>();
          }
          dis_detail_msg.amount = drug["amount"].get<int>();
          msg.material_box.slots[i].dispensing_detail.push_back(dis_detail_msg);
        }
      }
    }
    
    msg.header.stamp = this->get_clock()->now();
    msg.id = mtrl_box["id"];
    msg.location = mtrl_box["location"];
    msg.status = MaterialBoxStatus::STATUS_ERROR;

    mtrl_box_status_pub_->publish(msg);
  }
}

void ProdLineCtrl::pkg_mac_status_cb(const PackagingMachineStatus::SharedPtr msg)
{
  const std::lock_guard<std::mutex> lock(mutex_);
  pkg_mac_status[msg->packaging_machine_id] = *msg;
}

void ProdLineCtrl::init_pkg_mac_srv_handler(const uint8_t pkg_mac_id)
{
  RCLCPP_INFO(this->get_logger(), "Started to initiate the packaging machine [%d]", pkg_mac_id);
}

void ProdLineCtrl::dis_result_srv_handler(std::map<uint8_t, std::shared_ptr<DispenseDrug::Request>> dis_reqs)
{
  RCLCPP_DEBUG(this->get_logger(), "dis_reqs size: %ld", dis_reqs.size());

  using ServiceSharedFutureAndRequestId = rclcpp::Client<DispenseDrug>::SharedFutureAndRequestId;
  // tuple<station_id, result, future>
  std::vector<std::tuple<uint8_t, bool, ServiceSharedFutureAndRequestId>> futures_tuple;
  for (const auto &req_pair : dis_reqs)
  {
    using ServiceResponseFuture = rclcpp::Client<DispenseDrug>::SharedFuture;
    auto response_received_cb = [this](ServiceResponseFuture future) {
      auto response = future.get();
      if (response) 
        RCLCPP_INFO(this->get_logger(), "Sent a dispense drug request.");
      else 
      {
        const std::string err_msg = "Service call failed or returned no result";
        RCLCPP_ERROR(this->get_logger(), err_msg.c_str());
      }
    };
    
    auto future = dis_req_cli_[req_pair.first - 1]->async_send_request(req_pair.second, response_received_cb);
    futures_tuple.push_back(std::make_tuple(req_pair.first, false, std::move(future)));
  }

  for (auto &tuple : futures_tuple)
  {
    RCLCPP_DEBUG(this->get_logger(), "start to wait a future");
    std::get<2>(tuple).wait(); // wait forever until the future is done
    std::get<1>(tuple) = true;
  }

  std::this_thread::sleep_for(1.5s);
  
  for (const auto &tuple : futures_tuple)
  {
    nlohmann::json result_req_json = {
      { "dispenserStation", std::get<0>(tuple) },
      { "isCompleted", std::get<1>(tuple) ? 1 : 0 }
    };
    nlohmann::json result_res_json;

    // std::thread(std::bind(&ProdLineCtrl::dis_result_until_success, this, result_req_json, result_req_json)).detach(); 
    auto func = std::bind(&ProdLineCtrl::dis_result, this, _1, _2);
    std::thread result_thread(
      std::bind(&ProdLineCtrl::perform_until_success, 
        this, 
        std::cref(result_req_json), 
        std::ref(result_res_json), 
        std::ref(func)));
    result_thread.detach(); 
  }
  
  RCLCPP_DEBUG(this->get_logger(), "%s is done.", __FUNCTION__);
}

void ProdLineCtrl::perform_until_success(
  const nlohmann::json &req_json, 
  nlohmann::json &res_json, 
  std::function<bool(const nlohmann::json&, nlohmann::json&)> func) 
{
  while (rclcpp::ok() && !func(req_json, res_json)) 
  {
    res_json.clear();
    std::this_thread::sleep_for(100ms);
  }
}

void ProdLineCtrl::new_order_until_success(const nlohmann::json &req_json, nlohmann::json &res_json)
{
  while (rclcpp::ok() && !new_order(req_json, res_json))
  {
    res_json.clear();
    std::this_thread::sleep_for(100ms);
  }
}

void ProdLineCtrl::dis_result_until_success(const nlohmann::json &req_json, nlohmann::json &res_json)
{
  while (rclcpp::ok() && !dis_result(req_json, res_json))
  {
    res_json.clear();
    std::this_thread::sleep_for(100ms);
  }
}

rclcpp_action::GoalResponse ProdLineCtrl::handle_goal(
  const rclcpp_action::GoalUUID & uuid, 
  std::shared_ptr<const NewOrder::Goal> goal)
{
  (void)uuid;
  (void)goal;
  RCLCPP_INFO(this->get_logger(), "Received a goal, start to proceess");

  // FIXME: quick fix, assume no error
  if (0)
    return rclcpp_action::GoalResponse::REJECT;

  return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
}

rclcpp_action::CancelResponse ProdLineCtrl::handle_cancel(
  const std::shared_ptr<GaolHandlerNewOrder> goal_handle)
{
  RCLCPP_INFO(this->get_logger(), "Received request to cancel goal");
  (void)goal_handle;

  return rclcpp_action::CancelResponse::ACCEPT;
}

void ProdLineCtrl::handle_accepted(const std::shared_ptr<GaolHandlerNewOrder> goal_handle)
{
  std::thread{std::bind(&ProdLineCtrl::order_execute, this, _1), goal_handle}.detach();
}

void ProdLineCtrl::order_execute(const std::shared_ptr<GaolHandlerNewOrder> goal_handle)
{
  const auto goal = goal_handle->get_goal();
  auto feedback = std::make_shared<NewOrder::Feedback>();
  auto &running = feedback->running;
  auto result = std::make_shared<NewOrder::Result>();
  RCLCPP_INFO(this->get_logger(), "Executing goal");
  
  nlohmann::json req_json, res_json;
  for (size_t i = 0; i < goal->request.material_box.slots.size(); i++) 
  {
    auto &slots_i = goal->request.material_box.slots[i];
    nlohmann::json _cell;

    for (const auto &drugs_j : slots_i.drugs)
    {
      nlohmann::json _drug;
      _drug["amount"] = drugs_j.amount;
      _drug["drugId"] = drugs_j.drug_id;

      for (const auto &location_j : drugs_j.locations)
      {
        nlohmann::json location = {
          {"dispenserStation", location_j.dispenser_station},
          {"dispenserUnit", location_j.dispenser_unit}
        };

        _drug["locations"].push_back(location);
      }
      RCLCPP_INFO(this->get_logger(), "a drug is added to a cell");
      _cell["drugs"].push_back(_drug);
    }

    req_json["cells"].push_back(_cell);
    RCLCPP_INFO(this->get_logger(), "a cell is added to req_json, i: %ld", i);
  }

  running = true;
  goal_handle->publish_feedback(feedback);
  RCLCPP_INFO(this->get_logger(), "A new order json is created. Set running to %s", running ? "true" : "false");
  RCLCPP_INFO(this->get_logger(), "\n%s", req_json.dump().c_str());

  auto func = std::bind(&ProdLineCtrl::new_order, this, _1, _2);
  perform_until_success(req_json, res_json, func);
  // new_order_until_success(req_json, res_json);

  // if (!new_order(req_json, res_json))
  // {
  //   const std::string msg = "newOrder API is failed";
  //   result->response.success = false;
  //   result->response.message = msg;

  //   if (rclcpp::ok()) 
  //   {
  //     goal_handle->succeed(result);
  //     RCLCPP_ERROR(this->get_logger(), msg.c_str());
  //   }
  //   return;
  // }

  RCLCPP_INFO(this->get_logger(), "A new order is sent, waiting for material box id...");

  const httplib::Params params = {
    {"orderId", res_json["orderId"]}
  };

  uint32_t retries = 0;
  const uint32_t MAX_RETRY = 600; // FIXME: this unit is seconds
  rclcpp::Rate loop_rate(1s); 

  for (; retries < MAX_RETRY && rclcpp::ok(); ++retries) 
  {
    RCLCPP_INFO(this->get_logger(), "Waiting for the material box id (%d times)...", retries + 1);
    nlohmann::json order_by_id_res_json;

    if (get_order_by_id(params, order_by_id_res_json))
    {
      int id = order_by_id_res_json["materialBoxId"].get<int>();
      if (id != 0)
      {
        result->response.material_box_id = id;
        result->response.success = true;
        break;
      }
      else
      {
        RCLCPP_ERROR(this->get_logger(), "Received a material box id is 0");
      }
    }

    goal_handle->publish_feedback(feedback);
    loop_rate.sleep();
  }

  if (retries >= MAX_RETRY)
  {
    result->response.success = false;
    result->response.message = "The material box is waited too long";
    RCLCPP_ERROR(this->get_logger(), "retries (%d) >= MAX_RETRY", retries);
  }

  if (rclcpp::ok()) 
  {
    goal_handle->succeed(result);
    RCLCPP_INFO(this->get_logger(), "Goal succeeded");
  }
}

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);

  auto exec = std::make_shared<rclcpp::executors::MultiThreadedExecutor>();
  auto options = rclcpp::NodeOptions();
  auto node = std::make_shared<ProdLineCtrl>(options);

  exec->add_node(node->get_node_base_interface());
  exec->spin();

  rclcpp::shutdown();
}