/* Author: Zachary Kingston */

#include <deque>
#include <numeric>

#include <urdf_parser/urdf_parser.h>

#include <moveit/collision_detection/collision_common.h>
#include <moveit/robot_state/conversions.h>
#include <moveit/robot_state/robot_state.h>

#include <robowflex_library/geometry.h>
#include <robowflex_library/io.h>
#include <robowflex_library/io/yaml.h>
#include <robowflex_library/macros.h>
#include <robowflex_library/robot.h>
#include <robowflex_library/scene.h>
#include <robowflex_library/tf.h>

using namespace robowflex;

const std::string Robot::ROBOT_DESCRIPTION = "robot_description";
const std::string Robot::ROBOT_SEMANTIC = "_semantic";
const std::string Robot::ROBOT_PLANNING = "_planning";
const std::string Robot::ROBOT_KINEMATICS = "_kinematics";

namespace
{
    std::pair<bool, geometry_msgs::Pose> sampleInVolume(const GeometryConstPtr &geometry,
                                                        const RobotPose &pose,
                                                        const Eigen::Quaterniond &orientation,
                                                        const Eigen::Vector3d &tolerances)
    {
        geometry_msgs::Pose msg;

        auto point = geometry->sample();
        if (point.first)
        {
            RobotPose sampled_pose = pose;
            sampled_pose.translate(point.second);
            sampled_pose.rotate(TF::sampleOrientation(orientation, tolerances));

            msg = TF::poseEigenToMsg(sampled_pose);
        }

        return std::make_pair(point.first, msg);
    }
}  // namespace

Robot::Robot(const std::string &name) : name_(name), handler_(name_)
{
}

bool Robot::initialize(const std::string &urdf_file, const std::string &srdf_file,
                       const std::string &limits_file, const std::string &kinematics_file)
{
    if (!loadRobotDescription(urdf_file, srdf_file, limits_file, kinematics_file))
        return false;

    loadRobotModel();

    return true;
}

bool Robot::loadRobotDescription(const std::string &urdf_file, const std::string &srdf_file,
                                 const std::string &limits_file, const std::string &kinematics_file)
{
    bool success =
        loadXMLFile(ROBOT_DESCRIPTION, urdf_file, urdf_function_)                           // urdf
        && loadXMLFile(ROBOT_DESCRIPTION + ROBOT_SEMANTIC, srdf_file, srdf_function_)       // srdf
        && loadYAMLFile(ROBOT_DESCRIPTION + ROBOT_PLANNING, limits_file, limits_function_)  // joint limits
        && loadYAMLFile(ROBOT_DESCRIPTION + ROBOT_KINEMATICS, kinematics_file, kinematics_function_);

    return success;
}

void Robot::setURDFPostProcessFunction(const PostProcessXMLFunction &function)
{
    urdf_function_ = function;
}

bool Robot::isLinkURDF(tinyxml2::XMLDocument &doc, const std::string &name)
{
    auto node = doc.FirstChildElement("robot")->FirstChildElement("link");
    while (node != nullptr)
    {
        if (node->Attribute("name", name.c_str()))
            return true;

        node = node->NextSiblingElement("link");
    }
    return false;
}

void Robot::setSRDFPostProcessFunction(const PostProcessXMLFunction &function)
{
    srdf_function_ = function;
}

void Robot::setLimitsPostProcessFunction(const PostProcessYAMLFunction &function)
{
    limits_function_ = function;
}

void Robot::setKinematicsPostProcessFunction(const PostProcessYAMLFunction &function)
{
    kinematics_function_ = function;
}

bool Robot::loadYAMLFile(const std::string &name, const std::string &file)
{
    PostProcessYAMLFunction function;
    return loadYAMLFile(name, file, function);
}

bool Robot::loadYAMLFile(const std::string &name, const std::string &file,
                         const PostProcessYAMLFunction &function)
{
    auto &yaml = IO::loadFileToYAML(file);
    if (!yaml.first)
    {
        ROS_ERROR("Failed to load YAML file `%s`.", file.c_str());
        return false;
    }

    if (function)
    {
        YAML::Node copy = yaml.second;
        if (!function(copy))
        {
            ROS_ERROR("Failed to process YAML file `%s`.", file.c_str());
            return false;
        }

        handler_.loadYAMLtoROS(copy, name);
    }
    else
        handler_.loadYAMLtoROS(yaml.second, name);

    return true;
}

bool Robot::loadXMLFile(const std::string &name, const std::string &file)
{
    PostProcessXMLFunction function;
    return loadXMLFile(name, file, function);
}

bool Robot::loadXMLFile(const std::string &name, const std::string &file,
                        const PostProcessXMLFunction &function)
{
    const std::string string = IO::loadXMLToString(file);
    if (string.empty())
    {
        ROS_ERROR("Failed to load XML file `%s`.", file.c_str());
        return false;
    }

    if (function)
    {
        tinyxml2::XMLDocument doc;
        doc.Parse(string.c_str());

        if (!function(doc))
        {
            ROS_ERROR("Failed to process XML file `%s`.", file.c_str());
            return false;
        }

        tinyxml2::XMLPrinter printer;
        doc.Print(&printer);

        handler_.setParam(name, std::string(printer.CStr()));
    }
    else
        handler_.setParam(name, string);

    return true;
}

void Robot::loadRobotModel(bool namespaced)
{
    std::string description = ((namespaced) ? handler_.getNamespace() : "") + "/" + ROBOT_DESCRIPTION;

    robot_model_loader::RobotModelLoader::Options options(description);
    options.load_kinematics_solvers_ = false;

    loader_.reset(new robot_model_loader::RobotModelLoader(options));
    kinematics_.reset(new kinematics_plugin_loader::KinematicsPluginLoader(loader_->getRobotDescription()));

    model_ = loader_->getModel();
    scratch_.reset(new robot_state::RobotState(model_));
    scratch_->setToDefaultValues();

    handler_.getParam("/" + description, urdf_);
    handler_.getParam("/" + description + ROBOT_SEMANTIC, srdf_);
}

bool Robot::loadKinematics(const std::string &name)
{
    if (imap_.find(name) != imap_.end())
        return true;

    robot_model::SolverAllocatorFn allocator = kinematics_->getLoaderFunction(loader_->getSRDF());

    const auto &groups = kinematics_->getKnownGroups();
    if (groups.empty())
    {
        ROS_WARN("No kinematics plugins defined. Fill and load kinematics.yaml!");
        return false;
    }

    if (!model_->hasJointModelGroup(name) || std::find(groups.begin(), groups.end(), name) == groups.end())
    {
        ROS_WARN("No JMG or Kinematics defined for `%s`!", name.c_str());
        return false;
    }

    robot_model::JointModelGroup *jmg = model_->getJointModelGroup(name);
    kinematics::KinematicsBasePtr solver = allocator(jmg);

    if (solver)
    {
        std::string error_msg;
        if (solver->supportsGroup(jmg, &error_msg))
            imap_[name] = allocator;

        else
        {
            ROS_ERROR("Kinematics solver %s does not support joint group %s.  Error: %s",
                      typeid(*solver).name(), name.c_str(), error_msg.c_str());
            return false;
        }
    }
    else
    {
        ROS_ERROR("Kinematics solver could not be instantiated for joint group `%s`.", name.c_str());
        return false;
    }

    auto timeout = kinematics_->getIKTimeout();
    // auto attempts = kinematics_->getIKAttempts();

    jmg->setDefaultIKTimeout(timeout[name]);
    // jmg->setDefaultIKAttempts(attempts[name]);

    model_->setKinematicsAllocators(imap_);

    return true;
}

const std::string &Robot::getModelName() const
{
    return model_->getName();
}

const std::string &Robot::getName() const
{
    return name_;
}

const robot_model::RobotModelPtr &Robot::getModelConst() const
{
    return model_;
}

robot_model::RobotModelPtr &Robot::getModel()
{
    return model_;
}

urdf::ModelInterfaceConstSharedPtr Robot::getURDF() const
{
    return model_->getURDF();
}

const std::string &Robot::getURDFString() const
{
    return urdf_;
}

srdf::ModelConstSharedPtr Robot::getSRDF() const
{
    return model_->getSRDF();
}

const std::string &Robot::getSRDFString() const
{
    return srdf_;
}

const robot_model::RobotStatePtr &Robot::getScratchStateConst() const
{
    return scratch_;
}

robot_model::RobotStatePtr &Robot::getScratchState()
{
    return scratch_;
}

const IO::Handler &Robot::getHandlerConst() const
{
    return handler_;
}

IO::Handler &Robot::getHandler()
{
    return handler_;
}

void Robot::setState(const std::vector<double> &positions)
{
    scratch_->setVariablePositions(positions);
    scratch_->update();
}

void Robot::setState(const std::map<std::string, double> &variable_map)
{
    scratch_->setVariablePositions(variable_map);
    scratch_->update();
}

void Robot::setState(const std::vector<std::string> &variable_names,
                     const std::vector<double> &variable_position)
{
    scratch_->setVariablePositions(variable_names, variable_position);
    scratch_->update();
}

void Robot::setState(const moveit_msgs::RobotState &state)
{
    moveit::core::robotStateMsgToRobotState(state, *scratch_);
    scratch_->update();
}

void Robot::setStateFromYAMLFile(const std::string &file)
{
    moveit_msgs::RobotState state;
    IO::fromYAMLFile(state, file);

    setState(state);
}

void Robot::setGroupState(const std::string &name, const std::vector<double> &positions)
{
    scratch_->setJointGroupPositions(name, positions);
    scratch_->update();
}

std::vector<double> Robot::getState() const
{
    const double *positions = scratch_->getVariablePositions();
    std::vector<double> state(positions, positions + scratch_->getVariableCount());
    return state;
}

std::vector<std::string> Robot::getJointNames() const
{
    return scratch_->getVariableNames();
}

bool Robot::hasJoint(const std::string &joint) const
{
    const auto &joint_names = getJointNames();
    return (std::find(joint_names.begin(), joint_names.end(), joint) != joint_names.end());
}

void Robot::setIKAttempts(unsigned int attempts)
{
    ik_attempts_ = attempts;
}

bool Robot::setFromIK(const std::string &group,                               //
                      const GeometryConstPtr &region, const RobotPose &pose,  //
                      const Eigen::Quaterniond &orientation, const Eigen::Vector3d &tolerances)
{
    robot_model::JointModelGroup *jmg = model_->getJointModelGroup(group);

    for (unsigned int i = 0; i < ik_attempts_; ++i)
    {
        auto sampled = sampleInVolume(region, pose, orientation, tolerances);
        if (!sampled.first)
            continue;

        if (scratch_->setFromIK(jmg, sampled.second, 1, 0.))
        {
            scratch_->update();
            return true;
        }
    }

    return false;
}

bool Robot::setFromIKCollisionAware(const ScenePtr &scene, const std::string &group,
                                    const GeometryConstPtr &region, const RobotPose &pose,
                                    const Eigen::Quaterniond &orientation, const Eigen::Vector3d &tolerances,
                                    bool verbose)
{
    robot_model::JointModelGroup *jmg = model_->getJointModelGroup(group);

    const auto &gsvcf =                                               //
        [&scene, &verbose](robot_state::RobotState *state,            //
                           const moveit::core::JointModelGroup *jmg,  //
                           const double *values)                      //
    {
        state->setJointGroupPositions(jmg, values);
        state->updateCollisionBodyTransforms();

        collision_detection::CollisionRequest request;
        request.verbose = verbose;

        auto result = scene->checkCollision(*state, request);
        return !result.collision;
    };

    for (unsigned int i = 0; i < ik_attempts_; ++i)
    {
        auto sampled = sampleInVolume(region, pose, orientation, tolerances);
        if (!sampled.first)
            continue;

        if (scratch_->setFromIK(jmg, sampled.second, 1, 0., gsvcf))
        {
            scratch_->update();
            return true;
        }
    }

    return false;
}

const RobotPose &Robot::getLinkTF(const std::string &name) const
{
    return scratch_->getGlobalLinkTransform(name);
}

const RobotPose Robot::getRelativeLinkTF(const std::string &base, const std::string &target) const
{
    auto base_tf = scratch_->getGlobalLinkTransform(base);
    auto target_tf = scratch_->getGlobalLinkTransform(target);

    return base_tf.inverse() * target_tf;
}

bool Robot::toYAMLFile(const std::string &file) const
{
    moveit_msgs::RobotState msg;
    moveit::core::robotStateToRobotStateMsg(*scratch_, msg);

    const auto &yaml = IO::toNode(msg);
    return IO::YAMLToFile(yaml, file);
}

robot_model::RobotStatePtr Robot::allocState() const
{
    // No make_shared() for indigo compatibility
    robot_state::RobotStatePtr state;
    state.reset(new robot_state::RobotState(getModelConst()));
    state->setToDefaultValues();

    return state;
}

namespace
{
    YAML::Node addLinkGeometry(const urdf::GeometrySharedPtr &geometry, bool resolve = true)
    {
        YAML::Node node;
        switch (geometry->type)
        {
            case urdf::Geometry::MESH:
            {
                const auto &mesh = static_cast<urdf::Mesh *>(geometry.get());
                node["type"] = "mesh";

                if (resolve)
                    node["resource"] = IO::resolvePath(mesh->filename);
                else
                    node["resource"] = mesh->filename;

                if (mesh->scale.x != 1 || mesh->scale.y != 1 || mesh->scale.z != 1)
                {
                    node["dimensions"] = std::vector<double>({mesh->scale.x, mesh->scale.y, mesh->scale.z});
                    ROBOWFLEX_YAML_FLOW(node["dimensions"]);
                }
                break;
            }
            case urdf::Geometry::BOX:
            {
                const auto &box = static_cast<urdf::Box *>(geometry.get());
                node["type"] = "box";
                node["dimensions"] = std::vector<double>({box->dim.x, box->dim.y, box->dim.z});
                ROBOWFLEX_YAML_FLOW(node["dimensions"]);
                break;
            }
            case urdf::Geometry::SPHERE:
            {
                const auto &sphere = static_cast<urdf::Sphere *>(geometry.get());
                node["type"] = "sphere";
                node["dimensions"] = std::vector<double>({sphere->radius});
                ROBOWFLEX_YAML_FLOW(node["dimensions"]);
                break;
            }
            case urdf::Geometry::CYLINDER:
            {
                const auto &cylinder = static_cast<urdf::Cylinder *>(geometry.get());
                node["type"] = "cylinder";
                node["dimensions"] = std::vector<double>({cylinder->length, cylinder->radius});
                ROBOWFLEX_YAML_FLOW(node["dimensions"]);
                break;
            }
            default:
                break;
        }

        return node;
    }

    void addLinkMaterial(YAML::Node &node, const urdf::MaterialSharedPtr &material)
    {
        node["color"] =
            std::vector<double>({material->color.r, material->color.g, material->color.b, material->color.a});

        ROBOWFLEX_YAML_FLOW(node["color"]);
        // node["texture"] = visual->texture_filename;
    }

    void addLinkOrigin(YAML::Node &node, const urdf::Pose &pose)
    {
        YAML::Node origin;
        origin["position"] = std::vector<double>({pose.position.x, pose.position.y, pose.position.z});
        origin["orientation"] =
            std::vector<double>({pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w});
        node["origin"] = origin;
        ROBOWFLEX_YAML_FLOW(node["origin"]);
    }

    YAML::Node addLinkVisual(const urdf::VisualSharedPtr &visual, bool resolve = true)
    {
        YAML::Node node;
        if (visual)
        {
            if (visual->geometry)
            {
                const auto &geometry = visual->geometry;
                node = addLinkGeometry(geometry, resolve);

                if (visual->material)
                {
                    const auto &material = visual->material;
                    addLinkMaterial(node, material);
                }

                const auto &pose = visual->origin;

                Eigen::Vector3d position(pose.position.x, pose.position.y, pose.position.z);
                Eigen::Quaterniond rotation(pose.rotation.w,  //
                                            pose.rotation.x, pose.rotation.y, pose.rotation.z);
                Eigen::Quaterniond identity = Eigen::Quaterniond::Identity();

                // TODO: Also check if rotation is not zero.
                if (position.norm() > 0 || rotation.angularDistance(identity) > 0)
                    addLinkOrigin(node, pose);
            }
        }

        return node;
    }

    YAML::Node addLinkCollision(const urdf::CollisionSharedPtr &collision)
    {
        YAML::Node node;
        if (collision)
        {
            if (collision->geometry)
            {
                const auto &geometry = collision->geometry;
                node = addLinkGeometry(geometry);
            }
        }

        return node;
    }

    RobotPose urdfPoseToEigen(const urdf::Pose &pose)
    {
        geometry_msgs::Pose msg;
        msg.position.x = pose.position.x;
        msg.position.y = pose.position.y;
        msg.position.z = pose.position.z;

        msg.orientation.x = pose.rotation.x;
        msg.orientation.y = pose.rotation.y;
        msg.orientation.z = pose.rotation.z;
        msg.orientation.w = pose.rotation.w;

        return TF::poseMsgToEigen(msg);
    }
}  // namespace

bool Robot::dumpGeometry(const std::string &filename) const
{
    YAML::Node link_geometry;
    const auto &urdf = model_->getURDF();

    std::vector<urdf::LinkSharedPtr> links;
    urdf->getLinks(links);

    for (const auto &link : links)
    {
        YAML::Node node;

        YAML::Node visual;
#if ROBOWFLEX_AT_LEAST_KINETIC
        for (const auto &element : link->visual_array)
            if (element)
                visual["elements"].push_back(addLinkVisual(element));
#else
        if (link->visual)
            visual["elements"].push_back(addLinkVisual(link->visual));
#endif

        YAML::Node collision;
#if ROBOWFLEX_AT_LEAST_KINETIC
        for (const auto &element : link->collision_array)
            if (element)
                collision["elements"].push_back(addLinkCollision(element));
#else
        if (link->collision)
            collision["elements"].push_back(addLinkCollision(link->collision));
#endif

        if (!visual.IsNull() || !collision.IsNull())
        {
            YAML::Node add;
            add["name"] = link->name;

            if (!visual.IsNull())
                add["visual"] = visual;

            if (!collision.IsNull())
                add["collision"] = collision;

            link_geometry.push_back(add);
        }
    }

    return IO::YAMLToFile(link_geometry, filename);
}

bool Robot::dumpTransforms(const std::string &filename) const
{
    robot_trajectory::RobotTrajectory trajectory(model_, "");
    trajectory.addSuffixWayPoint(scratch_, 0.0);

    return dumpPathTransforms(trajectory, filename, 0., 0.);
}

bool Robot::dumpPath(const robot_trajectory::RobotTrajectory &path, const std::string &filename) const
{
    moveit_msgs::RobotTrajectory traj;
    path.getRobotTrajectoryMsg(traj);

    auto node = robowflex::IO::toNode(traj);
    return IO::YAMLToFile(node, filename);
}

bool Robot::dumpPathTransforms(const robot_trajectory::RobotTrajectory &path, const std::string &filename,
                               double fps, double threshold) const
{
    YAML::Node node, values;
    const double rate = 1.0 / fps;

    // Find the total duration of the path.
    const std::deque<double> &durations = path.getWayPointDurations();
    double total_duration = std::accumulate(durations.begin(), durations.end(), 0.0);

    const auto &urdf = model_->getURDF();

    robot_state::RobotStatePtr previous, state(new robot_state::RobotState(model_));

    for (double duration = 0.0, delay = 0.0; duration <= total_duration; duration += rate, delay += rate)
    {
        YAML::Node point;

        path.getStateAtDurationFromStart(duration, state);
        if (previous && state->distance(*previous) < threshold)
            continue;

        state->update();
        for (const auto &link_name : model_->getLinkModelNames())
        {
            const auto &urdf_link = urdf->getLink(link_name);
            if (urdf_link->visual)
            {
                const auto &link = model_->getLinkModel(link_name);
                RobotPose tf =
                    state->getGlobalLinkTransform(link);  // * urdfPoseToEigen(urdf_link->visual->origin);
                point[link->getName()] = IO::toNode(TF::poseEigenToMsg(tf));
            }
        }

        YAML::Node value;
        value["point"] = point;
        value["duration"] = delay;
        values.push_back(value);

        delay = 0;

        if (!previous)
            previous.reset(new robot_state::RobotState(model_));

        *previous = *state;
    }

    node["transforms"] = values;
    node["fps"] = fps;

    return IO::YAMLToFile(node, filename);
}

bool Robot::dumpToScene(const std::string &filename) const
{
    YAML::Node node;

    const auto &urdf = model_->getURDF();
    for (const auto &link_name : model_->getLinkModelNames())
    {
        const auto &urdf_link = urdf->getLink(link_name);
        if (urdf_link->visual)
        {
            const auto &link = model_->getLinkModel(link_name);

            std::vector<YAML::Node> visuals;
#if ROBOWFLEX_AT_LEAST_KINETIC
            for (const auto &element : urdf_link->visual_array)
                if (element)
                    visuals.emplace_back(addLinkVisual(element, false));
#else
            if (urdf_link->visual)
                visuals.push_back(addLinkVisual(urdf_link->visual, false));
#endif

            YAML::Node object;
            object["id"] = link->getName();

            YAML::Node meshes;
            YAML::Node mesh_poses;
            YAML::Node primitives;
            YAML::Node primitive_poses;

            RobotPose tf = scratch_->getGlobalLinkTransform(link);
            for (const auto &visual : visuals)
            {
                RobotPose pose = tf;
                if (IO::isNode(visual["origin"]))
                {
                    RobotPose offset = TF::poseMsgToEigen(IO::poseFromNode(visual["origin"]));
                    pose = pose * offset;
                }

                const auto &posey = IO::toNode(TF::poseEigenToMsg(pose));

                const auto &type = visual["type"].as<std::string>();
                if (type == "mesh")
                {
                    YAML::Node mesh;
                    mesh["resource"] = visual["resource"];
                    if (IO::isNode(visual["dimensions"]))
                        mesh["dimensions"] = visual["dimensions"];
                    else
                    {
                        mesh["dimensions"].push_back(1.);
                        mesh["dimensions"].push_back(1.);
                        mesh["dimensions"].push_back(1.);
                    }

                    ROBOWFLEX_YAML_FLOW(mesh["dimensions"]);

                    meshes.push_back(mesh);
                    mesh_poses.push_back(posey);
                }
                else
                {
                    YAML::Node primitive;
                    primitive["type"] = type;
                    primitive["dimensions"] = visual["dimensions"];
                    primitive_poses.push_back(posey);
                }
            }

            if (meshes.size())
            {
                object["meshes"] = meshes;
                object["mesh_poses"] = mesh_poses;
            }

            if (primitives.size())
            {
                object["primitives"] = primitives;
                object["primitive_poses"] = primitive_poses;
            }

            node["collision_objects"].push_back(object);
        }
    }

    YAML::Node scene;
    scene["world"] = node;

    return IO::YAMLToFile(scene, filename);
}

///
/// robowflex::ParamRobot
///

ParamRobot::ParamRobot(const std::string &name) : Robot(name)
{
    loadRobotModel(false);
}
