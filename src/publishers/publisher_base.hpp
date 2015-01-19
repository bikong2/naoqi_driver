#ifndef BASE_PUBLISHER_HPP
#define BASE_PUBLISHER_HPP

#include <iostream>

namespace alros
{
namespace publisher
{

class BasePublisher
{

public:
  BasePublisher( const std::string& name, const std::string& topic ):
    name_( name ),
    topic_( topic ),
    is_initialized_( false )
  {}

  inline std::string name() const
  {
    return name_;
  }

  inline std::string topic() const
  {
    return topic_;
  }

  inline bool isInitialized() const
  {
    return is_initialized_;
  }

  inline bool isSubscribed() const
  {
    if (is_initialized_ == false) return false;
    return pub_.getNumSubscribers() > 0;
  }

protected:
  std::string name_, topic_;

  ros::Publisher pub_;
  bool is_initialized_;

}; // class

} //publisher
} // alros

#endif
