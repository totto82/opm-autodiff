/*
*/
#ifndef OPM_ADAPTIVE_TIME_STEPPING_EBOS_HPP
#define OPM_ADAPTIVE_TIME_STEPPING_EBOS_HPP

#include <iostream>
#include <utility>

#include <opm/core/simulator/SimulatorReport.hpp>
#include <opm/grid/utility/StopWatch.hpp>
#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/common/utility/parameters/ParameterGroup.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <opm/simulators/timestepping/SimulatorTimer.hpp>
#include <opm/simulators/timestepping/AdaptiveSimulatorTimer.hpp>
#include <opm/simulators/timestepping/TimeStepControlInterface.hpp>
#include <opm/simulators/timestepping/TimeStepControl.hpp>
#include <opm/core/props/phaseUsageFromDeck.hpp>

BEGIN_PROPERTIES

NEW_TYPE_TAG(FlowTimeSteppingParameters);

NEW_PROP_TAG(Scalar);

NEW_PROP_TAG(SolverRestartFactor);
NEW_PROP_TAG(SolverGrowthFactor);
NEW_PROP_TAG(SolverMaxGrowth);
NEW_PROP_TAG(SolverMaxTimeStepInDays);
NEW_PROP_TAG(SolverMaxRestarts);
NEW_PROP_TAG(SolverVerbosity);
NEW_PROP_TAG(TimeStepVerbosity);
NEW_PROP_TAG(InitialTimeStepInDays);
NEW_PROP_TAG(FullTimeStepInitially);
NEW_PROP_TAG(TimeStepAfterEventInDays);
NEW_PROP_TAG(TimeStepControl);
NEW_PROP_TAG(TimeStepControlTolerance);
NEW_PROP_TAG(TimeStepControlTargetIterations);
NEW_PROP_TAG(TimeStepControlTargetNewtonIterations);
NEW_PROP_TAG(TimeStepControlDecayRate);
NEW_PROP_TAG(TimeStepControlGrowthRate);
NEW_PROP_TAG(TimeStepControlFileName);

SET_SCALAR_PROP(FlowTimeSteppingParameters, SolverRestartFactor, 0.33);
SET_SCALAR_PROP(FlowTimeSteppingParameters, SolverGrowthFactor, 2.0);
SET_SCALAR_PROP(FlowTimeSteppingParameters, SolverMaxGrowth, 3.0);
SET_SCALAR_PROP(FlowTimeSteppingParameters, SolverMaxTimeStepInDays, 365.0);
SET_INT_PROP(FlowTimeSteppingParameters, SolverMaxRestarts, 10);
SET_INT_PROP(FlowTimeSteppingParameters, SolverVerbosity, 1);
SET_INT_PROP(FlowTimeSteppingParameters, TimeStepVerbosity, 1);
SET_SCALAR_PROP(FlowTimeSteppingParameters, InitialTimeStepInDays, 1.0);
SET_BOOL_PROP(FlowTimeSteppingParameters, FullTimeStepInitially, false);
SET_SCALAR_PROP(FlowTimeSteppingParameters, TimeStepAfterEventInDays, -1.0);
SET_STRING_PROP(FlowTimeSteppingParameters, TimeStepControl, "pid");
SET_SCALAR_PROP(FlowTimeSteppingParameters, TimeStepControlTolerance, 1e-1);
SET_INT_PROP(FlowTimeSteppingParameters, TimeStepControlTargetIterations, 30);
SET_INT_PROP(FlowTimeSteppingParameters, TimeStepControlTargetNewtonIterations, 8);
SET_SCALAR_PROP(FlowTimeSteppingParameters, TimeStepControlDecayRate, 0.75);
SET_SCALAR_PROP(FlowTimeSteppingParameters, TimeStepControlGrowthRate, 1.25);
SET_STRING_PROP(FlowTimeSteppingParameters, TimeStepControlFileName, "timesteps");

END_PROPERTIES

namespace Opm {
    // AdaptiveTimeStepping
    //---------------------

    template<class TypeTag>
    class AdaptiveTimeSteppingEbos
    {
        template <class Solver>
        class SolutionTimeErrorSolverWrapperEbos : public RelativeChangeInterface
        {
            const Solver& solver_;
        public:
            SolutionTimeErrorSolverWrapperEbos(const Solver& solver)
              : solver_(solver)
            {}

            /// return || u^n+1 - u^n || / || u^n+1 ||
            double relativeChange() const
            { return solver_.model().relativeChange(); }
        };

        template<class E>
        void logException_(const E& exception, bool verbose)
        {
            if (verbose) {
                std::string message;
                message = "Caught Exception: ";
                message += exception.what();
                OpmLog::debug(message);
            }
        }

    public:
        //! \brief contructor taking parameter object
        AdaptiveTimeSteppingEbos(const bool terminalOutput = true)
            : timeStepControl_()
            , restartFactor_(EWOMS_GET_PARAM(TypeTag, double, SolverRestartFactor)) // 0.33
            , growthFactor_(EWOMS_GET_PARAM(TypeTag, double, SolverGrowthFactor)) // 2.0
            , maxGrowth_(EWOMS_GET_PARAM(TypeTag, double, SolverMaxGrowth)) // 3.0
            , maxTimeStep_(EWOMS_GET_PARAM(TypeTag, double, SolverMaxTimeStepInDays)*24*60*60) // 365.25
            , solverRestartMax_(EWOMS_GET_PARAM(TypeTag, int, SolverMaxRestarts)) // 10
            , solverVerbose_(EWOMS_GET_PARAM(TypeTag, int, SolverVerbosity) > 0 && terminalOutput) // 2
            , timestepVerbose_(EWOMS_GET_PARAM(TypeTag, int, TimeStepVerbosity) > 0 && terminalOutput) // 2
            , suggestedNextTimestep_(EWOMS_GET_PARAM(TypeTag, double, InitialTimeStepInDays)*24*60*60) // 1.0
            , fullTimestepInitially_(EWOMS_GET_PARAM(TypeTag, bool, FullTimeStepInitially)) // false
            , timestepAfterEvent_(EWOMS_GET_PARAM(TypeTag, double, TimeStepAfterEventInDays)*24*60*60) // 1e30
            , useNewtonIteration_(false)
        {
            init_();
        }



        //! \brief contructor taking parameter object
        //! \param tuning Pointer to ecl TUNING keyword
        //! \param timeStep current report step
        AdaptiveTimeSteppingEbos(const Tuning& tuning,
                                 size_t timeStep,
                                 const bool terminalOutput = true)
            : timeStepControl_()
            , restartFactor_(tuning.getTSFCNV(timeStep))
            , growthFactor_(tuning.getTFDIFF(timeStep))
            , maxGrowth_(tuning.getTSFMAX(timeStep))
            , maxTimeStep_(EWOMS_GET_PARAM(TypeTag, double, SolverMaxTimeStepInDays)*24*60*60) // 365.25
            , solverRestartMax_(EWOMS_GET_PARAM(TypeTag, int, SolverMaxRestarts)) // 10
            , solverVerbose_(EWOMS_GET_PARAM(TypeTag, int, SolverVerbosity) > 0 && terminalOutput) // 2
            , timestepVerbose_(EWOMS_GET_PARAM(TypeTag, int, TimeStepVerbosity) > 0 && terminalOutput) // 2
            , suggestedNextTimestep_(EWOMS_GET_PARAM(TypeTag, double, InitialTimeStepInDays)*24*60*60) // 1.0
            , fullTimestepInitially_(EWOMS_GET_PARAM(TypeTag, bool, FullTimeStepInitially)) // false
            , timestepAfterEvent_(EWOMS_GET_PARAM(TypeTag, double, TimeStepAfterEventInDays)*24*60*60) // 1e30
            , useNewtonIteration_(false)
        {
            init_();
        }

        static void registerParameters()
        {
            // TODO: make sure the help messages are correct (and useful)
            EWOMS_REGISTER_PARAM(TypeTag, double, SolverRestartFactor,
                                 "The factor time steps are elongated after restarts");
            EWOMS_REGISTER_PARAM(TypeTag, double, SolverGrowthFactor,
                                 "The factor time steps are elongated after a successful substep");
            EWOMS_REGISTER_PARAM(TypeTag, double, SolverMaxGrowth,
                                 "The maximum factor time steps are elongated after a report step");
            EWOMS_REGISTER_PARAM(TypeTag, double, SolverMaxTimeStepInDays,
                                 "The maximum size of a time step in days");
            EWOMS_REGISTER_PARAM(TypeTag, int, SolverMaxRestarts,
                                 "The maximum number of breakdowns before a substep is given up and the simulator is terminated");
            EWOMS_REGISTER_PARAM(TypeTag, int, SolverVerbosity,
                                 "Specify the \"chattiness\" of the non-linear solver itself");
            EWOMS_REGISTER_PARAM(TypeTag, int, TimeStepVerbosity,
                                 "Specify the \"chattiness\" during the time integration");
            EWOMS_REGISTER_PARAM(TypeTag, double, InitialTimeStepInDays,
                                 "The size of the initial time step in days");
            EWOMS_REGISTER_PARAM(TypeTag, bool, FullTimeStepInitially,
                                 "Always attempt to finish a report step using a single substep");
            EWOMS_REGISTER_PARAM(TypeTag, double, TimeStepAfterEventInDays,
                                 "Time step size of the first time step after an event occurs during the simulation in days");
            EWOMS_REGISTER_PARAM(TypeTag, std::string, TimeStepControl,
                                 "The algorithm used to determine time-step sizes. valid options are: 'pid' (default), 'pid+iteration', 'pid+newtoniteration', 'iterationcount' and 'hardcoded'");
            EWOMS_REGISTER_PARAM(TypeTag, double, TimeStepControlTolerance,
                                 "The tolerance used by the time step size control algorithm");
            EWOMS_REGISTER_PARAM(TypeTag, int, TimeStepControlTargetIterations,
                                 "The number of linear iterations which the time step control scheme should aim for (if applicable)");
            EWOMS_REGISTER_PARAM(TypeTag, int, TimeStepControlTargetNewtonIterations,
                                 "The number of Newton iterations which the time step control scheme should aim for (if applicable)");
            EWOMS_REGISTER_PARAM(TypeTag, double, TimeStepControlDecayRate,
                                 "The decay rate of the time step size of the number of target iterations is exceeded");
            EWOMS_REGISTER_PARAM(TypeTag, double, TimeStepControlGrowthRate,
                                 "The growth rate of the time step size of the number of target iterations is undercut");
            EWOMS_REGISTER_PARAM(TypeTag, std::string, TimeStepControlFileName,
                                 "The name of the file which contains the hardcoded time steps sizes");
        }

        /** \brief  step method that acts like the solver::step method
                    in a sub cycle of time steps
        */
        template <class Solver>
        SimulatorReport step(const SimulatorTimer& simulatorTimer,
                             Solver& solver,
                             const bool isEvent,
                             const std::vector<int>* fipnum = nullptr)
        {
            SimulatorReport report;
            const double timestep = simulatorTimer.currentStepLength();

            // init last time step as a fraction of the given time step
            if (suggestedNextTimestep_ < 0) {
                suggestedNextTimestep_ = restartFactor_ * timestep;
            }

            if (fullTimestepInitially_) {
                suggestedNextTimestep_ = timestep;
            }

            // use seperate time step after event
            if (isEvent && timestepAfterEvent_ > 0) {
                suggestedNextTimestep_ = timestepAfterEvent_;
            }

            auto& ebosSimulator = solver.model().ebosSimulator();
            auto& ebosProblem = ebosSimulator.problem();

            // create adaptive step timer with previously used sub step size
            AdaptiveSimulatorTimer substepTimer(simulatorTimer, suggestedNextTimestep_, maxTimeStep_);

            // reset the statistics for the failed substeps
            failureReport_ = SimulatorReport();

            // counter for solver restarts
            int restarts = 0;

            // sub step time loop
            while (!substepTimer.done()) {
                // get current delta t
                const double dt = substepTimer.currentStepLength() ;
                if (timestepVerbose_) {
                    std::ostringstream ss;
                    ss <<"\nTime step " << substepTimer.currentStepNum() << ", stepsize "
                       << unit::convert::to(substepTimer.currentStepLength(), unit::day) << " days.";
                    OpmLog::info(ss.str());
                }

                SimulatorReport substepReport;
                std::string causeOfFailure = "";
                try {
                    //substepReport = solver.step(substepTimer);
                    prepareEbos_(ebosSimulator, substepTimer);
                    substepReport.converged = ebosSimulator.model().newtonMethod().apply();
                    ebosSimulator.problem().endTimeStep();
                    report += substepReport;

                    if (solverVerbose_) {
                        // report number of linear iterations
                        OpmLog::debug("Overall linear iterations used: " + std::to_string(substepReport.total_linear_iterations));
                    }
                }
                catch (const Opm::TooManyIterations& e) {
                    substepReport += solver.failureReport();
                    causeOfFailure = "Solver convergence failure - Iteration limit reached";

                    logException_(e, solverVerbose_);
                    // since linearIterations is < 0 this will restart the solver
                }
                catch (const Opm::LinearSolverProblem& e) {
                    substepReport += solver.failureReport();
                    causeOfFailure = "Linear solver convergence failure";

                    logException_(e, solverVerbose_);
                    // since linearIterations is < 0 this will restart the solver
                }
                catch (const Opm::NumericalIssue& e) {
                    substepReport += solver.failureReport();
                    causeOfFailure = "Solver convergence failure - Numerical problem encountered";

                    logException_(e, solverVerbose_);
                    // since linearIterations is < 0 this will restart the solver
                }
                catch (const std::runtime_error& e) {
                    substepReport += solver.failureReport();

                    logException_(e, solverVerbose_);
                    // also catch linear solver not converged
                }
                catch (const Dune::ISTLError& e) {
                    substepReport += solver.failureReport();

                    logException_(e, solverVerbose_);
                    // also catch errors in ISTL AMG that occur when time step is too large
                }
                catch (const Dune::MatrixBlockError& e) {
                    substepReport += solver.failureReport();

                    logException_(e, solverVerbose_);
                    // this can be thrown by ISTL's ILU0 in block mode, yet is not an ISTLError
                }

                if (substepReport.converged) {
                    // advance by current dt
                    ++substepTimer;

                    // create object to compute the time error, simply forwards the call to the model
                    SolutionTimeErrorSolverWrapperEbos<Solver> relativeChange(solver);

                    // compute new time step estimate
                    const int iterations = useNewtonIteration_ ? substepReport.total_newton_iterations
                        : substepReport.total_linear_iterations;
                    double dtEstimate = timeStepControl_->computeTimeStepSize(dt, iterations, relativeChange,
                                                                               substepTimer.simulationTimeElapsed());

                    // limit the growth of the timestep size by the growth factor
                    dtEstimate = std::min(dtEstimate, double(maxGrowth_ * dt));

                    // further restrict time step size growth after convergence problems
                    if (restarts > 0) {
                        dtEstimate = std::min(growthFactor_ * dt, dtEstimate);
                        // solver converged, reset restarts counter
                        restarts = 0;
                    }

                    if (timestepVerbose_) {
                        std::ostringstream ss;
                        substepReport.reportStep(ss);
                        OpmLog::info(ss.str());
                    }

                    // write data if outputWriter was provided
                    // if the time step is done we do not need
                    // to write it as this will be done by the simulator
                    // anyway.
                    if (!substepTimer.done()) {
                        if (fipnum) {
                            solver.computeFluidInPlace(*fipnum);
                        }
                        Opm::time::StopWatch perfTimer;
                        perfTimer.start();

                        ebosProblem.writeOutput(/*isSubStep=*/true);

                        report.output_write_time += perfTimer.secsSinceStart();
                    }

                    // set new time step length
                    substepTimer.provideTimeStepEstimate(dtEstimate);

                    report.converged = substepTimer.done();
                    substepTimer.setLastStepFailed(false);

                }
                else { // in case of no convergence (linearIterations < 0)
                    substepTimer.setLastStepFailed(true);

                    failureReport_ += substepReport;

                    // increase restart counter
                    if (restarts >= solverRestartMax_) {
                        const auto msg = std::string("Solver failed to converge after cutting timestep ")
                            + std::to_string(restarts) + " times.";
                        if (solverVerbose_) {
                            OpmLog::error(msg);
                        }
                        OPM_THROW_NOLOG(Opm::NumericalIssue, msg);
                    }

                    const double newTimeStep = restartFactor_ * dt;
                    // we need to revise this
                    substepTimer.provideTimeStepEstimate(newTimeStep);
                    if (solverVerbose_) {
                        std::string msg;
                        msg = causeOfFailure + "\nTimestep chopped to "
                            + std::to_string(unit::convert::to(substepTimer.currentStepLength(), unit::day)) + " days\n";
                        OpmLog::problem(msg);
                    }

                    ++restarts;
                }
                ebosProblem.setNextTimeStepSize(substepTimer.currentStepLength());
            }


            // store estimated time step for next reportStep
            suggestedNextTimestep_ = substepTimer.currentStepLength();
            if (timestepVerbose_) {
                std::ostringstream ss;
                substepTimer.report(ss);
                ss << "Suggested next step size = " << unit::convert::to(suggestedNextTimestep_, unit::day) << " (days)" << std::endl;
                OpmLog::debug(ss.str());
            }

            if (! std::isfinite(suggestedNextTimestep_)) { // check for NaN
                suggestedNextTimestep_ = timestep;
            }
            return report;
        }

        /** \brief Returns the simulator report for the failed substeps of the last
         *         report step.
         */
        const SimulatorReport& failureReport() const
        { return failureReport_; };

        double suggestedNextStep() const
        { return suggestedNextTimestep_; }

        void setSuggestedNextStep(const double x)
        { suggestedNextTimestep_ = x; }

        void updateTUNING(const Tuning& tuning, size_t timeStep)
        {
            restartFactor_ = tuning.getTSFCNV(timeStep);
            growthFactor_ = tuning.getTFDIFF(timeStep);
            maxGrowth_ = tuning.getTSFMAX(timeStep);
            maxTimeStep_ = tuning.getTSMAXZ(timeStep);
            suggestedNextTimestep_ = tuning.getTSINIT(timeStep);
            timestepAfterEvent_ = tuning.getTMAXWC(timeStep);
        }


    protected:
        void init_()
        {
            // valid are "pid" and "pid+iteration"
            std::string control = EWOMS_GET_PARAM(TypeTag, std::string, TimeStepControl); // "pid"

            const double tol =  EWOMS_GET_PARAM(TypeTag, double, TimeStepControlTolerance); // 1e-1
            if (control == "pid") {
                timeStepControl_ = TimeStepControlType(new PIDTimeStepControl(tol));
            }
            else if (control == "pid+iteration") {
                const int iterations =  EWOMS_GET_PARAM(TypeTag, int, TimeStepControlTargetIterations); // 30
                timeStepControl_ = TimeStepControlType(new PIDAndIterationCountTimeStepControl(iterations, tol));
            }
            else if (control == "pid+newtoniteration") {
                const int iterations =  EWOMS_GET_PARAM(TypeTag, int, TimeStepControlTargetNewtonIterations); // 8
                timeStepControl_ = TimeStepControlType(new PIDAndIterationCountTimeStepControl(iterations, tol));
                useNewtonIteration_ = true;
            }
            else if (control == "iterationcount") {
                const int iterations =  EWOMS_GET_PARAM(TypeTag, int, TimeStepControlTargetIterations); // 30
                const double decayrate = EWOMS_GET_PARAM(TypeTag, double, TimeStepControlDecayRate); // 0.75
                const double growthrate = EWOMS_GET_PARAM(TypeTag, double, TimeStepControlGrowthRate); // 1.25
                timeStepControl_ = TimeStepControlType(new SimpleIterationCountTimeStepControl(iterations, decayrate, growthrate));
            }
            else if (control == "hardcoded") {
                const std::string filename = EWOMS_GET_PARAM(TypeTag, std::string, TimeStepControlFileName); // "timesteps"
                timeStepControl_ = TimeStepControlType(new HardcodedTimeStepControl(filename));

            }
            else
                OPM_THROW(std::runtime_error,"Unsupported time step control selected "<< control);

            // make sure growth factor is something reasonable
            assert(growthFactor_ >= 1.0);
        }
        template <class Simulator>
        void prepareEbos_(Simulator& ebosSimulator, const SimulatorTimerInterface& timer) {
            // update the solution variables in ebos
            if ( timer.lastStepFailed() ) {
                ebosSimulator.model().updateFailed();
            } else {
                ebosSimulator.model().advanceTimeLevel();
            }

            // set the timestep size and episode index for ebos explicitly. ebos needs to
            // know the report step/episode index because of timing dependend data
            // despide the fact that flow uses its own time stepper. (The length of the
            // episode does not matter, though.)
            auto t = timer.simulationTimeElapsed();
            ebosSimulator.startNextEpisode(/*episodeStartTime=*/t, /*episodeLength=*/1e30);
            ebosSimulator.setEpisodeIndex(timer.reportStepNum());
            ebosSimulator.setTime(t);
            ebosSimulator.setTimeStepSize(timer.currentStepLength());
            ebosSimulator.setTimeStepIndex(ebosSimulator.timeStepIndex() + 1);
            ebosSimulator.problem().beginTimeStep();
        }

        SimulatorReport prepareSubStepReport_() {

        }


        typedef std::unique_ptr<TimeStepControlInterface> TimeStepControlType;

        SimulatorReport failureReport_;       //!< statistics for the failed substeps of the last timestep
        TimeStepControlType timeStepControl_; //!< time step control object
        double restartFactor_;               //!< factor to multiply time step with when solver fails to converge
        double growthFactor_;                //!< factor to multiply time step when solver recovered from failed convergence
        double maxGrowth_;                   //!< factor that limits the maximum growth of a time step
        double maxTimeStep_;                //!< maximal allowed time step size
        int solverRestartMax_;        //!< how many restart of solver are allowed
        bool solverVerbose_;           //!< solver verbosity
        bool timestepVerbose_;         //!< timestep verbosity
        double suggestedNextTimestep_;      //!< suggested size of next timestep
        bool fullTimestepInitially_;        //!< beginning with the size of the time step from data file
        double timestepAfterEvent_;         //!< suggested size of timestep after an event
        bool useNewtonIteration_;           //!< use newton iteration count for adaptive time step control
    };
}

#endif // OPM_ADAPTIVE_TIME_STEPPING_EBOS_HPP
