/*
  Copyright 2016 SINTEF ICT, Applied Mathematics.
  Copyright 2016 - 2017 Statoil ASA.
  Copyright 2017 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2016 - 2018 IRIS AS

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


#ifndef OPM_BLACKOILWELLMODEL_HEADER_INCLUDED
#define OPM_BLACKOILWELLMODEL_HEADER_INCLUDED

#include <ebos/eclproblem.hh>
#include <opm/common/OpmLog/OpmLog.hpp>

#include <opm/common/utility/platform_dependent/disable_warnings.h>
#include <opm/common/utility/platform_dependent/reenable_warnings.h>

#include <cassert>
#include <tuple>

#include <opm/parser/eclipse/EclipseState/Schedule/Schedule.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/WellTestState.hpp>

#include <opm/core/wells.h>
#include <opm/core/wells/DynamicListEconLimited.hpp>
#include <opm/core/wells/WellCollection.hpp>
#include <opm/core/simulator/SimulatorReport.hpp>
#include <opm/autodiff/VFPProperties.hpp>
#include <opm/autodiff/WellHelpers.hpp>
#include <opm/autodiff/WellDensitySegmented.hpp>
#include <opm/autodiff/BlackoilPropsAdFromDeck.hpp>
#include <opm/autodiff/BlackoilDetails.hpp>
#include <opm/autodiff/BlackoilModelParameters.hpp>
#include <opm/autodiff/WellStateFullyImplicitBlackoil.hpp>
#include <opm/autodiff/RateConverter.hpp>
#include <opm/autodiff/WellInterface.hpp>
#include <opm/autodiff/StandardWell.hpp>
#include <opm/autodiff/MultisegmentWell.hpp>
#include <opm/autodiff/Compat.hpp>
#include<opm/autodiff/SimFIBODetails.hpp>
#include<dune/common/fmatrix.hh>
#include<dune/istl/bcrsmatrix.hh>
#include<dune/istl/matrixmatrix.hh>

#include <opm/material/densead/Math.hpp>

#include <opm/simulators/WellSwitchingLogger.hpp>

BEGIN_PROPERTIES

NEW_PROP_TAG(EnableTerminalOutput);

END_PROPERTIES

namespace Opm {

        /// Class for handling the blackoil well model.
        template<typename TypeTag>
        class BlackoilWellModel : public Ewoms::BaseAuxiliaryModule<TypeTag>
        {
        public:
            // ---------      Types      ---------
            typedef WellStateFullyImplicitBlackoil WellState;
            typedef BlackoilModelParametersEbos<TypeTag> ModelParameters;

            typedef typename GET_PROP_TYPE(TypeTag, Grid)                Grid;
            typedef typename GET_PROP_TYPE(TypeTag, FluidSystem)         FluidSystem;
            typedef typename GET_PROP_TYPE(TypeTag, ElementContext)      ElementContext;
            typedef typename GET_PROP_TYPE(TypeTag, Indices)             Indices;
            typedef typename GET_PROP_TYPE(TypeTag, Simulator)           Simulator;
            typedef typename GET_PROP_TYPE(TypeTag, Scalar)              Scalar;
            typedef typename GET_PROP_TYPE(TypeTag, RateVector)          RateVector;
            typedef typename GET_PROP_TYPE(TypeTag, GlobalEqVector)      GlobalEqVector;
            typedef typename GET_PROP_TYPE(TypeTag, JacobianMatrix)      JacobianMatrix;

            typedef typename Ewoms::BaseAuxiliaryModule<TypeTag>::NeighborSet NeighborSet;

            static const int numEq = Indices::numEq;
            static const int solventSaturationIdx = Indices::solventSaturationIdx;

            // TODO: where we should put these types, WellInterface or Well Model?
            // or there is some other strategy, like TypeTag
            typedef Dune::FieldVector<Scalar, numEq    > VectorBlockType;
            typedef Dune::BlockVector<VectorBlockType> BVector;

#if  DUNE_VERSION_NEWER_REV(DUNE_ISTL, 2 , 5, 1)
            // 3x3 matrix block inversion was unstable from at least 2.3 until and
            // including 2.5.0
            typedef Dune::FieldMatrix<Scalar, numEq, numEq > MatrixBlockType;
#else
            typedef Dune::FieldMatrix<Scalar, numEq, numEq > MatrixBlockType;
#endif
            typedef Dune::BCRSMatrix <MatrixBlockType> Mat;

            typedef Ewoms::BlackOilPolymerModule<TypeTag> PolymerModule;

            // For the conversion between the surface volume rate and resrevoir voidage rate
            using RateConverterType = RateConverter::
                SurfaceToReservoirVoidage<FluidSystem, std::vector<int> >;

            BlackoilWellModel(Simulator& ebosSimulator);

            void init(const Opm::EclipseState& eclState, const Opm::Schedule& schedule);

            /////////////
            // <eWoms auxiliary module stuff>
            /////////////
            unsigned numDofs() const
            // No extra dofs are inserted for wells. (we use a Schur complement.)
            { return 0; }

            void addNeighbors(std::vector<NeighborSet>& neighbors) const;

            void applyInitial()
            {}

            void linearize(JacobianMatrix& mat , GlobalEqVector& res);

            void postSolve(GlobalEqVector& deltaX)
            {
                recoverWellSolutionAndUpdateWellState(deltaX);
            }

            /////////////
            // </ eWoms auxiliary module stuff>
            /////////////

            template <class Restarter>
            void deserialize(Restarter& res)
            {
                // TODO (?)
            }

            /*!
             * \brief This method writes the complete state of the well
             *        to the harddisk.
             */
            template <class Restarter>
            void serialize(Restarter& res)
            {
                // TODO (?)
            }

            void beginEpisode(const Opm::EclipseState& eclState,
                              const Opm::Schedule& schedule,
                              bool isRestart)
            {
                size_t episodeIdx = ebosSimulator_.episodeIndex();
                // beginEpisode in eclProblem advances the episode index
                // we don't want this when we are at the beginning of an
                // restart.
                if (isRestart)
                    episodeIdx -= 1;

                beginReportStep(episodeIdx);
            }

            void beginTimeStep();

            void beginIteration()
            {
                assemble(ebosSimulator_.model().newtonMethod().numIterations(),
                         ebosSimulator_.timeStepSize());
            }

            void endIteration()
            { }

            void endTimeStep()
            {
                timeStepSucceeded(ebosSimulator_.time());
            }

            void endEpisode()
            {
                endReportStep();
            }

            template <class Context>
            void computeTotalRatesForDof(RateVector& rate,
                                         const Context& context,
                                         unsigned spaceIdx,
                                         unsigned timeIdx) const;

            void initFromRestartFile(const RestartValue& restartValues);

            Opm::data::Wells wellData() const
            { return well_state_.report(phase_usage_, Opm::UgGridHelpers::globalCell(grid())); }

            // subtract B*inv(D)*C * x from A*x
            void apply(const BVector& x, BVector& Ax) const;

            // apply well model with scaling of alpha
            void applyScaleAdd(const Scalar alpha, const BVector& x, BVector& Ax) const;

            // Check if well equations is converged.
            bool getWellConvergence(const std::vector<Scalar>& B_avg) const;

            // return all the wells.
            const WellCollection& wellCollection() const;
            // return non const reference to all the wells.
            WellCollection& wellCollection();

            // return the internal well state, ignore the passed one.
            // Used by the legacy code to make it compatible with the legacy well models.
            const WellState& wellState(const WellState& well_state OPM_UNUSED) const;

            // return the internal well state
            const WellState& wellState() const;

            const SimulatorReport& lastReport() const;

            // called at the beginning of a report step
            void beginReportStep(const int time_step);

        protected:

            void extractLegacyPressure_(std::vector<double>& cellPressure) const
            {
                size_t nc = number_of_cells_;
                std::vector<double> cellPressures(nc, 0.0);
                ElementContext elemCtx(ebosSimulator_);
                const auto& gridView = ebosSimulator_.vanguard().gridView();
                const auto& elemEndIt = gridView.template end</*codim=*/0>();
                for (auto elemIt = gridView.template begin</*codim=*/0>();
                     elemIt != elemEndIt;
                     ++elemIt)
                {
                    const auto& elem = *elemIt;
                    if (elem.partitionType() != Dune::InteriorEntity) {
                        continue;
                    }
                    elemCtx.updatePrimaryStencil(elem);
                    elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);

                    const unsigned cellIdx = elemCtx.globalSpaceIndex(/*spaceIdx=*/0, /*timeIdx=*/0);
                    const auto& intQuants = elemCtx.intensiveQuantities(/*spaceIdx=*/0, /*timeIdx=*/0);
                    const auto& fs = intQuants.fluidState();

                    const double p = fs.pressure(FluidSystem::oilPhaseIdx).value();
                    cellPressures[cellIdx] = p;
                }
            }

            Simulator& ebosSimulator_;
            std::unique_ptr<WellsManager> wells_manager_;
            std::vector< const Well* > wells_ecl_;

            bool wells_active_;

            using WellInterfacePtr = std::unique_ptr<WellInterface<TypeTag> >;
            // a vector of all the wells.
            std::vector<WellInterfacePtr > well_container_;

            // map from logically cartesian cell indices to compressed ones
            std::vector<int> cartesian_to_compressed_;

            using ConvergenceReport = typename WellInterface<TypeTag>::ConvergenceReport;

            // create the well container
            std::vector<WellInterfacePtr > createWellContainer(const int time_step);

            WellState well_state_;
            WellState previous_well_state_;

            const ModelParameters param_;
            bool terminal_output_;
            bool has_solvent_;
            bool has_polymer_;
            std::vector<int> pvt_region_idx_;
            PhaseUsage phase_usage_;
            size_t global_nc_;
            // the number of the cells in the local grid
            size_t number_of_cells_;
            double gravity_;
            std::vector<double> depth_;
            bool initial_step_;

            std::unique_ptr<RateConverterType> rateConverter_;
            std::unique_ptr<VFPProperties> vfp_properties_;

            SimulatorReport last_report_;

            WellTestState wellTestState_;

            // used to better efficiency of calcuation
            mutable BVector scaleAddRes_;

            const Wells* wells() const { return wells_manager_->c_wells(); }

            const Grid& grid() const
            { return ebosSimulator_.vanguard().grid(); }

            const EclipseState& eclState() const
            { return ebosSimulator_.vanguard().eclState(); }

            const Schedule& schedule() const
            { return ebosSimulator_.vanguard().schedule(); }

            // compute the well fluxes and assemble them in to the reservoir equations as source terms
            // and in the well equations.
            void assemble(const int iterationIdx,
                          const double dt);

            // called at the end of a time step
            void timeStepSucceeded(const double& simulationTime);

            // called at the end of a report step
            void endReportStep();

            // using the solution x to recover the solution xw for wells and applying
            // xw to update Well State
            void recoverWellSolutionAndUpdateWellState(const BVector& x);

            void updateWellControls();

            void updateGroupControls();

            // setting the well_solutions_ based on well_state.
            void updatePrimaryVariables();

            void setupCartesianToCompressed_(const int* global_cell, int number_of_cells);

            void computeRepRadiusPerfLength(const Grid& grid);


            void computeAverageFormationFactor(std::vector<double>& B_avg) const;

            void applyVREPGroupControl();

            void computeWellVoidageRates(std::vector<double>& well_voidage_rates,
                                         std::vector<double>& voidage_conversion_coeffs) const;

            // Calculating well potentials for each well
            void computeWellPotentials(std::vector<double>& well_potentials);

            const std::vector<double>& wellPerfEfficiencyFactors() const;

            void calculateEfficiencyFactors();

            // it should be able to go to prepareTimeStep(), however, the updateWellControls() and initPrimaryVariablesEvaluation()
            // makes it a little more difficult. unless we introduce if (iterationIdx != 0) to avoid doing the above functions
            // twice at the beginning of the time step
            /// Calculating the explict quantities used in the well calculation. By explicit, we mean they are cacluated
            /// at the beginning of the time step and no derivatives are included in these quantities
            void calculateExplicitQuantities() const;

            SimulatorReport solveWellEq(const double dt);

            void initPrimaryVariablesEvaluation() const;

            // The number of components in the model.
            int numComponents() const;

            int numWells() const;

            int numPhases() const;

            void resetWellControlFromState() const;

            void assembleWellEq(const double dt);

            // some preparation work, mostly related to group control and RESV,
            // at the beginning of each time step (Not report step)
            void prepareTimeStep();

            void prepareGroupControl();

            void computeRESV(const std::size_t step);

            void extractLegacyCellPvtRegionIndex_();

            void extractLegacyDepth_();

            /// return true if wells are available in the reservoir
            bool wellsActive() const;

            void setWellsActive(const bool wells_active);

            /// return true if wells are available on this process
            bool localWellsActive() const;

            /// upate the wellTestState related to economic limits
            void updateWellTestState(const double& simulationTime, WellTestState& wellTestState) const;

            void updatePerforationIntensiveQuantities();

            void wellTesting(const int timeStepIdx, const double simulationTime);

        };


} // namespace Opm

#include "BlackoilWellModel_impl.hpp"
#endif
