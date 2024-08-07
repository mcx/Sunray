// run Sunray-based robot as an ROS node 

// start like this:
//
// 1. sudo bash
// 2. source devel/setup.bash
// 3. roslaunch sunray_node test.launch


// http://wiki.ros.org/ROS/Tutorials/WritingPublisherSubscriber%28c%2B%2B%29
// http://wiki.ros.org/tf/Tutorials/Writing%20a%20tf%20listener%20%28C%2B%2B%29

#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <mcl_3dl_msgs/Status.h>
#include <std_msgs/Int8.h>
#include <std_srvs/Trigger.h>
#include <tf/transform_listener.h>


#include "config.h"  
#include "robot.h"



char **argv = NULL;
int argc = 0;


ros::NodeHandle *node;
ros::Rate *rate;
ros::Subscriber obstacle_state_sub;
ros::Subscriber localization_status_sub;
ros::ServiceClient src_global_localization;
tf::TransformListener *tfListener;
double nextErrorTime = 0;
double nextPrintTime = 0;
double nextCheckTime = 0;
double convergenceTimeout = 0;
double match_ratio = 0;
double match_ratio_lp = 0;
int convergence_status = 0;
int globalLocalizationTriggerCounter = 0;


void obstacleStateCallback(const std_msgs::Int8 &msg)
{
  //ROS_INFO("obstacleStateCallback %d", msg.data);
  if (msg.data == 1){
    // near obstacle
    lidarBumper.triggerNearObstacle = true;
    lidarBumper.triggerBumper = false;
  } else if (msg.data == 2){
    // obstacle
    lidarBumper.triggerNearObstacle = false;
    lidarBumper.triggerBumper = true;
  } else {
    // no obstacle
    lidarBumper.triggerNearObstacle = false;
    lidarBumper.triggerBumper = false;  
  }
}


void localizationStatusCallback(const mcl_3dl_msgs::Status& msg)
{
  convergence_status = msg.convergence_status;
  match_ratio = msg.match_ratio;
}



void triggerGlobalLocalization()
{
    globalLocalizationTriggerCounter++;
    //ROS_WARN("triggerGlobalLocalization");
    std_srvs::Trigger trigger;
    if (src_global_localization.call(trigger)){
      // call success
    } else {
      // call failed
      //ROS_ERROR("Failed to call global localization service");
    }
}


void setup(){  
  #ifdef GPS_LIDAR 
    imuDriver.imuFound = true;
  #endif
  start();

  // Initialize the node
  ros::init(argc, argv, "sunray_node");
  ros::Time::init();

  node = new ros::NodeHandle; 
  tfListener = new tf::TransformListener;

  // Loop at 20Hz, until we shut down
  ROS_INFO("--------------started sunray_node-------------");

  rate = new ros::Rate(50);
  obstacle_state_sub = node->subscribe("/obstacle_state", 1, &obstacleStateCallback);

  localization_status_sub = node->subscribe("mcl_3dl/status", 1, localizationStatusCallback);

  src_global_localization = node->serviceClient<std_srvs::TriggerRequest, std_srvs::TriggerResponse>("global_localization");

} 


void loop(){      
  run(); 

  if (ros::ok()) {
    double tim = ros::Time::now().toSec();     

    float x = 0;
    float y = 0;
    float z = 0;
    
    double roll, pitch, yaw;
        

    #ifdef GPS_LIDAR       

      // lookup ROS localization (mathematically, a frame transformation) 
      tf::StampedTransform transform;
      try{
          //  http://wiki.ros.org/tf/Tutorials/Time%20travel%20with%20tf%20%28C%2B%2B%29
          //tfListener->lookupTransform("robot/odom", "gps_link",  ros::Time(0), transform); // target_frame, source_frame
          tfListener->lookupTransform("map", "gps_link",  ros::Time(0), transform); // target_frame, source_frame
    
          x = transform.getOrigin().x();
          y = transform.getOrigin().y();
          z = transform.getOrigin().z();
          
          // https://gist.github.com/LimHyungTae/2499a68ea8ee4d8a876a149858a5b08e
          tf::Quaternion q = transform.getRotation(); 
          
          //float yaw = tf::getYaw(q); 
          
          tf::Matrix3x3 m;  
          m.setRotation(q);  //  quaternion -> rotation Matrix 
          
          // rotation Matrix -> rpy 
          m.getRPY(roll, pitch, yaw);
        
          // let the magic happen (here we transfer ROS localization into Sunray GPS localization) 
          gps.relPosN = y;
          gps.relPosE = x;
          gps.relPosD = z;
          //gps.solution = SOL_FIXED;
          gps.solutionAvail = true;
          //gps.dgpsAge = millis();

          imuDriver.quatX = q.x(); // quaternion
          imuDriver.quatY = q.y(); // quaternion
          imuDriver.quatZ = q.z(); // quaternion        
          imuDriver.quatW = q.w(); // quaternion      
          imuDriver.roll = roll; // euler radiant
          imuDriver.pitch = pitch; // euler radiant
          imuDriver.yaw = yaw;   // euler radiant                
      }
      catch (tf::TransformException ex){
          if (tim > nextErrorTime){
            nextErrorTime = tim + 10.0;
            ROS_ERROR("%s",ex.what());
            //ros::Duration(0.2).sleep();
          }
      }
  
      // pretend IMU avail (so firmware does not try to calibrate IMU if no ROS pose available)
      imuDriver.dataAvail = true; 

      if (tim > nextPrintTime){
        nextPrintTime = tim + 0.5;
        ROS_WARN("ROS: mr=%.2f cs=%d gc=%d  x=%.2f  y=%.2f  z=%.2f yaw=%.2f", 
          match_ratio_lp, convergence_status, globalLocalizationTriggerCounter,  
          x, y, z, yaw/3.1415*180.0);
      } 

      if (tim > nextCheckTime){
        nextCheckTime = tim + 0.2;
      
        //if (!gps.isRelocalizing){
          if (match_ratio_lp > 0.5){
            // convergence status workaround: sometimes we have no convergence for a valid position, but the match ratio is high
            convergenceTimeout = tim + 20.0;                        
            gps.isRelocalizing = false;
            gps.solution = SOL_FIXED;          
          }
        //}      

        if (convergence_status == 1){
          gps.isRelocalizing = false;
          convergenceTimeout = tim + 60.0;                
          gps.dgpsAge = millis();      // TODO: not the most elegant way to visualize the last convergence time
          gps.solution = SOL_FIXED;          
        }

        if ((convergence_status == 0) && (tim > convergenceTimeout)) {
          triggerGlobalLocalization();
          gps.solution = SOL_INVALID;          
          gps.isRelocalizing = true;
          convergenceTimeout = tim + 30.0;
          match_ratio_lp = 0;
        }

        gps.numSV = convergence_status; // TODO: not the most elegant way to visualize the convergence status
        gps.accuracy = match_ratio_lp;     // TODO: not the most elegant way to visualize the match ratio

        match_ratio_lp = 0.95 * match_ratio_lp + 0.05 * match_ratio;  // low-pass filter
      }

    #endif

    // https://stackoverflow.com/questions/23227024/difference-between-spin-and-rate-sleep-in-ros
    //rate->sleep();
    ros::spinOnce();
  } 
  else {
    ROS_ERROR("ROS shutdown");
    //ros::Duration(0.2).sleep();
    exit(0);
  }
}




