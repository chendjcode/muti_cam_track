//
// Created by cdj on 2020/10/14.
//
#include <ros/ros.h>
#include <apriltag_ros/AprilTagDetectionArray.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <sensor_msgs/CameraInfo.h>
#include <visualization_msgs/Marker.h>

#include <actionlib/client/simple_action_client.h>
#include "darknet_ros_msgs/CheckForObjectsAction.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace message_filters;
using namespace apriltag_ros;
using namespace sensor_msgs;

ros::Publisher marker_pub;
Eigen::Isometry3d T_tag2northwest;
Eigen::Isometry3d T_tag2southwest;
bool coord_calib;

cv::Point2f pixel2cam(darknet_ros_msgs::BoundingBox &bbox, const CameraInfoConstPtr &cam_info) {
    auto x = (bbox.xmin + bbox.xmax) / 2;
    auto y = (bbox.ymin + bbox.xmax) / 2;
    auto fx = cam_info->K[0], fy = cam_info->K[4], cx = cam_info->K[2], cy = cam_info->K[5];
    return cv::Point2f
            (
                    (x - cx) / fx,
                    (y - cy) / fy
            );
}
//cv::Point2f pixel2cam(const cv::Point2d &p, const CameraInfoConstPtr &cam_info) {
//    auto fx = cam_info->K[0], fy = cam_info->K[4], cx = cam_info->K[2], cy = cam_info->K[5];
//    return cv::Point2f
//            (
//                    (p.x - cx) / fx,
//                    (p.y - cy) / fy
//            );
//}

cv::Mat Eigen2Mat(Eigen::Isometry3d &T) {
    return (cv::Mat_<float>(3, 4)
            <<
            T(0, 0), T(0, 1), T(0, 2), T(0, 3),
            T(1, 0), T(1, 1), T(1, 2), T(1, 3),
            T(2, 0), T(2, 1), T(2, 2), T(2, 3));
}

void _coord_calib(const AprilTagDetectionArrayConstPtr &northwest_array, const AprilTagDetectionArrayConstPtr &southwest_array) {
    auto orientation_northwest2tag = northwest_array->detections[0].pose.pose.pose.orientation;
    auto position_northwest2tag = northwest_array->detections[0].pose.pose.pose.position;
    Eigen::Quaterniond quaternion_northwest2tag(orientation_northwest2tag.w, orientation_northwest2tag.x,
                                                orientation_northwest2tag.y, orientation_northwest2tag.z);
    quaternion_northwest2tag.normalize();
    Eigen::Matrix3d R_tag2northwest = quaternion_northwest2tag.inverse().toRotationMatrix();
    T_tag2northwest = Eigen::Isometry3d::Identity();
    T_tag2northwest.rotate(R_tag2northwest);
    T_tag2northwest.translate(
            Eigen::Vector3d(-position_northwest2tag.x, -position_northwest2tag.y, -position_northwest2tag.z));

    auto orientation_southwest2tag = southwest_array->detections[0].pose.pose.pose.orientation;
    auto position_southwest2tag = southwest_array->detections[0].pose.pose.pose.position;
    Eigen::Quaterniond quaternion_southwest2tag(orientation_southwest2tag.w, orientation_southwest2tag.x,
                                                orientation_southwest2tag.y, orientation_southwest2tag.z);
    quaternion_southwest2tag.normalize();
    Eigen::Matrix3d R_tag2southwest = quaternion_southwest2tag.inverse().toRotationMatrix();

    T_tag2southwest = Eigen::Isometry3d::Identity();
    T_tag2southwest.rotate(R_tag2southwest);
    T_tag2southwest.translate(
            Eigen::Vector3d(-position_southwest2tag.x, -position_southwest2tag.y, -position_southwest2tag.z));
}

void find_feature_matches(const cv::Mat &img_1, const cv::Mat &img_2,
                          std::vector<cv::KeyPoint> &keypoints_1,
                          std::vector<cv::KeyPoint> &keypoints_2,
                          std::vector<cv::DMatch> &matches) {
    //-- 初始化
    cv::Mat descriptors_1, descriptors_2;
    // used in OpenCV3
    cv::Ptr<cv::FeatureDetector> detector = cv::ORB::create();
    cv::Ptr<cv::DescriptorExtractor> descriptor = cv::ORB::create();
    // use this if you are in OpenCV2
    // Ptr<FeatureDetector> detector = FeatureDetector::create ( "ORB" );
    // Ptr<DescriptorExtractor> descriptor = DescriptorExtractor::create ( "ORB" );
    cv::Ptr<cv::DescriptorMatcher> matcher = cv::DescriptorMatcher::create("BruteForce-Hamming");
    //-- 第一步:检测 Oriented FAST 角点位置
    detector->detect(img_1, keypoints_1);
    detector->detect(img_2, keypoints_2);

    //-- 第二步:根据角点位置计算 BRIEF 描述子
    descriptor->compute(img_1, keypoints_1, descriptors_1);
    descriptor->compute(img_2, keypoints_2, descriptors_2);

    //-- 第三步:对两幅图像中的BRIEF描述子进行匹配，使用 Hamming 距离
    vector<cv::DMatch> match;
    // BFMatcher matcher ( NORM_HAMMING );
    matcher->match(descriptors_1, descriptors_2, match);

    //-- 第四步:匹配点对筛选
    double min_dist = 10000, max_dist = 0;

    //找出所有匹配之间的最小距离和最大距离, 即是最相似的和最不相似的两组点之间的距离
    for (int i = 0; i < descriptors_1.rows; i++) {
        double dist = match[i].distance;
        if (dist < min_dist) min_dist = dist;
        if (dist > max_dist) max_dist = dist;
    }

    printf("-- Max dist : %f \n", max_dist);
    printf("-- Min dist : %f \n", min_dist);

    //当描述子之间的距离大于两倍的最小距离时,即认为匹配有误.但有时候最小距离会非常小,设置一个经验值30作为下限.
    for (int i = 0; i < descriptors_1.rows; i++) {
        if (match[i].distance <= max(2 * min_dist, 30.0)) {
            matches.push_back(match[i]);
        }
    }
}

void
callback(const AprilTagDetectionArrayConstPtr &northwest_array, const AprilTagDetectionArrayConstPtr &southwest_array,
         const CameraInfoConstPtr &northwest_cam_info, const CameraInfoConstPtr &southwest_cam_info,
         const ImageConstPtr &northwest_img, const ImageConstPtr &southwest_img) {
    if (!coord_calib) {
        _coord_calib(northwest_array, southwest_array);
        coord_calib = true;
        cout << "坐标系标定完成" << endl;
    }

    actionlib::SimpleActionClient<darknet_ros_msgs::CheckForObjectsAction> northwest_client("/darknet_ros/check_for_objects",
                                                                                  true);
    // wait for the action server to start
    northwest_client.waitForServer(); //will wait for infinite time
    // send a goal to the action
    darknet_ros_msgs::CheckForObjectsActionGoal goal_northwest;
    goal_northwest.goal.id = 0;
    goal_northwest.goal.image = *northwest_img;
    northwest_client.sendGoal(goal_northwest.goal);
    northwest_client.waitForResult(ros::Duration(5.0));
    auto bboxs_northwest = northwest_client.getResult()->bounding_boxes.bounding_boxes;

    actionlib::SimpleActionClient<darknet_ros_msgs::CheckForObjectsAction> southwest_client("/darknet_ros/check_for_objects",
                                                                                            true);
    // wait for the action server to start
    southwest_client.waitForServer(); //will wait for infinite time
    darknet_ros_msgs::CheckForObjectsActionGoal goal_southwest;
    goal_southwest.goal.id = 1;
    goal_southwest.goal.image = *southwest_img;
    southwest_client.sendGoal(goal_southwest.goal);
    southwest_client.waitForResult(ros::Duration(5.0));
    auto bboxs_southwest = southwest_client.getResult()->bounding_boxes.bounding_boxes;

    vector<darknet_ros_msgs::BoundingBox> persons;
    for (const auto &bbox : bboxs_northwest) {
        if (string(bbox.Class) == "person") {
            persons.push_back(bbox);
            break;
        }
    }
    for (const auto &bbox : bboxs_southwest) {
        if (string(bbox.Class) == "person") {
            persons.push_back(bbox);
            break;
        }
    }
    if (persons.size() < 2)
        return;

//    cout << persons[0].xmin << " " << persons[0].xmax << " " << persons[0].ymin << " " << persons[0].ymax << persons[0].probability << endl;
//    cout << persons[1].xmin << " " << persons[1].xmax << " " << persons[1].ymin << " " << persons[1].ymax << persons[1].probability << endl << endl;

    cv_bridge::CvImagePtr northwest_cv_ptr = cv_bridge::toCvCopy(northwest_img, sensor_msgs::image_encodings::BGR8);
    cv_bridge::CvImagePtr southwest_cv_ptr = cv_bridge::toCvCopy(southwest_img, sensor_msgs::image_encodings::BGR8);

    cv::Rect northwest_ROI(cv::Point2i(persons[1].xmin, persons[1].ymin), cv::Point2i(persons[1].xmax, persons[1].ymax));
    cv::Mat northwest_croppedImage = northwest_cv_ptr->image(northwest_ROI);


    cv::Rect southwest_ROI(cv::Point2i(persons[0].xmin, persons[0].ymin), cv::Point2i(persons[0].xmax, persons[0].ymax));
    cv::Mat southwest_croppedImage = southwest_cv_ptr->image(southwest_ROI);

//    cv::rectangle(northwest_cv_ptr->image, northwest_ROI, cv::Scalar(255, 0, 0));
//    cv::imshow("v1", northwest_cv_ptr->image);
//    cv::waitKey(1000);
//
//    cv::rectangle(southwest_cv_ptr->image, southwest_ROI, cv::Scalar(255, 0, 0));
//    cv::imshow("v1", southwest_cv_ptr->image);
//    cv::waitKey(1000);

//    cout << northwest_croppedImage.size() << " " << southwest_croppedImage.size() << endl;


//    vector<cv::KeyPoint> keypoints_1, keypoints_2;
//    vector<cv::DMatch> matches;
//    find_feature_matches(northwest_cv_ptr->image, southwest_cv_ptr->image,
//                         keypoints_1, keypoints_2, matches);
//    cout << "一共找到了" << matches.size() << "组匹配点" << endl;

    cv::Mat T1 = Eigen2Mat(T_tag2northwest);
    cv::Mat T2 = Eigen2Mat(T_tag2southwest);

    vector<cv::Point2f> pts_1, pts_2;
    pts_1.push_back(pixel2cam(persons[1], northwest_cam_info));
    pts_2.push_back(pixel2cam(persons[0], southwest_cam_info));

//    for (cv::DMatch m : matches) {
//        // 将像素坐标转换至相机坐标
//        auto pt0 = keypoints_1[m.queryIdx].pt;
//        auto pt1 = keypoints_2[m.queryIdx].pt;
//        if (pt0.x <= persons[0].xmax && pt0.x >= persons[0].xmin && pt0.y <= persons[0].ymax && pt0.y >= persons[0].ymin &&
//        pt1.x <= persons[1].xmax && pt1.x >= persons[1].xmin && pt1.y <= persons[1].ymax && pt1.y >= persons[1].ymin) {
//            pts_2.push_back(pixel2cam(keypoints_2[m.trainIdx].pt, southwest_cam_info));
//            pts_1.push_back(pixel2cam(keypoints_1[m.queryIdx].pt, northwest_cam_info));
//        }
//    }

    cout << pts_1.size() << " " << pts_2.size() << endl;

    cv::Mat pts_4d;
    cv::triangulatePoints(T1, T2, pts_1, pts_2, pts_4d);

    // 转换成非齐次坐标
    cv::Mat P = pts_4d.col(0);
    P /= P.at<float>(3, 0); // 归一化
    cout << "检测对象的三位坐标："
    << P.at<float>(0, 0) << ", " << P.at<float>(1, 0) << ", " << P.at<float>(2, 0) << endl;

    visualization_msgs::Marker marker;
    marker.header.frame_id = "/AprilTag";
    marker.header.stamp = ros::Time::now();
    marker.id = 0;
    marker.type = visualization_msgs::Marker::CUBE;
    marker.action = visualization_msgs::Marker::ADD;
    marker.pose.position.x = P.at<float>(0, 0);
    marker.pose.position.y = P.at<float>(1, 0);
    marker.pose.position.z = P.at<float>(2, 0);
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    marker.scale.x = 1.0;
    marker.scale.y = 1.0;
    marker.scale.z = 2.0;
    marker.color.r = 0.0f;
    marker.color.g = 1.0f;
    marker.color.b = 0.0f;
    marker.color.a = 1.0;
    marker.lifetime = ros::Duration();
    marker_pub.publish(marker);
}


int main(int argc, char **argv) {
    ros::init(argc, argv, "tracker");
    ros::NodeHandle node;

    coord_calib = false;
    marker_pub = node.advertise<visualization_msgs::Marker>("/object", 1);

    message_filters::Subscriber<AprilTagDetectionArray> northwest_tag_detections_sub(node, "/northwest/tag_detections",
                                                                                     1);
    message_filters::Subscriber<AprilTagDetectionArray> southwest_tag_detections_sub(node, "/southwest/tag_detections",
                                                                                     1);
    message_filters::Subscriber<CameraInfo> northwest_camera_info_sub(node, "/northwest/webcam/camera_info", 1);
    message_filters::Subscriber<CameraInfo> southwest_camera_info_sub(node, "/southwest/webcam/camera_info", 1);
    message_filters::Subscriber<Image> northwest_img_sub(node, "/northwest/webcam/image_rect_color", 1);
    message_filters::Subscriber<Image> southwest_img_sub(node, "/southwest/webcam/image_rect_color", 1);

    typedef sync_policies::ApproximateTime<AprilTagDetectionArray, AprilTagDetectionArray, CameraInfo, CameraInfo, Image, Image> SyncPolicy;
    // ApproximateTime takes a queue size as its constructor argument, hence SyncPolicy(10)
    Synchronizer<SyncPolicy> sync(SyncPolicy(10), northwest_tag_detections_sub, southwest_tag_detections_sub,
                                  northwest_camera_info_sub, southwest_camera_info_sub, northwest_img_sub,
                                  southwest_img_sub);
    sync.registerCallback(boost::bind(&callback, _1, _2, _3, _4, _5, _6));

    ros::spin();

    return 0;
};
