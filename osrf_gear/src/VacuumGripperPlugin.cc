/*
 * Copyright (C) 2016 Open Source Robotics Foundation
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
#ifdef _WIN32
  // Ensure that Winsock2.h is included before Windows.h, which can get
  // pulled in by anybody (e.g., Boost).
  #include <Winsock2.h>
#endif

#include <memory>
#include <mutex>
#include <string>
#include <gazebo/common/Events.hh>
#include <gazebo/physics/Collision.hh>
#include <gazebo/physics/Contact.hh>
#include <gazebo/physics/ContactManager.hh>
#include <gazebo/physics/Joint.hh>
#include <gazebo/physics/Link.hh>
#include <gazebo/physics/Model.hh>
#include <gazebo/physics/PhysicsEngine.hh>
#include <gazebo/physics/World.hh>
#include <gazebo/transport/Node.hh>
#include <gazebo/transport/Subscriber.hh>
#include "osrf_gear/VacuumGripperPlugin.hh"

namespace gazebo
{
  /// \internal
  /// \brief Private data for the VacuumGripperPlugin class
  struct VacuumGripperPluginPrivate
  {
    /// \brief Model that contains this gripper.
    public: physics::ModelPtr model;

    /// \brief Pointer to the world.
    public: physics::WorldPtr world;

    /// \brief A fixed joint to connect the gripper to an object.
    public: physics::JointPtr fixedJoint;

    /// \brief The suction cup link.
    public: physics::LinkPtr suctionCupLink;

    /// \brief Connection event.
    public: event::ConnectionPtr connection;

    /// \brief The collisions for the links in the gripper.
    public: std::map<std::string, physics::CollisionPtr> collisions;

    /// \brief The current contacts.
    public: std::vector<msgs::Contact> contacts;

    /// \brief Mutex used to protect reading/writing the sonar message.
    public: std::mutex mutex;

    /// \brief True if the gripper has an object.
    public: bool attached;

    /// \brief Rate at which to update the gripper.
    public: common::Time updateRate;

    /// \brief Previous time when the gripper was updated.
    public: common::Time prevUpdateTime;

    /// \brief Number of iterations the gripper was contacting the same
    /// object.
    public: int posCount;

    /// \brief Number of iterations the gripper was not contacting the same
    /// object.
    public: int zeroCount;

    /// \brief Minimum number of links touching.
    public: unsigned int minContactCount;

    /// \brief Steps touching before engaging fixed joint
    public: int attachSteps;

    /// \brief Steps not touching before disengaging fixed joint
    public: int detachSteps;

    /// \brief Name of the gripper.
    public: std::string name;

    /// \brief Node for communication.
    public: transport::NodePtr node;

    /// \brief Subscription to contact messages from the physics engine.
    public: transport::SubscriberPtr contactSub;

    /// \brief Whether the suction is enabled or not.
    public: bool enabled;
  };
}

using namespace gazebo;
using namespace physics;

GZ_REGISTER_MODEL_PLUGIN(VacuumGripperPlugin)

/////////////////////////////////////////////////
VacuumGripperPlugin::VacuumGripperPlugin()
  : dataPtr(new VacuumGripperPluginPrivate)
{
  gzmsg << "VacuumGripper plugin loaded" << std::endl;

  this->dataPtr->attached = false;
  this->dataPtr->updateRate = common::Time(0, common::Time::SecToNano(0.75));
}

/////////////////////////////////////////////////
VacuumGripperPlugin::~VacuumGripperPlugin()
{
  if (this->dataPtr->world && this->dataPtr->world->GetRunning())
  {
    auto mgr = this->dataPtr->world->GetPhysicsEngine()->GetContactManager();
    mgr->RemoveFilter(this->Name());
  }
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  this->dataPtr->model = _model;
  this->dataPtr->world = this->dataPtr->model->GetWorld();

  this->dataPtr->node = transport::NodePtr(new transport::Node());
  this->dataPtr->node->Init(this->dataPtr->world->GetName());
  this->dataPtr->name = _sdf->Get<std::string>("name");

  // Create the joint that will attach the objects to the suction cup
  this->dataPtr->fixedJoint =
      this->dataPtr->world->GetPhysicsEngine()->CreateJoint(
        "fixed", this->dataPtr->model);
  this->dataPtr->fixedJoint->SetName(this->dataPtr->model->GetName() +
      "__vacuum_gripper_fixed_joint__");

  // Read the SDF parameters
  sdf::ElementPtr graspCheck = _sdf->GetElement("grasp_check");
  this->dataPtr->minContactCount =
      graspCheck->Get<unsigned int>("min_contact_count");
  this->dataPtr->attachSteps = graspCheck->Get<int>("attach_steps");
  this->dataPtr->detachSteps = graspCheck->Get<int>("detach_steps");
  sdf::ElementPtr suctionCupLinkElem = _sdf->GetElement("suction_cup_link");
  this->dataPtr->suctionCupLink =
    this->dataPtr->model->GetLink(suctionCupLinkElem->Get<std::string>());
  if (!this->dataPtr->suctionCupLink)
  {
    gzerr << "Suction cup link [" << suctionCupLinkElem->Get<std::string>()
          << "] not found!\n";
    return;
  }

  // Find out the collision elements of the suction cup
  for (auto j = 0u; j < this->dataPtr->suctionCupLink->GetChildCount(); ++j)
  {
    physics::CollisionPtr collision =
       this->dataPtr->suctionCupLink->GetCollision(j);
    std::map<std::string, physics::CollisionPtr>::iterator collIter
      = this->dataPtr->collisions.find(collision->GetScopedName());
    if (collIter != this->dataPtr->collisions.end())
      continue;

    this->dataPtr->collisions[collision->GetScopedName()] = collision;
  }

  if (!this->dataPtr->collisions.empty())
  {
    // Create a filter to receive collision information
    auto mgr = this->dataPtr->world->GetPhysicsEngine()->GetContactManager();
    auto topic = mgr->CreateFilter(this->Name(), this->dataPtr->collisions);
    if (!this->dataPtr->contactSub)
    {
      this->dataPtr->contactSub = this->dataPtr->node->Subscribe(topic,
          &VacuumGripperPlugin::OnContacts, this);
    }
  }

  this->Reset();

  this->dataPtr->connection = event::Events::ConnectWorldUpdateEnd(
      boost::bind(&VacuumGripperPlugin::OnUpdate, this));
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::Reset()
{
  this->dataPtr->prevUpdateTime = common::Time::GetWallTime();
  this->dataPtr->zeroCount = 0;
  this->dataPtr->posCount = 0;
  this->dataPtr->attached = false;
  this->dataPtr->enabled = false;
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::OnUpdate()
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  if (common::Time::GetWallTime() -
      this->dataPtr->prevUpdateTime < this->dataPtr->updateRate ||
      !this->dataPtr->enabled)
  {
    return;
  }

  if (this->dataPtr->enabled)
    std::cout << "gripper enabled" << std::endl;

  if (this->dataPtr->attached)
    std::cout << "Attached" << std::endl;
  else
    std::cout << "Not attached" << std::endl;

  std::cout << "Contacts: " << this->dataPtr->contacts.size() << std::endl;

  // @todo: should package the decision into a function
  if (this->dataPtr->contacts.size() >= this->dataPtr->minContactCount)
  {
    this->dataPtr->posCount++;
    this->dataPtr->zeroCount = 0;
  }
  else
  {
    this->dataPtr->zeroCount++;
    this->dataPtr->posCount = std::max(0, this->dataPtr->posCount-1);
  }

  std::cout << "Pos count: " << this->dataPtr->posCount << std::endl;
  std::cout << "AttachSteps: " << this->dataPtr->attachSteps << std::endl;

  if (this->dataPtr->posCount > this->dataPtr->attachSteps &&
      !this->dataPtr->attached)
  {
    this->HandleAttach();
  }
  else if (this->dataPtr->zeroCount > this->dataPtr->detachSteps &&
           this->dataPtr->attached)
  {
    this->HandleDetach();
  }

  this->dataPtr->contacts.clear();
  this->dataPtr->prevUpdateTime = common::Time::GetWallTime();
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::HandleAttach()
{
  std::map<std::string, physics::CollisionPtr> cc;
  std::map<std::string, int> contactCounts;
  std::map<std::string, int>::iterator iter;

  // This function is only called from the OnUpdate function so
  // the call to contacts.clear() is not going to happen in
  // parallel with the reads in the following code, no mutex needed.
  for (unsigned int i = 0; i < this->dataPtr->contacts.size(); ++i)
  {
    std::string name1 = this->dataPtr->contacts[i].collision1();
    std::string name2 = this->dataPtr->contacts[i].collision2();

    if (this->dataPtr->collisions.find(name1) ==
        this->dataPtr->collisions.end())
    {
      cc[name1] = boost::dynamic_pointer_cast<Collision>(
          this->dataPtr->world->GetEntity(name1));
      contactCounts[name1] += 1;
    }

    if (this->dataPtr->collisions.find(name2) ==
        this->dataPtr->collisions.end())
    {
      cc[name2] = boost::dynamic_pointer_cast<Collision>(
          this->dataPtr->world->GetEntity(name2));
      contactCounts[name2] += 1;
    }
  }

  iter = contactCounts.begin();
  while (iter != contactCounts.end())
  {
    if (iter->second < 2)
      contactCounts.erase(iter++);
    else
    {
      if (!this->dataPtr->attached && cc[iter->first])
      {
        this->dataPtr->attached = true;

        this->dataPtr->fixedJoint->Load(this->dataPtr->suctionCupLink,
            cc[iter->first]->GetLink(), math::Pose());
        this->dataPtr->fixedJoint->Init();
      }
      ++iter;
    }
  }
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::HandleDetach()
{
  this->dataPtr->attached = false;
  this->dataPtr->fixedJoint->Detach();
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::OnContacts(ConstContactsPtr &_msg)
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  for (int i = 0; i < _msg->contact_size(); ++i)
  {
    CollisionPtr collision1 = boost::dynamic_pointer_cast<Collision>(
        this->dataPtr->world->GetEntity(_msg->contact(i).collision1()));
    CollisionPtr collision2 = boost::dynamic_pointer_cast<Collision>(
        this->dataPtr->world->GetEntity(_msg->contact(i).collision2()));

    if ((collision1 && !collision1->IsStatic()) &&
        (collision2 && !collision2->IsStatic()))
    {
      this->dataPtr->contacts.push_back(_msg->contact(i));
    }
  }
}

/////////////////////////////////////////////////
std::string VacuumGripperPlugin::Name() const
{
  return this->dataPtr->name;
}

/////////////////////////////////////////////////
bool VacuumGripperPlugin::IsAttached() const
{
  return this->dataPtr->attached;
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::Enable()
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
  this->dataPtr->enabled = true;
}

/////////////////////////////////////////////////
void VacuumGripperPlugin::Disable()
{
  {
    std::lock_guard<std::mutex> lock(this->dataPtr->mutex);
    this->dataPtr->enabled = false;
  }
  this->HandleDetach();
}