#include <ros/ros.h>
#include <actionlib/server/simple_action_server.h>
#include <muti_cam_track/FibonacciAction.h>

class FibonacciAction {
protected:
    ros::NodeHandle nh_;
    // NodeHandle instance must be created before this line. Otherwise strange error occurs.
    actionlib::SimpleActionServer<muti_cam_track::FibonacciAction> as_;
    std::string action_name_;
    // create messages that are used to published feedback/result
    muti_cam_track::FibonacciActionFeedback feedback_;
    muti_cam_track::FibonacciResult result_;
public:
    FibonacciAction(std::string name) : as_(nh_, name, boost::bind(&FibonacciAction::executeCB, this, _1)),
                                        action_name_(name) {
        as_.start();
    }

    ~FibonacciAction(void) {}

    void executeCB(const muti_cam_track::FibonacciGoalConstPtr &goal) {
        // helper variables
        ros::Rate r(10);
        bool success = true;
        // push_back the seeds for the fibonacci sequence
        feedback_.feedback.sequence.clear();
        feedback_.feedback.sequence.push_back(0);
        feedback_.feedback.sequence.push_back(1);
        // create messages that are used to published feedback/result
        // publish info to the console for the user
        ROS_INFO("%s: Executing, creating fibonacci sequence of order %i with seeds %i, %i", action_name_.c_str(),
                 goal->order, feedback_.feedback.sequence[0], feedback_.feedback.sequence[1]);
        // start executing the action
        for (int i = 0; i < goal->order; i++) {
            // check that preempt has not been requested by the client
            if (as_.isPreemptRequested() || !ros::ok()) {
                ROS_INFO("%s: Preempted", action_name_.c_str());
                // set the action state to preempted
                as_.setPreempted();
                success = false;
                break;
            }
            feedback_.feedback.sequence.push_back(feedback_.feedback.sequence[i] + feedback_.feedback.sequence[i-1]);
            // publish the feedback
            as_.publishFeedback(feedback_.feedback);
            // this sleep is not necessary, the sequence is computed at 1 Hz for demonstration purposes
            r.sleep();
        }
        if (success) {
            result_.sequence = feedback_.feedback.sequence;
            ROS_INFO("%s: Succeeded", action_name_.c_str());
            // set the action state to succeeded
            as_.setSucceeded(result_);
        }
    }
};

int main(int argc, char** argv) {
    ros::init(argc, argv, "fibonacci");
    FibonacciAction fibonacci("fibonacci");
    ros::spin();
    return 0;
}