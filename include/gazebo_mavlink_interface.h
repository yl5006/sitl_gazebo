/*
 * Copyright 2015 Fadri Furrer, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Michael Burri, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Mina Kamel, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Janosch Nikolic, ASL, ETH Zurich, Switzerland
 * Copyright 2015 Markus Achtelik, ASL, ETH Zurich, Switzerland
 * Copyright 2015-2017 PX4 Pro Development Team
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>
#include <deque>
#include <random>
#include <stdio.h>
#include <math.h>
#include <cstdlib>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <boost/bind.hpp>
#include <Eigen/Eigen>

#include <gazebo/gazebo.hh>
#include <gazebo/math/Vector3.hh>
#include <gazebo/common/common.hh>
#include <gazebo/common/Plugin.hh>
#include <gazebo/physics/physics.hh>
#include <gazebo/transport/transport.hh>
#include <gazebo/msgs/msgs.hh>

#include <sdf/sdf.hh>
#include <common.h>
#include <CommandMotorSpeed.pb.h>
#include <MotorSpeed.pb.h>
#include <SensorImu.pb.h>
#include <opticalFlow.pb.h>
#include <lidar.pb.h>
#include <sonarSens.pb.h>
#include <SITLGps.pb.h>
#include <irlock.pb.h>
#include <Groundtruth.pb.h>

#include <mavlink/v2.0/common/mavlink.h>

#include <geo_mag_declination.h>

static const uint32_t kDefaultMavlinkUdpPort = 14560;

namespace gazebo {
typedef const boost::shared_ptr<const mav_msgs::msgs::CommandMotorSpeed> CommandMotorSpeedPtr;
typedef const boost::shared_ptr<const sensor_msgs::msgs::Imu> ImuPtr;
typedef const boost::shared_ptr<const lidar_msgs::msgs::lidar> LidarPtr;
typedef const boost::shared_ptr<const opticalFlow_msgs::msgs::opticalFlow> OpticalFlowPtr;
typedef const boost::shared_ptr<const sonarSens_msgs::msgs::sonarSens> SonarSensPtr;
typedef const boost::shared_ptr<const irlock_msgs::msgs::irlock> IRLockPtr;
typedef const boost::shared_ptr<const gps_msgs::msgs::SITLGps> GpsPtr;
typedef const boost::shared_ptr<const gps_msgs::msgs::Groundtruth> GtPtr;

// Default values
static const std::string kDefaultNamespace = "";

// This just proxies the motor commands from command/motor_speed to the single motors via internal
// ConsPtr passing, such that the original commands don't have to go n_motors-times over the wire.
static const std::string kDefaultMotorVelocityReferencePubTopic = "/gazebo/command/motor_speed";

static const std::string kDefaultImuTopic = "/imu";
static const std::string kDefaultLidarTopic = "/lidar/link/lidar";
static const std::string kDefaultOpticalFlowTopic = "/camera/link/opticalFlow";
static const std::string kDefaultSonarTopic = "/sonar_model/link/sonar";
static const std::string kDefaultIRLockTopic = "/camera/link/irlock";

class GazeboMavlinkInterface : public ModelPlugin {
public:
  GazeboMavlinkInterface() : ModelPlugin(),
    received_first_referenc_(false),
    namespace_(kDefaultNamespace),
    motor_velocity_reference_pub_topic_(kDefaultMotorVelocityReferencePubTopic),
    imu_sub_topic_(kDefaultImuTopic),
    opticalFlow_sub_topic_(kDefaultOpticalFlowTopic),
    lidar_sub_topic_(kDefaultLidarTopic),
    sonar_sub_topic_(kDefaultSonarTopic),
    irlock_sub_topic_(kDefaultIRLockTopic),
    model_ {},
    world_(nullptr),
    left_elevon_joint_(nullptr),
    right_elevon_joint_(nullptr),
    elevator_joint_(nullptr),
    propeller_joint_(nullptr),
    gimbal_yaw_joint_(nullptr),
    gimbal_pitch_joint_(nullptr),
    gimbal_roll_joint_(nullptr),
    input_offset_ {},
    input_scaling_ {},
    zero_position_disarmed_ {},
    zero_position_armed_ {},
    input_index_ {},
    groundtruth_lat_rad(0.0),
    groundtruth_lon_rad(0.0),
    groundtruth_altitude(0.0),
    mavlink_udp_port_(kDefaultMavlinkUdpPort)
  {}

  ~GazeboMavlinkInterface();

  void Publish();

protected:
  void Load(physics::ModelPtr _model, sdf::ElementPtr _sdf);
  void OnUpdate(const common::UpdateInfo&  /*_info*/);

private:

  bool received_first_referenc_;
  Eigen::VectorXd input_reference_;

  std::string namespace_;
  std::string motor_velocity_reference_pub_topic_;
  std::string mavlink_control_sub_topic_;
  std::string link_name_;

  transport::NodePtr node_handle_;
  transport::PublisherPtr motor_velocity_reference_pub_;
  transport::SubscriberPtr mav_control_sub_;

  physics::ModelPtr model_;
  physics::WorldPtr world_;
  physics::JointPtr left_elevon_joint_;
  physics::JointPtr right_elevon_joint_;
  physics::JointPtr elevator_joint_;
  physics::JointPtr propeller_joint_;
  physics::JointPtr gimbal_yaw_joint_;
  physics::JointPtr gimbal_pitch_joint_;
  physics::JointPtr gimbal_roll_joint_;
  common::PID propeller_pid_;
  common::PID elevator_pid_;
  common::PID left_elevon_pid_;
  common::PID right_elevon_pid_;
  bool use_propeller_pid_;
  bool use_elevator_pid_;
  bool use_left_elevon_pid_;
  bool use_right_elevon_pid_;

  std::vector<physics::JointPtr> joints_;
  std::vector<common::PID> pids_;

  /// \brief Pointer to the update event connection.
  event::ConnectionPtr updateConnection_;

  boost::thread callback_queue_thread_;
  void QueueThread();
  void ImuCallback(ImuPtr& imu_msg);
  void GpsCallback(GpsPtr& gps_msg);
  void GroundtruthCallback(GtPtr& groundtruth_msg);
  void LidarCallback(LidarPtr& lidar_msg);
  void SonarCallback(SonarSensPtr& sonar_msg);
  void OpticalFlowCallback(OpticalFlowPtr& opticalFlow_msg);
  void IRLockCallback(IRLockPtr& irlock_msg);
  void send_mavlink_message(const mavlink_message_t *message, const int destination_port = 0);
  void handle_message(mavlink_message_t *msg);
  void pollForMAVLinkMessages(double _dt, uint32_t _timeoutMs);

  static const unsigned n_out_max = 16;
  double alt_home = 488.0;   // meters

  math::Vector3 ev_bias;
  math::Vector3 noise_ev;
  math::Vector3 random_walk_ev;

  // vision position estimate noise parameters
  static constexpr double ev_corellation_time = 60.0;  // s
  static constexpr double ev_random_walk = 2.0;        // (m/s) / sqrt(hz)
  static constexpr double ev_noise_density = 2e-4;     // (m) / sqrt(hz)

  unsigned _rotor_count;

  double input_offset_[n_out_max];
  double input_scaling_[n_out_max];
  std::string joint_control_type_[n_out_max];
  std::string gztopic_[n_out_max];
  double zero_position_disarmed_[n_out_max];
  double zero_position_armed_[n_out_max];
  int input_index_[n_out_max];
  transport::PublisherPtr joint_control_pub_[n_out_max];

  transport::SubscriberPtr imu_sub_;
  transport::SubscriberPtr lidar_sub_;
  transport::SubscriberPtr sonar_sub_;
  transport::SubscriberPtr opticalFlow_sub_;
  transport::SubscriberPtr irlock_sub_;
  transport::SubscriberPtr gps_sub_;
  transport::SubscriberPtr groundtruth_sub_;

  std::string imu_sub_topic_;
  std::string lidar_sub_topic_;
  std::string opticalFlow_sub_topic_;
  std::string sonar_sub_topic_;
  std::string irlock_sub_topic_;
  std::string gps_sub_topic_;
  std::string groundtruth_sub_topic_;

  common::Time last_time_;
  common::Time last_imu_time_;
  common::Time last_ev_time_;
  common::Time last_actuator_time_;

  bool set_imu_rate_;
  double imu_rate_;

  double groundtruth_lat_rad;
  double groundtruth_lon_rad;
  double groundtruth_altitude;

  double ev_update_interval_;
  double gps_update_interval_;

  void handle_control(double _dt);

  math::Vector3 gravity_W_;
  math::Vector3 velocity_prev_W_;
  math::Vector3 mag_d_;

  std::default_random_engine rand_;
  std::normal_distribution<float> randn_;

  int _fd;
  struct sockaddr_in _myaddr;     ///< The locally bound address
  struct sockaddr_in _srcaddr;    ///< SITL instance
  socklen_t _addrlen;
  unsigned char _buf[65535];
  struct pollfd fds[1];

  struct sockaddr_in _srcaddr_2;  ///< MAVROS

  //so we dont have to do extra callbacks
  math::Vector3 optflow_gyro {};
  double optflow_distance;
  double sonar_distance;

  in_addr_t mavlink_addr_;
  int mavlink_udp_port_;
};
}
