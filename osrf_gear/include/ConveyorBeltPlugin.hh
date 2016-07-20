/*
 * Copyright (C) 2012-2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
/*
 * Desc: Conveyor Belt Plugin
 * Author: Nate Koenig mod by John Hsu and Deanna Hood
 */
#ifndef _GAZEBO_CONTACT_PLUGIN_HH_
#define _GAZEBO_CONTACT_PLUGIN_HH_

#include <string>

#include <gazebo/common/Plugin.hh>
#include <gazebo/msgs/msgs.hh>
#include <gazebo/sensors/sensors.hh>
#include <gazebo/util/system.hh>

namespace gazebo
{
  /// \brief A plugin for a contact sensor. Inherit from this class to make
  /// your own contact plugin.
  class GAZEBO_VISIBLE ConveyorBeltPlugin : public SensorPlugin
  {
    /// \brief Constructor.
    public: ConveyorBeltPlugin();

    /// \brief Destructor.
    public: virtual ~ConveyorBeltPlugin();

    /// \brief Load the sensor plugin.
    /// \param[in] _sensor Pointer to the sensor that loaded this plugin.
    /// \param[in] _sdf SDF element that describes the plugin.
    public: virtual void Load(sensors::SensorPtr _sensor, sdf::ElementPtr _sdf);

    /// \brief Callback that recieves the contact sensor's update signal.
    /// Override this this function to get callbacks when the contact sensor
    /// is updated with new data.
    protected: void OnUpdate();

    /// \brief Pointer to the contact sensor
    protected: sensors::ContactSensorPtr parentSensor;

    /// \brief Connection that maintains a link between the contact sensor's
    /// updated signal and the OnUpdate callback.
    protected: event::ConnectionPtr updateConnection;

    /// \brief Pointer to the world
    protected: physics::WorldPtr world;

    /// \brief Pointer to this node for publishing/subscribing
    protected: transport::NodePtr node;

    /// \brief Subscriber for the control commands
    protected: transport::SubscriberPtr controlCommandSub;

    /// \brief Callback for responding to control commands
    protected: void OnControlCommand(ConstHeaderPtr& _msg);

    /// \brief Belt velocity (m/s, in the +Y direction of the belt frame)
    protected: double beltVelocity;

    /// \brief Mutex to protect the belt velocity
    protected: std::mutex mutex;

    /// \brief Set the state of the conveyor belt
    public: void SetVelocity(double velocity);

    /// \brief Generate a scoped topic name from a local one
    /// \param local local topic name
    protected: std::string Topic(std::string topicName) const;

    /// \brief Name of the collision of the belt
    protected: std::string beltCollisionName;

    /// \brief Pointer to the belt link
    protected: physics::LinkPtr beltLink;

    /// \brief Set of pointers to links which have collisions with the belt
    protected: std::set<physics::LinkPtr> contactingLinks;

    /// \brief Determine which links are ontop of the belt
    protected: void CalculateContactingLinks();

    /// \brief Act on links that are ontop of the belt
    protected: void ActOnContactingLinks(double velocity);
  };
}
#endif

