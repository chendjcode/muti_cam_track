//
// Created by cdj on 2020/10/15.
//
#include <ros/ros.h>
#include <actionlib/client/simple_action_client.h>
#include <actionlib/client/terminal_state.h>
#include "darknet_ros_msgs/CheckForObjectsAction.h"

int main(int argc, char** argv) {
    ros::init(argc, argv, "test_node");
    // create the action client
    // true causes the client to spin its own thread
    actionlib::SimpleActionClient<darknet_ros_msgs::CheckForObjectsAction> ac("/darknet_ros/check_for_objects", true);
    ROS_INFO("Waiting for action server to start.");
    // wait for the action server to start
    ac.waitForServer(); //will wait for infinite time
    ROS_INFO("Action server started, sending goal.");

    //exit
    return 0;
}
