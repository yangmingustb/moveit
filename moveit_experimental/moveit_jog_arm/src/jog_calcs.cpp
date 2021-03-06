/*******************************************************************************
 * BSD 3-Clause License
 *
 * Copyright (c) 2019, Los Alamos National Security, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

/*      Title     : jog_calcs.cpp
 *      Project   : moveit_jog_arm
 *      Created   : 1/11/2019
 *      Author    : Brian O'Neil, Andy Zelenak, Blake Anderson
 */

#include <moveit_jog_arm/jog_calcs.h>

static const std::string LOGNAME = "jog_calcs";

namespace moveit_jog_arm
{
// Constructor for the class that handles jogging calculations
JogCalcs::JogCalcs(const JogArmParameters& parameters, const robot_model_loader::RobotModelLoaderPtr& model_loader_ptr)
  : parameters_(parameters), default_sleep_rate_(1000)
{
  // Publish collision status
  warning_pub_ = nh_.advertise<std_msgs::Bool>(parameters_.warning_topic, 1);

  // MoveIt Setup
  while (ros::ok() && !model_loader_ptr)
  {
    ROS_WARN_THROTTLE_NAMED(5, LOGNAME, "Waiting for a non-null robot_model_loader pointer");
    default_sleep_rate_.sleep();
  }
  const robot_model::RobotModelPtr& kinematic_model = model_loader_ptr->getModel();
  kinematic_state_ = std::make_shared<robot_state::RobotState>(kinematic_model);
  kinematic_state_->setToDefaultValues();

  joint_model_group_ = kinematic_model->getJointModelGroup(parameters_.move_group_name);
}

void JogCalcs::startMainLoop(JogArmShared& shared_variables, std::mutex& mutex)
{
  // Reset flags
  stop_jog_loop_requested_ = false;
  halt_outgoing_jog_cmds_ = false;
  is_initialized_ = false;

  // Wait for initial messages
  ROS_INFO_NAMED(LOGNAME, "jog_calcs_thread: Waiting for first joint msg.");
  ros::topic::waitForMessage<sensor_msgs::JointState>(parameters_.joint_topic);
  ROS_INFO_NAMED(LOGNAME, "jog_calcs_thread: Received first joint msg.");

  joint_state_.name = joint_model_group_->getVariableNames();
  num_joints_ = joint_state_.name.size();
  joint_state_.position.resize(num_joints_);
  joint_state_.velocity.resize(num_joints_);
  joint_state_.effort.resize(num_joints_);
  // A map for the indices of incoming joint commands
  for (std::size_t i = 0; i < joint_state_.name.size(); ++i)
  {
    joint_state_name_map_[joint_state_.name[i]] = i;
  }

  // Low-pass filters for the joint positions & velocities
  for (size_t i = 0; i < num_joints_; ++i)
  {
    position_filters_.emplace_back(parameters_.low_pass_filter_coeff);
  }

  // Initialize the position filters to initial robot joints
  while (!updateJoints(mutex, shared_variables) && ros::ok())
  {
    if (stop_jog_loop_requested_)
      return;

    mutex.lock();
    incoming_joints_ = shared_variables.joints;
    mutex.unlock();
    default_sleep_rate_.sleep();
  }

  is_initialized_ = true;

  // Wait for the first jogging cmd.
  // Store it in a class member for further calcs, then free up the shared variable again.
  geometry_msgs::TwistStamped cartesian_deltas;
  control_msgs::JointJog joint_deltas;
  while (ros::ok() && (cartesian_deltas.header.stamp == ros::Time(0.)) && (joint_deltas.header.stamp == ros::Time(0.)))
  {
    if (stop_jog_loop_requested_)
      return;

    default_sleep_rate_.sleep();

    // Ensure the low-pass filter matches reality
    for (std::size_t i = 0; i < num_joints_; ++i)
      position_filters_[i].reset(joint_state_.position[i]);

    //  Check for a new command
    mutex.lock();
    cartesian_deltas = shared_variables.command_deltas;
    joint_deltas = shared_variables.joint_command_deltas;
    incoming_joints_ = shared_variables.joints;
    mutex.unlock();

    kinematic_state_->setVariableValues(joint_state_);

    // Always update the end-effector transform in case the getCommandFrameTransform() method is being used
    // Get the transform from MoveIt planning frame to jogging command frame
    // We solve (planning_frame -> base -> robot_link_command_frame)
    // by computing (base->planning_frame)^-1 * (base->robot_link_command_frame)
    tf_moveit_to_cmd_frame_ = kinematic_state_->getGlobalLinkTransform(parameters_.planning_frame).inverse() *
                              kinematic_state_->getGlobalLinkTransform(parameters_.robot_link_command_frame);

    mutex.lock();
    shared_variables.tf_moveit_to_cmd_frame = tf_moveit_to_cmd_frame_;
    mutex.unlock();
  }

  // Track the number of cycles during which motion has not occurred.
  // Will avoid re-publishing zero velocities endlessly.
  int zero_velocity_count = 0;

  ros::Rate loop_rate(1. / parameters_.publish_period);

  // Now do jogging calcs
  while (ros::ok() && !stop_jog_loop_requested_)
  {
    // Always update the joints and end-effector transform for 2 reasons:
    // 1) in case the getCommandFrameTransform() method is being used
    // 2) so the low-pass filters are up to date and don't cause a jump
    while (!updateJoints(mutex, shared_variables) && ros::ok())
    {
      default_sleep_rate_.sleep();
    }
    kinematic_state_->setVariableValues(joint_state_);

    // Get the transform from MoveIt planning frame to jogging command frame
    // We solve (planning_frame -> base -> robot_link_command_frame)
    // by computing (base->planning_frame)^-1 * (base->robot_link_command_frame)
    tf_moveit_to_cmd_frame_ = kinematic_state_->getGlobalLinkTransform(parameters_.planning_frame).inverse() *
                              kinematic_state_->getGlobalLinkTransform(parameters_.robot_link_command_frame);
    mutex.lock();
    shared_variables.tf_moveit_to_cmd_frame = tf_moveit_to_cmd_frame_;
    mutex.unlock();

    // If paused, just keep the low-pass filters up to date with current joints so a jump doesn't occur when
    // restarting
    if (halt_outgoing_jog_cmds_)
    {
      for (std::size_t i = 0; i < num_joints_; ++i)
        position_filters_[i].reset(joint_state_.position[i]);
    }
    // Do jogging calculations only if the robot should move, for efficiency
    else
    {
      // Flag that incoming commands are all zero. May be used to skip calculations/publication
      mutex.lock();
      bool zero_cartesian_cmd_flag = shared_variables.zero_cartesian_cmd_flag;
      bool zero_joint_cmd_flag = shared_variables.zero_joint_cmd_flag;
      mutex.unlock();

      // Prioritize cartesian jogging above joint jogging
      if (!zero_cartesian_cmd_flag)
      {
        mutex.lock();
        cartesian_deltas = shared_variables.command_deltas;
        mutex.unlock();

        if (!cartesianJogCalcs(cartesian_deltas, shared_variables, mutex))
          continue;
      }
      else if (!zero_joint_cmd_flag)
      {
        mutex.lock();
        joint_deltas = shared_variables.joint_command_deltas;
        mutex.unlock();

        if (!jointJogCalcs(joint_deltas, shared_variables))
          continue;
      }
      else
      {
        outgoing_command_ = composeJointTrajMessage(joint_state_);
      }

      // Halt if the command is stale or inputs are all zero, or commands were zero
      mutex.lock();
      bool stale_command = shared_variables.command_is_stale;
      mutex.unlock();

      if (stale_command || (zero_cartesian_cmd_flag && zero_joint_cmd_flag))
      {
        suddenHalt(outgoing_command_);
        zero_cartesian_cmd_flag = true;
        zero_joint_cmd_flag = true;
      }

      bool valid_nonzero_command = !zero_cartesian_cmd_flag || !zero_joint_cmd_flag;

      // Send the newest target joints
      mutex.lock();
      // If everything normal, share the new traj to be published
      if (valid_nonzero_command)
      {
        shared_variables.outgoing_command = outgoing_command_;
        shared_variables.ok_to_publish = true;
      }
      // Skip the jogging publication if all inputs have been zero for several cycles in a row.
      // num_outgoing_halt_msgs_to_publish == 0 signifies that we should keep republishing forever.
      else if ((parameters_.num_outgoing_halt_msgs_to_publish != 0) &&
               (zero_velocity_count > parameters_.num_outgoing_halt_msgs_to_publish))
      {
        shared_variables.ok_to_publish = false;
      }
      // The command is invalid but we are publishing num_outgoing_halt_msgs_to_publish
      else
      {
        shared_variables.outgoing_command = outgoing_command_;
        shared_variables.ok_to_publish = true;
      }
      mutex.unlock();

      // Store last zero-velocity message flag to prevent superfluous warnings.
      // Cartesian and joint commands must both be zero.
      if (zero_cartesian_cmd_flag && zero_joint_cmd_flag)
      {
        // Avoid overflow
        if (zero_velocity_count < std::numeric_limits<int>::max())
          ++zero_velocity_count;
      }
      else
        zero_velocity_count = 0;
    }

    loop_rate.sleep();
  }
}

void JogCalcs::stopMainLoop()
{
  stop_jog_loop_requested_ = true;
}

void JogCalcs::haltOutgoingJogCmds()
{
  halt_outgoing_jog_cmds_ = true;
}

bool JogCalcs::isInitialized()
{
  return is_initialized_;
}

// Perform the jogging calculations
bool JogCalcs::cartesianJogCalcs(geometry_msgs::TwistStamped& cmd, JogArmShared& shared_variables, std::mutex& mutex)
{
  // Check for nan's in the incoming command
  if (std::isnan(cmd.twist.linear.x) || std::isnan(cmd.twist.linear.y) || std::isnan(cmd.twist.linear.z) ||
      std::isnan(cmd.twist.angular.x) || std::isnan(cmd.twist.angular.y) || std::isnan(cmd.twist.angular.z))
  {
    ROS_WARN_STREAM_THROTTLE_NAMED(2, LOGNAME, "nan in incoming command. Skipping this datapoint.");
    return false;
  }

  // If incoming commands should be in the range [-1:1], check for |delta|>1
  if (parameters_.command_in_type == "unitless")
  {
    if ((fabs(cmd.twist.linear.x) > 1) || (fabs(cmd.twist.linear.y) > 1) || (fabs(cmd.twist.linear.z) > 1) ||
        (fabs(cmd.twist.angular.x) > 1) || (fabs(cmd.twist.angular.y) > 1) || (fabs(cmd.twist.angular.z) > 1))
    {
      ROS_WARN_STREAM_THROTTLE_NAMED(2, LOGNAME, "Component of incoming command is >1. Skipping this datapoint.");
      return false;
    }
  }

  // Transform the command to the MoveGroup planning frame
  if (cmd.header.frame_id != parameters_.planning_frame)
  {
    Eigen::Vector3d translation_vector(cmd.twist.linear.x, cmd.twist.linear.y, cmd.twist.linear.z);
    Eigen::Vector3d angular_vector(cmd.twist.angular.x, cmd.twist.angular.y, cmd.twist.angular.z);

    translation_vector = tf_moveit_to_cmd_frame_.linear() * translation_vector;
    angular_vector = tf_moveit_to_cmd_frame_.linear() * angular_vector;

    // Put these components back into a TwistStamped
    cmd.header.frame_id = parameters_.planning_frame;
    cmd.twist.linear.x = translation_vector(0);
    cmd.twist.linear.y = translation_vector(1);
    cmd.twist.linear.z = translation_vector(2);
    cmd.twist.angular.x = angular_vector(0);
    cmd.twist.angular.y = angular_vector(1);
    cmd.twist.angular.z = angular_vector(2);
  }

  Eigen::VectorXd delta_x = scaleCartesianCommand(cmd);

  // Convert from cartesian commands to joint commands
  Eigen::MatrixXd jacobian = kinematic_state_->getJacobian(joint_model_group_);

  // May allow some dimensions to drift, based on shared_variables.drift_dimensions
  // i.e. take advantage of task redundancy.
  // Remove the Jacobian rows corresponding to True in the vector shared_variables.drift_dimensions
  // Work backwards through the 6-vector so indices don't get out of order
  for (auto dimension = jacobian.rows(); dimension >= 0; --dimension)
  {
    if (shared_variables.drift_dimensions[dimension] && jacobian.rows() > 1)
    {
      removeDimension(jacobian, delta_x, dimension);
    }
  }

  Eigen::JacobiSVD<Eigen::MatrixXd> svd =
      Eigen::JacobiSVD<Eigen::MatrixXd>(jacobian, Eigen::ComputeThinU | Eigen::ComputeThinV);
  Eigen::MatrixXd matrix_s = svd.singularValues().asDiagonal();
  Eigen::MatrixXd pseudo_inverse = svd.matrixV() * matrix_s.inverse() * svd.matrixU().transpose();

  delta_theta_ = pseudo_inverse * delta_x;

  // If close to a collision or a singularity, decelerate
  if (!applyVelocityScaling(shared_variables, mutex, delta_theta_,
                            velocityScalingFactorForSingularity(delta_x, svd, jacobian, pseudo_inverse)))
  {
    has_warning_ = true;
    suddenHalt(outgoing_command_);
  }

  return convertDeltasToOutgoingCmd();
}

bool JogCalcs::jointJogCalcs(const control_msgs::JointJog& cmd, JogArmShared& /*shared_variables*/)
{
  // Check for nan's or |delta|>1 in the incoming command
  for (double velocity : cmd.velocities)
  {
    if (std::isnan(velocity) || (fabs(velocity) > 1))
    {
      ROS_WARN_STREAM_THROTTLE_NAMED(2, LOGNAME, "nan in incoming command. Skipping this datapoint.");
      return false;
    }
  }

  // Apply user-defined scaling
  delta_theta_ = scaleJointCommand(cmd);

  kinematic_state_->setVariableValues(joint_state_);

  return convertDeltasToOutgoingCmd();
}

bool JogCalcs::convertDeltasToOutgoingCmd()
{
  if (!addJointIncrements(joint_state_, delta_theta_))
    return false;

  lowPassFilterPositions(joint_state_);

  // Calculate joint velocities here so that positions are filtered and SRDF bounds still get checked
  calculateJointVelocities(joint_state_, delta_theta_);

  outgoing_command_ = composeJointTrajMessage(joint_state_);

  if (!enforceSRDFJointBounds(outgoing_command_))
  {
    suddenHalt(outgoing_command_);
    has_warning_ = true;
  }

  publishWarning(has_warning_);
  has_warning_ = false;

  // done with calculations
  if (parameters_.use_gazebo)
  {
    insertRedundantPointsIntoTrajectory(outgoing_command_, gazebo_redundant_message_count_);
  }

  return true;
}

// Spam several redundant points into the trajectory. The first few may be skipped if the
// time stamp is in the past when it reaches the client. Needed for gazebo simulation.
// Start from 2 because the first point's timestamp is already 1*parameters_.publish_period
void JogCalcs::insertRedundantPointsIntoTrajectory(trajectory_msgs::JointTrajectory& trajectory, int count) const
{
  auto point = trajectory.points[0];
  // Start from 2 because we already have the first point. End at count+1 so (total #) == count
  for (int i = 2; i < count + 1; ++i)
  {
    point.time_from_start = ros::Duration(i * parameters_.publish_period);
    trajectory.points.push_back(point);
  }
}

void JogCalcs::lowPassFilterPositions(sensor_msgs::JointState& joint_state)
{
  for (size_t i = 0; i < position_filters_.size(); ++i)
  {
    joint_state.position[i] = position_filters_[i].filter(joint_state.position[i]);
  }
}

void JogCalcs::calculateJointVelocities(sensor_msgs::JointState& joint_state, const Eigen::ArrayXd& delta_theta)
{
  for (int i = 0; i < delta_theta.size(); ++i)
  {
    joint_state.velocity[i] = delta_theta[i] / parameters_.publish_period;
  }
}

trajectory_msgs::JointTrajectory JogCalcs::composeJointTrajMessage(sensor_msgs::JointState& joint_state) const
{
  trajectory_msgs::JointTrajectory new_joint_traj;
  new_joint_traj.header.frame_id = parameters_.planning_frame;
  new_joint_traj.header.stamp = ros::Time::now();
  new_joint_traj.joint_names = joint_state.name;

  trajectory_msgs::JointTrajectoryPoint point;
  point.time_from_start = ros::Duration(parameters_.publish_period);
  if (parameters_.publish_joint_positions)
    point.positions = joint_state.position;
  if (parameters_.publish_joint_velocities)
    point.velocities = joint_state.velocity;
  if (parameters_.publish_joint_accelerations)
  {
    // I do not know of a robot that takes acceleration commands.
    // However, some controllers check that this data is non-empty.
    // Send all zeros, for now.
    std::vector<double> acceleration(num_joints_);
    point.accelerations = acceleration;
  }
  new_joint_traj.points.push_back(point);

  return new_joint_traj;
}

// Apply velocity scaling for proximity of collisions and singularities.
// Scale for collisions is read from a shared variable.
bool JogCalcs::applyVelocityScaling(const JogArmShared& shared_variables, std::mutex& mutex,
                                    Eigen::ArrayXd& delta_theta, double singularity_scale)
{
  mutex.lock();
  double collision_scale = shared_variables.collision_velocity_scale;
  mutex.unlock();

  delta_theta = collision_scale * singularity_scale * delta_theta;

  // Heuristic: flag that we are stuck if velocity scaling is < X%
  return collision_scale * singularity_scale >= 0.1;
}

// Possibly calculate a velocity scaling factor, due to proximity of singularity and direction of motion
double JogCalcs::velocityScalingFactorForSingularity(const Eigen::VectorXd& commanded_velocity,
                                                     const Eigen::JacobiSVD<Eigen::MatrixXd>& svd,
                                                     const Eigen::MatrixXd& jacobian,
                                                     const Eigen::MatrixXd& pseudo_inverse)
{
  double velocity_scale = 1;
  std::size_t num_dimensions = jacobian.rows();

  // Find the direction away from nearest singularity.
  // The last column of U from the SVD of the Jacobian points directly toward or away from the singularity.
  // The sign can flip at any time, so we have to do some extra checking.
  // Look ahead to see if the Jacobian's condition will decrease.
  Eigen::VectorXd vector_toward_singularity = svd.matrixU().col(num_dimensions - 1);

  double ini_condition = svd.singularValues()(0) / svd.singularValues()(svd.singularValues().size() - 1);

  // This singular vector tends to flip direction unpredictably. See R. Bro,
  // "Resolving the Sign Ambiguity in the Singular Value Decomposition".
  // Look ahead to see if the Jacobian's condition will decrease in this
  // direction. Start with a scaled version of the singular vector
  Eigen::VectorXd delta_x(num_dimensions);
  double scale = 100;
  delta_x = vector_toward_singularity / scale;

  // Calculate a small change in joints
  Eigen::VectorXd new_theta;
  kinematic_state_->copyJointGroupPositions(joint_model_group_, new_theta);
  new_theta += pseudo_inverse * delta_x;
  kinematic_state_->setJointGroupPositions(joint_model_group_, new_theta);

  Eigen::JacobiSVD<Eigen::MatrixXd> new_svd(jacobian);
  double new_condition = new_svd.singularValues()(0) / new_svd.singularValues()(new_svd.singularValues().size() - 1);
  // If new_condition < ini_condition, the singular vector does point towards a
  // singularity. Otherwise, flip its direction.
  if (ini_condition >= new_condition)
  {
    vector_toward_singularity *= -1;
  }

  // If this dot product is positive, we're moving toward singularity ==> decelerate
  double dot = vector_toward_singularity.dot(commanded_velocity);
  if (dot > 0)
  {
    // Ramp velocity down linearly when the Jacobian condition is between lower_singularity_threshold and
    // hard_stop_singularity_threshold, and we're moving towards the singularity
    if ((ini_condition > parameters_.lower_singularity_threshold) &&
        (ini_condition < parameters_.hard_stop_singularity_threshold))
    {
      velocity_scale = 1. -
                       (ini_condition - parameters_.lower_singularity_threshold) /
                           (parameters_.hard_stop_singularity_threshold - parameters_.lower_singularity_threshold);
    }

    // Very close to singularity, so halt.
    else if (ini_condition > parameters_.hard_stop_singularity_threshold)
    {
      velocity_scale = 0;
      ROS_WARN_THROTTLE_NAMED(2, LOGNAME, "Close to a singularity. Halting.");
    }
  }

  return velocity_scale;
}

bool JogCalcs::enforceSRDFJointBounds(trajectory_msgs::JointTrajectory& new_joint_traj)
{
  bool halting = false;

  if (new_joint_traj.points.empty())
  {
    ROS_WARN_STREAM_THROTTLE_NAMED(2, LOGNAME, "Empty trajectory passed into checkIfJointsWithinURDFBounds().");
    return true;  // technically an empty trajectory is still within bounds
  }

  for (auto joint : joint_model_group_->getJointModels())
  {
    if (!kinematic_state_->satisfiesVelocityBounds(joint))
    {
      ROS_WARN_STREAM_THROTTLE_NAMED(2, LOGNAME, ros::this_node::getName() << " " << joint->getName() << " "
                                                                           << " close to a "
                                                                              " velocity limit. Enforcing limit.");
      kinematic_state_->enforceVelocityBounds(joint);
      for (std::size_t c = 0; c < new_joint_traj.joint_names.size(); ++c)
      {
        if (new_joint_traj.joint_names[c] == joint->getName())
        {
          // TODO(andyz): This is caused by publishing in position mode -- which does not initialize the velocity
          // members.
          // TODO(andyz): Also need to adjust the joint positions that would be published.
          if (new_joint_traj.points[0].velocities.size() > c + 1)
          {
            new_joint_traj.points[0].velocities[c] = *(kinematic_state_->getJointVelocities(joint));
            break;
          }
        }
      }
    }

    // Halt if we're past a joint margin and joint velocity is moving even farther past
    double joint_angle = 0;
    for (std::size_t c = 0; c < original_joint_state_.name.size(); ++c)
    {
      if (original_joint_state_.name[c] == joint->getName())
      {
        joint_angle = original_joint_state_.position.at(c);
        break;
      }
    }
    if (!kinematic_state_->satisfiesPositionBounds(joint, -parameters_.joint_limit_margin))
    {
      const std::vector<moveit_msgs::JointLimits> limits = joint->getVariableBoundsMsg();

      // Joint limits are not defined for some joints. Skip them.
      if (!limits.empty())
      {
        if ((kinematic_state_->getJointVelocities(joint)[0] < 0 &&
             (joint_angle < (limits[0].min_position + parameters_.joint_limit_margin))) ||
            (kinematic_state_->getJointVelocities(joint)[0] > 0 &&
             (joint_angle > (limits[0].max_position - parameters_.joint_limit_margin))))
        {
          ROS_WARN_STREAM_THROTTLE_NAMED(2, LOGNAME, ros::this_node::getName() << " " << joint->getName()
                                                                               << " close to a "
                                                                                  " position limit. Halting.");
          halting = true;
        }
      }
    }
  }
  return !halting;
}

void JogCalcs::publishWarning(bool active) const
{
  std_msgs::Bool status;
  status.data = static_cast<std_msgs::Bool::_data_type>(active);
  warning_pub_.publish(status);
}

// Suddenly halt for a joint limit or other critical issue.
// Is handled differently for position vs. velocity control.
void JogCalcs::suddenHalt(trajectory_msgs::JointTrajectory& joint_traj)
{
  for (std::size_t i = 0; i < num_joints_; ++i)
  {
    // For position-controlled robots, can reset the joints to a known, good state
    if (parameters_.publish_joint_positions)
      joint_traj.points[0].positions[i] = original_joint_state_.position[i];

    // For velocity-controlled robots, stop
    if (parameters_.publish_joint_velocities)
      joint_traj.points[0].velocities[i] = 0;
  }
}

// Parse the incoming joint msg for the joints of our MoveGroup
bool JogCalcs::updateJoints(std::mutex& mutex, const JogArmShared& shared_variables)
{
  mutex.lock();
  incoming_joints_ = shared_variables.joints;
  mutex.unlock();

  // Check that the msg contains enough joints
  if (incoming_joints_.name.size() < num_joints_)
    return false;

  // Store joints in a member variable
  for (std::size_t m = 0; m < incoming_joints_.name.size(); ++m)
  {
    std::size_t c;
    try
    {
      c = joint_state_name_map_.at(incoming_joints_.name[m]);
    }
    catch (const std::out_of_range& e)
    {
      ROS_WARN_STREAM_THROTTLE_NAMED(5, LOGNAME, "Ignoring joint " << incoming_joints_.name[m]);
      continue;
    }

    joint_state_.position[c] = incoming_joints_.position[m];
  }

  // Cache the original joints in case they need to be reset
  original_joint_state_ = joint_state_;

  return true;
}

// Scale the incoming jog command
Eigen::VectorXd JogCalcs::scaleCartesianCommand(const geometry_msgs::TwistStamped& command) const
{
  Eigen::VectorXd result(6);

  // Apply user-defined scaling if inputs are unitless [-1:1]
  if (parameters_.command_in_type == "unitless")
  {
    result[0] = parameters_.linear_scale * parameters_.publish_period * command.twist.linear.x;
    result[1] = parameters_.linear_scale * parameters_.publish_period * command.twist.linear.y;
    result[2] = parameters_.linear_scale * parameters_.publish_period * command.twist.linear.z;
    result[3] = parameters_.rotational_scale * parameters_.publish_period * command.twist.angular.x;
    result[4] = parameters_.rotational_scale * parameters_.publish_period * command.twist.angular.y;
    result[5] = parameters_.rotational_scale * parameters_.publish_period * command.twist.angular.z;
  }
  // Otherwise, commands are in m/s and rad/s
  else if (parameters_.command_in_type == "speed_units")
  {
    result[0] = command.twist.linear.x * parameters_.publish_period;
    result[1] = command.twist.linear.y * parameters_.publish_period;
    result[2] = command.twist.linear.z * parameters_.publish_period;
    result[3] = command.twist.angular.x * parameters_.publish_period;
    result[4] = command.twist.angular.y * parameters_.publish_period;
    result[5] = command.twist.angular.z * parameters_.publish_period;
  }
  else
    ROS_ERROR_STREAM_NAMED(LOGNAME, "Unexpected command_in_type");

  return result;
}

Eigen::VectorXd JogCalcs::scaleJointCommand(const control_msgs::JointJog& command) const
{
  Eigen::VectorXd result(num_joints_);

  for (std::size_t i = 0; i < num_joints_; ++i)
  {
    result[i] = 0.0;
  }

  std::size_t c;
  for (std::size_t m = 0; m < command.joint_names.size(); ++m)
  {
    try
    {
      c = joint_state_name_map_.at(command.joint_names[m]);
    }
    catch (const std::out_of_range& e)
    {
      ROS_WARN_STREAM_THROTTLE_NAMED(5, LOGNAME, "Ignoring joint " << incoming_joints_.name[m]);
      continue;
    }
    // Apply user-defined scaling if inputs are unitless [-1:1]
    if (parameters_.command_in_type == "unitless")
      result[c] = command.velocities[m] * parameters_.joint_scale * parameters_.publish_period;
    // Otherwise, commands are in m/s and rad/s
    else if (parameters_.command_in_type == "speed_units")
      result[c] = command.velocities[m] * parameters_.publish_period;
    else
      ROS_ERROR_STREAM_NAMED(LOGNAME, "Unexpected command_in_type, check yaml file.");
  }

  return result;
}

// Add the deltas to each joint
bool JogCalcs::addJointIncrements(sensor_msgs::JointState& output, const Eigen::VectorXd& increments) const
{
  for (std::size_t i = 0, size = static_cast<std::size_t>(increments.size()); i < size; ++i)
  {
    try
    {
      output.position[i] += increments[static_cast<long>(i)];
    }
    catch (const std::out_of_range& e)
    {
      ROS_ERROR_STREAM_NAMED(LOGNAME, ros::this_node::getName() << " Lengths of output and "
                                                                   "increments do not match.");
      return false;
    }
  }

  return true;
}

void JogCalcs::removeDimension(Eigen::MatrixXd& jacobian, Eigen::VectorXd& delta_x, unsigned int row_to_remove)
{
  unsigned int num_rows = jacobian.rows() - 1;
  unsigned int num_cols = jacobian.cols();

  if (row_to_remove < num_rows)
  {
    jacobian.block(row_to_remove, 0, num_rows - row_to_remove, num_cols) =
        jacobian.block(row_to_remove + 1, 0, num_rows - row_to_remove, num_cols);
    delta_x.segment(row_to_remove, num_rows - row_to_remove) =
        delta_x.segment(row_to_remove + 1, num_rows - row_to_remove);
  }
  jacobian.conservativeResize(num_rows, num_cols);
  delta_x.conservativeResize(num_rows);
}
}  // namespace moveit_jog_arm
