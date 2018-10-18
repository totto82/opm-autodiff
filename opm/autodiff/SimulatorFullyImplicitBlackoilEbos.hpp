/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2015 Andreas Lauser
  Copyright 2017 IRIS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_SIMULATORFULLYIMPLICITBLACKOILEBOS_HEADER_INCLUDED
#define OPM_SIMULATORFULLYIMPLICITBLACKOILEBOS_HEADER_INCLUDED

#include <opm/autodiff/IterationReport.hpp>
#include <opm/autodiff/NonlinearSolverEbos.hpp>
#include <opm/autodiff/BlackoilModelEbos.hpp>
#include <opm/autodiff/BlackoilModelParametersEbos.hpp>
#include <opm/autodiff/WellStateFullyImplicitBlackoil.hpp>
#include <opm/autodiff/BlackoilWellModel.hpp>
#include <opm/autodiff/BlackoilAquiferModel.hpp>
#include <opm/autodiff/moduleVersion.hpp>
#include <opm/simulators/timestepping/AdaptiveTimeSteppingEbos.hpp>
#include <opm/grid/utility/StopWatch.hpp>

#include <opm/common/Exceptions.hpp>
#include <opm/common/ErrorMacros.hpp>

BEGIN_PROPERTIES

NEW_PROP_TAG(EnableTerminalOutput);
NEW_PROP_TAG(EnableAdaptiveTimeStepping);
NEW_PROP_TAG(EnableTuning);

SET_BOOL_PROP(EclFlowProblem, EnableTerminalOutput, true);
SET_BOOL_PROP(EclFlowProblem, EnableAdaptiveTimeStepping, true);
SET_BOOL_PROP(EclFlowProblem, EnableTuning, false);

END_PROPERTIES

namespace Opm {


/// a simulator for the blackoil model
template<class TypeTag>
class SimulatorFullyImplicitBlackoilEbos
{
public:
    typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
    typedef typename GET_PROP_TYPE(TypeTag, Grid) Grid;
    typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;
    typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;
    typedef typename GET_PROP_TYPE(TypeTag, Indices) BlackoilIndices;
    typedef typename GET_PROP_TYPE(TypeTag, PrimaryVariables)  PrimaryVariables;
    typedef typename GET_PROP_TYPE(TypeTag, MaterialLaw) MaterialLaw;
    typedef typename GET_PROP_TYPE(TypeTag, SolutionVector)    SolutionVector ;
    typedef typename GET_PROP_TYPE(TypeTag, MaterialLawParams) MaterialLawParams;

    typedef AdaptiveTimeSteppingEbos<TypeTag> TimeStepper;
    typedef Ewoms::BlackOilPolymerModule<TypeTag> PolymerModule;

    typedef WellStateFullyImplicitBlackoil WellState;
    typedef BlackoilState ReservoirState;
    typedef BlackoilModelEbos<TypeTag> Model;
    typedef NonlinearSolverEbos<TypeTag, Model> Solver;
    typedef typename Model::ModelParameters ModelParameters;
    typedef typename Solver::SolverParameters SolverParameters;
    typedef BlackoilWellModel<TypeTag> WellModel;
    typedef BlackoilAquiferModel<TypeTag> AquiferModel;


    /// Initialise from parameters and objects to observe.
    /// \param[in] param       parameters, this class accepts the following:
    ///     parameter (default)            effect
    ///     -----------------------------------------------------------
    ///     output (true)                  write output to files?
    ///     output_dir ("output")          output directoty
    ///     output_interval (1)            output every nth step
    ///     nl_pressure_residual_tolerance (0.0) pressure solver residual tolerance (in Pascal)
    ///     nl_pressure_change_tolerance (1.0)   pressure solver change tolerance (in Pascal)
    ///     nl_pressure_maxiter (10)       max nonlinear iterations in pressure
    ///     nl_maxiter (30)                max nonlinear iterations in transport
    ///     nl_tolerance (1e-9)            transport solver absolute residual tolerance
    ///     num_transport_substeps (1)     number of transport steps per pressure step
    ///     use_segregation_split (false)  solve for gravity segregation (if false,
    ///                                    segregation is ignored).
    ///
    /// \param[in] props         fluid and rock properties
    /// \param[in] linsolver     linear solver
    /// \param[in] eclipse_state the object which represents an internalized ECL deck
    /// \param[in] output_writer
    /// \param[in] threshold_pressures_by_face   if nonempty, threshold pressures that inhibit flow
    SimulatorFullyImplicitBlackoilEbos(Simulator& ebosSimulator,
                                       ISTLSolverEbos<TypeTag>& linearSolver)
        : ebosSimulator_(ebosSimulator)
        , linearSolver_(linearSolver)
    {
        phaseUsage_ = phaseUsageFromDeck(eclState());

        // Only rank 0 does print to std::cout
        const auto& comm = grid().comm();
        terminalOutput_ = EWOMS_GET_PARAM(TypeTag, bool, EnableTerminalOutput);
        terminalOutput_ = terminalOutput_ && (comm.rank() == 0);
    }

    static void registerParameters()
    {
        ModelParameters::registerParameters();
        SolverParameters::registerParameters();
        TimeStepper::registerParameters();

        EWOMS_REGISTER_PARAM(TypeTag, bool, EnableTerminalOutput,
                             "Print high-level information about the simulation's progress to the terminal");
        EWOMS_REGISTER_PARAM(TypeTag, bool, EnableAdaptiveTimeStepping,
                             "Use adaptive time stepping between report steps");
        EWOMS_REGISTER_PARAM(TypeTag, bool, EnableTuning,
                             "Honor some aspects of the TUNING keyword.");
    }

    /// Run the simulation.
    /// This will run succesive timesteps until timer.done() is true. It will
    /// modify the reservoir and well states.
    /// \param[in,out] timer       governs the requested reporting timesteps
    /// \param[in,out] state       state of reservoir: pressure, fluxes
    /// \return                    simulation report, with timing data
    SimulatorReport run(SimulatorTimer& timer)
    {
        failureReport_ = SimulatorReport();

        // handle restarts
        std::unique_ptr<RestartValue> restartValues;
        if (isRestart()) {
            std::vector<RestartKey> extraKeys = {
                {"OPMEXTRA" , Opm::UnitSystem::measure::identity, false}
            };

            std::vector<RestartKey> solutionKeys = {};
            restartValues.reset(new RestartValue(ebosSimulator_.problem().eclIO().loadRestart(solutionKeys, extraKeys)));
        }

        // Create timers and file for writing timing info.
        Opm::time::StopWatch solverTimer;
        Opm::time::StopWatch totalTimer;
        totalTimer.start();

        // adaptive time stepping
        const auto& events = schedule().getEvents();
        std::unique_ptr<TimeStepper > adaptiveTimeStepping;
        bool enableAdaptive = EWOMS_GET_PARAM(TypeTag, bool, EnableAdaptiveTimeStepping);
        bool enableTUNING = EWOMS_GET_PARAM(TypeTag, bool, EnableTuning);
        if (enableAdaptive) {
            if (enableTUNING) {
                adaptiveTimeStepping.reset(new TimeStepper(schedule().getTuning(), timer.currentStepNum(), terminalOutput_));
            }
            else {
                adaptiveTimeStepping.reset(new TimeStepper(terminalOutput_));
            }

            double suggestedStepSize = -1.0;
            if (isRestart()) {
                // This is a restart, determine the time step size from the restart data
                if (restartValues->hasExtra("OPMEXTRA")) {
                    std::vector<double> opmextra = restartValues->getExtra("OPMEXTRA");
                    assert(opmextra.size() == 1);
                    suggestedStepSize = opmextra[0];
                }
                else {
                    OpmLog::warning("Restart data is missing OPMEXTRA field, restart run may deviate from original run.");
                    suggestedStepSize = -1.0;
                }

                if (suggestedStepSize > 0.0) {
                    adaptiveTimeStepping->setSuggestedNextStep(suggestedStepSize);
                }
            }
        }

        SimulatorReport report;
        SimulatorReport stepReport;

        if (isRestart()) {
            wellModel_().initFromRestartFile(*restartValues);
        }

        // beginReportStep(...) wants to know when we are at the
        // beginning of a restart
        bool firstRestartStep = isRestart();

        AquiferModel aquifer_model(ebosSimulator_);

        // Main simulation loop.
        while (!timer.done()) {
            // Report timestep.
            if (terminalOutput_) {
                std::ostringstream ss;
                timer.report(ss);
                OpmLog::debug(ss.str());
            }


            // write the inital state at the report stage
            if (timer.initialStep()) {
                Dune::Timer perfTimer;
                perfTimer.start();

                wellModel_().beginReportStep(timer.currentStepNum());
                ebosSimulator_.problem().writeOutput(false);

                report.output_write_time += perfTimer.stop();
            }

            // Run a multiple steps of the solver depending on the time step control.
            solverTimer.start();

            auto solver = createSolver(wellModel_(), aquifer_model);

            solver->model().beginReportStep(firstRestartStep);
            firstRestartStep = false;

            if (terminalOutput_) {
                std::ostringstream stepMsg;
                boost::posix_time::time_facet* facet = new boost::posix_time::time_facet("%d-%b-%Y");
                stepMsg.imbue(std::locale(std::locale::classic(), facet));
                stepMsg << "\nReport step " << std::setw(2) <<timer.currentStepNum()
                         << "/" << timer.numSteps()
                         << " at day " << (double)unit::convert::to(timer.simulationTimeElapsed(), unit::day)
                         << "/" << (double)unit::convert::to(timer.totalTime(), unit::day)
                         << ", date = " << timer.currentDateTime();
                OpmLog::info(stepMsg.str());
            }

            // If sub stepping is enabled allow the solver to sub cycle
            // in case the report steps are too large for the solver to converge
            //
            // \Note: The report steps are met in any case
            // \Note: The sub stepping will require a copy of the state variables
            if (adaptiveTimeStepping) {
                if (enableTUNING) {
                    if (events.hasEvent(ScheduleEvents::TUNING_CHANGE,timer.currentStepNum())) {
                        adaptiveTimeStepping->updateTUNING(schedule().getTuning(), timer.currentStepNum());
                    }
                }

                bool event = events.hasEvent(ScheduleEvents::NEW_WELL, timer.currentStepNum()) ||
                        events.hasEvent(ScheduleEvents::PRODUCTION_UPDATE, timer.currentStepNum()) ||
                        events.hasEvent(ScheduleEvents::INJECTION_UPDATE, timer.currentStepNum()) ||
                        events.hasEvent(ScheduleEvents::WELL_STATUS_CHANGE, timer.currentStepNum());
                stepReport = adaptiveTimeStepping->step(timer, *solver, event, nullptr);
                report += stepReport;
                failureReport_ += adaptiveTimeStepping->failureReport();
            }
            else {
                // solve for complete report step
                stepReport = solver->step(timer);
                report += stepReport;
                failureReport_ += solver->failureReport();

                if (terminalOutput_) {
                    std::ostringstream ss;
                    stepReport.reportStep(ss);
                    OpmLog::info(ss.str());
                }
            }

            solver->model().endReportStep();

            // take time that was used to solve system for this reportStep
            solverTimer.stop();

            // update timing.
            report.solver_time += solverTimer.secsSinceStart();

            // Increment timer, remember well state.
            ++timer;


            if (terminalOutput_) {
                if (!timer.initialStep()) {
                    const std::string version = moduleVersionName();
                    outputTimestampFIP(timer, version);
                }
            }

            // write simulation state at the report stage
            Dune::Timer perfTimer;
            perfTimer.start();
            const double nextstep = adaptiveTimeStepping ? adaptiveTimeStepping->suggestedNextStep() : -1.0;
            ebosSimulator_.problem().setNextTimeStepSize(nextstep);
            ebosSimulator_.problem().writeOutput(false);
            report.output_write_time += perfTimer.stop();

            if (terminalOutput_) {
                std::string msg =
                    "Time step took " + std::to_string(solverTimer.secsSinceStart()) + " seconds; "
                    "total solver time " + std::to_string(report.solver_time) + " seconds.";
                OpmLog::debug(msg);
            }

        }

        // Stop timer and create timing report
        totalTimer.stop();
        report.total_time = totalTimer.secsSinceStart();
        report.converged = true;

        return report;
    }

    /** \brief Returns the simulator report for the failed substeps of the simulation.
     */
    const SimulatorReport& failureReport() const
    { return failureReport_; };

    const Grid& grid() const
    { return ebosSimulator_.vanguard().grid(); }

protected:

    std::unique_ptr<Solver> createSolver(WellModel& wellModel, AquiferModel& aquifer_model)
    {
        auto model = std::unique_ptr<Model>(new Model(ebosSimulator_,
                                                      modelParam_,
                                                      wellModel,
                                                      aquifer_model,
                                                      linearSolver_,
                                                      terminalOutput_));

        return std::unique_ptr<Solver>(new Solver(solverParam_, std::move(model)));
    }

    void outputTimestampFIP(const SimulatorTimer& timer, const std::string version)
    {
        std::ostringstream ss;
        boost::posix_time::time_facet* facet = new boost::posix_time::time_facet("%d %b %Y");
        ss.imbue(std::locale(std::locale::classic(), facet));
        ss << "\n                              **************************************************************************\n"
        << "  Balance  at" << std::setw(10) << (double)unit::convert::to(timer.simulationTimeElapsed(), unit::day) << "  Days"
        << " *" << std::setw(30) << eclState().getTitle() << "                                          *\n"
        << "  Report " << std::setw(4) << timer.reportStepNum() << "    " << timer.currentDateTime()
        << "  *                                             Flow  version " << std::setw(11) << version << "  *\n"
        << "                              **************************************************************************\n";
        OpmLog::note(ss.str());
    }

    const EclipseState& eclState() const
    { return ebosSimulator_.vanguard().eclState(); }


    const Schedule& schedule() const
    { return ebosSimulator_.vanguard().schedule(); }

    bool isRestart() const
    {
        const auto& initconfig = eclState().getInitConfig();
        return initconfig.restartRequested();
    }

    WellModel& wellModel_()
    { return ebosSimulator_.problem().wellModel(); }

    const WellModel& wellModel_() const
    { return ebosSimulator_.problem().wellModel(); }

    // Data.
    Simulator& ebosSimulator_;
    std::unique_ptr<WellConnectionAuxiliaryModule<TypeTag>> wellAuxMod_;
    SimulatorReport failureReport_;

    ModelParameters modelParam_;
    SolverParameters solverParam_;

    // Observed objects.
    ISTLSolverEbos<TypeTag>& linearSolver_;
    PhaseUsage phaseUsage_;
    // Misc. data
    bool terminalOutput_;
};

} // namespace Opm

#endif // OPM_SIMULATOR_FULLY_IMPLICIT_BLACKOIL_EBOS_HPP
