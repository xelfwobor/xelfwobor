/* Author: Zachary Kingston */

#ifndef ROBOWFLEX_PLANNER_
#define ROBOWFLEX_PLANNER_

#include <moveit/planning_pipeline/planning_pipeline.h>

#include <robowflex_library/class_forward.h>
#include <robowflex_library/pool.h>
#include <robowflex_library/io/handler.h>
#include <robowflex_library/scene.h>

namespace robowflex
{
    /** \cond IGNORE */
    ROBOWFLEX_CLASS_FORWARD(Robot);
    ROBOWFLEX_CLASS_FORWARD(Geometry);
    /** \endcond */

    /** \cond IGNORE */
    ROBOWFLEX_CLASS_FORWARD(Planner);
    /** \endcond */

    /** \class robowflex::PlannerPtr
        \brief A shared pointer wrapper for robowflex::Planner. */

    /** \class robowflex::PlannerConstPtr
        \brief A const shared pointer wrapper for robowflex::Planner. */

    /** \brief An abstract interface to a motion planning algorithm.
     */
    class Planner
    {
    public:
        /** \brief A function that returns the value of a planner property over the course of a run.
         */
        using ProgressProperty = std::function<std::string()>;

        /** \brief Constructor.
         *  Takes in a \a robot description and an optional namespace \a name.
         *  If \a name is specified, planner parameters are namespaced under the namespace of \a robot.
         *  \param[in] robot The robot to plan for.
         *  \param[in] name Optional namespace for planner.
         */
        Planner(const RobotPtr &robot, const std::string &name = "");

        // non-copyable
        Planner(Planner const &) = delete;
        void operator=(Planner const &) = delete;

        /** \brief Plan a motion given a \a request and a \a scene.
         *  A virtual method that must be implemented by any robowflex::Planner.
         *  \param[in] scene A planning scene for the same \a robot_ to compute the plan in.
         *  \param[in] request The motion planning request to solve.
         *  \return The motion planning response generated by the planner.
         */
        virtual planning_interface::MotionPlanResponse
        plan(const SceneConstPtr &scene, const planning_interface::MotionPlanRequest &request) = 0;

        /** \brief Return all planner configurations offered by this planner.
         *  Any of the configurations returned can be set as the planner for a motion planning query sent to
         *  plan().
         *  \return A vector of strings of planner configuration names.
         */
        virtual std::vector<std::string> getPlannerConfigs() const = 0;

        /** \brief Retrieve the planner progress property map for this planner given a specific request.
         *  \param[in] scene A planning scene for the same \a robot_ to compute the plan in.
         *  \param[in] request Request to get progress properties for.
         *  \return The map of progress properties.
         */
        virtual std::map<std::string, ProgressProperty> getProgressProperties(
            const SceneConstPtr &scene, const planning_interface::MotionPlanRequest &request) const;

        /** \brief Return the robot for this planner.
         *  \return Get the robot associated with the planner.
         */
        const RobotPtr getRobot() const;

        /** \brief Get the name of the planner.
         *  \return The planner's name.
         */
        const std::string &getName() const;

        /** \brief This function is called before benchmarking.
         *  \param[in] scene Scene to plan for.
         *  \param[in] request Planning request.
         */
        virtual void preRun(const SceneConstPtr &scene, const planning_interface::MotionPlanRequest &request);

    protected:
        RobotPtr robot_;          ///< The robot to plan for.
        IO::Handler handler_;     ///< The parameter handler for the planner.
        const std::string name_;  ///< Namespace for the planner.
    };

    /** \brief A thread pool of planners \a P to service requests in a multi-threaded environment
     *  simultaneously.
     */
    class PoolPlanner : public Planner
    {
    public:
        /** \brief Constructor.
         *  Takes in a \a robot description and an optional namespace \a name.
         *  If \a name is specified, planner parameters are namespaced under the namespace of \a robot.
         *  \param[in] robot The robot to plan for.
         *  \param[in] n The number of threads to use. By default uses maximum available on the machine.
         *  \param[in] name Optional namespace for planner.
         */
        PoolPlanner(const RobotPtr &robot, unsigned int n = std::thread::hardware_concurrency(),
                    const std::string &name = "");

        // non-copyable
        PoolPlanner(PoolPlanner const &) = delete;
        void operator=(PoolPlanner const &) = delete;

        /** \brief Initialize the planner pool.
         *  Forwards template arguments \a Args to the initializer of the templated planner \a P. Assumes that
         *  the constructor of the planner takes \a robot_ and \a name_.
         *  \param[in] args Arguments to initializer of planner \a P.
         *  \tparam P The robowflex::Planner to pool.
         *  \tparam Args Argument types to initializer of planner \a P.
         *  \return True on success, false on failure.
         */
        template <typename P, typename... Args>
        bool initialize(Args &&... args)
        {
            for (unsigned int i = 0; i < pool_.getThreadCount(); ++i)
            {
                auto planner = std::make_shared<P>(robot_, name_);

                if (!planner->initialize(std::forward<Args>(args)...))
                    return false;

                planners_.emplace(std::move(planner));
            }

            return true;
        }

        /** \brief Submit a motion planning request job to the queue.
         *  \param[in] scene Planning scene to plane for.
         *  \param[in] request Motion plan request to service.
         *  \return Job that will service the planning request. This job can be canceled.
         */
        std::shared_ptr<Pool::Job<planning_interface::MotionPlanResponse>>
        submit(const SceneConstPtr &scene, const planning_interface::MotionPlanRequest &request);

        /** \brief Plan a motion given a \a request and a \a scene.
         *  Forwards the planning request onto the thread pool to be executed. Blocks until complete and
         *  returns result.
         *  \param[in] scene A planning scene for the same \a robot_ to compute the plan in.
         *  \param[in] request The motion planning request to solve.
         *  \return The motion planning response generated by the planner.
         */
        planning_interface::MotionPlanResponse
        plan(const SceneConstPtr &scene, const planning_interface::MotionPlanRequest &request) override;

        std::vector<std::string> getPlannerConfigs() const override;

    private:
        Pool pool_;  ///< Thread pool

        std::queue<PlannerPtr> planners_;  ///< Motion planners
        std::mutex mutex_;                 ///< Planner mutex
        std::condition_variable cv_;       ///< Planner condition variable
    };

    /** \cond IGNORE */
    ROBOWFLEX_CLASS_FORWARD(PipelinePlanner);
    /** \endcond */

    /** \class robowflex::PipelinePlannerPtr
        \brief A shared pointer wrapper for robowflex::PipelinePlanner. */

    /** \class robowflex::PipelinePlannerConstPtr
        \brief A const shared pointer wrapper for robowflex::PipelinePlanner. */

    /** \brief A motion planner that uses the \a MoveIt! planning pipeline to load a planner plugin.
     */
    class PipelinePlanner : public Planner
    {
    public:
        /** \brief Constructor.
         */
        PipelinePlanner(const RobotPtr &robot, const std::string &name = "");

        // non-copyable
        PipelinePlanner(PipelinePlanner const &) = delete;
        void operator=(PipelinePlanner const &) = delete;

        /** \brief Plan a motion given a \a request and a \a scene.
         *  Uses the planning pipeline's generatePlan() method, which goes through planning adapters.
         *  \param[in] scene A planning scene for the same \a robot_ to compute the plan in.
         *  \param[in] request The motion planning request to solve.
         *  \return The motion planning response generated by the planner.
         */
        planning_interface::MotionPlanResponse
        plan(const SceneConstPtr &scene, const planning_interface::MotionPlanRequest &request) override;

        /** \brief Retrieve planning context and dynamically cast to desired type from planning pipeline.
         *  \param[in] scene A planning scene for the same \a robot_ to compute the plan in.
         *  \param[in] request The motion planning request to solve.
         *  \tparam T Type of underlying planning context.
         *  \return The casted context for the motion planning request. On failure, nullptr.
         */
        template <typename T>
        std::shared_ptr<T> extractPlanningContext(const SceneConstPtr &scene,
                                                  const planning_interface::MotionPlanRequest &request) const
        {
            if (not pipeline_)
                return nullptr;

            auto pc = pipeline_->getPlannerManager()->getPlanningContext(scene->getSceneConst(), request);
            return std::dynamic_pointer_cast<T>(pc);
        }

    protected:
        planning_pipeline::PlanningPipelinePtr pipeline_;  ///< Loaded planning pipeline plugin.
    };

    /** \brief OMPL specific planners and features.
     */
    namespace OMPL
    {
        /** \brief Loads an OMPL configuration YAML file onto the parameter server.
         */
        bool loadOMPLConfig(IO::Handler &handler, const std::string &config_file,
                            std::vector<std::string> &configs);

        /** \brief Settings descriptor for settings provided by the default \a MoveIt! OMPL planning pipeline.
         */
        class Settings
        {
        public:
            /** \brief Constructor.
             *  Initialized here so an empty class can be used as default arguments in a function.
             */
            Settings();

            int max_goal_samples;                   ///< Maximum number of successful goal samples to keep.
            int max_goal_sampling_attempts;         ///< Maximum number of attempts to sample a goal.
            int max_planning_threads;               ///< Maximum number of threads used to service a request.
            double max_solution_segment_length;     ///< Maximum solution segment length.
            int max_state_sampling_attempts;        ///< Maximum number of attempts to sample a new state.
            int minimum_waypoint_count;             ///< Minimum number of waypoints in output path.
            bool simplify_solutions;                ///< Whether or not planner should simplify solutions.
            bool use_constraints_approximations;    ///< Absolute silliness.
            bool display_random_valid_states;       ///< N/A, defunct.
            std::string link_for_exploration_tree;  ///< N/A, defunct.
            double maximum_waypoint_distance;       ///< Maximum distance between waypoints in path.

            /** \brief Sets member variables on the parameter server using \a handler.
             */
            void setParam(IO::Handler &handler) const;
        };

        /** \cond IGNORE */
        ROBOWFLEX_CLASS_FORWARD(OMPLPipelinePlanner);
        /** \endcond */

        /** \class robowflex::OMPL::OMPLPipelinePlannerPtr
            \brief A shared pointer wrapper for robowflex::OMPL::OMPLPipelinePlanner. */

        /** \class robowflex::OMPL::OMPLPipelinePlannerConstPtr
            \brief A const shared pointer wrapper for robowflex::OMPL::OMPLPipelinePlanner. */

        /** \brief A robowflex::PipelinePlanner that uses the \a MoveIt! default OMPL planning pipeline.
         */
        class OMPLPipelinePlanner : public PipelinePlanner
        {
        public:
            OMPLPipelinePlanner(const RobotPtr &robot, const std::string &name = "");

            // non-copyable
            OMPLPipelinePlanner(OMPLPipelinePlanner const &) = delete;
            void operator=(OMPLPipelinePlanner const &) = delete;

            /** \brief Initialize planning pipeline.
             *  Loads OMPL planning plugin \a plugin with the planning adapters \a adapters. Parameters are
             *  set on the parameter server from \a settings and planning configurations are loaded from the
             *  YAML file \a config_file.
             *  \param[in] config_file A YAML file containing OMPL planner configurations.
             *  \param[in] settings Settings to set on the parameter server.
             *  \param[in] plugin Planning plugin to load.
             *  \param[in] adapters Planning adapters to load.
             *  \return True upon success, false on failure.
             */
            bool initialize(const std::string &config_file = "", const Settings &settings = Settings(),
                            const std::string &plugin = DEFAULT_PLUGIN,
                            const std::vector<std::string> &adapters = DEFAULT_ADAPTERS);

            std::vector<std::string> getPlannerConfigs() const override;

        protected:
            static const std::string DEFAULT_PLUGIN;                 ///< The default OMPL plugin.
            static const std::vector<std::string> DEFAULT_ADAPTERS;  ///< The default planning adapters.

        private:
            std::vector<std::string> configs_;  ///< Planning configurations loaded from \a config_file in
                                                ///< initialize()
        };
    }  // namespace OMPL

    namespace Opt
    {
        /** \brief Loads configuration YAML file onto the parameter server.
         */
        bool loadConfig(IO::Handler &handler, const std::string &config_file);

        /** \cond IGNORE */
        ROBOWFLEX_CLASS_FORWARD(CHOMPPipelinePlanner);
        /** \endcond */

        /** \class robowflex::CHOMP::CHOMPPipelinePlannerPtr
            \brief A shared pointer wrapper for robowflex::CHOMP::CHOMPPipelinePlanner. */

        /** \class robowflex::CHOMP::CHOMPPipelinePlannerConstPtr
            \brief A const shared pointer wrapper for robowflex::CHOMP::CHOMPPipelinePlanner. */

        /** \brief A robowflex::PipelinePlanner that uses the \a MoveIt! CHOMP planning pipeline.
         */
        class CHOMPPipelinePlanner : public PipelinePlanner
        {
        public:
            CHOMPPipelinePlanner(const RobotPtr &robot, const std::string &name = "");

            // non-copyable
            CHOMPPipelinePlanner(CHOMPPipelinePlanner const &) = delete;
            void operator=(CHOMPPipelinePlanner const &) = delete;

            /** \brief Initialize planning pipeline.
             *  Loads CHOMP planning plugin \a plugin with the planning adapters \a adapters. Parameters are
             *  set on the parameter server from \a settings and planning configurations are loaded from the
             *  YAML file \a config_file.
             *  \param[in] config_file A YAML file containing CHOMP configuration.
             *  \param[in] plugin Planning plugin to load.
             *  \param[in] adapters Planning adapters to load.
             *  \return True upon success, false on failure.
             */
            bool initialize(const std::string &config_file = "", const std::string &plugin = DEFAULT_PLUGIN,
                            const std::vector<std::string> &adapters = DEFAULT_ADAPTERS);

        protected:
            static const std::string DEFAULT_PLUGIN;                 ///< The default CHOMP plugin.
            static const std::vector<std::string> DEFAULT_ADAPTERS;  ///< The default planning adapters.
        };

        /** \cond IGNORE */
        ROBOWFLEX_CLASS_FORWARD(TrajOptPipelinePlanner);
        /** \endcond */

        /** \class robowflex::TrajOpt::TrajOptPipelinePlannerPtr
            \brief A shared pointer wrapper for robowflex::TrajOpt::TrajOptPipelinePlanner. */

        /** \class robowflex::TrajOpt::TrajOptPipelinePlannerConstPtr
            \brief A const shared pointer wrapper for robowflex::TrajOpt::TrajOptPipelinePlanner. */

        /** \brief A robowflex::PipelinePlanner that uses the \a MoveIt! TrajOpt planning pipeline.
         */
        class TrajOptPipelinePlanner : public PipelinePlanner
        {
        public:
            TrajOptPipelinePlanner(const RobotPtr &robot, const std::string &name = "");

            // non-copyable
            TrajOptPipelinePlanner(TrajOptPipelinePlanner const &) = delete;
            void operator=(TrajOptPipelinePlanner const &) = delete;

            /** \brief Initialize planning pipeline.
             *  Loads TrajOpt planning plugin \a plugin with the planning adapters \a adapters. Parameters are
             *  set on the parameter server from \a settings and planning configurations are loaded from the
             *  YAML file \a config_file.
             *  \param[in] config_file A YAML file containing TrajOpt configuration.
             *  \param[in] plugin Planning plugin to load.
             *  \param[in] adapters Planning adapters to load.
             *  \return True upon success, false on failure.
             */
            bool initialize(const std::string &config_file = "", const std::string &plugin = DEFAULT_PLUGIN,
                            const std::vector<std::string> &adapters = DEFAULT_ADAPTERS);

        protected:
            static const std::string DEFAULT_PLUGIN;                 ///< The default TrajOpt plugin.
            static const std::vector<std::string> DEFAULT_ADAPTERS;  ///< The default planning adapters.
        };
    }  // namespace Opt
}  // namespace robowflex

#endif
