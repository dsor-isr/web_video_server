#include "web_video_server/image_streamer.h"
#include <cv_bridge/cv_bridge.h>
#include <iostream>
#include <ctime>

namespace web_video_server
{

ImageStreamer::ImageStreamer(const async_web_server_cpp::HttpRequest &request,
                             async_web_server_cpp::HttpConnectionPtr connection, ros::NodeHandle& nh) :
    request_(request), connection_(connection), nh_(nh), inactive_(false)
{
  topic_ = request.get_query_param_value_or_default("topic", "");
}

ImageStreamer::~ImageStreamer()
{
}

ImageTransportImageStreamer::ImageTransportImageStreamer(const async_web_server_cpp::HttpRequest &request,
                             async_web_server_cpp::HttpConnectionPtr connection, ros::NodeHandle& nh) :
  ImageStreamer(request, connection, nh), it_(nh), initialized_(false)
{
  output_width_ = request.get_query_param_value_or_default<int>("width", -1);
  output_height_ = request.get_query_param_value_or_default<int>("height", -1);
  invert_ = request.has_query_param("invert");
  default_transport_ = request.get_query_param_value_or_default("default_transport", "raw");
  timestamp_ = request.has_query_param("timestamp");
  skip_n_ = request.get_query_param_value_or_default<int>("skip", 0);
  n_frame_ = 0;
}

ImageTransportImageStreamer::~ImageTransportImageStreamer()
{
}

void ImageTransportImageStreamer::start()
{
  image_transport::TransportHints hints(default_transport_);
  ros::master::V_TopicInfo available_topics;
  ros::master::getTopics(available_topics);
  inactive_ = true;
  for (size_t it = 0; it<available_topics.size(); it++){
    std::string available_topic_name = available_topics[it].name;
    if(available_topic_name == topic_ || (available_topic_name.find("/") == 0 &&
                                          available_topic_name.substr(1) == topic_)) {
      inactive_ = false;
    }
  }
  image_sub_ = it_.subscribe(topic_, 1, &ImageTransportImageStreamer::imageCallback, this, hints);
}

void ImageTransportImageStreamer::initialize(const cv::Mat &)
{
}

void ImageTransportImageStreamer::restreamFrame(double max_age)
{
  if (inactive_ || !initialized_ )
    return;
  try {
    if ( last_frame + ros::Duration(max_age) < ros::Time::now() ) {
      boost::mutex::scoped_lock lock(send_mutex_);
      sendImage(output_size_image, ros::Time::now() ); // don't update last_frame, it may remain an old value.
    }
  }
  catch (boost::system::system_error &e)
  {
    // happens when client disconnects
    ROS_DEBUG("system_error exception: %s", e.what());
    inactive_ = true;
    return;
  }
  catch (std::exception &e)
  {
    ROS_ERROR_THROTTLE(30, "exception: %s", e.what());
    inactive_ = true;
    return;
  }
  catch (...)
  {
    ROS_ERROR_THROTTLE(30, "exception");
    inactive_ = true;
    return;
  }
}

std::string stampToString(const ros::Time& stamp, const std::string format="%H:%M:%S")
{
  const int output_size = 100;
  char output[output_size];
  std::time_t raw_time = static_cast<time_t>(stamp.sec);
  struct tm* timeinfo = localtime(&raw_time);
  std::strftime(output, output_size, format.c_str(), timeinfo);
  std::stringstream ss;
  ss << std::setw(9) << std::setfill('0') << stamp.nsec;
  const size_t fractional_second_digits = 1;
  return std::string(output) + "." + ss.str().substr(0, fractional_second_digits);
}

void ImageTransportImageStreamer::imageCallback(const sensor_msgs::ImageConstPtr &msg)
{
  if (inactive_)
    return;

  n_frame_++;

  // Skip every n frames (for bandwidth control)
  if (n_frame_ % (skip_n_+1))
  {
    return;
  }

  cv::Mat img;
  try
  {
    if (msg->encoding.find("F") != std::string::npos)
    {
      // scale floating point images
      cv::Mat float_image_bridge = cv_bridge::toCvCopy(msg, msg->encoding)->image;
      cv::Mat_<float> float_image = float_image_bridge;
      double max_val;
      cv::minMaxIdx(float_image, 0, &max_val);

      if (max_val > 0)
      {
        float_image *= (255 / max_val);
      }
      img = float_image;
    }
    else
    {
      // Convert to OpenCV native BGR color
      img = cv_bridge::toCvCopy(msg, "bgr8")->image;
    }

    int input_width = img.cols;
    int input_height = img.rows;

    
    output_width_ = input_width;
    output_height_ = input_height;

    if (invert_)
    {
      // Rotate 180 degrees
      cv::flip(img, img, false);
      cv::flip(img, img, true);
    }

    boost::mutex::scoped_lock lock(send_mutex_); // protects output_size_image
    if (output_width_ != input_width || output_height_ != input_height)
    {
      cv::Mat img_resized;
      cv::Size new_size(output_width_, output_height_);
      cv::resize(img, img_resized, new_size);
      output_size_image = img_resized;
    }
    else
    {
      output_size_image = img;
    }

    if (!initialized_)
    {
      initialize(output_size_image);
      initialized_ = true;
    }

    last_frame = ros::Time::now();

    if (timestamp_)
    {
      cv::putText(output_size_image, //target image
              stampToString(last_frame).c_str(), //text
              cv::Point(10, 40), //top-left position
              cv::FONT_HERSHEY_DUPLEX,
              1.0,  // font scale
              CV_RGB(0, 255, 0), //font color
              1,  // thickness
              cv::LINE_AA);
    }

    sendImage(output_size_image, msg->header.stamp);

  }
  catch (cv_bridge::Exception &e)
  {
    ROS_ERROR_THROTTLE(30, "cv_bridge exception: %s", e.what());
    inactive_ = true;
    return;
  }
  catch (cv::Exception &e)
  {
    ROS_ERROR_THROTTLE(30, "cv_bridge exception: %s", e.what());
    inactive_ = true;
    return;
  }
  catch (boost::system::system_error &e)
  {
    // happens when client disconnects
    ROS_DEBUG("system_error exception: %s", e.what());
    inactive_ = true;
    return;
  }
  catch (std::exception &e)
  {
    ROS_ERROR_THROTTLE(30, "exception: %s", e.what());
    inactive_ = true;
    return;
  }
  catch (...)
  {
    ROS_ERROR_THROTTLE(30, "exception");
    inactive_ = true;
    return;
  }
}

}
