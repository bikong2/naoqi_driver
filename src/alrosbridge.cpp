/*
 * Copyright 2015 Aldebaran
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

#include <iostream>

/*
* NAOQI
*/
#include <qi/anyobject.hpp>
#include <alvision/alvisiondefinitions.h> // for kTop...
/*
* BOOST
*/
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#define for_each BOOST_FOREACH
/*
* ROS
*/
#include <std_msgs/Int32.h>
#include <std_msgs/String.h>

/*
* PUBLIC INTERFACE
*/
#include <alrosbridge/alrosbridge.hpp>
#include <alrosbridge/message_actions.h>
/*
 * CONVERTERS
 */
#include "converters/camera.hpp"
#include "converters/imu.hpp"
#include "converters/int.hpp"
#include "converters/joint_state.hpp"
#include "converters/laser.hpp"
#include "converters/sonar.hpp"
#include "converters/string.hpp"
/*
* PUBLISHERS
*/
#include "publishers/camera.hpp"
//#include "publishers/diagnostics.hpp"
#include "publishers/imu.hpp"
#include "publishers/int.hpp"
//#include "publishers/info.hpp"
#include "publishers/joint_state.hpp"
#include "publishers/laser.hpp"
//#include "publishers/log.hpp"
//#include "publishers/nao_joint_state.hpp"
//#include "publishers/odometry.hpp"
#include "publishers/sonar.hpp"
#include "publishers/string.hpp"

/*
 * SUBSCRIBERS
 */
#include "subscribers/teleop.hpp"
#include "subscribers/moveto.hpp"

/*
 * RECORDERS
 */
#include "recorder/camera.hpp"
#include "recorder/imu.hpp"
#include "recorder/int.hpp"
#include "recorder/joint_state.hpp"
#include "recorder/laser.hpp"
#include "recorder/sonar.hpp"
#include "recorder/string.hpp"

/*
* STATIC FUNCTIONS INCLUDE
*/
#include "ros_env.hpp"
#include "helpers.hpp"

/*
 * ROS
 */
#include <tf2_ros/buffer.h>

#define DEBUG 0

namespace alros
{

Bridge::Bridge( qi::SessionPtr& session )
  : sessionPtr_( session ),
  freq_(15),
  publish_enabled_(false),
  publish_cancelled_(false),
  record_enabled_(false),
  record_cancelled_(false),
  recorder_(boost::make_shared<recorder::GlobalRecorder>(::alros::ros_env::getPrefix()))
{
}

Bridge::~Bridge()
{
  std::cout << "ALRosBridge is shutting down.." << std::endl;
  // destroy nodehandle?
  if(nhPtr_)
  {
    nhPtr_->shutdown();
    ros::shutdown();
  }
}

void Bridge::stopService() {
  publish_cancelled_ = true;
  stopPublishing();
  if (publisherThread_.get_id() !=  boost::thread::id())
    publisherThread_.join();
  converters_.clear();
  subscribers_.clear();
}


void Bridge::rosLoop()
{
  static std::vector<message_actions::MessageAction> actions;

  while( !publish_cancelled_ && !record_cancelled_ )
  {
    // clear the callback triggers
    actions.clear();
    {
      boost::mutex::scoped_lock lock( mutex_reinit_ );
      if (!conv_queue_.empty())
      {
        // Wait for the next Publisher to be ready
        size_t conv_index = conv_queue_.top().conv_index_;
        converter::Converter& conv = converters_[conv_index];
        ros::Time schedule = conv_queue_.top().schedule_;

        ros::Duration d(schedule - ros::Time::now());
        if ( d > ros::Duration(0))
        {
          d.sleep();
        }
        // check the publishing condition
        // publishing enabled
        // has to be registered
        // has to be subscribed
#ifdef DEBUG
        ros::Time before = ros::Time::now();
#endif
        PubConstIter pub_it = pub_map_.find( conv.name() );
        if ( publish_enabled_ &&  pub_it != pub_map_.end() && pub_it->second.isSubscribed() )
        {
          actions.push_back(message_actions::PUBLISH);
        }

        // check the recording condition
        // recording enabled
        // has to be registered
        // has to be subscribed (configured to be recorded)
        RecConstIter rec_it = rec_map_.find( conv.name() );
        {
          boost::mutex::scoped_lock lock_record( mutex_record_ );
          if ( record_enabled_ && rec_it != rec_map_.end() && rec_it->second.isSubscribed() )
          {
            actions.push_back(message_actions::RECORD);
          }
        }
        if (actions.size() >0)
        {
          conv.callAll( actions );
        }
#if DEBUG
        ros::Time after = ros::Time::now();
        std::cerr << "round trip last " << after-before << std::endl;
#endif
        // Schedule for a future time or not
        conv_queue_.pop();
        if ( conv.frequency() != 0 )
          conv_queue_.push(ScheduledConverter(schedule + ros::Duration(1.0f / conv.frequency()), conv_index));
      } else
        // sleep one second
        ros::Duration(1).sleep();
    }
    ros::spinOnce();
  }
}

void Bridge::registerConverter( const converter::Converter& conv )
{
  converters_.push_back( conv );
}

void Bridge::registerConverter( const converter::Converter& conv, const publisher::Publisher& pub, const recorder::Recorder& rec )
{
  registerConverter( conv );
  // Concept classes don't have any default constructors needed by operator[]
  // Cannot use this operator here. So we use insert
  pub_map_.insert( std::map<std::string, publisher::Publisher>::value_type(conv.name(), pub) );
  rec_map_.insert( std::map<std::string, recorder::Recorder>::value_type(conv.name(), rec) );
}

void Bridge::registerPublisher( const converter::Converter& conv, const publisher::Publisher& pub )
{
  registerConverter( conv );
  // Concept classes don't have any default constructors needed by operator[]
  // Cannot use this operator here. So we use insert
  pub_map_.insert( std::map<std::string, publisher::Publisher>::value_type(conv.name(), pub) );
}

void Bridge::registerRecorder( const converter::Converter& conv, const recorder::Recorder& rec )
{
  registerConverter( conv );
  // Concept classes don't have any default constructors needed by operator[]
  // Cannot use this operator here. So we use insert
  rec_map_.insert( std::map<std::string, recorder::Recorder>::value_type(conv.name(), rec) );
}

void Bridge::registerDefaultConverter()
{
  // init global tf2 buffer
  tf2_buffer_.reset<tf2_ros::Buffer>( new tf2_ros::Buffer() );
  tf2_buffer_->setUsingDedicatedThread(true);

//  // Info should be at 0 (latched) but somehow that does not work ...
//  publisher::Publisher info = alros::publisher::InfoPublisher("info", "info", 0.1, sessionPtr_);
//  registerPublisher( info );
//
//  registerPublisher( alros::publisher::DiagnosticsPublisher("diagnostics", 1, sessionPtr_) );
//  registerPublisher( alros::publisher::LogPublisher("logger", "", 5, sessionPtr_) );

  alros::Robot robot_type;

  /** String Publisher */
  boost::shared_ptr<publisher::StringPublisher> sp = boost::make_shared<publisher::StringPublisher>( "string" );
  sp->reset( *nhPtr_ );
  boost::shared_ptr<recorder::StringRecorder> sr = boost::make_shared<recorder::StringRecorder>( "string" );
  sr->reset(recorder_);
  boost::shared_ptr<converter::StringConverter> sc = boost::make_shared<converter::StringConverter>( "string", 10, sessionPtr_ );
  sc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::StringPublisher::publish, sp, _1) );
  sc->registerCallback( message_actions::RECORD, boost::bind(&recorder::StringRecorder::write, sr, _1) );
  registerConverter( sc, sp, sr );

  robot_type = sc->robot();

  /** IMU TORSO **/
  boost::shared_ptr<publisher::ImuPublisher> imutp = boost::make_shared<publisher::ImuPublisher>( "imu_torso" );
  imutp->reset( *nhPtr_ );
  boost::shared_ptr<recorder::ImuRecorder> imutr = boost::make_shared<recorder::ImuRecorder>( "imu_torso" );
  imutr->reset(recorder_);

  boost::shared_ptr<converter::ImuConverter> imutc = boost::make_shared<converter::ImuConverter>( "imu_torso", converter::IMU::TORSO, 15, sessionPtr_);
  imutc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::ImuPublisher::publish, imutp, _1) );
  imutc->registerCallback( message_actions::RECORD, boost::bind(&recorder::ImuRecorder::write, imutr, _1) );
  registerConverter( imutc, imutp, imutr );

  if(robot_type == alros::PEPPER){
    /** IMU BASE **/
    boost::shared_ptr<publisher::ImuPublisher> imubp = boost::make_shared<publisher::ImuPublisher>( "imu_base" );
    imubp->reset( *nhPtr_ );
    boost::shared_ptr<recorder::ImuRecorder> imubr = boost::make_shared<recorder::ImuRecorder>( "imu_base" );
    imubr->reset(recorder_);

    boost::shared_ptr<converter::ImuConverter> imubc = boost::make_shared<converter::ImuConverter>( "imu_base", converter::IMU::BASE, 15, sessionPtr_);
    imubc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::ImuPublisher::publish, imubp, _1) );
    imubc->registerCallback( message_actions::RECORD, boost::bind(&recorder::ImuRecorder::write, imubr, _1) );
    registerConverter( imubc, imubp, imubr );

  }

  /** Int Publisher */
  boost::shared_ptr<publisher::IntPublisher> ip = boost::make_shared<publisher::IntPublisher>( "int" );
  ip->reset( *nhPtr_ );
  boost::shared_ptr<recorder::IntRecorder> ir = boost::make_shared<recorder::IntRecorder>( "int" );
  ir->reset(recorder_);
  boost::shared_ptr<converter::IntConverter> ic = boost::make_shared<converter::IntConverter>( "int", 15, sessionPtr_);
  ic->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::IntPublisher::publish, ip, _1) );
  ic->registerCallback( message_actions::RECORD, boost::bind(&recorder::IntRecorder::write, ir, _1) );
  registerConverter( ic, ip, ir  );

  /** Front Camera */
  boost::shared_ptr<publisher::CameraPublisher> fcp = boost::make_shared<publisher::CameraPublisher>( "camera/front/image_raw", AL::kTopCamera );
  fcp->reset( *nhPtr_ );
  boost::shared_ptr<recorder::CameraRecorder> fcr = boost::make_shared<recorder::CameraRecorder>( "camera/front/image_raw" );
  fcr->reset(recorder_);
  boost::shared_ptr<converter::CameraConverter> fcc = boost::make_shared<converter::CameraConverter>( "front_camera", 10, sessionPtr_, AL::kTopCamera, AL::kQVGA );
  fcc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, fcp, _1, _2) );
  fcc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, fcr, _1, _2) );
  registerConverter( fcc, fcp, fcr );
  //registerPublisher( fcc, *fcp );

  if(robot_type == alros::PEPPER){
    /** Depth Camera */
    boost::shared_ptr<publisher::CameraPublisher> dcp = boost::make_shared<publisher::CameraPublisher>( "camera/depth/image_raw", AL::kDepthCamera );
    dcp->reset( *nhPtr_ );
    boost::shared_ptr<recorder::CameraRecorder> dcr = boost::make_shared<recorder::CameraRecorder>( "camera/depth/image_raw" );
    dcr->reset(recorder_);
    boost::shared_ptr<converter::CameraConverter> dcc = boost::make_shared<converter::CameraConverter>( "depth_camera", 10, sessionPtr_, AL::kDepthCamera, AL::kQVGA );
    dcc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::CameraPublisher::publish, dcp, _1, _2) );
    dcc->registerCallback( message_actions::RECORD, boost::bind(&recorder::CameraRecorder::write, dcr, _1, _2) );
    registerConverter( dcc, dcp, dcr );
  }

  /** Joint States */
  boost::shared_ptr<publisher::JointStatePublisher> jsp = boost::make_shared<publisher::JointStatePublisher>( "/joint_states" );
  jsp->reset( *nhPtr_ );
  boost::shared_ptr<recorder::JointStateRecorder> jsr = boost::make_shared<recorder::JointStateRecorder>( "/joint_states" );
  jsr->reset(recorder_);
  boost::shared_ptr<converter::JointStateConverter> jsc = boost::make_shared<converter::JointStateConverter>( "joint_states", 15, tf2_buffer_, sessionPtr_, *nhPtr_ );
  jsc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::JointStatePublisher::publish, jsp, _1, _2) );
  jsc->registerCallback( message_actions::RECORD, boost::bind(&recorder::JointStateRecorder::write, jsr, _1, _2) );
  registerConverter( jsc, jsp, jsr );

  if(robot_type == alros::PEPPER){
    /** Laser */
    boost::shared_ptr<publisher::LaserPublisher> lp = boost::make_shared<publisher::LaserPublisher>( "laser" );
    lp->reset( *nhPtr_ );
    boost::shared_ptr<recorder::LaserRecorder> lr = boost::make_shared<recorder::LaserRecorder>( "laser" );
    lr->reset(recorder_);
    boost::shared_ptr<converter::LaserConverter> lc = boost::make_shared<converter::LaserConverter>( "laser", 10, sessionPtr_ );
    lc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::LaserPublisher::publish, lp, _1) );
    lc->registerCallback( message_actions::RECORD, boost::bind(&recorder::LaserRecorder::write, lr, _1) );
    registerConverter( lc, lp, lr );
  }

  /** Sonar */
  std::vector<std::string> sonar_topics;
  if (robot_type == alros::PEPPER)
  {
    sonar_topics.push_back("sonar/front");
    sonar_topics.push_back("sonar/back");
  }
  else
  {
    sonar_topics.push_back("sonar/left");
    sonar_topics.push_back("sonar/right");
  }
  boost::shared_ptr<publisher::SonarPublisher> usp = boost::make_shared<publisher::SonarPublisher>( sonar_topics );
  usp->reset( *nhPtr_ );
  boost::shared_ptr<recorder::SonarRecorder> usr = boost::make_shared<recorder::SonarRecorder>( sonar_topics );
  usr->reset(recorder_);
  boost::shared_ptr<converter::SonarConverter> usc = boost::make_shared<converter::SonarConverter>( "sonar", 10, sessionPtr_ );
  usc->registerCallback( message_actions::PUBLISH, boost::bind(&publisher::SonarPublisher::publish, usp, _1) );
  usc->registerCallback( message_actions::RECORD, boost::bind(&recorder::SonarRecorder::write, usr, _1) );
  registerConverter( usc, usp, usr );

}

// public interface here
void Bridge::registerSubscriber( subscriber::Subscriber sub )
{
  std::vector<subscriber::Subscriber>::iterator it;
  it = std::find( subscribers_.begin(), subscribers_.end(), sub );
  size_t sub_index = 0;

  // if subscriber is not found, register it!
  if (it == subscribers_.end() )
  {
    sub_index = subscribers_.size();
    subscribers_.push_back( sub );
    std::cout << "registered subscriber:\t" << sub.name() << std::endl;
  }
  // if found, re-init them
  else
  {
    std::cout << "re-initialized existing subscriber:\t" << it->name() << std::endl;
  }
}

void Bridge::registerDefaultSubscriber()
{
  if (!subscribers_.empty())
    return;
  registerSubscriber( boost::make_shared<alros::subscriber::TeleopSubscriber>("teleop", "/cmd_vel", sessionPtr_) );
  registerSubscriber( boost::make_shared<alros::subscriber::MovetoSubscriber>("moveto", "/move_base_simple/goal", sessionPtr_, tf2_buffer_) );
}

std::vector<std::string> Bridge::getAvailableConverters()
{
  std::vector<std::string> conv_list;
  for_each( const converter::Converter& conv, converters_ )
  {
    conv_list.push_back(conv.name());
  }

  return conv_list;
}


void Bridge::init()
{
  // init converters
  conv_queue_ =  std::priority_queue<ScheduledConverter>();
  size_t conv_index = 0;
  for_each( converter::Converter& conv, converters_ )
  {
    conv.reset();
    // Schedule it for the next publish
    conv_queue_.push(ScheduledConverter(ros::Time::now(), conv_index));
    ++conv_index;
  }

  // init subscribers
  for_each( subscriber::Subscriber& sub, subscribers_ )
  {
    sub.reset( *nhPtr_ );
  }
}


/*
* EXPOSED FUNCTIONS
*/

std::string Bridge::getMasterURI() const
{
  return ros_env::getMasterURI();
}

void Bridge::setMasterURI( const std::string& uri)
{
  setMasterURINet(uri, "eth0");
}
void Bridge::setMasterURINet( const std::string& uri, const std::string& network_interface)
{
  // Stopping publishing
  stopPublishing();

  // Reinitializing ROS
  {
    boost::mutex::scoped_lock lock( mutex_reinit_ );
    nhPtr_.reset();
    std::cout << "nodehandle reset " << std::endl;
    ros_env::setMasterURI( uri, network_interface );
    nhPtr_.reset( new ros::NodeHandle("~") );
  }
  // Create the publishing thread if needed
  if (publisherThread_.get_id() ==  boost::thread::id())
    publisherThread_ = boost::thread( &Bridge::rosLoop, this );

  // register publishers, that will not start them
  registerDefaultConverter();
  registerDefaultSubscriber();
  // initialize the publishers and subscribers with nodehandle
  init();
  // Start publishing again
  startPublishing();
}

void Bridge::startPublishing()
{
  boost::mutex::scoped_lock lock( mutex_reinit_ );
  publish_enabled_ = true;
}

void Bridge::stopPublishing()
{
  boost::mutex::scoped_lock lock( mutex_reinit_ );
  publish_enabled_ = false;
}

std::vector<std::string> Bridge::getSubscribedPublishers() const
{
  std::vector<std::string> publisher;
  for(PubConstIter iterator = pub_map_.begin(); iterator != pub_map_.end(); iterator++)
  {
    // iterator->first = key
    // iterator->second = value
    // Repeat if you also want to iterate through the second map.
    if ( iterator->second.isSubscribed() )
    {
      publisher.push_back( iterator->second.topic() );
    }
  }
  return publisher;
}

void Bridge::startRecord()
{
  boost::mutex::scoped_lock lock_record( mutex_record_ );
  recorder_->startRecord();
  for_each( converter::Converter& conv, converters_ )
  {
    RecIter it = rec_map_.find(conv.name());
    if ( it != rec_map_.end() )
    {
      it->second.subscribe(true);
      std::cout << HIGHGREEN << "Topic "
                << BOLDCYAN << conv.name() << RESETCOLOR
                << HIGHGREEN << " is subscribed for recording" << RESETCOLOR << std::endl;
    }
  }
  record_enabled_ = true;
}

void Bridge::startRecordTopics(const std::vector<std::string>& names)
{
  boost::mutex::scoped_lock lock_record( mutex_record_ );
  recorder_->startRecord();
  for_each( const std::string& name, names)
  {
    RecIter it = rec_map_.find(name);
    if ( it != rec_map_.end() )
    {
      it->second.subscribe(true);
      std::cout << HIGHGREEN << "Topic "
                << BOLDCYAN << name << RESETCOLOR
                << HIGHGREEN << " is subscribed for recording" << RESETCOLOR << std::endl;
    }
    else
    {
      std::cout << BOLDRED << "Could not find topic "
                << BOLDCYAN << name
                << BOLDRED << " in recorders" << RESETCOLOR << std::endl
                << BOLDYELLOW << "To get the list of all available converter's name, please run:" << RESETCOLOR << std::endl
                << GREEN << "\t$ qicli call BridgeService.getAvailableConverters" << RESETCOLOR << std::endl;
    }
  }
  record_enabled_ = true;
}

std::string Bridge::stopRecord()
{
  boost::mutex::scoped_lock lock_record( mutex_record_ );
  record_enabled_ = false;
  for_each( converter::Converter& conv, converters_ )
  {
    RecIter it = rec_map_.find(conv.name());
    if ( it != rec_map_.end() )
    {
      it->second.subscribe(false);
    }
  }
  return recorder_->stopRecord(::alros::ros_env::getROSIP("eth0"));
}

QI_REGISTER_OBJECT( Bridge,
                    _whoIsYourDaddy,
                    startPublishing,
                    stopPublishing,
                    getMasterURI,
                    setMasterURI,
                    setMasterURINet,
                    getAvailableConverters,
                    getSubscribedPublishers,
                    startRecord,
                    startRecordTopics,
                    stopRecord );
} //alros
