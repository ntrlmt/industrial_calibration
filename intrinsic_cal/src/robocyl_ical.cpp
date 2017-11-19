/*
 * Software License Agreement (Apache License)
 *
 * Copyright (c) 2014, Southwest Research Institute
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ros/ros.h>
#include <ros/package.h>
#include <ros/console.h>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>
#include <industrial_extrinsic_cal/camera_observer_trigger.h>
#include <industrial_extrinsic_cal/user_accept.h>
#include <industrial_extrinsic_cal/ros_camera_observer.h>
#include <industrial_extrinsic_cal/basic_types.h>
#include <industrial_extrinsic_cal/camera_definition.h>
#include <industrial_extrinsic_cal/ceres_costs_utils.h> 
#include <industrial_extrinsic_cal/ceres_costs_utils.hpp> 
#include <intrinsic_cal/rail_ical_run.h>
#include <intrinsic_cal/rail_ical_run.h>
#include <intrinsic_cal/rail_ical_run.h>
#include <robo_cylinder/HomeCmd.h>
#include <robo_cylinder/MoveMeters.h>
#include <robo_cylinder/MovePulses.h>
#include <robo_cylinder/PowerIO.h>
#include <robo_cylinder/StatusUpdate.h>
#include <robo_cylinder/VelAcc.h>
#include "ceres/ceres.h"
#include "ceres/rotation.h"
#include "ceres/types.h"

using std::string;
using boost::shared_ptr;
using ceres::CostFunction;
using ceres::Problem;
using ceres::Solver;
using industrial_extrinsic_cal::Target;
using industrial_extrinsic_cal::CameraObservations;
using industrial_extrinsic_cal::ROSCameraObserver;
using industrial_extrinsic_cal::Roi;
using industrial_extrinsic_cal::Pose6d;
using industrial_extrinsic_cal::Point3d;
using industrial_extrinsic_cal::Camera;
using industrial_extrinsic_cal::CameraParameters;
using industrial_extrinsic_cal::NoWaitTrigger;

class RobocylCalService 
{
public:
  RobocylCalService(ros::NodeHandle nh);
  ~RobocylCalService()  {  } ;
  bool executeCallBack( intrinsic_cal::rail_ical_run::Request &req, intrinsic_cal::rail_ical_run::Response &res);
  void  initMCircleTarget(int rows, int cols, double circle_dia, double spacing);
  void cameraCallback(const sensor_msgs::Image& image);

private:
  ros::NodeHandle nh_;
  ros::ServiceServer rail_cal_server_;
  ros::Subscriber rgb_sub_;
  ros::Publisher rgb_pub_;
  ros::ServiceClient move_client_; /**< a client for calling the service to move the robo-cylinder to a new location */
  ros::ServiceClient power_client_; /**< a client for calling the service to turn on the robo-cylinder */
  ros::ServiceClient home_client_; /**< a client for calling the service to move robo-cylinder to its home position */
  shared_ptr<Target> target_;
  shared_ptr<Camera> camera_;
  double focal_length_x_;
  double focal_length_y_;
  double center_x_;
  double center_y_;
  string image_topic_;
  string camera_name_;
  int target_type_;
  int target_rows_;
  int target_cols_;
  double circle_spacing_;
  double circle_diameter_;
  int num_camera_locations_;
  double camera_spacing_;
  int image_height_;
  int image_width_;
  double D0_;
  double qx_, qy_, qz_, qw_;
  CameraParameters camera_parameters_;
};

RobocylCalService::RobocylCalService(ros::NodeHandle nh)
{
  
  nh_ = nh;
  ros::NodeHandle pnh("~");

  if(!pnh.getParam( "image_topic", image_topic_)){
    ROS_ERROR("Must set param:  image_topic");
  }

  if(!pnh.getParam( "camera_name", camera_name_)){
    ROS_ERROR("Must set param:  camera_name");
  }

  target_type_ == pattern_options::ModifiedCircleGrid;

  if(!pnh.getParam( "target_rows", target_rows_)){
    ROS_ERROR("Must set param:  target_rows");
  }
  if(!pnh.getParam( "target_cols", target_cols_)){
    ROS_ERROR("Must set param:  target_cols");
  }
  if(!pnh.getParam( "target_circle_dia", circle_diameter_)){
    ROS_ERROR("Must set param:  target_circle_dia");
  }
  if(!pnh.getParam( "target_spacing", circle_spacing_)){
    ROS_ERROR("Must set param:  target_spacing");
  }
  if(!pnh.getParam( "num_camera_locations", num_camera_locations_)){
    ROS_ERROR("Must set param:  num_camera_locations");
  }
  if(!pnh.getParam( "camera_spacing", camera_spacing_)){
    ROS_ERROR("Must set param:  camera_spacing");
  }
  if(!pnh.getParam( "image_height", image_height_)){
    ROS_ERROR("Must set param:  image_height_");
  }
  if(!pnh.getParam( "image_width", image_width_)){
    ROS_ERROR("Must set param:  image_width_");
  }
  if(!pnh.getParam( "target_to_rail_distance", D0_)){
    ROS_ERROR("Must set param:  target_to_rail_distance");
  }

  bool use_quaternion = false;
  if(pnh.getParam( "qx", qx_))
  {
    if(pnh.getParam( "qy", qy_))
    {
      if(pnh.getParam( "qz", qz_))
      {
        if(pnh.getParam( "qw", qw_))
        {
          use_quaternion = true;
        }
      }
    }
  }
  string move_client_name("/move_meters");
  if(!pnh.getParam( "move_client", move_client_name)){
    ROS_WARN("move_client = %s", move_client_name.c_str());
  }
  string power_client_name("/power_io");
  if(!pnh.getParam( "power_client", power_client_name)){
    ROS_WARN("power_client = %s", power_client_name.c_str());
  }
  string home_client_name("/home");
  if(!pnh.getParam( "home_client", home_client_name)){
    ROS_WARN("home_client = %s", home_client_name.c_str());
  }

  move_client_  = nh.serviceClient<robo_cylinder::MoveMeters>(move_client_name);
  power_client_  = nh.serviceClient<robo_cylinder::PowerIO>(power_client_name);
  home_client_  = nh.serviceClient<robo_cylinder::HomeCmd>(home_client_name);

  u_int32_t queue_size = 5;
  rgb_sub_ = nh_.subscribe("color_image", queue_size, &RobocylCalService::cameraCallback, this);
  rgb_pub_ = nh_.advertise<sensor_msgs::Image>("color_image_center", 1);

  if(!use_quaternion)
  {
    qx_ = 1.0;
    qy_ = qz_ = qw_ = 0.0;
    ROS_WARN("parameters qx, qy, qz, and qw not provided, using default values of (%.2f, %.2f, %.2f, %.2f)", qx_, qy_, qz_, qw_);
  }

  bool is_moving = true;
  camera_ =  boost::make_shared<industrial_extrinsic_cal::Camera>("my_camera", camera_parameters_, is_moving);
  camera_->trigger_ = boost::make_shared<NoWaitTrigger>();
  camera_->camera_observer_ = boost::make_shared<ROSCameraObserver>(image_topic_, camera_name_);
  if(!camera_->camera_observer_->pullCameraInfo(camera_->camera_parameters_.focal_length_x,
                                           camera_->camera_parameters_.focal_length_y,
                                           camera_->camera_parameters_.center_x,
                                           camera_->camera_parameters_.center_y,
                                           camera_->camera_parameters_.distortion_k1,
                                           camera_->camera_parameters_.distortion_k2,
                                           camera_->camera_parameters_.distortion_k3,
                                           camera_->camera_parameters_.distortion_p1,
                                           camera_->camera_parameters_.distortion_p2,
                                           image_width_, image_height_))
  {
    ROS_FATAL("Could not get camera information for %s from topic %s. Shutting down node.", camera_name_.c_str(), image_topic_.c_str());
    ros::shutdown();
  }

  ROS_INFO("initial camera info focal:%f %f center:%f %f  radial:%f %f %f tang: %f %f",
            camera_->camera_parameters_.focal_length_x,
            camera_->camera_parameters_.focal_length_y,
            camera_->camera_parameters_.center_x,
            camera_->camera_parameters_.center_y,
            camera_->camera_parameters_.distortion_k1,
            camera_->camera_parameters_.distortion_k2,
            camera_->camera_parameters_.distortion_k3,
            camera_->camera_parameters_.distortion_p1,
            camera_->camera_parameters_.distortion_p2);
            
  initMCircleTarget(target_rows_, target_cols_, circle_diameter_, circle_spacing_);
 
  rail_cal_server_ = nh_.advertiseService( "RobocylCalService", &RobocylCalService::executeCallBack, this);
}

void RobocylCalService::cameraCallback(const sensor_msgs::Image &image)
{
  cv_bridge::CvImagePtr bridge = cv_bridge::toCvCopy(image, image.encoding);

  cv::Mat mod_img = bridge->image;
  cv::circle(mod_img, cv::Point2d(image.width / 2.0, image.height / 2.0), 4, cv::Scalar(255,0,0), 2);
  bridge->image = mod_img;

  sensor_msgs::Image out_img;
  bridge->toImageMsg(out_img);
  rgb_pub_.publish(out_img);

}

bool RobocylCalService::executeCallBack( intrinsic_cal::rail_ical_run::Request &req, intrinsic_cal::rail_ical_run::Response &res)
{
  ros::NodeHandle nh;
  CameraObservations camera_observations;
  robo_cylinder::MoveMeters::Request mm_request; /**< request when transform is part of a mutable set */
  robo_cylinder::MoveMeters::Response mm_response; /**< request when transform is part of a mutable set */
  robo_cylinder::PowerIO::Request pio_request; /**< request when transform is part of a mutable set */
  robo_cylinder::PowerIO::Response pio_response; /**< request when transform is part of a mutable set */
  robo_cylinder::HomeCmd::Request hc_request; /**< request when transform is part of a mutable set */
  robo_cylinder::HomeCmd::Response hc_response; /**< request when transform is part of a mutable set */

  int num_observations ;
  int total_observations=0;
  double rxry[2]; // pitch and yaw of camera relative to rail
  rxry[0] = 0.0;
  rxry[1] = 0.0;

  camera_->camera_observer_->clearObservations();
  camera_->camera_observer_->clearTargets();

  // turn power on to robo-cylinder, and move it home
  pio_request.io = 1;
  power_client_.call(pio_request, pio_response);
  home_client_.call(hc_request, hc_response);

  // set the roi to the whole image
  Roi roi;
  roi.x_min = 0;
  roi.y_min = 0;
  roi.x_max = image_width_;
  roi.y_max = image_height_;

  industrial_extrinsic_cal::Cost_function cost_type = industrial_extrinsic_cal::cost_functions::CameraReprjErrorWithDistortion;
  Problem problem; // note, a new problem gets created each time execute is called, so old observation data is not re-used.

  // set initial conditions,
  // Need to use setQuaternion because setBasis does not work for some rotations (results in flipped yaw values and negative focal lengths)
  target_->pose_.setQuaternion(qx_, qy_, qz_, qw_);
  target_->pose_.setOrigin(0.011, 0.05, D0_);
  target_->pose_.show("initial target pose");
  ros::NodeHandle pnh("~");
  bool camera_ready = false;
  pnh.setParam("camera_ready", camera_ready);
  for(int i=0; i<num_camera_locations_; i++){
    double rail_position = i*camera_spacing_;
    ROS_ERROR("moving to %lf",rail_position);
    mm_request.meters = rail_position;
    move_client_.call(mm_request, mm_response);
    // wait for camera to be moved
    ros::Duration(0.2).sleep();

    // gather next image
    camera_->camera_observer_->clearTargets();
    camera_->camera_observer_->clearObservations();
    camera_->camera_observer_->addTarget(target_, roi, cost_type);
    camera_->camera_observer_->triggerCamera();
    while (!camera_->camera_observer_->observationsDone()) ;
    camera_->camera_observer_->getObservations(camera_observations);
    ROS_INFO("Found %d observations",(int) camera_observations.size());
    num_observations = (int) camera_observations.size();
    if(num_observations != target_rows_* target_cols_){
      ROS_ERROR("Target Locator could not find target %d", num_observations);
    }
    
    // add a new cost to the problem for each observation
    CostFunction* cost_function[num_observations]; // not sure I need a new cost function each time, but this just uses memory
    total_observations += num_observations;
    for(int i=0; i<num_observations; i++){
      double image_x = camera_observations[i].image_loc_x;
      double image_y = camera_observations[i].image_loc_y;
      Point3d point = target_->pts_[i]; // assume correct ordering from camera observer
      cost_function[i] = industrial_extrinsic_cal::RailICal::Create(image_x, image_y, rail_position, point);
      problem.AddResidualBlock(cost_function[i], NULL ,
             camera_->camera_parameters_.pb_intrinsics,
             target_->pose_.pb_pose);
      double residual[2];
      industrial_extrinsic_cal::RailICal RC(image_x, image_y, rail_position, point);
    } // for each observation at this camera_location
  }// for each camera_location

  // set up and solve the problem
  Solver::Options options;
  Solver::Summary summary;
  options.linear_solver_type = ceres::DENSE_SCHUR;
  options.minimizer_progress_to_stdout = true;
  options.max_num_iterations = 2000;
  ceres::Solve(options, &problem, &summary);
  if(summary.termination_type != ceres::NO_CONVERGENCE){
    double initial_cost = summary.initial_cost/total_observations;
    double final_cost = summary.final_cost/total_observations;
    ROS_INFO("Problem solved, initial cost = %lf, final cost = %lf", initial_cost, final_cost);
    target_->pose_.show("target_pose");
    ROS_INFO("camera_matrix data: [ %lf, 0.0, %lf, 0.0, %lf, %lf, 0.0, 0.0, 1.0]", 
	      camera_->camera_parameters_.focal_length_x,
	      camera_->camera_parameters_.center_x,
	      camera_->camera_parameters_.focal_length_y,
	      camera_->camera_parameters_.center_y);
    ROS_INFO("distortion data: [ %lf,  %lf,  %lf,  %lf,  %lf]",
   	      camera_->camera_parameters_.distortion_k1,
   	      camera_->camera_parameters_.distortion_k2,
   	      camera_->camera_parameters_.distortion_p1,
   	      camera_->camera_parameters_.distortion_p2,
   	      camera_->camera_parameters_.distortion_k3);
    ROS_INFO("projection_matrix data: [ %lf, 0.0, %lf, 0.0, 0.0, %lf, %lf, 0.0, 0.0, 0.0, 1.0, 0.0]", 
	      camera_->camera_parameters_.focal_length_x,
	      camera_->camera_parameters_.center_x,
	      camera_->camera_parameters_.focal_length_y,
	      camera_->camera_parameters_.center_y);
    if(final_cost <= req.allowable_cost_per_observation){
      res.final_pose.position.x = target_->pose_.x;
      res.final_pose.position.y = target_->pose_.y;
      res.final_pose.position.z = target_->pose_.z;
      res.final_cost_per_observation  = final_cost;
      target_->pose_.getQuaternion(res.final_pose.orientation.x,
				   res.final_pose.orientation.y, 
				   res.final_pose.orientation.z,
				   res.final_pose.orientation.w);
      camera_->camera_observer_->pushCameraInfo(camera_->camera_parameters_.focal_length_x,
                                                camera_->camera_parameters_.focal_length_y,
                                                camera_->camera_parameters_.center_x,
                                                camera_->camera_parameters_.center_y,
                                                camera_->camera_parameters_.distortion_k1,
                                                camera_->camera_parameters_.distortion_k2,
                                                camera_->camera_parameters_.distortion_k3,
                                                camera_->camera_parameters_.distortion_p1,
                                                camera_->camera_parameters_.distortion_p2);

      return true;
    }
    else{
      res.final_cost_per_observation  = final_cost;
      ROS_ERROR("allowable cost exceeded %f > %f", final_cost, req.allowable_cost_per_observation);
      return(false);
    }
  }
}

void RobocylCalService::initMCircleTarget(int rows, int cols, double circle_dia, double spacing)
{
  target_ =  boost::make_shared<industrial_extrinsic_cal::Target>();
  target_->is_moving_ = true;
  target_->target_name_ = "modified_circle_target";
  target_->target_frame_ = "target_frame";
  target_->target_type_ =  2;
  target_->circle_grid_parameters_.pattern_rows =rows;
  target_->circle_grid_parameters_.pattern_cols = cols;
  target_->circle_grid_parameters_.circle_diameter = circle_dia;
  target_->circle_grid_parameters_.is_symmetric = true; 
  // create a grid of points
  target_->pts_.clear();
  target_->num_points_ = rows*cols;
  for(int i=rows-1; i>=0; i--){
    for(int j=0; j<cols; j++){
      Point3d point;
      point.x = j*spacing;
      point.y = i*spacing;
      point.z = 0.0;
      target_->pts_.push_back(point);
    }
  }
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "rail_cal_service");
  ros::NodeHandle node_handle;
  RobocylCalService rail_cal(node_handle);
  ros::spin();
  ros::waitForShutdown();
  return 0;
}
