#include "thruster_private.hpp"

#include <gz/sim/components/ExternalWorldWrenchCmd.hh>
#include <gz/sim/components/JointVelocity.hh>
#include <gz/sim/components/JointVelocityCmd.hh>
#include <gz/sim/components/ParentLinkName.hh>
#include <gz/sim/components/Pose.hh>

#include "common.hpp"

namespace thruster {
namespace motor_direction {
static constexpr int CCW = -1;
static constexpr int CW = 1;
}  // namespace motor_direction
namespace propeller_direction {
static constexpr int CCW = 1;
static constexpr int CW = -1;
}  // namespace propeller_direction

void PluginPrivate::ParseSdf(const std::shared_ptr<const sdf::Element> &_sdf) {
  AssignSdfParam(_sdf, "link", sdf_params_.link);
  AssignSdfParam(_sdf, "joint", sdf_params_.joint);
  AssignSdfParam(_sdf, "publish_rate", sdf_params_.publish_rate);
  AssignSdfParam(_sdf, "rpm_base_topic", sdf_params_.rpm_base_topic);
  AssignSdfParam(_sdf, "throttle_cmd_base_topic",
                 sdf_params_.throttle_cmd_base_topic);
  AssignSdfParam(_sdf, "constant_coeff", sdf_params_.constant_coeff);
  AssignSdfParam(_sdf, "linear_coeff", sdf_params_.linear_coeff);
  AssignSdfParam(_sdf, "quadratic_coeff", sdf_params_.quadratic_coeff);
  AssignSdfParam(_sdf, "torque_coeff", sdf_params_.torque_coeff);
  AssignSdfParam(_sdf, "rpm_scaler", sdf_params_.rpm_scaler);
  AssignSdfParam(_sdf, "maximum_rpm", sdf_params_.maximum_rpm);
  AssignSdfParam(_sdf, "thruster_number", sdf_params_.thruster_number);
  AssignSdfParam(_sdf, "turning_direction", sdf_params_.turning_direction);
  if (sdf_params_.turning_direction == "cw") {
    turning_direction_ = motor_direction::CW;
  } else {
    turning_direction_ = motor_direction::CCW;
  }
  AssignSdfParam(_sdf, "propeller_direction", sdf_params_.propeller_direction);
  if (sdf_params_.propeller_direction == "cw") {
    propeller_direction_ = propeller_direction::CW;
  } else {
    propeller_direction_ = propeller_direction::CCW;
  }
  AssignSdfParam(_sdf, "timeconstant_up", sdf_params_.timeconstant_up);
  AssignSdfParam(_sdf, "timeconstant_down", sdf_params_.timeconstant_down);
}

bool PluginPrivate::InitModel(gz::sim::EntityComponentManager &_ecm,
                              gz::sim::Entity _entity) {
  model_ = gz::sim::Model(_entity);
  if (!model_.Valid(_ecm)) {
    return false;
  }
  model_name_ = model_.Name(_ecm);
  InitComponents(_ecm);
  rotor_velocity_filter_ = std::make_unique<FirstOrderFilter<double>>(
      sdf_params_.timeconstant_up, sdf_params_.timeconstant_down,
      rotor_velocity_setpoint_);
  return true;
}

void PluginPrivate::ApplyWrench(gz::sim::EntityComponentManager &_ecm) {
  if (!_ecm.HasEntity(link_.Entity())) {
    // ignerr << "Entity not exsting/deleted.";
    return;
  }
  auto pose = link_.WorldPose(_ecm);
  auto parent_pose = parent_link_.WorldPose(_ecm);
  auto pose_diff = (*parent_pose).Inverse() * (*pose);
  link_.AddWorldForce(_ecm, pose->Rot().RotateVector(ThrusterForce()));

  auto parent_wrench_component =
      _ecm.Component<gz::sim::components::ExternalWorldWrenchCmd>(
          parent_link_.Entity());
  auto parent_torque = pose_diff.Rot().RotateVector(Torque());
  auto parent_torque_world = parent_pose->Rot().RotateVector(parent_torque);
  gz::msgs::Set(parent_wrench_component->Data().mutable_torque(),
                gz::msgs::Convert(parent_wrench_component->Data().torque()) +
                    parent_torque_world);
}

void PluginPrivate::UpdateRotorVelocity(gz::sim::EntityComponentManager &_ecm,
                                        double _dt) {
  {
    std::lock_guard<std::mutex> lock(thrust_cmd_mutex_);
    rotor_velocity_ =
        rotor_velocity_filter_->Update(rotor_velocity_setpoint_, _dt);
  }
  SetRotorVelocity(_ecm, rotor_velocity_);
}

/**
 * @brief Computes thruster force  as T(x) = a * x² + b * x, where x is
 * rotations per second. The sign also depends on the propeller direction.
 *
 * @return gz::math::Vector3d
 */
gz::math::Vector3d PluginPrivate::ThrusterForce() {
  double thrust;
  // get rotations per second
  double tmp = std::abs(rotor_velocity_ / 6.28);
  thrust = tmp * tmp * sdf_params_.quadratic_coeff +
           tmp * sdf_params_.linear_coeff + sdf_params_.constant_coeff;
  thrust = thrust < 0.0 ? 0.0 : thrust;
  if (rotor_velocity_ < 0.0) {
    thrust *= -1.0;
  }
  double force = propeller_direction_ * thrust;
  return gz::math::Vector3d(force, 0, 0);
}

gz::math::Vector3d PluginPrivate::Torque() {
  return turning_direction_ * propeller_direction_ * ThrusterForce() *
         sdf_params_.torque_coeff;
}

void PluginPrivate::SetRotorVelocity(gz::sim::EntityComponentManager &_ecm,
                                     double _velocity) {
  auto velocity_component =
      _ecm.Component<gz::sim::components::JointVelocityCmd>(joint_entity_);
  if (!velocity_component) {
    // ignerr << "Missing JointVelocityCmd component!" << std::endl;
    return;
  }
  if (!velocity_component->Data().empty()) {
    velocity_component->Data()[0] = _velocity / sdf_params_.rpm_scaler;
  }
}

double PluginPrivate::RotorVelocity(
    const gz::sim::EntityComponentManager &_ecm) {
  auto velocity_component =
      _ecm.Component<gz::sim::components::JointVelocity>(joint_entity_);
  if (!velocity_component) {
    // ignerr << "Joint has no JointVelocity component!" << std::endl;
    return 0.0;
  }
  if (!velocity_component->Data().empty()) {
    return velocity_component->Data()[0] * sdf_params_.rpm_scaler;
  }
  // ignerr << "Joint velocity data component is empty!" << std::endl;
  return 0.0;
}

void PluginPrivate::PublishRpm(const gz::sim::EntityComponentManager &_ecm) {
  gz::msgs::Double msg;
  msg.set_data(RotorVelocity(_ecm));
  rpm_publisher_.Publish(msg);
}

void PluginPrivate::PublishThrust() {
  gz::math::Vector3d f = ThrusterForce();
  gz::msgs::Double msg;
  msg.set_data(f.X());
  thrust_publisher_.Publish(msg);
}

void PluginPrivate::ThrottleCmdTimedOut() {
  std::lock_guard<std::mutex> lock(thrust_cmd_mutex_);
  rotor_velocity_setpoint_ = 0.0;
}

void PluginPrivate::OnThrottleCmd(const gz::msgs::Double &_msg) {
  std::lock_guard<std::mutex> lock(thrust_cmd_mutex_);
  rotor_velocity_setpoint_ = ThrottleToVelocity(_msg.data());
  throttle_cmd_updated_ = true;
}

void PluginPrivate::InitComponents(gz::sim::EntityComponentManager &_ecm) {
  //////////////////////////////////////////////////////////////////////////////
  // joint components
  //////////////////////////////////////////////////////////////////////////////
  joint_entity_ = model_.JointByName(_ecm, sdf_params_.joint);
  if (joint_entity_ == gz::sim::kNullEntity) {
    ignerr << "Joint with name [" << sdf_params_.joint << "] not found!"
           << std::endl;
    return;
  }

  if (!_ecm.Component<gz::sim::components::JointVelocity>(joint_entity_)) {
    _ecm.CreateComponent(joint_entity_,
                         gz::sim::components::JointVelocity({0.0}));
  }

  if (!_ecm.Component<gz::sim::components::JointVelocityCmd>(joint_entity_)) {
    _ecm.CreateComponent(joint_entity_,
                         gz::sim::components::JointVelocityCmd({0.0}));
  }

  //////////////////////////////////////////////////////////////////////////////
  // thruster link
  //////////////////////////////////////////////////////////////////////////////
  link_ = gz::sim::Link(model_.LinkByName(_ecm, sdf_params_.link));
  if (!_ecm.Component<gz::sim::components::WorldPose>(link_.Entity())) {
    _ecm.CreateComponent(link_.Entity(), gz::sim::components::WorldPose());
  }

  //////////////////////////////////////////////////////////////////////////////
  // parent link components
  //////////////////////////////////////////////////////////////////////////////
  std::string parent_name =
      _ecm.Component<gz::sim::components::ParentLinkName>(joint_entity_)
          ->Data();
  auto parent_entity = model_.LinkByName(_ecm, parent_name);
  parent_link_ = gz::sim::Link(parent_entity);

  if (!_ecm.Component<gz::sim::components::WorldPose>(parent_entity)) {
    _ecm.CreateComponent(parent_entity, gz::sim::components::WorldPose());
  }

  if (!_ecm.Component<gz::sim::components::ExternalWorldWrenchCmd>(
          parent_entity)) {
    _ecm.CreateComponent(parent_entity,
                         gz::sim::components::ExternalWorldWrenchCmd());
  }
}
}  // namespace thruster
