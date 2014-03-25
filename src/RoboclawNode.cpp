#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/Twist.h>
#include <diagnostic_msgs/DiagnosticArray.h>
#include "Roboclaw.h"

#define address 0x80


class RoboclawNode{
public:
    RoboclawNode(): nh(), priv_nh("~"){
        priv_nh.param<std::string>("port", port, "/dev/ttyACM0");
        priv_nh.param<std::string>("base_frame_id", base_frame_id, "base_link");
        if(!priv_nh.getParam("baud_rate", baud_rate)){
            baud_rate = 38400;
        }
        if(!priv_nh.getParam("rate", update_rate)){
            baud_rate = 30;
        }
        if(!priv_nh.getParam("base_width", base_width)){
            baud_rate = 0.5;
        }
        if(!priv_nh.getParam("ticks_per_metre", ticks_per_m)){
            baud_rate = 100;
        }
        if(!priv_nh.getParam("KP", KP)){
            KP = 0.1;
        }
        if(!priv_nh.getParam("KI", KI)){
            KI = 0.5;
        }
        if(!priv_nh.getParam("KD", KD)){
            KD = 0.25;
        }
        if(!priv_nh.getParam("QPPS", QPPS)){
            QPPS = 1000;
        }

        ROS_INFO_STREAM("Starting roboclaw node with params:");
        ROS_INFO_STREAM("Base Width:\t" << base_width);
        ROS_INFO_STREAM("Ticks Per Metre:\t" << ticks_per_m);
        ROS_INFO_STREAM("KP:\t" << KP);
        ROS_INFO_STREAM("KI:\t" << KI);
        ROS_INFO_STREAM("KD:\t:" << KD);
        ROS_INFO_STREAM("QPPS:\t" << QPPS);

        claw = new Roboclaw(port, baud_rate, address);
        last_motor = ros::Time::now();

        x = y = theta = 0.0;
        last_enc_left = last_enc_right = 0;
        last_odom  = ros::Time::now();

        quaternion.x = 0.0;
        quaternion.y = 0.0;

        odom.header.frame_id = "odom";
        odom.child_frame_id = base_frame_id;
        odom.pose.pose.position.z = 0.0;

        cmd_vel_sub = nh.subscribe("cmd_vel", 10, &RoboclawNode::twistCb, this);
        diag_pub = nh.advertise<diagnostic_msgs::DiagnosticArray>("diagnostics", 10);
        odom_pub = nh.advertise<nav_msgs::Odometry>("odom", 10);

        claw->SetM1VelocityPID(KD,KP,KI,QPPS);
        claw->SetM2VelocityPID(KD,KP,KI,QPPS);
        claw->ResetEncoders();

        roboclaw_version = claw->ReadVersion();

        ROS_INFO_STREAM("Connected to: " << roboclaw_version);
   }

    void upateOdom(){

        // calcuate time elapsed since last update
        ros::Time now = ros::Time::now();
        ros::Duration elapsed_t = now - last_odom;
        last_odom = now;
        double elapsed = elapsed_t.toSec();

        uint8_t status;
        bool valid1, valid2;

        // read the encoder counts
        long encoder_left = claw->ReadEncoderM1(status, valid1);
        long encoder_right = claw->ReadEncoderM2(status, valid2);

        if (!valid1 || !valid2){
            ROS_WARN_STREAM("Invalid encoder count reading");
            return;
        }

        // calculate distance travelled by each wheel
        double dist_left = (float)(encoder_left - last_enc_left) / ticks_per_m;
        double dist_right = (float)(encoder_right - last_enc_right) / ticks_per_m;

        last_enc_left = encoder_left;
        last_enc_right = encoder_right;

        // distance robot has travelled is the average of the distance travelled by both
        // wheels
        double dist_travelled = (dist_left + dist_right) / 2;
        // calculate appoximate heading change (radians), this works for small angles
        double delta_th = (dist_right - dist_left) / base_width;
        double vx = dist_travelled / elapsed;
        double vth = delta_th / elapsed;

        if (dist_travelled != 0){
            double delta_x = cos(delta_th) * dist_travelled;
            double delta_y = -sin(delta_th) * dist_travelled;

            x += (cos(theta) * delta_x - sin(theta) * delta_y);
            y += (sin(theta) * delta_x + cos(theta) * delta_y);
        }

        if (delta_th != 0){
            theta += delta_th;
        }

        tf::Transform transform;
        transform.setOrigin(tf::Vector3(x,y,0.0));
        tf::Quaternion q;
        q.setRPY(0.0,0.0,theta);
        transform.setRotation(q);
        br.sendTransform(tf::StampedTransform(transform, now, "odom", base_frame_id));

        odom.header.stamp = now;
        odom.pose.pose.position.x = x;
        odom.pose.pose.position.y = y;
        odom.pose.pose.orientation.x = odom.pose.pose.orientation.y = 0;
        odom.pose.pose.orientation.z = sin(theta/2);
        odom.pose.pose.orientation.w = cos(theta/2);
        odom.twist.twist.linear.x = vx;
        odom.twist.twist.angular.z = vth;
        odom_pub.publish(odom);

    }

    void twistCb(geometry_msgs::Twist msg){
        last_motor = ros::Time::now();
        double lin = msg.linear.x;
        double ang = msg.angular.z;
        double left = 1.0 * lin - ang * base_width / 2.0;
        double right = 1.0 * lin + ang * base_width / 2.0;
        left *= ticks_per_m;
        right *= ticks_per_m;

        claw->SetMixedSpeed(left, right);
    }

    void shutdown(){
        claw->SetMixedSpeed(0, 0);
    }

    void spin(){
        ros::Rate r(update_rate);
        while (ros::ok()){
            ros::spinOnce();
            this->upateOdom();
            r.sleep();
        }
        this->shutdown();
    }


private:
    ros::NodeHandle nh, priv_nh;
    ros::Subscriber cmd_vel_sub;
    ros::Publisher odom_pub, diag_pub;
    std::string port;
    int baud_rate;
    int update_rate;
    double base_width;
    double ticks_per_m;
    double KP;
    double KI;
    double KD;
    int QPPS;
    std::string base_frame_id;
    ros::Time last_motor;
    Roboclaw* claw;

    double x, y, theta;
    long last_enc_left, last_enc_right;
    ros::Time last_odom;

    nav_msgs::Odometry odom;
    geometry_msgs::Quaternion quaternion;

    ros::Time last_diag;
    std::string roboclaw_version;

    tf::TransformBroadcaster br;

};


int main(int argc, char **argv){
	
	ros::init(argc, argv, "roboclaw_driver");
    RoboclawNode node;
    node.spin();
    /*ros::Time::init();
    ROS_INFO_STREAM("starting node");
    Roboclaw myclaw(std::string("/dev/ttyUSB0"), 38400, address);
    ROS_INFO_STREAM("version: " << myclaw.ReadVersion());
    uint8_t status;
    bool valid;
    myclaw.SetM1VelocityPID(0.25,0.1,0.5,850);
    myclaw.SetM2VelocityPID(0.25,0.1,0.5,850);

    ROS_INFO_STREAM("battery: " << (float)myclaw.ReadMainBatteryVoltage(valid)/10);
    uint16_t temp;
    myclaw.ReadTemperature(temp);

    ROS_INFO_STREAM("temp: " << (float)temp/10.0);
    ROS_INFO_STREAM("m1m2 encoder: " << myclaw.ReadEncoderM1(status, valid) << " " << myclaw.ReadEncoderM2(status, valid));

    myclaw.SetMixedSpeed(500, 500);
    uint8_t current1, current2;
    if (myclaw.ReadCurrents(current1, current2)){
        ROS_INFO_STREAM("m1m2 currents: " << current1 << " " << current2);
    }

    ros::Duration time(0.5);
    time.sleep();
    ROS_INFO_STREAM("m1m2 encoder: " << myclaw.ReadSpeedM1(status, valid) << " " << myclaw.ReadSpeedM2(status, valid));
    time.sleep();
    myclaw.SetMixedSpeed(0, 0);*/


}
