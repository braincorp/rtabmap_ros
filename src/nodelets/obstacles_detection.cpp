/*
Copyright (c) 2010-2014, Mathieu Labbe - IntRoLab - Universite de Sherbrooke
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Universite de Sherbrooke nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <ros/ros.h>
#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>

#include <tf/transform_listener.h>

#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/CameraInfo.h>
#include <stereo_msgs/DisparityImage.h>

#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

#include <image_geometry/pinhole_camera_model.h>

#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/subscriber.h>

#include <cv_bridge/cv_bridge.h>
#include <opencv2/highgui/highgui.hpp>

#include <rtabmap_ros/MsgConversion.h>

#include "rtabmap/core/util3d.h"
#include "rtabmap/core/util3d_filtering.h"
#include "rtabmap/core/util3d_mapping.h"
#include "rtabmap/core/util3d_transforms.h"

namespace rtabmap_ros
{

class ObstaclesDetection : public nodelet::Nodelet
{
public:
	ObstaclesDetection() :
		frameId_("base_link"),
		normalEstimationRadius_(0.05),
		groundNormalAngle_(M_PI_4),
		minClusterSize_(20),
		maxFloorHeight_(-1),
		maxObstaclesHeight_(1.5),
		waitForTransform_(false),
		simpleSegmentation_(false),
		optimizeForCloseObject_(true)
	{}

	virtual ~ObstaclesDetection()
	{}

private:
	virtual void onInit()
	{
		ros::NodeHandle & nh = getNodeHandle();
		ros::NodeHandle & pnh = getPrivateNodeHandle();

		int queueSize = 10;
		pnh.param("queue_size", queueSize, queueSize);
		pnh.param("frame_id", frameId_, frameId_);
		pnh.param("normal_estimation_radius", normalEstimationRadius_, normalEstimationRadius_);
		pnh.param("ground_normal_angle", groundNormalAngle_, groundNormalAngle_);
		pnh.param("min_cluster_size", minClusterSize_, minClusterSize_);
		pnh.param("max_obstacles_height", maxObstaclesHeight_, maxObstaclesHeight_);
		pnh.param("max_floor_height", maxFloorHeight_, maxFloorHeight_);
		pnh.param("wait_for_transform", waitForTransform_, waitForTransform_);
		pnh.param("simple_segmentation", simpleSegmentation_, simpleSegmentation_);
		pnh.param("optimize_for_close_object", optimizeForCloseObject_, optimizeForCloseObject_);

		cloudSub_ = nh.subscribe("cloud", 1, &ObstaclesDetection::callback, this);

		groundPub_ = nh.advertise<sensor_msgs::PointCloud2>("ground", 1);
		obstaclesPub_ = nh.advertise<sensor_msgs::PointCloud2>("obstacles", 1);

		this->_lastFrameTime = ros::Time::now();
	}



	void callback(const sensor_msgs::PointCloud2ConstPtr & cloudMsg)
	{
		if (groundPub_.getNumSubscribers() == 0 && obstaclesPub_.getNumSubscribers() == 0)
		{
			// no one wants the results
			return;
		}

		rtabmap::Transform localTransform;
		try
		{
			if(waitForTransform_)
			{
				if(!tfListener_.waitForTransform(frameId_, cloudMsg->header.frame_id, cloudMsg->header.stamp, ros::Duration(1)))
				{
					ROS_ERROR("Could not get transform from %s to %s after 1 second!", frameId_.c_str(), cloudMsg->header.frame_id.c_str());
					return;
				}
			}
			tf::StampedTransform tmp;
			tfListener_.lookupTransform(frameId_, cloudMsg->header.frame_id, cloudMsg->header.stamp, tmp);
			localTransform = rtabmap_ros::transformFromTF(tmp);
		}
		catch(tf::TransformException & ex)
		{
			ROS_ERROR("%s",ex.what());
			return;
		}

		pcl::PointCloud<pcl::PointXYZ>::Ptr originalCloud(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::fromROSMsg(*cloudMsg, *originalCloud);

		//Even if the original cloud is empty, we need to publish the empty cloud,
		//Otherwise, the aggregator of point cloud would wait indefinitely to get a valid pointcloud
		if(originalCloud->size() == 0)
		{
			ROS_ERROR("Recieved empty point cloud!");
			if(groundPub_.getNumSubscribers())
			{
				sensor_msgs::PointCloud2 rosCloud;
				pcl::toROSMsg(*originalCloud, rosCloud);
				rosCloud.header.stamp = cloudMsg->header.stamp;
				rosCloud.header.frame_id = frameId_;

				//publish the message
				groundPub_.publish(rosCloud);
			}

			if(obstaclesPub_.getNumSubscribers())
			{
				sensor_msgs::PointCloud2 rosCloud;
				pcl::toROSMsg(*originalCloud, rosCloud);
				rosCloud.header.stamp = cloudMsg->header.stamp;
				rosCloud.header.frame_id = frameId_;

				//publish the message
				obstaclesPub_.publish(rosCloud);
			}
			return;
		}

		//Common variables for all strategies
		pcl::PointCloud<pcl::PointXYZ>::Ptr hypotheticalGroundCloud(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::PointCloud<pcl::PointXYZ>::Ptr obstaclesCloud(new pcl::PointCloud<pcl::PointXYZ>);
		pcl::IndicesPtr ground, obstacles;
		pcl::PointCloud<pcl::PointXYZ>::Ptr groundCloud(new pcl::PointCloud<pcl::PointXYZ>);

		ros::Time lasttime = ros::Time::now();

		originalCloud = rtabmap::util3d::transformPointCloud(originalCloud, localTransform);
		hypotheticalGroundCloud  = rtabmap::util3d::passThrough(originalCloud, "z", std::numeric_limits<int>::min(), maxFloorHeight_);
		obstaclesCloud = rtabmap::util3d::passThrough(originalCloud, "z", maxFloorHeight_, maxObstaclesHeight_);

		if (simpleSegmentation_) {
		    // If the option simple segmentation has been set to true,
		    // the floor is just the hypothetical ground cloud, simply
		    // cut off based on z
		    groundCloud = hypotheticalGroundCloud;
		}

		else if (!optimizeForCloseObject_) {
			// This is the default strategy
			// The cloud is divided in two based on reported Z and the position of the camera.
			// One is the hypothetical ground cloud and the other one is the obstacles pointcloud.
			// The algorithm then extracts (and removes) from the hypothetical ground cloud
			// the detected obstacles, and adds them to the obstacles pointcloud

			rtabmap::util3d::segmentObstaclesFromGround<pcl::PointXYZ>(hypotheticalGroundCloud,
					ground, obstacles, normalEstimationRadius_, groundNormalAngle_, minClusterSize_);

			if(ground.get() && ground->size())
			{
				pcl::copyPointCloud(*hypotheticalGroundCloud, *ground, *groundCloud);
			}

			if(obstacles.get() && obstacles->size())
			{
				pcl::PointCloud<pcl::PointXYZ>::Ptr obstaclesFloorCloud(new pcl::PointCloud<pcl::PointXYZ>);
				pcl::copyPointCloud(*hypotheticalGroundCloud, *obstacles, *obstaclesFloorCloud);
				*obstaclesCloud += *obstaclesFloorCloud;
			}

		}

		else {
			// in this case optimizeForCloseObject_ is true:
			// we divide the floor point cloud into two subsections, one for all potential floor points up to 1m
			// one for potential floor points further away than 1m.
			// For the points at closer range, we use a smaller normal estimation radius and ground normal angle,
			// which allows to detect smaller objects, without increasing the number of false positive.
			// For all other points, we use a bigger normal estimation radius (* 3.) and tolerance for the
			// grond normal angle (* 2.).

			pcl::PointCloud<pcl::PointXYZ>::Ptr hypotheticalGroundCloud_near = rtabmap::util3d::passThrough(hypotheticalGroundCloud, "x", std::numeric_limits<int>::min(), 1.);
			pcl::PointCloud<pcl::PointXYZ>::Ptr hypotheticalGroundCloud_far = rtabmap::util3d::passThrough(hypotheticalGroundCloud, "x", 1., std::numeric_limits<int>::max());

			obstaclesCloud = rtabmap::util3d::passThrough(obstaclesCloud, "x",  0.8, std::numeric_limits<int>::max());

			// Part 1: segment floor and obstacles near the robot
			rtabmap::util3d::segmentObstaclesFromGround<pcl::PointXYZ>(hypotheticalGroundCloud_near,
								ground, obstacles, normalEstimationRadius_, groundNormalAngle_, minClusterSize_);

			if(ground.get() && ground->size())
			{
				pcl::copyPointCloud(*hypotheticalGroundCloud_near, *ground, *groundCloud);
			}


			if(obstacles.get() && obstacles->size())
			{
				pcl::PointCloud<pcl::PointXYZ>::Ptr obstaclesFloorCloud_near(new pcl::PointCloud<pcl::PointXYZ>);
				pcl::copyPointCloud(*hypotheticalGroundCloud_near, *obstacles, *obstaclesFloorCloud_near);
				*obstaclesCloud += *obstaclesFloorCloud_near;
			}

			// Part 2: segment floor and obstacles far from the robot
			rtabmap::util3d::segmentObstaclesFromGround<pcl::PointXYZ>(hypotheticalGroundCloud_far,
											ground, obstacles, 3.*normalEstimationRadius_, 2.*groundNormalAngle_, minClusterSize_);

			if(ground.get() && ground->size())
			{
				pcl::PointCloud<pcl::PointXYZ>::Ptr groundCloud2 (new pcl::PointCloud<pcl::PointXYZ>);
				pcl::copyPointCloud(*hypotheticalGroundCloud_far, *ground, *groundCloud2);
				*groundCloud += *groundCloud2;
			}


			if(obstacles.get() && obstacles->size())
			{
				pcl::PointCloud<pcl::PointXYZ>::Ptr obstaclesFloorCloud_far(new pcl::PointCloud<pcl::PointXYZ>);
				pcl::copyPointCloud(*hypotheticalGroundCloud_far, *obstacles, *obstaclesFloorCloud_far);
				*obstaclesCloud += *obstaclesFloorCloud_far;
			}

		}

		if(groundPub_.getNumSubscribers())
		{
			sensor_msgs::PointCloud2 rosCloud;
			pcl::toROSMsg(*groundCloud, rosCloud);
			rosCloud.header.stamp = cloudMsg->header.stamp;
			rosCloud.header.frame_id = frameId_;

			//publish the message
			groundPub_.publish(rosCloud);
		}

		if(obstaclesPub_.getNumSubscribers())
		{
			sensor_msgs::PointCloud2 rosCloud;
			pcl::toROSMsg(*obstaclesCloud, rosCloud);
			rosCloud.header.stamp = cloudMsg->header.stamp;
			rosCloud.header.frame_id = frameId_;

			//publish the message
			obstaclesPub_.publish(rosCloud);
		}

		ros::Time curtime = ros::Time::now();

		ros::Duration process_duration = curtime - lasttime;
		ros::Duration between_frames = curtime - this->_lastFrameTime;
		this->_lastFrameTime = curtime;
	}

private:
	std::string frameId_;
	double normalEstimationRadius_;
	double groundNormalAngle_;
	int minClusterSize_;
	double maxObstaclesHeight_;
	double maxFloorHeight_;
	bool waitForTransform_;
	bool simpleSegmentation_;
	bool optimizeForCloseObject_;

	tf::TransformListener tfListener_;

	ros::Publisher groundPub_;
	ros::Publisher obstaclesPub_;

	ros::Subscriber cloudSub_;
	ros::Time _lastFrameTime;
};

PLUGINLIB_EXPORT_CLASS(rtabmap_ros::ObstaclesDetection, nodelet::Nodelet);
}

