#include <ros/ros.h>
#include <signal.h>

#include <robowflex_library/robowflex.h>

using namespace robowflex;

int main(int argc, char **argv)
{
    startROS(argc, argv);

    Robot ur5("ur5");
    ur5.initialize("package://ur_description/urdf/ur5_robotiq_robot_limited.urdf.xacro",  // urdf
                   "package://ur5_robotiq85_moveit_config/config/ur5_robotiq85.srdf",     // srdf
                   "package://ur5_robotiq85_moveit_config/config/joint_limits.yaml",      // joint limits
                   "package://ur5_robotiq85_moveit_config/config/kinematics.yaml"         // kinematics
    );

    Scene scene(ur5);

    OMPL::OMPLPipelinePlanner planner(ur5);
    planner.initialize("package://ur5_robotiq85_moveit_config/config/ompl_planning.yaml"  // planner config
    );

    MotionRequestBuilder joint_request(planner, "manipulator");
    joint_request.setStartConfiguration({0.0677, -0.8235, 0.9860, -0.1624, 0.0678, 0.0});
    joint_request.setGoalConfiguration({-0.39, -0.69, -2.12, 2.82, -0.39, 0});

    MotionRequestBuilder pose_request(planner, "manipulator");
    pose_request.setStartConfiguration({0.0677, -0.8235, 0.9860, -0.1624, 0.0678, 0.0});

    Eigen::Affine3d pose = Eigen::Affine3d::Identity();
    pose.translate(Eigen::Vector3d{-0.268, -0.826, 1.313});
    Eigen::Quaterniond orn{0, 0, 1, 0};

    pose_request.setGoalRegion("ee_link", "world",                                         // links
                               pose, Geometry(Geometry::ShapeType::SPHERE, {0.01, 0, 0}),  // position
                               orn, {0.01, 0.01, 0.01}                                     // orientation
    );

    ur5.loadKinematics("manipulator");

    Benchmarker benchmark;
    benchmark.addBenchmarkingRequest("joint", scene, planner, joint_request);
    benchmark.addBenchmarkingRequest("pose", scene, planner, pose_request);

    OMPLBenchmarkOutputter out("robowflex_ur5_test/");
    benchmark.benchmark(out);

    return 0;
}