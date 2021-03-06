/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2016,
 *  TU Dortmund - Institute of Control Theory and Systems Engineering.
 *  All rights reserved.
 *
 *  Copyright (c) 2016 LAAS/CNRS
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the institute nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Christoph Rösmann
 *          Harmish Khambhaita (harmish@laas.fr)
 *********************************************************************/

#define THROTTLE_RATE 1.0 // seconds

#include <teb_local_planner/optimal_planner.h>

namespace teb_local_planner {

// ============== Implementation ===================

TebOptimalPlanner::TebOptimalPlanner()
    : cfg_(NULL), obstacles_(NULL), via_points_(NULL), cost_(HUGE_VAL),
      robot_model_(new PointRobotFootprint()),
      human_model_(new CircularRobotFootprint()), initialized_(false),
      optimized_(false) {}

TebOptimalPlanner::TebOptimalPlanner(
    const TebConfig &cfg, ObstContainer *obstacles,
    RobotFootprintModelPtr robot_model, TebVisualizationPtr visual,
    const ViaPointContainer *via_points, CircularRobotFootprintPtr human_model,
    const std::map<uint64_t, ViaPointContainer> *humans_via_points_map) {
  initialize(cfg, obstacles, robot_model, visual, via_points, human_model,
             humans_via_points_map);
}

TebOptimalPlanner::~TebOptimalPlanner() {
  clearGraph();
  // free dynamically allocated memory
  // if (optimizer_)
  //  g2o::Factory::destroy();
  // g2o::OptimizationAlgorithmFactory::destroy();
  // g2o::HyperGraphActionLibrary::destroy();
}

void TebOptimalPlanner::initialize(
    const TebConfig &cfg, ObstContainer *obstacles,
    RobotFootprintModelPtr robot_model, TebVisualizationPtr visual,
    const ViaPointContainer *via_points, CircularRobotFootprintPtr human_model,
    const std::map<uint64_t, ViaPointContainer> *humans_via_points_map) {
  // init optimizer (set solver and block ordering settings)
  optimizer_ = initOptimizer();

  cfg_ = &cfg;
  obstacles_ = obstacles;
  robot_model_ = robot_model;
  human_model_ = human_model;
  via_points_ = via_points;
  humans_via_points_map_ = humans_via_points_map;
  cost_ = HUGE_VAL;
  setVisualization(visual);

  vel_start_.first = true;
  vel_start_.second.setZero();

  vel_goal_.first = true;
  vel_goal_.second.setZero();

  robot_radius_ = robot_model_->getCircumscribedRadius();
  human_radius_ = human_model_->getCircumscribedRadius();

  initialized_ = true;
}

void TebOptimalPlanner::setVisualization(TebVisualizationPtr visualization) {
  visualization_ = visualization;
}

void TebOptimalPlanner::visualize() {
  if (!visualization_)
    return;

  visualization_->publishLocalPlanAndPoses(teb_, *robot_model_);
  visualization_->publishHumanPlanPoses(humans_tebs_map_, *human_model_);

  if (teb_.sizePoses() > 0)
    visualization_->publishRobotFootprintModel(teb_.Pose(0), *robot_model_);

  if (cfg_->trajectory.publish_feedback)
    visualization_->publishFeedbackMessage(*this, *obstacles_);
}

/*
 * registers custom vertices and edges in g2o framework
 */
void TebOptimalPlanner::registerG2OTypes() {
  g2o::Factory *factory = g2o::Factory::instance();
  factory->registerType("VERTEX_POSE",
                        new g2o::HyperGraphElementCreator<VertexPose>);
  factory->registerType("VERTEX_TIMEDIFF",
                        new g2o::HyperGraphElementCreator<VertexTimeDiff>);

  factory->registerType("EDGE_TIME_OPTIMAL",
                        new g2o::HyperGraphElementCreator<EdgeTimeOptimal>);
  factory->registerType("EDGE_VELOCITY",
                        new g2o::HyperGraphElementCreator<EdgeVelocity>);
  factory->registerType("EDGE_VELOCITY_HUMAN",
                        new g2o::HyperGraphElementCreator<EdgeVelocityHuman>);
  factory->registerType("EDGE_ACCELERATION",
                        new g2o::HyperGraphElementCreator<EdgeAcceleration>);
  factory->registerType(
      "EDGE_ACCELERATION_HUMAN",
      new g2o::HyperGraphElementCreator<EdgeAccelerationHuman>);
  factory->registerType(
      "EDGE_ACCELERATION_START",
      new g2o::HyperGraphElementCreator<EdgeAccelerationStart>);
  factory->registerType(
      "EDGE_ACCELERATION_HUMAN_START",
      new g2o::HyperGraphElementCreator<EdgeAccelerationHumanStart>);
  factory->registerType(
      "EDGE_ACCELERATION_GOAL",
      new g2o::HyperGraphElementCreator<EdgeAccelerationGoal>);
  factory->registerType(
      "EDGE_ACCELERATION_HUMAN_GOAL",
      new g2o::HyperGraphElementCreator<EdgeAccelerationHumanGoal>);
  factory->registerType(
      "EDGE_KINEMATICS_DIFF_DRIVE",
      new g2o::HyperGraphElementCreator<EdgeKinematicsDiffDrive>);
  factory->registerType(
      "EDGE_KINEMATICS_CARLIKE",
      new g2o::HyperGraphElementCreator<EdgeKinematicsCarlike>);
  factory->registerType("EDGE_OBSTACLE",
                        new g2o::HyperGraphElementCreator<EdgeObstacle>);
  factory->registerType("EDGE_DYNAMIC_OBSTACLE",
                        new g2o::HyperGraphElementCreator<EdgeDynamicObstacle>);
  factory->registerType("EDGE_VIA_POINT",
                        new g2o::HyperGraphElementCreator<EdgeViaPoint>);
  factory->registerType(
      "EDGE_HUMAN_ROBOT_SAFETY",
      new g2o::HyperGraphElementCreator<EdgeHumanRobotSafety>);
  factory->registerType(
      "EDGE_HUMAN_HUMAN_SAFETY",
      new g2o::HyperGraphElementCreator<EdgeHumanHumanSafety>);
  factory->registerType("EDGE_HUMAN_ROBOT_TTC",
                        new g2o::HyperGraphElementCreator<EdgeHumanRobotTTC>);
  factory->registerType(
      "EDGE_HUMAN_ROBOT_DIRECTIONAL",
      new g2o::HyperGraphElementCreator<EdgeHumanRobotDirectional>);
  return;
}

/*
 * initialize g2o optimizer. Set solver settings here.
 * Return: pointer to new SparseOptimizer Object.
 */
boost::shared_ptr<g2o::SparseOptimizer> TebOptimalPlanner::initOptimizer() {
  // Call register_g2o_types once, even for multiple TebOptimalPlanner instances
  // (thread-safe)
  static boost::once_flag flag = BOOST_ONCE_INIT;
  boost::call_once(&registerG2OTypes, flag);

  // allocating the optimizer
  boost::shared_ptr<g2o::SparseOptimizer> optimizer =
      boost::make_shared<g2o::SparseOptimizer>();
  TEBLinearSolver *linearSolver =
      new TEBLinearSolver(); // see typedef in optimization.h
  linearSolver->setBlockOrdering(true);
  TEBBlockSolver *blockSolver = new TEBBlockSolver(linearSolver);
  g2o::OptimizationAlgorithmLevenberg *solver =
      new g2o::OptimizationAlgorithmLevenberg(blockSolver);

  optimizer->setAlgorithm(solver);

  optimizer->initMultiThreading(); // required for >Eigen 3.1

  return optimizer;
}

bool TebOptimalPlanner::optimizeTEB(unsigned int iterations_innerloop,
                                    unsigned int iterations_outerloop,
                                    bool compute_cost_afterwards,
                                    double obst_cost_scale,
                                    double viapoint_cost_scale,
                                    bool alternative_time_cost) {
  if (cfg_->optim.optimization_activate == false)
    return false;
  bool success = false;
  optimized_ = false;
  for (unsigned int i = 0; i < iterations_outerloop; ++i) {
    if (cfg_->trajectory.teb_autosize) {
      teb_.autoResize(cfg_->trajectory.dt_ref, cfg_->trajectory.dt_hysteresis,
                      cfg_->trajectory.min_samples);

      for (auto &human_teb_kv : humans_tebs_map_)
        human_teb_kv.second.autoResize(cfg_->trajectory.dt_ref,
                                       cfg_->trajectory.dt_hysteresis,
                                       cfg_->trajectory.min_samples);
    }

    success = buildGraph();
    if (!success) {
      clearGraph();
      return false;
    }
    success = optimizeGraph(iterations_innerloop, false);
    if (!success) {
      clearGraph();
      return false;
    }
    optimized_ = true;

    if (compute_cost_afterwards &&
        i ==
            iterations_outerloop -
                1) // compute cost vec only in the last iteration
      computeCurrentCost(obst_cost_scale, viapoint_cost_scale,
                         alternative_time_cost);

    clearGraph();
  }

  return true;
}

void TebOptimalPlanner::setVelocityStart(
    const Eigen::Ref<const Eigen::Vector2d> &vel_start) {
  vel_start_.first = true;
  vel_start_.second = vel_start;
}

void TebOptimalPlanner::setVelocityStart(
    const geometry_msgs::Twist &vel_start) {
  vel_start_.first = true;
  vel_start_.second.coeffRef(0) = vel_start.linear.x;
  vel_start_.second.coeffRef(1) = vel_start.angular.z;
}

void TebOptimalPlanner::setVelocityGoal(
    const Eigen::Ref<const Eigen::Vector2d> &vel_goal) {
  vel_goal_.first = true;
  vel_goal_.second = vel_goal;
}

bool TebOptimalPlanner::plan(
    const std::vector<geometry_msgs::PoseStamped> &initial_plan,
    const geometry_msgs::Twist *start_vel, bool free_goal_vel,
    const HumanPlanVelMap *initial_human_plan_vel_map) {
  ROS_ASSERT_MSG(initialized_, "Call initialize() first.");
  auto prep_start_time = ros::Time::now();
  if (!teb_.isInit()) {
    // init trajectory
    teb_.initTEBtoGoal(initial_plan, cfg_->trajectory.dt_ref, true,
                       cfg_->trajectory.min_samples,
                       cfg_->trajectory.teb_init_skip_dist);
  } else if (cfg_->optim.disable_warm_start) {
    teb_.clearTimedElasticBand();
    teb_.initTEBtoGoal(initial_plan, cfg_->trajectory.dt_ref, true,
                       cfg_->trajectory.min_samples,
                       cfg_->trajectory.teb_init_skip_dist);
  } else { // warm start
    PoseSE2 start_(initial_plan.front().pose);
    PoseSE2 goal_(initial_plan.back().pose);
    if (teb_.sizePoses() > 0 &&
        (goal_.position() - teb_.BackPose().position()).norm() <
            cfg_->trajectory.force_reinit_new_goal_dist) {
      // actual warm start!, update TEB
      teb_.updateAndPruneTEB(start_, goal_, cfg_->trajectory.min_samples);
    } else {
      // goal too far away -> reinit
      ROS_DEBUG("New goal: distance to existing goal is higher than the "
                "specified threshold. Reinitalizing trajectories.");
      teb_.clearTimedElasticBand();
      teb_.initTEBtoGoal(initial_plan, cfg_->trajectory.dt_ref, true,
                         cfg_->trajectory.min_samples,
                         cfg_->trajectory.teb_init_skip_dist);
    }
  }
  if (start_vel)
    setVelocityStart(*start_vel);
  if (free_goal_vel)
    setVelocityGoalFree();
  else
    vel_goal_.first = true; // we just reactivate and use the previously set
                            // velocity (should be zero if nothing was modified)
  auto prep_time = ros::Time::now() - prep_start_time;

  auto human_prep_time_start = ros::Time::now();
  humans_vel_start_.clear();
  humans_vel_goal_.clear();
  switch (cfg_->planning_mode) {
  case 0:
    humans_tebs_map_.clear();
    break;
  case 1: {
    auto itr = humans_tebs_map_.begin();
    while (itr != humans_tebs_map_.end()) {
      if (initial_human_plan_vel_map->find(itr->first) ==
          initial_human_plan_vel_map->end())
        itr = humans_tebs_map_.erase(itr);
      else
        ++itr;
    }

    for (auto &initial_human_plan_vel_kv : *initial_human_plan_vel_map) {
      auto &human_id = initial_human_plan_vel_kv.first;
      auto &initial_human_plan = initial_human_plan_vel_kv.second.plan;

      // erase human-teb if human plan is empty
      if (initial_human_plan.empty()) {
        auto itr = humans_tebs_map_.find(human_id);
        if (itr != humans_tebs_map_.end()) {
          ROS_DEBUG("New plan: new human plan is empty. Removing human "
                    "trajectories.");
          humans_tebs_map_.erase(itr);
        }
        continue;
      }

      if (humans_tebs_map_.find(human_id) == humans_tebs_map_.end()) {
        // create new human-teb for new human
        humans_tebs_map_[human_id] = TimedElasticBand();
        humans_tebs_map_[human_id].initTEBtoGoal(
            initial_human_plan, cfg_->trajectory.dt_ref, true,
            cfg_->trajectory.human_min_samples,
            cfg_->trajectory.teb_init_skip_dist);
      } else if (cfg_->optim.disable_warm_start) {
        auto &human_teb = humans_tebs_map_[human_id];
        human_teb.clearTimedElasticBand();
        human_teb.initTEBtoGoal(initial_human_plan, cfg_->trajectory.dt_ref,
                                true, cfg_->trajectory.human_min_samples,
                                cfg_->trajectory.teb_init_skip_dist);
      } else {
        // modify human-teb for existing human
        PoseSE2 human_start_(initial_human_plan.front().pose);
        PoseSE2 human_goal_(initial_human_plan.back().pose);
        auto &human_teb = humans_tebs_map_[human_id];
        if (human_teb.sizePoses() > 0 &&
            (human_goal_.position() - human_teb.BackPose().position()).norm() <
                cfg_->trajectory.force_reinit_new_goal_dist)
          human_teb.updateAndPruneTEB(human_start_, human_goal_,
                                      cfg_->trajectory.human_min_samples);
        else {
          ROS_DEBUG("New goal: distance to existing goal is higher than the "
                    "specified threshold. Reinitializing human trajectories.");
          human_teb.clearTimedElasticBand();
          human_teb.initTEBtoGoal(initial_human_plan, cfg_->trajectory.dt_ref,
                                  true, cfg_->trajectory.human_min_samples,
                                  cfg_->trajectory.teb_init_skip_dist);
        }
      }
      // give start velocity for humans
      std::pair<bool, Eigen::Vector2d> human_start_vel;
      human_start_vel.first = true;
      human_start_vel.second.coeffRef(0) =
          initial_human_plan_vel_kv.second.start_vel.linear.x;
      human_start_vel.second.coeffRef(1) =
          initial_human_plan_vel_kv.second.start_vel.angular.z;
      humans_vel_start_[human_id] = human_start_vel;

      // do not set goal velocity for humans
      std::pair<bool, Eigen::Vector2d> human_goal_vel;
      human_goal_vel.first = false;
      // human_goal_vel.first = true;
      // human_goal_vel.second.coeffRef(0) =
      //     initial_human_plan_vel_kv.second.goal_vel.linear.x;
      // human_goal_vel.second.coeffRef(1) =
      //     initial_human_plan_vel_kv.second.goal_vel.angular.z;
      // humans_vel_goal_[human_id] = human_goal_vel;
    }
    break;
  }
  case 2: {
    if (initial_human_plan_vel_map->size() == 1) {
      auto &approach_plan = initial_human_plan_vel_map->begin()->second.plan;
      if (approach_plan.size() == 1) {
        approach_pose_ = approach_plan.front();
        // modify robot global plan
      } else {
        ROS_INFO("empty pose of the human for approaching");
        // set approach_pose_ same as the current robot pose
        approach_pose_ = initial_plan.front();
      }
    } else {
      ROS_INFO("no or multiple humans for approaching");
      // set approach_pose_ same as the current robot pose
      approach_pose_ = initial_plan.front();
    }
    break;
  }
  default:
    humans_tebs_map_.clear();
  }
  auto human_prep_time = ros::Time::now() - human_prep_time_start;

  // now optimize
  auto opt_start_time = ros::Time::now();
  bool teb_opt_result = optimizeTEB(cfg_->optim.no_inner_iterations,
                                    cfg_->optim.no_outer_iterations, true);
  auto opt_time = ros::Time::now() - opt_start_time;

  auto total_time = ros::Time::now() - prep_start_time;
  ROS_DEBUG_STREAM_COND(total_time.toSec() > 0.1,
                        "\nteb optimal plan times:\n"
                            << "\ttotal plan time                "
                            << std::to_string(total_time.toSec()) << "\n"
                            << "\toptimizatoin preparation time  "
                            << std::to_string(prep_time.toSec()) << "\n"
                            << "\thuman preparation time         "
                            << std::to_string(prep_time.toSec()) << "\n"
                            << "\tteb optimize time              "
                            << std::to_string(opt_time.toSec())
                            << "\n-------------------------");

  return teb_opt_result;
}

bool TebOptimalPlanner::plan(const tf::Pose &start, const tf::Pose &goal,
                             const geometry_msgs::Twist *start_vel,
                             bool free_goal_vel) {
  auto start_time = ros::Time::now();
  PoseSE2 start_(start);
  PoseSE2 goal_(goal);
  Eigen::Vector2d vel =
      start_vel ? Eigen::Vector2d(start_vel->linear.x, start_vel->angular.z)
                : Eigen::Vector2d::Zero();
  auto pre_plan_time = ros::Time::now() - start_time;
  return plan(start_, goal_, vel, free_goal_vel, pre_plan_time.toSec());
}

bool TebOptimalPlanner::plan(const PoseSE2 &start, const PoseSE2 &goal,
                             const Eigen::Vector2d &start_vel,
                             bool free_goal_vel, double pre_plan_time) {
  ROS_ASSERT_MSG(initialized_, "Call initialize() first.");
  auto prep_start_time = ros::Time::now();
  if (!teb_.isInit()) {
    // init trajectory
    teb_.initTEBtoGoal(start, goal, 0, 1,
                       cfg_->trajectory.min_samples); // 0 intermediate samples,
                                                      // but dt=1 -> autoResize
                                                      // will add more samples
                                                      // before calling first
                                                      // optimization
  } else                                              // warm start
  {
    if (teb_.sizePoses() > 0 &&
        (goal.position() - teb_.BackPose().position()).norm() <
            cfg_->trajectory.force_reinit_new_goal_dist) // actual warm start!
      teb_.updateAndPruneTEB(start, goal, cfg_->trajectory.min_samples);
    else // goal too far away -> reinit
    {
      ROS_DEBUG("New goal: distance to existing goal is higher than the "
                "specified threshold. Reinitalizing trajectories.");
      teb_.clearTimedElasticBand();
      teb_.initTEBtoGoal(start, goal, 0, 1, cfg_->trajectory.min_samples);
    }
  }
  setVelocityStart(start_vel);
  if (free_goal_vel)
    setVelocityGoalFree();
  else
    vel_goal_.first = true; // we just reactivate and use the previously set
  // velocity (should be zero if nothing was modified)
  auto prep_time = ros::Time::now() - prep_start_time;

  // now optimize
  auto opt_start_time = ros::Time::now();
  bool teb_opt_result = optimizeTEB(cfg_->optim.no_inner_iterations,
                                    cfg_->optim.no_outer_iterations);
  auto opt_time = ros::Time::now() - opt_start_time;

  auto total_time = ros::Time::now() - prep_start_time;
  ROS_INFO_STREAM_COND(
      (total_time.toSec() + pre_plan_time) > 0.05,
      "\nteb optimal plan times:\n"
          << "\ttotal plan time                "
          << std::to_string(total_time.toSec() + pre_plan_time) << "\n"
          << "\tpre-plan time                  "
          << std::to_string(pre_plan_time) << "\n"
          << "\toptimizatoin preparation time  "
          << std::to_string(prep_time.toSec()) << "\n"
          << "\tteb optimize time              "
          << std::to_string(opt_time.toSec()) << "\n-------------------------");

  return teb_opt_result;
}

bool TebOptimalPlanner::buildGraph() {
  if (!optimizer_->edges().empty() || !optimizer_->vertices().empty()) {
    ROS_WARN("Cannot build graph, because it is not empty. Call graphClear()!");
    return false;
  }

  // add TEB vertices
  AddTEBVertices();

  // add Edges (local cost functions)
  AddEdgesObstacles();
  AddEdgesDynamicObstacles();

  AddEdgesViaPoints();

  AddEdgesVelocity();
  AddEdgesAcceleration();

  AddEdgesTimeOptimal();

  if (cfg_->robot.min_turning_radius == 0 ||
      cfg_->optim.weight_kinematics_turning_radius == 0)
    AddEdgesKinematicsDiffDrive(); // we have a differential drive robot
  else
    AddEdgesKinematicsCarlike(); // we have a carlike robot since the turning
  // radius is bounded from below.

  switch (cfg_->planning_mode) {
  case 0:
    break;
  case 1:
    AddEdgesObstaclesForHumans();
    // AddEdgesDynamicObstaclesForHumans();

    AddEdgesViaPointsForHumans();

    AddEdgesVelocityForHumans();
    AddEdgesAccelerationForHumans();

    AddEdgesTimeOptimalForHumans();

    AddEdgesKinematicsDiffDriveForHumans();

    if (cfg_->optim.use_human_robot_safety_c) {
      AddEdgesHumanRobotSafety();
    }

    if (cfg_->optim.use_human_human_safety_c) {
      AddEdgesHumanHumanSafety();
    }

    if (cfg_->optim.use_human_robot_ttc_c) {
      AddEdgesHumanRobotTTC();
    }

    if (cfg_->optim.use_human_robot_dir_c) {
      AddEdgesHumanRobotDirectional();
    }
    break;
  case 2:
    AddVertexEdgesApproach();
    break;
  default:
    break;
  }

  return true;
}

bool TebOptimalPlanner::optimizeGraph(int no_iterations, bool clear_after) {
  if (cfg_->robot.max_vel_x < 0.01) {
    ROS_WARN("optimizeGraph(): Robot Max Velocity is smaller than 0.01m/s. "
             "Optimizing aborted...");
    if (clear_after)
      clearGraph();
    return false;
  }

  if (!teb_.isInit() || (int)teb_.sizePoses() < cfg_->trajectory.min_samples) {
    ROS_WARN("optimizeGraph(): TEB is empty or has too less elements. Skipping "
             "optimization.");
    if (clear_after)
      clearGraph();
    return false;
  }

  optimizer_->setVerbose(cfg_->optim.optimization_verbose);
  optimizer_->initializeOptimization();

  int iter = optimizer_->optimize(no_iterations);

  if (!iter) {
    ROS_ERROR("optimizeGraph(): Optimization failed! iter=%i", iter);
    return false;
  }

  if (clear_after)
    clearGraph();

  return true;
}

void TebOptimalPlanner::clearGraph() {
  // optimizer.clear deletes edges!!! Therefore do not run
  // optimizer.edges().clear()
  optimizer_->vertices().clear(); // neccessary, because optimizer->clear
                                  // deletes pointer-targets (therefore it
                                  // deletes TEB states!)
  optimizer_->clear();
}

void TebOptimalPlanner::AddTEBVertices() {
  // add vertices to graph
  ROS_DEBUG_COND(cfg_->optim.optimization_verbose, "Adding TEB vertices ...");
  unsigned int id_counter = 0; // used for vertices ids
  for (unsigned int i = 0; i < teb_.sizePoses(); ++i) {
    teb_.PoseVertex(i)->setId(id_counter++);
    optimizer_->addVertex(teb_.PoseVertex(i));
    if (teb_.sizeTimeDiffs() != 0 && i < teb_.sizeTimeDiffs()) {
      teb_.TimeDiffVertex(i)->setId(id_counter++);
      optimizer_->addVertex(teb_.TimeDiffVertex(i));
    }
  }

  switch (cfg_->planning_mode) {
  case 0:
    break;
  case 1: {
    for (auto &human_teb_kv : humans_tebs_map_) {
      auto &human_teb = human_teb_kv.second;
      for (unsigned int i = 0; i < human_teb.sizePoses(); ++i) {
        human_teb.PoseVertex(i)->setId(id_counter++);
        optimizer_->addVertex(human_teb.PoseVertex(i));
        if (teb_.sizeTimeDiffs() != 0 && i < human_teb.sizeTimeDiffs()) {
          human_teb.TimeDiffVertex(i)->setId(id_counter++);
          optimizer_->addVertex(human_teb.TimeDiffVertex(i));
        }
      }
    }
    break;
  }
  case 2: {
    PoseSE2 approach_pose_se2(approach_pose_.pose);
    approach_pose_vertex = new VertexPose(approach_pose_se2, true);
    approach_pose_vertex->setId(id_counter++);
    optimizer_->addVertex(approach_pose_vertex);
    break;
  }
  default:
    break;
  }
}

void TebOptimalPlanner::AddEdgesObstacles() {
  if (cfg_->optim.weight_obstacle == 0 || obstacles_ == NULL)
    return; // if weight equals zero skip adding edges!

  for (ObstContainer::const_iterator obst = obstacles_->begin();
       obst != obstacles_->end(); ++obst) {
    if ((*obst)->isDynamic()) // we handle dynamic obstacles differently below
      continue;

    unsigned int index;

    if (cfg_->obstacles.obstacle_poses_affected >= (int)teb_.sizePoses())
      index = teb_.sizePoses() / 2;
    else
      index = teb_.findClosestTrajectoryPose(*(obst->get()));

    // check if obstacle is outside index-range between start and goal
    if ((index <= 1) ||
        (index > teb_.sizePoses() - 2)) // start and goal are fixed and
                                        // findNearestBandpoint finds first or
                                        // last conf if intersection point is
                                        // outside the range
      continue;

    Eigen::Matrix<double, 1, 1> information;
    information.fill(cfg_->optim.weight_obstacle);

    EdgeObstacle *dist_bandpt_obst = new EdgeObstacle;
    dist_bandpt_obst->setVertex(0, teb_.PoseVertex(index));
    dist_bandpt_obst->setInformation(information);
    dist_bandpt_obst->setParameters(*cfg_, robot_model_.get(), obst->get());
    optimizer_->addEdge(dist_bandpt_obst);

    for (unsigned int neighbourIdx = 0;
         neighbourIdx < floor(cfg_->obstacles.obstacle_poses_affected / 2);
         neighbourIdx++) {
      if (index + neighbourIdx < teb_.sizePoses()) {
        EdgeObstacle *dist_bandpt_obst_n_r = new EdgeObstacle;
        dist_bandpt_obst_n_r->setVertex(0,
                                        teb_.PoseVertex(index + neighbourIdx));
        dist_bandpt_obst_n_r->setInformation(information);
        dist_bandpt_obst_n_r->setParameters(*cfg_, robot_model_.get(),
                                            obst->get());
        optimizer_->addEdge(dist_bandpt_obst_n_r);
      }
      if ((int)index - (int)neighbourIdx >=
          0) // needs to be casted to int to allow negative values
      {
        EdgeObstacle *dist_bandpt_obst_n_l = new EdgeObstacle;
        dist_bandpt_obst_n_l->setVertex(0,
                                        teb_.PoseVertex(index - neighbourIdx));
        dist_bandpt_obst_n_l->setInformation(information);
        dist_bandpt_obst_n_l->setParameters(*cfg_, robot_model_.get(),
                                            obst->get());
        optimizer_->addEdge(dist_bandpt_obst_n_l);
      }
    }
  }
}

void TebOptimalPlanner::AddEdgesObstaclesForHumans() {
  if (cfg_->optim.weight_obstacle == 0 || obstacles_ == NULL)
    return;

  for (ObstContainer::const_iterator obst = obstacles_->begin();
       obst != obstacles_->end(); ++obst) {
    if ((*obst)->isDynamic()) // we handle dynamic obstacles differently below
      continue;

    unsigned int index;

    for (auto &human_teb_kv : humans_tebs_map_) {
      auto &human_teb = human_teb_kv.second;

      if (cfg_->obstacles.obstacle_poses_affected >= (int)human_teb.sizePoses())
        index = human_teb.sizePoses() / 2;
      else
        index = human_teb.findClosestTrajectoryPose(*(obst->get()));

      if ((index <= 1) || (index > human_teb.sizePoses() - 1))
        continue;

      Eigen::Matrix<double, 1, 1> information;
      information.fill(cfg_->optim.weight_obstacle);

      EdgeObstacle *dist_bandpt_obst = new EdgeObstacle;
      dist_bandpt_obst->setVertex(0, human_teb.PoseVertex(index));
      dist_bandpt_obst->setInformation(information);
      dist_bandpt_obst->setParameters(
          *cfg_, static_cast<CircularRobotFootprintPtr>(human_model_).get(),
          obst->get());
      optimizer_->addEdge(dist_bandpt_obst);

      for (unsigned int neighbourIdx = 0;
           neighbourIdx < floor(cfg_->obstacles.obstacle_poses_affected / 2);
           neighbourIdx++) {
        if (index + neighbourIdx < human_teb.sizePoses()) {
          EdgeObstacle *dist_bandpt_obst_n_r = new EdgeObstacle;
          dist_bandpt_obst_n_r->setVertex(
              0, human_teb.PoseVertex(index + neighbourIdx));
          dist_bandpt_obst_n_r->setInformation(information);
          dist_bandpt_obst_n_r->setParameters(
              *cfg_, static_cast<CircularRobotFootprintPtr>(human_model_).get(),
              obst->get());
          optimizer_->addEdge(dist_bandpt_obst_n_r);
        }
        if ((int)index - (int)neighbourIdx >=
            0) { // TODO: may be > is enough instead of >=
          EdgeObstacle *dist_bandpt_obst_n_l = new EdgeObstacle;
          dist_bandpt_obst_n_l->setVertex(
              0, human_teb.PoseVertex(index - neighbourIdx));
          dist_bandpt_obst_n_l->setInformation(information);
          dist_bandpt_obst_n_l->setParameters(
              *cfg_, static_cast<CircularRobotFootprintPtr>(human_model_).get(),
              obst->get());
          optimizer_->addEdge(dist_bandpt_obst_n_l);
        }
      }
    }
  }
}

void TebOptimalPlanner::AddEdgesDynamicObstacles() {
  if (cfg_->optim.weight_obstacle == 0 || obstacles_ == NULL)
    return; // if weight equals zero skip adding edges!

  Eigen::Matrix<double, 1, 1> information;
  information.fill(cfg_->optim.weight_dynamic_obstacle);

  for (ObstContainer::const_iterator obst = obstacles_->begin();
       obst != obstacles_->end(); ++obst) {
    if (!(*obst)->isDynamic())
      continue;

    for (std::size_t i = 1; i < teb_.sizePoses() - 1; ++i) {
      EdgeDynamicObstacle *dynobst_edge = new EdgeDynamicObstacle(i);
      dynobst_edge->setVertex(0, teb_.PoseVertex(i));
      // dynobst_edge->setVertex(1,teb.PointVertex(i+1));
      dynobst_edge->setVertex(1, teb_.TimeDiffVertex(i));
      dynobst_edge->setInformation(information);
      dynobst_edge->setMeasurement(obst->get());
      dynobst_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(dynobst_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesDynamicObstaclesForHumans() {
  if (cfg_->optim.weight_obstacle == 0 || obstacles_ == NULL)
    return;

  Eigen::Matrix<double, 1, 1> information;
  information.fill(cfg_->optim.weight_dynamic_obstacle);

  for (ObstContainer::const_iterator obst = obstacles_->begin();
       obst != obstacles_->end(); ++obst) {
    if (!(*obst)->isDynamic())
      continue;

    for (auto &human_teb_kv : humans_tebs_map_) {
      auto &human_teb = human_teb_kv.second;

      for (std::size_t i = 1; i < human_teb.sizePoses() - 1; ++i) {
        EdgeDynamicObstacle *dynobst_edge = new EdgeDynamicObstacle(i);
        dynobst_edge->setVertex(0, human_teb.PoseVertex(i));
        dynobst_edge->setVertex(1, human_teb.TimeDiffVertex(i));
        dynobst_edge->setInformation(information);
        dynobst_edge->setMeasurement(obst->get());
        dynobst_edge->setTebConfig(*cfg_);
        optimizer_->addEdge(dynobst_edge);
      }
    }
  }
}

void TebOptimalPlanner::AddEdgesViaPoints() {
  if (cfg_->optim.weight_viapoint == 0 || via_points_ == NULL ||
      via_points_->empty())
    return; // if weight equals zero skip adding edges!

  int start_pose_idx = 0;

  int n = (int)teb_.sizePoses();
  if (n < 3) // we do not have any degrees of freedom for reaching via-points
    return;

  for (ViaPointContainer::const_iterator vp_it = via_points_->begin();
       vp_it != via_points_->end(); ++vp_it) {

    int index = teb_.findClosestTrajectoryPose(*vp_it, NULL, start_pose_idx);
    if (cfg_->trajectory.via_points_ordered)
      start_pose_idx =
          index +
          2; // skip a point to have a DOF inbetween for further via-points

    // check if point conicides with goal or is located behind it
    if (index > n - 2)
      index =
          n - 2; // set to a pose before the goal, since we can move it away!
    // check if point coincides with start or is located before it
    if (index < 1)
      index = 1;

    Eigen::Matrix<double, 1, 1> information;
    information.fill(cfg_->optim.weight_viapoint);

    EdgeViaPoint *edge_viapoint = new EdgeViaPoint;
    edge_viapoint->setVertex(0, teb_.PoseVertex(index));
    edge_viapoint->setInformation(information);
    edge_viapoint->setParameters(*cfg_, &(*vp_it));
    optimizer_->addEdge(edge_viapoint);
  }
}

void TebOptimalPlanner::AddEdgesViaPointsForHumans() {
  if (cfg_->optim.weight_human_viapoint == 0 || via_points_ == NULL ||
      via_points_->empty())
    return;

  int start_pose_idx = 0;

  int n = (int)teb_.sizePoses();
  if (n < 3)
    return;

  for (auto &human_via_points_kv : *humans_via_points_map_) {

    if (humans_tebs_map_.find(human_via_points_kv.first) ==
        humans_tebs_map_.end()) {
      ROS_WARN_THROTTLE(THROTTLE_RATE,
                        "inconsistant data between humans_tebs_map and "
                        "humans_via_points_map (for id %ld)",
                        human_via_points_kv.first);
      continue;
    }

    auto &human_via_points = human_via_points_kv.second;
    auto &human_teb = humans_tebs_map_[human_via_points_kv.first];

    for (ViaPointContainer::const_iterator vp_it = human_via_points.begin();
         vp_it != human_via_points.end(); ++vp_it) {
      int index =
          human_teb.findClosestTrajectoryPose(*vp_it, NULL, start_pose_idx);
      if (cfg_->trajectory.via_points_ordered)
        start_pose_idx = index + 2;

      if (index > n - 1)
        index = n - 1;
      if (index < 1)
        index = 1;

      Eigen::Matrix<double, 1, 1> information;
      information.fill(cfg_->optim.weight_human_viapoint);

      EdgeViaPoint *edge_viapoint = new EdgeViaPoint;
      edge_viapoint->setVertex(0, human_teb.PoseVertex(index));
      edge_viapoint->setInformation(information);
      edge_viapoint->setParameters(*cfg_, &(*vp_it));
      optimizer_->addEdge(edge_viapoint);
    }
  }
}

void TebOptimalPlanner::AddEdgesVelocity() {
  if (cfg_->optim.weight_max_vel_x == 0 &&
      cfg_->optim.weight_max_vel_theta == 0)
    return; // if weight equals zero skip adding edges!

  std::size_t NoBandpts(teb_.sizePoses());
  Eigen::Matrix<double, 2, 2> information;
  information.fill(0);
  information(0, 0) = cfg_->optim.weight_max_vel_x;
  information(1, 1) = cfg_->optim.weight_max_vel_theta;

  for (std::size_t i = 0; i < NoBandpts - 1; ++i) {
    EdgeVelocity *velocity_edge = new EdgeVelocity;
    velocity_edge->setVertex(0, teb_.PoseVertex(i));
    velocity_edge->setVertex(1, teb_.PoseVertex(i + 1));
    velocity_edge->setVertex(2, teb_.TimeDiffVertex(i));
    velocity_edge->setInformation(information);
    velocity_edge->setTebConfig(*cfg_);
    optimizer_->addEdge(velocity_edge);
  }
}

void TebOptimalPlanner::AddEdgesVelocityForHumans() {
  if (cfg_->optim.weight_max_human_vel_x == 0 &&
      cfg_->optim.weight_max_human_vel_theta == 0 &&
      cfg_->optim.weight_nominal_human_vel_x == 0)
    return;

  Eigen::Matrix<double, 3, 3> information;
  information.fill(0);
  information(0, 0) = cfg_->optim.weight_max_human_vel_x;
  information(1, 1) = cfg_->optim.weight_max_human_vel_theta;
  information(2, 2) = cfg_->optim.weight_nominal_human_vel_x;

  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_teb = human_teb_kv.second;

    std::size_t NoBandpts(human_teb.sizePoses());
    for (std::size_t i = 0; i < NoBandpts - 1; ++i) {
      EdgeVelocityHuman *human_velocity_edge = new EdgeVelocityHuman;
      human_velocity_edge->setVertex(0, human_teb.PoseVertex(i));
      human_velocity_edge->setVertex(1, human_teb.PoseVertex(i + 1));
      human_velocity_edge->setVertex(2, human_teb.TimeDiffVertex(i));
      human_velocity_edge->setInformation(information);
      human_velocity_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(human_velocity_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesAcceleration() {
  if (cfg_->optim.weight_acc_lim_x == 0 &&
      cfg_->optim.weight_acc_lim_theta == 0)
    return; // if weight equals zero skip adding edges!

  std::size_t NoBandpts(teb_.sizePoses());
  Eigen::Matrix<double, 2, 2> information;
  information.fill(0);
  information(0, 0) = cfg_->optim.weight_acc_lim_x;
  information(1, 1) = cfg_->optim.weight_acc_lim_theta;

  // check if an initial velocity should be taken into account
  if (vel_start_.first) {
    EdgeAccelerationStart *acceleration_edge = new EdgeAccelerationStart;
    acceleration_edge->setVertex(0, teb_.PoseVertex(0));
    acceleration_edge->setVertex(1, teb_.PoseVertex(1));
    acceleration_edge->setVertex(2, teb_.TimeDiffVertex(0));
    acceleration_edge->setInitialVelocity(vel_start_.second);
    acceleration_edge->setInformation(information);
    acceleration_edge->setTebConfig(*cfg_);
    optimizer_->addEdge(acceleration_edge);
  }

  // now add the usual acceleration edge for each tuple of three teb poses
  for (std::size_t i = 0; i < NoBandpts - 2; ++i) {
    EdgeAcceleration *acceleration_edge = new EdgeAcceleration;
    acceleration_edge->setVertex(0, teb_.PoseVertex(i));
    acceleration_edge->setVertex(1, teb_.PoseVertex(i + 1));
    acceleration_edge->setVertex(2, teb_.PoseVertex(i + 2));
    acceleration_edge->setVertex(3, teb_.TimeDiffVertex(i));
    acceleration_edge->setVertex(4, teb_.TimeDiffVertex(i + 1));
    acceleration_edge->setInformation(information);
    acceleration_edge->setTebConfig(*cfg_);
    optimizer_->addEdge(acceleration_edge);
  }

  // check if a goal velocity should be taken into account
  if (vel_goal_.first) {
    EdgeAccelerationGoal *acceleration_edge = new EdgeAccelerationGoal;
    acceleration_edge->setVertex(0, teb_.PoseVertex(NoBandpts - 2));
    acceleration_edge->setVertex(1, teb_.PoseVertex(NoBandpts - 1));
    acceleration_edge->setVertex(2,
                                 teb_.TimeDiffVertex(teb_.sizeTimeDiffs() - 1));
    acceleration_edge->setGoalVelocity(vel_goal_.second);
    acceleration_edge->setInformation(information);
    acceleration_edge->setTebConfig(*cfg_);
    optimizer_->addEdge(acceleration_edge);
  }
}

void TebOptimalPlanner::AddEdgesAccelerationForHumans() {
  if (cfg_->optim.weight_human_acc_lim_x == 0 &&
      cfg_->optim.weight_human_acc_lim_theta == 0)
    return;

  Eigen::Matrix<double, 2, 2> information;
  information.fill(0);
  information(0, 0) = cfg_->optim.weight_human_acc_lim_x;
  information(1, 1) = cfg_->optim.weight_human_acc_lim_theta;

  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_it = human_teb_kv.first;
    auto &human_teb = human_teb_kv.second;

    std::size_t NoBandpts(human_teb.sizePoses());

    if (humans_vel_start_[human_it].first) {
      EdgeAccelerationHumanStart *human_acceleration_edge =
          new EdgeAccelerationHumanStart;
      human_acceleration_edge->setVertex(0, human_teb.PoseVertex(0));
      human_acceleration_edge->setVertex(1, human_teb.PoseVertex(1));
      human_acceleration_edge->setVertex(2, human_teb.TimeDiffVertex(0));
      human_acceleration_edge->setInitialVelocity(
          humans_vel_start_[human_it].second);
      human_acceleration_edge->setInformation(information);
      human_acceleration_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(human_acceleration_edge);
    }

    for (std::size_t i = 0; i < NoBandpts - 2; ++i) {
      EdgeAccelerationHuman *human_acceleration_edge =
          new EdgeAccelerationHuman;
      human_acceleration_edge->setVertex(0, human_teb.PoseVertex(i));
      human_acceleration_edge->setVertex(1, human_teb.PoseVertex(i + 1));
      human_acceleration_edge->setVertex(2, human_teb.PoseVertex(i + 2));
      human_acceleration_edge->setVertex(3, human_teb.TimeDiffVertex(i));
      human_acceleration_edge->setVertex(4, human_teb.TimeDiffVertex(i + 1));
      human_acceleration_edge->setInformation(information);
      human_acceleration_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(human_acceleration_edge);
    }

    if (humans_vel_goal_[human_it].first) {
      EdgeAccelerationHumanGoal *human_acceleration_edge =
          new EdgeAccelerationHumanGoal;
      human_acceleration_edge->setVertex(0,
                                         human_teb.PoseVertex(NoBandpts - 2));
      human_acceleration_edge->setVertex(1,
                                         human_teb.PoseVertex(NoBandpts - 1));
      human_acceleration_edge->setVertex(
          2, human_teb.TimeDiffVertex(human_teb.sizeTimeDiffs() - 1));
      human_acceleration_edge->setGoalVelocity(
          humans_vel_goal_[human_it].second);
      human_acceleration_edge->setInformation(information);
      human_acceleration_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(human_acceleration_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesTimeOptimal() {
  if (local_weight_optimaltime_ == 0)
    return; // if weight equals zero skip adding edges!

  Eigen::Matrix<double, 1, 1> information;
  information.fill(local_weight_optimaltime_);

  for (std::size_t i = 0; i < teb_.sizeTimeDiffs(); ++i) {
    EdgeTimeOptimal *timeoptimal_edge = new EdgeTimeOptimal;
    timeoptimal_edge->setVertex(0, teb_.TimeDiffVertex(i));
    timeoptimal_edge->setInformation(information);
    timeoptimal_edge->setTebConfig(*cfg_);
    timeoptimal_edge->setInitialTime(teb_.TimeDiffVertex(i)->dt());
    optimizer_->addEdge(timeoptimal_edge);
  }
}

void TebOptimalPlanner::AddEdgesTimeOptimalForHumans() {
  if (cfg_->optim.weight_human_optimaltime == 0) {
    return;
  }

  Eigen::Matrix<double, 1, 1> information;
  information.fill(cfg_->optim.weight_human_optimaltime);

  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_teb = human_teb_kv.second;

    std::size_t NoTimeDiffs(human_teb.sizeTimeDiffs());
    for (std::size_t i = 0; i < NoTimeDiffs; ++i) {
      EdgeTimeOptimal *timeoptimal_edge = new EdgeTimeOptimal;
      timeoptimal_edge->setVertex(0, human_teb.TimeDiffVertex(i));
      timeoptimal_edge->setInformation(information);
      timeoptimal_edge->setTebConfig(*cfg_);
      timeoptimal_edge->setInitialTime(human_teb.TimeDiffVertex(i)->dt());
      optimizer_->addEdge(timeoptimal_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesKinematicsDiffDrive() {
  if (cfg_->optim.weight_kinematics_nh == 0 &&
      cfg_->optim.weight_kinematics_forward_drive == 0)
    return; // if weight equals zero skip adding edges!

  // create edge for satisfiying kinematic constraints
  Eigen::Matrix<double, 2, 2> information_kinematics;
  information_kinematics.fill(0.0);
  information_kinematics(0, 0) = cfg_->optim.weight_kinematics_nh;
  information_kinematics(1, 1) = cfg_->optim.weight_kinematics_forward_drive;

  for (unsigned int i = 0; i < teb_.sizePoses() - 1;
       i++) // ignore twiced start only
  {
    EdgeKinematicsDiffDrive *kinematics_edge = new EdgeKinematicsDiffDrive;
    kinematics_edge->setVertex(0, teb_.PoseVertex(i));
    kinematics_edge->setVertex(1, teb_.PoseVertex(i + 1));
    kinematics_edge->setInformation(information_kinematics);
    kinematics_edge->setTebConfig(*cfg_);
    optimizer_->addEdge(kinematics_edge);
  }
}

void TebOptimalPlanner::AddEdgesKinematicsDiffDriveForHumans() {
  if (cfg_->optim.weight_kinematics_nh == 0 &&
      cfg_->optim.weight_kinematics_forward_drive == 0)
    return; // if weight equals zero skip adding edges!

  // create edge for satisfiying kinematic constraints
  Eigen::Matrix<double, 2, 2> information_kinematics;
  information_kinematics.fill(0.0);
  information_kinematics(0, 0) = cfg_->optim.weight_kinematics_nh;
  information_kinematics(1, 1) = cfg_->optim.weight_kinematics_forward_drive;

  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_teb = human_teb_kv.second;
    for (unsigned int i = 0; i < human_teb.sizePoses() - 1; i++) {
      EdgeKinematicsDiffDrive *kinematics_edge = new EdgeKinematicsDiffDrive;
      kinematics_edge->setVertex(0, human_teb.PoseVertex(i));
      kinematics_edge->setVertex(1, human_teb.PoseVertex(i + 1));
      kinematics_edge->setInformation(information_kinematics);
      kinematics_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(kinematics_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesKinematicsCarlike() {
  if (cfg_->optim.weight_kinematics_nh == 0 &&
      cfg_->optim.weight_kinematics_turning_radius)
    return; // if weight equals zero skip adding edges!

  // create edge for satisfiying kinematic constraints
  Eigen::Matrix<double, 2, 2> information_kinematics;
  information_kinematics.fill(0.0);
  information_kinematics(0, 0) = cfg_->optim.weight_kinematics_nh;
  information_kinematics(1, 1) = cfg_->optim.weight_kinematics_turning_radius;

  for (unsigned int i = 0; i < teb_.sizePoses() - 1;
       i++) // ignore twiced start only
  {
    EdgeKinematicsCarlike *kinematics_edge = new EdgeKinematicsCarlike;
    kinematics_edge->setVertex(0, teb_.PoseVertex(i));
    kinematics_edge->setVertex(1, teb_.PoseVertex(i + 1));
    kinematics_edge->setInformation(information_kinematics);
    kinematics_edge->setTebConfig(*cfg_);
    optimizer_->addEdge(kinematics_edge);
  }
}

void TebOptimalPlanner::AddEdgesHumanRobotSafety() {
  auto robot_teb_size = teb_.sizePoses();

  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_teb = human_teb_kv.second;

    for (unsigned int i = 0;
         (i < human_teb.sizePoses()) && (i < robot_teb_size); i++) {
      Eigen::Matrix<double, 1, 1> information_human_robot;
      information_human_robot.fill(cfg_->optim.weight_human_robot_safety);

      EdgeHumanRobotSafety *human_robot_safety_edge = new EdgeHumanRobotSafety;
      human_robot_safety_edge->setVertex(0, teb_.PoseVertex(i));
      human_robot_safety_edge->setVertex(1, human_teb.PoseVertex(i));
      human_robot_safety_edge->setInformation(information_human_robot);
      human_robot_safety_edge->setParameters(*cfg_, robot_model_.get(),
                                             human_radius_);
      optimizer_->addEdge(human_robot_safety_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesHumanHumanSafety() {
  //std::map<uint64_t, TimedElasticBand>::iterator oi, ii;
  for (auto oi = humans_tebs_map_.begin(); oi != humans_tebs_map_.end();) {
    auto &human1_teb = oi->second;
    for (auto ii = ++oi; ii != humans_tebs_map_.end(); ii++) {
      auto &human2_teb = ii->second;

      for (unsigned int k = 0;
           (k < human1_teb.sizePoses()) && (k < human2_teb.sizePoses()); k++) {
        Eigen::Matrix<double, 1, 1> information_human_human;
        information_human_human.fill(cfg_->optim.weight_human_human_safety);

        EdgeHumanHumanSafety *human_human_safety_edge =
            new EdgeHumanHumanSafety;
        human_human_safety_edge->setVertex(0, human1_teb.PoseVertex(k));
        human_human_safety_edge->setVertex(1, human2_teb.PoseVertex(k));
        human_human_safety_edge->setInformation(information_human_human);
        human_human_safety_edge->setParameters(*cfg_, human_radius_);
        optimizer_->addEdge(human_human_safety_edge);
      }
    }
  }
}

void TebOptimalPlanner::AddEdgesHumanRobotTTC() {
  Eigen::Matrix<double, 1, 1> information_human_robot_ttc;
  information_human_robot_ttc.fill(cfg_->optim.weight_human_robot_ttc);

  auto robot_teb_size = teb_.sizePoses();
  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_teb = human_teb_kv.second;

    size_t human_teb_size = human_teb.sizePoses();
    for (unsigned int i = 0;
         (i < human_teb_size - 1) && (i < robot_teb_size - 1); i++) {

      EdgeHumanRobotTTC *human_robot_ttc_edge = new EdgeHumanRobotTTC;
      human_robot_ttc_edge->setVertex(0, teb_.PoseVertex(i));
      human_robot_ttc_edge->setVertex(1, teb_.PoseVertex(i + 1));
      human_robot_ttc_edge->setVertex(2, teb_.TimeDiffVertex(i));
      human_robot_ttc_edge->setVertex(3, human_teb.PoseVertex(i));
      human_robot_ttc_edge->setVertex(4, human_teb.PoseVertex(i + 1));
      human_robot_ttc_edge->setVertex(5, human_teb.TimeDiffVertex(i));
      human_robot_ttc_edge->setInformation(information_human_robot_ttc);
      human_robot_ttc_edge->setParameters(*cfg_, robot_radius_, human_radius_);
      optimizer_->addEdge(human_robot_ttc_edge);
    }
  }
}

void TebOptimalPlanner::AddEdgesHumanRobotDirectional() {
  Eigen::Matrix<double, 1, 1> information_human_robot_directional;
  information_human_robot_directional.fill(cfg_->optim.weight_human_robot_dir);

  auto robot_teb_size = teb_.sizePoses();
  for (auto &human_teb_kv : humans_tebs_map_) {
    auto &human_teb = human_teb_kv.second;

    size_t human_teb_size = human_teb.sizePoses();
    for (unsigned int i = 0;
         (i < human_teb_size - 1) && (i < robot_teb_size - 1); i++) {

      EdgeHumanRobotDirectional *human_robot_dir_edge =
          new EdgeHumanRobotDirectional;
      human_robot_dir_edge->setVertex(0, teb_.PoseVertex(i));
      human_robot_dir_edge->setVertex(1, teb_.PoseVertex(i + 1));
      human_robot_dir_edge->setVertex(2, teb_.TimeDiffVertex(i));
      human_robot_dir_edge->setVertex(3, human_teb.PoseVertex(i));
      human_robot_dir_edge->setVertex(4, human_teb.PoseVertex(i + 1));
      human_robot_dir_edge->setVertex(5, human_teb.TimeDiffVertex(i));
      human_robot_dir_edge->setInformation(information_human_robot_directional);
      human_robot_dir_edge->setTebConfig(*cfg_);
      optimizer_->addEdge(human_robot_dir_edge);
    }
  }
}

void TebOptimalPlanner::AddVertexEdgesApproach() {
  if (!approach_pose_vertex) {
    ROS_ERROR("approch pose vertex does not exist");
    return;
  }

  Eigen::Matrix<double, 1, 1> information_approach;
  information_approach.fill(cfg_->optim.weight_obstacle);

  for (auto &teb_pose : teb_.poses()) {
    EdgeHumanRobotSafety *approach_edge = new EdgeHumanRobotSafety;
    approach_edge->setVertex(0, teb_pose);
    approach_edge->setVertex(1, approach_pose_vertex);
    approach_edge->setInformation(information_approach);
    approach_edge->setParameters(*cfg_, robot_model_.get(), human_radius_);
    optimizer_->addEdge(approach_edge);
  }
}

void TebOptimalPlanner::computeCurrentCost(double obst_cost_scale,
                                           double viapoint_cost_scale,
                                           bool alternative_time_cost) {
  // check if graph is empty/exist  -> important if function is called between
  // buildGraph and optimizeGraph/clearGraph
  bool graph_exist_flag(false);
  if (optimizer_->edges().empty() && optimizer_->vertices().empty()) {
    // here the graph is build again, for time efficiency make sure to call this
    // function between buildGraph and Optimize (deleted), but it depends on the
    // application
    buildGraph();
    optimizer_->initializeOptimization();
  } else {
    graph_exist_flag = true;
  }

  optimizer_->computeInitialGuess();

  cost_ = 0;
  double time_opt_cost = 0.0, kinematics_dd_cost = 0.0,
         kinematics_cl_cost = 0.0, vel_cost = 0.0, acc_cost = 0.0,
         obst_cost = 0.0, dyn_obst_cost = 0.0, via_cost = 0.0,
         hr_safety_cost = 0.0, hh_safety_cost = 0.0, hr_ttc_cost = 0.0,
         hr_dir_cost = 0.0;

  if (alternative_time_cost) {
    cost_ += teb_.getSumOfAllTimeDiffs();
    // TEST we use SumOfAllTimeDiffs() here, because edge cost depends on number
    // of samples, which is not always the same for similar TEBs, since we are
    // using an AutoResize Function with hysteresis.
  }

  // now we need pointers to all edges -> calculate error for each edge-type
  // since we aren't storing edge pointers, we need to check every edge
  for (std::vector<g2o::OptimizableGraph::Edge *>::const_iterator it =
           optimizer_->activeEdges().begin();
       it != optimizer_->activeEdges().end(); it++) {
    EdgeTimeOptimal *edge_time_optimal = dynamic_cast<EdgeTimeOptimal *>(*it);
    if (edge_time_optimal != NULL && !alternative_time_cost) {
      cost_ += edge_time_optimal->getError().squaredNorm();
      time_opt_cost += edge_time_optimal->getError().squaredNorm();
      continue;
    }

    EdgeKinematicsDiffDrive *edge_kinematics_dd =
        dynamic_cast<EdgeKinematicsDiffDrive *>(*it);
    if (edge_kinematics_dd != NULL) {
      cost_ += edge_kinematics_dd->getError().squaredNorm();
      kinematics_dd_cost += edge_kinematics_dd->getError().squaredNorm();
      continue;
    }

    EdgeKinematicsCarlike *edge_kinematics_cl =
        dynamic_cast<EdgeKinematicsCarlike *>(*it);
    if (edge_kinematics_cl != NULL) {
      cost_ += edge_kinematics_cl->getError().squaredNorm();
      kinematics_cl_cost += edge_kinematics_cl->getError().squaredNorm();
      continue;
    }

    EdgeVelocity *edge_velocity = dynamic_cast<EdgeVelocity *>(*it);
    if (edge_velocity != NULL) {
      cost_ += edge_velocity->getError().squaredNorm();
      vel_cost += edge_velocity->getError().squaredNorm();
      continue;
    }

    EdgeAcceleration *edge_acceleration = dynamic_cast<EdgeAcceleration *>(*it);
    if (edge_acceleration != NULL) {
      cost_ += edge_acceleration->getError().squaredNorm();
      acc_cost += edge_acceleration->getError().squaredNorm();
      continue;
    }

    EdgeObstacle *edge_obstacle = dynamic_cast<EdgeObstacle *>(*it);
    if (edge_obstacle != NULL) {
      cost_ += edge_obstacle->getError().squaredNorm() * obst_cost_scale;
      obst_cost += edge_obstacle->getError().squaredNorm();
      continue;
    }

    EdgeDynamicObstacle *edge_dyn_obstacle =
        dynamic_cast<EdgeDynamicObstacle *>(*it);
    if (edge_dyn_obstacle != NULL) {
      cost_ += edge_dyn_obstacle->getError().squaredNorm() * obst_cost_scale;
      dyn_obst_cost += edge_dyn_obstacle->getError().squaredNorm();
      continue;
    }

    EdgeViaPoint *edge_viapoint = dynamic_cast<EdgeViaPoint *>(*it);
    if (edge_viapoint != NULL) {
      cost_ += edge_viapoint->getError().squaredNorm() * viapoint_cost_scale;
      via_cost += edge_viapoint->getError().squaredNorm();
      continue;
    }

    EdgeHumanRobotSafety *edge_human_robot_safety =
        dynamic_cast<EdgeHumanRobotSafety *>(*it);
    if (edge_human_robot_safety != NULL) {
      cost_ += edge_human_robot_safety->getError().squaredNorm();
      hr_safety_cost += edge_human_robot_safety->getError().squaredNorm();
      continue;
    }

    EdgeHumanHumanSafety *edge_human_human_safety =
        dynamic_cast<EdgeHumanHumanSafety *>(*it);
    if (edge_human_human_safety != NULL) {
      cost_ += edge_human_human_safety->getError().squaredNorm();
      hh_safety_cost += edge_human_human_safety->getError().squaredNorm();
      continue;
    }

    EdgeHumanRobotTTC *edge_human_robot_ttc =
        dynamic_cast<EdgeHumanRobotTTC *>(*it);
    if (edge_human_robot_ttc != NULL) {
      cost_ += edge_human_robot_ttc->getError().squaredNorm();
      hr_ttc_cost += edge_human_robot_ttc->getError().squaredNorm();
      continue;
    }

    EdgeHumanRobotDirectional *edge_human_robot_directional =
        dynamic_cast<EdgeHumanRobotDirectional *>(*it);
    if (edge_human_robot_directional != NULL) {
      cost_ += edge_human_robot_directional->getError().squaredNorm();
      hr_dir_cost += edge_human_robot_directional->getError().squaredNorm();
      continue;
    }
  }

  ROS_DEBUG("Costs:\n\ttime_opt_cost = %.2f\n\tkinematics_dd_cost = "
            "%.2f\n\tkinematics_cl_cost = %.2f\n\tvel_cost = %.2f\n\tacc_cost "
            "= %.2f\n\tobst_cost = %.2f\n\tdyn_obst_cost = %.2f\n\tvia_cost = "
            "%.2f\n\thr_safety_cost = %.2f\n\thh_safety_cost = "
            "%.2f\n\thr_ttc_cost = %.2f\n\thr_dir_cost = %.2f",
            time_opt_cost, kinematics_dd_cost, kinematics_cl_cost, vel_cost,
            acc_cost, obst_cost, dyn_obst_cost, via_cost, hr_safety_cost,
            hh_safety_cost, hr_ttc_cost, hr_dir_cost);

  // delete temporary created graph
  if (!graph_exist_flag)
    clearGraph();
}

void TebOptimalPlanner::extractVelocity(const PoseSE2 &pose1,
                                        const PoseSE2 &pose2, double dt,
                                        double &v, double &omega) const {
  Eigen::Vector2d deltaS = pose2.position() - pose1.position();
  Eigen::Vector2d conf1dir(cos(pose1.theta()), sin(pose1.theta()));
  // translational velocity
  double dir = deltaS.dot(conf1dir);
  v = (double)g2o::sign(dir) * deltaS.norm() / dt;

  // rotational velocity
  double orientdiff = g2o::normalize_theta(pose2.theta() - pose1.theta());
  omega = orientdiff / dt;
}

bool TebOptimalPlanner::getVelocityCommand(double &v, double &omega) const {
  if (teb_.sizePoses() < 2) {
    ROS_ERROR("TebOptimalPlanner::getVelocityCommand(): The trajectory "
              "contains less than 2 poses. Make sure to init and optimize/plan "
              "the trajectory fist.");
    v = 0;
    omega = 0;
    return false;
  }

  double dt = teb_.TimeDiff(0);
  if (dt <= 0) {
    ROS_ERROR(
        "TebOptimalPlanner::getVelocityCommand() - timediff<=0 is invalid!");
    v = 0;
    omega = 0;
    return false;
  }

  // Get velocity from the first two configurations
  extractVelocity(teb_.Pose(0), teb_.Pose(1), dt, v, omega);
  return true;
}

void TebOptimalPlanner::getVelocityProfile(
    std::vector<geometry_msgs::Twist> &velocity_profile) const {
  int n = (int)teb_.sizePoses();
  velocity_profile.resize(n + 1);

  // start velocity
  velocity_profile.front().linear.y = velocity_profile.front().linear.z = 0;
  velocity_profile.front().angular.x = velocity_profile.front().angular.y = 0;
  velocity_profile.front().linear.x = vel_start_.second.x();
  velocity_profile.front().angular.z = vel_start_.second.y();

  for (int i = 1; i < n; ++i) {
    velocity_profile[i].linear.y = velocity_profile[i].linear.z = 0;
    velocity_profile[i].angular.x = velocity_profile[i].angular.y = 0;
    extractVelocity(teb_.Pose(i - 1), teb_.Pose(i), teb_.TimeDiff(i - 1),
                    velocity_profile[i].linear.x,
                    velocity_profile[i].angular.z);
  }

  // goal velocity
  velocity_profile.back().linear.y = velocity_profile.back().linear.z = 0;
  velocity_profile.back().angular.x = velocity_profile.back().angular.y = 0;
  velocity_profile.back().linear.x = vel_goal_.second.x();
  velocity_profile.back().angular.z = vel_goal_.second.y();
}

void TebOptimalPlanner::getFullTrajectory(
    std::vector<TrajectoryPointMsg> &trajectory) const {
  int n = (int)teb_.sizePoses();

  trajectory.resize(n);

  if (n == 0)
    return;

  double curr_time = 0;

  // start
  TrajectoryPointMsg &start = trajectory.front();
  teb_.Pose(0).toPoseMsg(start.pose);
  start.velocity.linear.y = start.velocity.linear.z = 0;
  start.velocity.angular.x = start.velocity.angular.y = 0;
  start.velocity.linear.x = vel_start_.second.x();
  start.velocity.angular.z = vel_start_.second.y();
  start.time_from_start.fromSec(curr_time);

  curr_time += teb_.TimeDiff(0);

  // intermediate points
  for (int i = 1; i < n - 1; ++i) {
    TrajectoryPointMsg &point = trajectory[i];
    teb_.Pose(i).toPoseMsg(point.pose);
    point.velocity.linear.y = point.velocity.linear.z = 0;
    point.velocity.angular.x = point.velocity.angular.y = 0;
    double vel1, vel2, omega1, omega2;
    extractVelocity(teb_.Pose(i - 1), teb_.Pose(i), teb_.TimeDiff(i - 1), vel1,
                    omega1);
    extractVelocity(teb_.Pose(i), teb_.Pose(i + 1), teb_.TimeDiff(i), vel2,
                    omega2);
    point.velocity.linear.x = 0.5 * (vel1 + vel2);
    point.velocity.angular.z = 0.5 * (omega1 + omega2);
    point.time_from_start.fromSec(curr_time);

    curr_time += teb_.TimeDiff(i);
  }

  // goal
  TrajectoryPointMsg &goal = trajectory.back();
  teb_.BackPose().toPoseMsg(goal.pose);
  goal.velocity.linear.y = goal.velocity.linear.z = 0;
  goal.velocity.angular.x = goal.velocity.angular.y = 0;
  goal.velocity.linear.x = vel_goal_.second.x();
  goal.velocity.angular.z = vel_goal_.second.y();
  goal.time_from_start.fromSec(curr_time);
}

void TebOptimalPlanner::getFullHumanTrajectory(
    const uint64_t human_id,
    std::vector<TrajectoryPointMsg> &human_trajectory) {
  auto human_teb_it = humans_tebs_map_.find(human_id);
  if (human_teb_it != humans_tebs_map_.end()) {
    auto &human_teb = human_teb_it->second;

    auto human_teb_size = human_teb.sizePoses();
    if (human_teb_size < 3) {
      ROS_WARN("TEB size is %ld for human %ld", human_teb_size, human_id);
      return;
    }

    human_trajectory.resize(human_teb_size);

    double curr_time = 0;

    // start
    TrajectoryPointMsg &start = human_trajectory.front();
    human_teb.Pose(0).toPoseMsg(start.pose);
    start.velocity.linear.y = start.velocity.linear.z = 0;
    start.velocity.angular.x = start.velocity.angular.y = 0;
    start.velocity.linear.x = humans_vel_start_[human_id].second.x();
    start.velocity.angular.z = humans_vel_start_[human_id].second.y();
    start.time_from_start.fromSec(curr_time);

    curr_time += human_teb.TimeDiff(0);

    // intermediate points
    for (int i = 1; i < human_teb_size - 1; ++i) {
      TrajectoryPointMsg &point = human_trajectory[i];
      human_teb.Pose(i).toPoseMsg(point.pose);
      point.velocity.linear.y = point.velocity.linear.z = 0;
      point.velocity.angular.x = point.velocity.angular.y = 0;
      double vel1, vel2, omega1, omega2;
      extractVelocity(human_teb.Pose(i - 1), human_teb.Pose(i),
                      human_teb.TimeDiff(i - 1), vel1, omega1);
      extractVelocity(human_teb.Pose(i), human_teb.Pose(i + 1),
                      human_teb.TimeDiff(i), vel2, omega2);
      point.velocity.linear.x = 0.5 * (vel1 + vel2);
      point.velocity.angular.z = 0.5 * (omega1 + omega2);
      point.time_from_start.fromSec(curr_time);

      curr_time += human_teb.TimeDiff(i);
    }

    // goal
    TrajectoryPointMsg &goal = human_trajectory.back();
    human_teb.BackPose().toPoseMsg(goal.pose);
    goal.velocity.linear.y = goal.velocity.linear.z = 0;
    goal.velocity.angular.x = goal.velocity.angular.y = 0;
    goal.velocity.linear.x = humans_vel_goal_[human_id].second.x();
    goal.velocity.angular.z = humans_vel_goal_[human_id].second.y();
    goal.time_from_start.fromSec(curr_time);
  }
  return;
}

bool TebOptimalPlanner::isTrajectoryFeasible(
    base_local_planner::CostmapModel *costmap_model,
    const std::vector<geometry_msgs::Point> &footprint_spec,
    double inscribed_radius, double circumscribed_radius, int look_ahead_idx) {
  if (look_ahead_idx < 0 || look_ahead_idx >= (int)teb().sizePoses())
    look_ahead_idx = (int)teb().sizePoses() - 1;

  for (int i = 0; i <= look_ahead_idx; ++i) {
    if (costmap_model->footprintCost(
            teb().Pose(i).x(), teb().Pose(i).y(), teb().Pose(i).theta(),
            footprint_spec, inscribed_radius, circumscribed_radius) < 0)
      return false;

    // check if distance between two poses is higher than the robot radius and
    // interpolate in that case (if obstacles are pushing two consecutive poses
    // away, the center between two consecutive poses might coincide with the
    // obstacle ;-)!
    if (i < look_ahead_idx) {
      if ((teb().Pose(i + 1).position() - teb().Pose(i).position()).norm() >
          inscribed_radius) {
        // check one more time
        PoseSE2 center = PoseSE2::average(teb().Pose(i), teb().Pose(i + 1));
        if (costmap_model->footprintCost(center.x(), center.y(), center.theta(),
                                         footprint_spec, inscribed_radius,
                                         circumscribed_radius) < 0)
          return false;
      }
    }
  }
  return true;
}

bool TebOptimalPlanner::isHorizonReductionAppropriate(
    const std::vector<geometry_msgs::PoseStamped> &initial_plan) const {
  if (teb_.sizePoses() <
      std::size_t(
          1.5 *
          double(cfg_->trajectory.min_samples))) // trajectory is short already
    return false;

  // check if distance is at least 2m long // hardcoded for now
  double dist = 0;
  for (std::size_t i = 1; i < teb_.sizePoses(); ++i) {
    dist += (teb_.Pose(i).position() - teb_.Pose(i - 1).position()).norm();
    if (dist > 2)
      break;
  }
  if (dist <= 2)
    return false;

  // check if goal orientation is differing with more than 90° and the horizon
  // is still long enough to exclude parking maneuvers.
  // use case: Sometimes the robot accomplish the following navigation task:
  // 1. wall following 2. 180° curve 3. following along the other side of the
  // wall.
  // If the trajectory is too long, the trajectory might intersect with the
  // obstace and the optimizer does
  // push the trajectory to the correct side.
  if (std::abs(g2o::normalize_theta(teb_.Pose(0).theta() -
                                    teb_.BackPose().theta())) > M_PI / 2) {
    ROS_DEBUG("TebOptimalPlanner::isHorizonReductionAppropriate(): Goal "
              "orientation - start orientation > 90° ");
    return true;
  }

  // check if goal heading deviates more than 90° w.r.t. start orienation
  if (teb_.Pose(0).orientationUnitVec().dot(teb_.BackPose().position() -
                                            teb_.Pose(0).position()) < 0) {
    ROS_DEBUG("TebOptimalPlanner::isHorizonReductionAppropriate(): Goal "
              "heading - start orientation > 90° ");
    return true;
  }

  // check ratio: distance along the inital plan and distance of the trajectory
  // (maybe too much is cut off)
  int idx = 0; // first get point close to the robot (should be fast if the
               // global path is already pruned!)
  for (; idx < (int)initial_plan.size(); ++idx) {
    if (std::sqrt(
            std::pow(initial_plan[idx].pose.position.x - teb_.Pose(0).x(), 2) +
            std::pow(initial_plan[idx].pose.position.y - teb_.Pose(0).y(), 2)))
      break;
  }
  // now calculate length
  double ref_path_length = 0;
  for (; idx < int(initial_plan.size()) - 1; ++idx) {
    ref_path_length +=
        std::sqrt(std::pow(initial_plan[idx + 1].pose.position.x -
                               initial_plan[idx].pose.position.x,
                           2) +
                  std::pow(initial_plan[idx + 1].pose.position.y -
                               initial_plan[idx].pose.position.y,
                           2));
  }

  // check distances along the teb trajectory (by the way, we also check if the
  // distance between two poses is > obst_dist)
  double teb_length = 0;
  for (int i = 1; i < (int)teb_.sizePoses(); ++i) {
    double dist =
        (teb_.Pose(i).position() - teb_.Pose(i - 1).position()).norm();
    if (dist > 0.95 * cfg_->obstacles.min_obstacle_dist) {
      ROS_DEBUG("TebOptimalPlanner::isHorizonReductionAppropriate(): Distance "
                "between consecutive poses > 0.9*min_obstacle_dist");
      return true;
    }
    ref_path_length += dist;
  }
  if (ref_path_length > 0 &&
      teb_length / ref_path_length < 0.7) // now check ratio
  {
    ROS_DEBUG("TebOptimalPlanner::isHorizonReductionAppropriate(): Planned "
              "trajectory is at least 30° shorter than the initial plan");
    return true;
  }

  // otherwise we do not suggest shrinking the horizon:
  return false;
}

} // namespace teb_local_planner
