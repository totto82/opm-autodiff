/*
  Copyright 2017 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2017 Statoil ASA.

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


#include <opm/simulators/wells/MSWellHelpers.hpp>
#include <opm/simulators/utils/DeferredLoggingErrorHelpers.hpp>
#include <opm/parser/eclipse/EclipseState/Schedule/MSW/Valve.hpp>

namespace Opm
{


    template <typename TypeTag>
    MultisegmentWell<TypeTag>::
    MultisegmentWell(const Well& well, const int time_step,
                     const ModelParameters& param,
                     const RateConverterType& rate_converter,
                     const int pvtRegionIdx,
                     const int num_components,
                     const int num_phases,
                     const int index_of_well,
                     const int first_perf_index,
                     const std::vector<PerforationData>& perf_data)
        : Base(well, time_step, param, rate_converter, pvtRegionIdx, num_components, num_phases, index_of_well, first_perf_index, perf_data)
    , segment_perforations_(numberOfSegments())
    , segment_inlets_(numberOfSegments())
    , cell_perforation_depth_diffs_(number_of_perforations_, 0.0)
    , cell_perforation_pressure_diffs_(number_of_perforations_, 0.0)
    , perforation_segment_depth_diffs_(number_of_perforations_, 0.0)
    , segment_fluid_initial_(numberOfSegments(), std::vector<double>(num_components_, 0.0))
    , segment_densities_(numberOfSegments(), 0.0)
    , segment_viscosities_(numberOfSegments(), 0.0)
    , segment_mass_rates_(numberOfSegments(), 0.0)
    , segment_depth_diffs_(numberOfSegments(), 0.0)
    , upwinding_segments_(numberOfSegments(), 0)
    , segment_reservoir_volume_rates_(numberOfSegments(), 0.0)
    , segment_phase_fractions_(numberOfSegments(), std::vector<EvalWell>(num_components_, 0.0)) // number of phase here?
    , segment_phase_viscosities_(numberOfSegments(), std::vector<EvalWell>(num_components_, 0.0)) // number of phase here?
    {
        // not handling solvent or polymer for now with multisegment well
        if (has_solvent) {
            OPM_THROW(std::runtime_error, "solvent is not supported by multisegment well yet");
        }

        if (has_polymer) {
            OPM_THROW(std::runtime_error, "polymer is not supported by multisegment well yet");
        }

        if (Base::has_energy) {
            OPM_THROW(std::runtime_error, "energy is not supported by multisegment well yet");
        }
        // since we decide to use the WellSegments from the well parser. we can reuse a lot from it.
        // for other facilities needed but not available from parser, we need to process them here

        // initialize the segment_perforations_ and update perforation_segment_depth_diffs_
        const WellConnections& completion_set = well_ecl_.getConnections();
        // index of the perforation within wells struct
        // there might be some perforations not active, which causes the number of the perforations in
        // well_ecl_ and wells struct different
        // the current implementation is a temporary solution for now, it should be corrected from the parser
        // side
        int i_perf_wells = 0;
        perf_depth_.resize(number_of_perforations_, 0.);
        for (size_t perf = 0; perf < completion_set.size(); ++perf) {
            const Connection& connection = completion_set.get(perf);
            if (connection.state() == Connection::State::OPEN) {
                const int segment_index = segmentNumberToIndex(connection.segment());
                segment_perforations_[segment_index].push_back(i_perf_wells);
                perf_depth_[i_perf_wells] = connection.depth();
                const double segment_depth = segmentSet()[segment_index].depth();
                perforation_segment_depth_diffs_[i_perf_wells] = perf_depth_[i_perf_wells] - segment_depth;
                i_perf_wells++;
            }
        }

        // initialize the segment_inlets_
        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            const Segment& segment = segmentSet()[seg];
            const int segment_number = segment.segmentNumber();
            const int outlet_segment_number = segment.outletSegment();
            if (outlet_segment_number > 0) {
                const int segment_index = segmentNumberToIndex(segment_number);
                const int outlet_segment_index = segmentNumberToIndex(outlet_segment_number);
                segment_inlets_[outlet_segment_index].push_back(segment_index);
            }
        }

        // calculating the depth difference between the segment and its oulet_segments
        // for the top segment, we will make its zero unless we find other purpose to use this value
        for (int seg = 1; seg < numberOfSegments(); ++seg) {
            const double segment_depth = segmentSet()[seg].depth();
            const int outlet_segment_number = segmentSet()[seg].outletSegment();
            const Segment& outlet_segment = segmentSet()[segmentNumberToIndex(outlet_segment_number)];
            const double outlet_depth = outlet_segment.depth();
            segment_depth_diffs_[seg] = segment_depth - outlet_depth;
        }

        // update the flow scaling factors for sicd segments
        calculateSICDFlowScalingFactors();
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    init(const PhaseUsage* phase_usage_arg,
         const std::vector<double>& depth_arg,
         const double gravity_arg,
         const int num_cells)
    {
        Base::init(phase_usage_arg, depth_arg, gravity_arg, num_cells);

        // TODO: for StandardWell, we need to update the perf depth here using depth_arg.
        // for MultisegmentWell, it is much more complicated.
        // It can be specified directly, it can be calculated from the segment depth,
        // it can also use the cell center, which is the same for StandardWell.
        // For the last case, should we update the depth with the depth_arg? For the
        // future, it can be a source of wrong result with Multisegment well.
        // An indicator from the opm-parser should indicate what kind of depth we should use here.

        // \Note: we do not update the depth here. And it looks like for now, we only have the option to use
        // specified perforation depth
        initMatrixAndVectors(num_cells);

        // calcuate the depth difference between the perforations and the perforated grid block
        for (int perf = 0; perf < number_of_perforations_; ++perf) {
            const int cell_idx = well_cells_[perf];
            cell_perforation_depth_diffs_[perf] = depth_arg[cell_idx] - perf_depth_[perf];
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    initMatrixAndVectors(const int num_cells) const
    {
        duneB_.setBuildMode( OffDiagMatWell::row_wise );
        duneC_.setBuildMode( OffDiagMatWell::row_wise );
        duneD_.setBuildMode( DiagMatWell::row_wise );

        // set the size and patterns for all the matrices and vectors
        // [A C^T    [x    =  [ res
        //  B D] x_well]      res_well]

        // calculatiing the NNZ for duneD_
        // NNZ = number_of_segments + 2 * (number_of_inlets / number_of_outlets)
        {
            int nnz_d = numberOfSegments();
            for (const std::vector<int>& inlets : segment_inlets_) {
                nnz_d += 2 * inlets.size();
            }
            duneD_.setSize(numberOfSegments(), numberOfSegments(), nnz_d);
        }
        duneB_.setSize(numberOfSegments(), num_cells, number_of_perforations_);
        duneC_.setSize(numberOfSegments(), num_cells, number_of_perforations_);

        // we need to add the off diagonal ones
        for (auto row = duneD_.createbegin(), end = duneD_.createend(); row != end; ++row) {
            // the number of the row corrspnds to the segment now
            const int seg = row.index();
            // adding the item related to outlet relation
            const Segment& segment = segmentSet()[seg];
            const int outlet_segment_number = segment.outletSegment();
            if (outlet_segment_number > 0) { // if there is a outlet_segment
                const int outlet_segment_index = segmentNumberToIndex(outlet_segment_number);
                row.insert(outlet_segment_index);
            }

            // Add nonzeros for diagonal
            row.insert(seg);

            // insert the item related to its inlets
            for (const int& inlet : segment_inlets_[seg]) {
                row.insert(inlet);
            }
        }

        // make the C matrix
        for (auto row = duneC_.createbegin(), end = duneC_.createend(); row != end; ++row) {
            // the number of the row corresponds to the segment number now.
            for (const int& perf : segment_perforations_[row.index()]) {
                const int cell_idx = well_cells_[perf];
                row.insert(cell_idx);
            }
        }

        // make the B^T matrix
        for (auto row = duneB_.createbegin(), end = duneB_.createend(); row != end; ++row) {
            // the number of the row corresponds to the segment number now.
            for (const int& perf : segment_perforations_[row.index()]) {
                const int cell_idx = well_cells_[perf];
                row.insert(cell_idx);
            }
        }

        resWell_.resize( numberOfSegments() );

        primary_variables_.resize(numberOfSegments());
        primary_variables_evaluation_.resize(numberOfSegments());
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    initPrimaryVariablesEvaluation() const
    {
        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
                primary_variables_evaluation_[seg][eq_idx] = 0.0;
                primary_variables_evaluation_[seg][eq_idx].setValue(primary_variables_[seg][eq_idx]);
                primary_variables_evaluation_[seg][eq_idx].setDerivative(eq_idx + numEq, 1.0);
            }
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleWellEq(const Simulator& ebosSimulator,
                   const std::vector<Scalar>& B_avg,
                   const double dt,
                   WellState& well_state,
                   Opm::DeferredLogger& deferred_logger)
    {
        const auto& summary_state = ebosSimulator.vanguard().summaryState();
        const auto inj_controls = well_ecl_.isInjector() ? well_ecl_.injectionControls(summary_state) : Well::InjectionControls(0);
        const auto prod_controls = well_ecl_.isProducer() ? well_ecl_.productionControls(summary_state) : Well::ProductionControls(0);

        const bool use_inner_iterations = param_.use_inner_iterations_ms_wells_;
        if (use_inner_iterations) {

            iterateWellEquations(ebosSimulator, B_avg, dt, inj_controls, prod_controls, well_state, deferred_logger);
        }

        assembleWellEqWithoutIteration(ebosSimulator, dt, inj_controls, prod_controls, well_state, deferred_logger);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updateWellStateWithTarget(const Simulator& ebos_simulator,
                              WellState& well_state,
                              Opm::DeferredLogger&  deferred_logger) const
    {
        // segRates and  segPressure are used to initilize the primaryvariables for MSW wells
        // first initialize wellRates and then use it to compute segRates
        // When THP is supported for MSW wells this code and its fried in the standard model
        // can be merge.

        const auto& well = well_ecl_;
        const int well_index = index_of_well_;
        const int top_segment_index = well_state.topSegmentIndex(index_of_well_);
        const auto& pu = phaseUsage();
        const int np = well_state.numPhases();
        const auto& summaryState = ebos_simulator.vanguard().summaryState();

        if (wellIsStopped_) {
            for (int p = 0; p<np; ++p) {
                well_state.wellRates()[well_index*np + p] = 0.0;
            }
            return;
        }

        if (well.isInjector() )
        {
            const auto& controls = well.injectionControls(summaryState);

            Well::InjectorType injectorType = controls.injector_type;
            int phasePos;
            switch (injectorType) {
            case Well::InjectorType::WATER:
            {
                phasePos = pu.phase_pos[BlackoilPhases::Aqua];
                break;
            }
            case Well::InjectorType::OIL:
            {
                phasePos = pu.phase_pos[BlackoilPhases::Liquid];
                break;
            }
            case Well::InjectorType::GAS:
            {
                phasePos = pu.phase_pos[BlackoilPhases::Vapour];
                break;
            }
            default:
                throw("Expected WATER, OIL or GAS as type for injectors " + well.name());
            }

            const Opm::Well::InjectorCMode& current = well_state.currentInjectionControls()[well_index];

            switch(current) {
            case Well::InjectorCMode::RATE:
            {
                well_state.wellRates()[well_index*np + phasePos] = controls.surface_rate;
                break;
            }

            case Well::InjectorCMode::RESV:
            {
                std::vector<double> convert_coeff(number_of_phases_, 1.0);
                Base::rateConverter_.calcCoeff(/*fipreg*/ 0, Base::pvtRegionIdx_, convert_coeff);
                const double coeff = convert_coeff[phasePos];
                well_state.wellRates()[well_index*np + phasePos] = controls.reservoir_rate/coeff;
                break;
            }

            case Well::InjectorCMode::THP:
            {
                std::vector<double> rates(3, 0.0);
                for (int p = 0; p<np; ++p) {
                    rates[p] = well_state.wellRates()[well_index*np + p];
                }
                double bhp = calculateBhpFromThp(rates, well, summaryState, deferred_logger);
                well_state.bhp()[well_index] = bhp;
                break;
            }
            case Well::InjectorCMode::BHP:
            {
                well_state.segPress()[top_segment_index] = controls.bhp_limit;
                break;
            }
            case Well::InjectorCMode::GRUP:
            {
                //do nothing at the moment
                break;
            }
            case Well::InjectorCMode::CMODE_UNDEFINED:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "Well control must be specified for well "  + name(), deferred_logger );
            }

            }
        }
        //Producer
        else
        {
            const Well::ProducerCMode& current = well_state.currentProductionControls()[well_index];
            const auto& controls = well.productionControls(summaryState);

            switch (current) {
            case Well::ProducerCMode::ORAT:
            {
                double current_rate = -well_state.wellRates()[ well_index*np + pu.phase_pos[Oil] ];

                if (current_rate == 0.0)
                    break;

                for (int p = 0; p<np; ++p) {
                    well_state.wellRates()[well_index*np + p] *= controls.oil_rate/current_rate;
                }
                break;
            }
            case Well::ProducerCMode::WRAT:
            {
                double current_rate = -well_state.wellRates()[ well_index*np + pu.phase_pos[Water] ];

                if (current_rate == 0.0)
                    break;

                for (int p = 0; p<np; ++p) {
                    well_state.wellRates()[well_index*np + p] *= controls.water_rate/current_rate;
                }
                break;
            }
            case Well::ProducerCMode::GRAT:
            {
                double current_rate = -well_state.wellRates()[ well_index*np + pu.phase_pos[Gas] ];

                if (current_rate == 0.0)
                    break;

                for (int p = 0; p<np; ++p) {
                    well_state.wellRates()[well_index*np + p] *= controls.gas_rate/current_rate;
                }
                break;

            }
            case Well::ProducerCMode::LRAT:
            {
                double current_rate = -well_state.wellRates()[ well_index*np + pu.phase_pos[Water] ]
                        - well_state.wellRates()[ well_index*np + pu.phase_pos[Oil] ];

                if (current_rate == 0.0)
                    break;

                for (int p = 0; p<np; ++p) {
                    well_state.wellRates()[well_index*np + p] *= controls.liquid_rate/current_rate;
                }
                break;
            }
            case Well::ProducerCMode::CRAT:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "CRAT control not supported " << name(), deferred_logger);
            }
            case Well::ProducerCMode::RESV:
            {
                std::vector<double> convert_coeff(number_of_phases_, 1.0);
                Base::rateConverter_.calcCoeff(/*fipreg*/ 0, Base::pvtRegionIdx_, convert_coeff);
                double total_res_rate = 0.0;
                for (int p = 0; p<np; ++p) {
                    total_res_rate -= well_state.wellRates()[well_index*np + p] * convert_coeff[p];
                }
                if (total_res_rate == 0.0)
                    break;

                if (controls.prediction_mode) {
                    for (int p = 0; p<np; ++p) {
                        well_state.wellRates()[well_index*np + p] *= controls.resv_rate/total_res_rate;
                    }
                } else {
                    std::vector<double> hrates(number_of_phases_,0.);
                    if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                        hrates[pu.phase_pos[Water]] = controls.water_rate;
                    }
                    if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                        hrates[pu.phase_pos[Oil]] = controls.oil_rate;
                    }
                    if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                        hrates[pu.phase_pos[Gas]] = controls.gas_rate;
                    }
                    std::vector<double> hrates_resv(number_of_phases_,0.);
                    Base::rateConverter_.calcReservoirVoidageRates(/*fipreg*/ 0, Base::pvtRegionIdx_, hrates, hrates_resv);
                    double target = std::accumulate(hrates_resv.begin(), hrates_resv.end(), 0.0);
                    for (int p = 0; p<np; ++p) {
                        well_state.wellRates()[well_index*np + p] *= target/total_res_rate;
                    }

                }
                break;
            }
            case Well::ProducerCMode::BHP:
            {
                well_state.segPress()[top_segment_index] = controls.bhp_limit;
                break;
            }
            case Well::ProducerCMode::THP:
            {
                std::vector<double> rates(3, 0.0);
                for (int p = 0; p<np; ++p) {
                    rates[p] = well_state.wellRates()[well_index*np + p];
                }
                double bhp = calculateBhpFromThp(rates, well, summaryState, deferred_logger);
                well_state.bhp()[well_index] = bhp;
                break;
            }
            case Well::ProducerCMode::GRUP:
            {
                //do nothing at the moment
                break;
            }
            case Well::ProducerCMode::CMODE_UNDEFINED:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "Well control must be specified for well "  + name(), deferred_logger );
            }
            case Well::ProducerCMode::NONE:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "Well control must be specified for well "  + name() , deferred_logger);
            }

            }
        }

        // compute the segment rates based on the wellRates
        initSegmentRatesWithWellRates(well_state);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    initSegmentRatesWithWellRates(WellState& well_state) const
    {
        for (int phase = 0; phase < number_of_phases_; ++phase) {
            const double perf_phaserate = well_state.wellRates()[number_of_phases_ * index_of_well_ + phase] / number_of_perforations_;
            for (int perf = 0; perf < number_of_perforations_; ++perf) {
                well_state.perfPhaseRates()[number_of_phases_ * (first_perf_ + perf) + phase] = perf_phaserate;
            }
        }

        const std::vector<double> perforation_rates(well_state.perfPhaseRates().begin() + number_of_phases_ * first_perf_,
                                                    well_state.perfPhaseRates().begin() +
                                                    number_of_phases_ * (first_perf_ + number_of_perforations_) );
        std::vector<double> segment_rates;
        WellState::calculateSegmentRates(segment_inlets_, segment_perforations_, perforation_rates, number_of_phases_,
                                         0, segment_rates);
        const int top_segment_index = well_state.topSegmentIndex(index_of_well_);
        std::copy(segment_rates.begin(), segment_rates.end(),
                  well_state.segRates().begin() + number_of_phases_ * top_segment_index );
        // we need to check the top segment rates should be same with the well rates
    }





    template <typename TypeTag>
    ConvergenceReport
    MultisegmentWell<TypeTag>::
    getWellConvergence(const WellState& well_state, const std::vector<double>& B_avg, Opm::DeferredLogger& deferred_logger) const
    {
        assert(int(B_avg.size()) == num_components_);

        // checking if any residual is NaN or too large. The two large one is only handled for the well flux
        std::vector<std::vector<double>> abs_residual(numberOfSegments(), std::vector<double>(numWellEq, 0.0));
        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
                abs_residual[seg][eq_idx] = std::abs(resWell_[seg][eq_idx]);
            }
        }

        std::vector<double> maximum_residual(numWellEq, 0.0);

        ConvergenceReport report;
        // TODO: the following is a little complicated, maybe can be simplified in some way?
        for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
            for (int seg = 0; seg < numberOfSegments(); ++seg) {
                if (eq_idx < num_components_) { // phase or component mass equations
                    const double flux_residual = B_avg[eq_idx] * abs_residual[seg][eq_idx];
                    if (flux_residual > maximum_residual[eq_idx]) {
                        maximum_residual[eq_idx] = flux_residual;
                    }
                } else { // pressure or control equation
                    // for the top segment (seg == 0), it is control equation, will be checked later separately
                    if (seg > 0) {
                        // Pressure equation
                        const double pressure_residual = abs_residual[seg][eq_idx];
                        if (pressure_residual > maximum_residual[eq_idx]) {
                            maximum_residual[eq_idx] = pressure_residual;
                        }
                    }
                }
            }
        }

        using CR = ConvergenceReport;
        for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
            if (eq_idx < num_components_) { // phase or component mass equations
                const double flux_residual = maximum_residual[eq_idx];
                // TODO: the report can not handle the segment number yet.
                if (std::isnan(flux_residual)) {
                    report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::NotANumber, eq_idx, name()});
                } else if (flux_residual > param_.max_residual_allowed_) {
                    report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::TooLarge, eq_idx, name()});
                } else if (flux_residual > param_.tolerance_wells_) {
                    report.setWellFailed({CR::WellFailure::Type::MassBalance, CR::Severity::Normal, eq_idx, name()});
                }
            } else { // pressure equation
                const double pressure_residual = maximum_residual[eq_idx];
                const int dummy_component = -1;
                if (std::isnan(pressure_residual)) {
                    report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::NotANumber, dummy_component, name()});
                } else if (std::isinf(pressure_residual)) {
                    report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::TooLarge, dummy_component, name()});
                } else if (pressure_residual > param_.tolerance_pressure_ms_wells_) {
                    report.setWellFailed({CR::WellFailure::Type::Pressure, CR::Severity::Normal, dummy_component, name()});
                }
            }
        }

        checkConvergenceControlEq(well_state, report, deferred_logger);

        return report;
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    apply(const BVector& x, BVector& Ax) const
    {
        BVectorWell Bx(duneB_.N());

        duneB_.mv(x, Bx);

        // invDBx = duneD^-1 * Bx_
        const BVectorWell invDBx = mswellhelpers::invDXDirect(duneD_, Bx);

        // Ax = Ax - duneC_^T * invDBx
        duneC_.mmtv(invDBx,Ax);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    apply(BVector& r) const
    {
        // invDrw_ = duneD^-1 * resWell_
        const BVectorWell invDrw = mswellhelpers::invDXDirect(duneD_, resWell_);
        // r = r - duneC_^T * invDrw
        duneC_.mmtv(invDrw, r);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    recoverWellSolutionAndUpdateWellState(const BVector& x,
                                          WellState& well_state,
                                          Opm::DeferredLogger& deferred_logger) const
    {
        BVectorWell xw(1);
        recoverSolutionWell(x, xw);
        updateWellState(xw, well_state, deferred_logger);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computeWellPotentials(const Simulator& ebosSimulator,
                          const std::vector<Scalar>& B_avg,
                          const WellState& well_state,
                          std::vector<double>& well_potentials,
                          Opm::DeferredLogger& deferred_logger)
    {
        const int np = number_of_phases_;
        well_potentials.resize(np, 0.0);

        // Stopped wells have zero potential.
        if (this->wellIsStopped()) {
            return;
        }

        // If the well is pressure controlled the potential equals the rate.
        {
            bool pressure_controlled_well = false;
            if (this->isInjector()) {
                const Opm::Well::InjectorCMode& current = well_state.currentInjectionControls()[index_of_well_];
                if (current == Well::InjectorCMode::BHP || current == Well::InjectorCMode::THP) {
                    pressure_controlled_well = true;
                }
            } else {
                const Opm::Well::ProducerCMode& current = well_state.currentProductionControls()[index_of_well_];
                if (current == Well::ProducerCMode::BHP || current == Well::ProducerCMode::THP) {
                    pressure_controlled_well = true;
                }
            }
            if (pressure_controlled_well) {
                for (int compIdx = 0; compIdx < num_components_; ++compIdx) {
                    const EvalWell rate = this->getSegmentRate(0, compIdx);
                    well_potentials[ebosCompIdxToFlowCompIdx(compIdx)] = rate.value();
                }
                return;
            }
        }

        // creating a copy of the well itself, to avoid messing up the explicit informations
        // during this copy, the only information not copied properly is the well controls
        MultisegmentWell<TypeTag> well(*this);
        well.debug_cost_counter_ = 0;

        well.updatePrimaryVariables(well_state, deferred_logger);

        // initialize the primary variables in Evaluation, which is used in computePerfRate for computeWellPotentials
        // TODO: for computeWellPotentials, no derivative is required actually
        well.initPrimaryVariablesEvaluation();

        // does the well have a THP related constraint?
        const auto& summaryState = ebosSimulator.vanguard().summaryState();
        const Well::ProducerCMode& current_control = well_state.currentProductionControls()[this->index_of_well_];
        if ( !well.Base::wellHasTHPConstraints(summaryState) || current_control == Well::ProducerCMode::BHP) {
            well.computeWellRatesAtBhpLimit(ebosSimulator, B_avg, well_potentials, deferred_logger);
        } else {
            well_potentials = well.computeWellPotentialWithTHP(ebosSimulator, B_avg, deferred_logger);
        }
        deferred_logger.debug("Cost in iterations of finding well potential for well "
                              + name() + ": " + std::to_string(well.debug_cost_counter_));
    }




    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computeWellRatesAtBhpLimit(const Simulator& ebosSimulator,
                               const std::vector<Scalar>& B_avg,
                               std::vector<double>& well_flux,
                               Opm::DeferredLogger& deferred_logger) const
    {
        if (well_ecl_.isInjector()) {
            const auto controls = well_ecl_.injectionControls(ebosSimulator.vanguard().summaryState());
            computeWellRatesWithBhp(ebosSimulator, B_avg, controls.bhp_limit, well_flux, deferred_logger);
        } else {
            const auto controls = well_ecl_.productionControls(ebosSimulator.vanguard().summaryState());
            computeWellRatesWithBhp(ebosSimulator, B_avg, controls.bhp_limit, well_flux, deferred_logger);
        }
    }




    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computeWellRatesWithBhp(const Simulator& ebosSimulator,
                            const std::vector<Scalar>& B_avg,
                            const Scalar bhp,
                            std::vector<double>& well_flux,
                            Opm::DeferredLogger& deferred_logger) const
    {
        // creating a copy of the well itself, to avoid messing up the explicit informations
        // during this copy, the only information not copied properly is the well controls
        MultisegmentWell<TypeTag> well_copy(*this);
        well_copy.debug_cost_counter_ = 0;

        // store a copy of the well state, we don't want to update the real well state
        WellState well_state_copy = ebosSimulator.problem().wellModel().wellState();

        // Get the current controls.
        const auto& summary_state = ebosSimulator.vanguard().summaryState();
        auto inj_controls = well_copy.well_ecl_.isInjector()
            ? well_copy.well_ecl_.injectionControls(summary_state)
            : Well::InjectionControls(0);
        auto prod_controls = well_copy.well_ecl_.isProducer()
            ? well_copy.well_ecl_.productionControls(summary_state) :
            Well::ProductionControls(0);

        //  Set current control to bhp, and bhp value in state, modify bhp limit in control object.
        if (well_copy.well_ecl_.isInjector()) {
            inj_controls.bhp_limit = bhp;
            well_state_copy.currentInjectionControls()[index_of_well_] = Well::InjectorCMode::BHP;
        } else {
            prod_controls.bhp_limit = bhp;
            well_state_copy.currentProductionControls()[index_of_well_] = Well::ProducerCMode::BHP;
        }
        well_state_copy.bhp()[well_copy.index_of_well_] = bhp;

        well_copy.updatePrimaryVariables(well_state_copy, deferred_logger);
        well_copy.initPrimaryVariablesEvaluation();
        const double dt = ebosSimulator.timeStepSize();
        // iterate to get a solution at the given bhp.
        well_copy.iterateWellEquations(ebosSimulator, B_avg, dt, inj_controls, prod_controls, well_state_copy, deferred_logger);

        // compute the potential and store in the flux vector.
        well_flux.clear();
        const int np = number_of_phases_;
        well_flux.resize(np, 0.0);
        for (int compIdx = 0; compIdx < num_components_; ++compIdx) {
            const EvalWell rate = well_copy.getSegmentRate(0, compIdx);
            well_flux[ebosCompIdxToFlowCompIdx(compIdx)] = rate.value();
        }
        debug_cost_counter_ += well_copy.debug_cost_counter_;
    }



    template<typename TypeTag>
    std::vector<double>
    MultisegmentWell<TypeTag>::
    computeWellPotentialWithTHP(const Simulator& ebos_simulator,
                                const std::vector<Scalar>& B_avg,
                                Opm::DeferredLogger& deferred_logger) const
    {
        std::vector<double> potentials(number_of_phases_, 0.0);
        const auto& summary_state = ebos_simulator.vanguard().summaryState();

        const auto& well = well_ecl_;
        if (well.isInjector()){
            auto bhp_at_thp_limit = computeBhpAtThpLimitInj(ebos_simulator, B_avg, summary_state, deferred_logger);
            if (bhp_at_thp_limit) {
                const auto& controls = well_ecl_.injectionControls(summary_state);
                const double bhp = std::min(*bhp_at_thp_limit, controls.bhp_limit);
                computeWellRatesWithBhp(ebos_simulator, B_avg, bhp, potentials, deferred_logger);
                deferred_logger.debug("Converged thp based potential calculation for well "
                                      + name() + ", at bhp = " + std::to_string(bhp));
            } else {
                deferred_logger.warning("FAILURE_GETTING_CONVERGED_POTENTIAL",
                                        "Failed in getting converged thp based potential calculation for well "
                                        + name() + ". Instead the bhp based value is used");
                const auto& controls = well_ecl_.injectionControls(summary_state);
                const double bhp = controls.bhp_limit;
                computeWellRatesWithBhp(ebos_simulator, B_avg, bhp, potentials, deferred_logger);
            }
        } else {
            auto bhp_at_thp_limit = computeBhpAtThpLimitProd(ebos_simulator, B_avg, summary_state, deferred_logger);
            if (bhp_at_thp_limit) {
                const auto& controls = well_ecl_.productionControls(summary_state);
                const double bhp = std::max(*bhp_at_thp_limit, controls.bhp_limit);
                computeWellRatesWithBhp(ebos_simulator, B_avg, bhp, potentials, deferred_logger);
                deferred_logger.debug("Converged thp based potential calculation for well "
                                      + name() + ", at bhp = " + std::to_string(bhp));
            } else {
                deferred_logger.warning("FAILURE_GETTING_CONVERGED_POTENTIAL",
                                        "Failed in getting converged thp based potential calculation for well "
                                        + name() + ". Instead the bhp based value is used");
                const auto& controls = well_ecl_.productionControls(summary_state);
                const double bhp = controls.bhp_limit;
                computeWellRatesWithBhp(ebos_simulator, B_avg, bhp, potentials, deferred_logger);
            }
        }

        return potentials;
    }



    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updatePrimaryVariables(const WellState& well_state, Opm::DeferredLogger& /* deferred_logger */) const
    {
        // TODO: to test using rate conversion coefficients to see if it will be better than
        // this default one

        const Well& well = Base::wellEcl();

        // the index of the top segment in the WellState
        const int top_segment_index = well_state.topSegmentIndex(index_of_well_);
        const std::vector<double>& segment_rates = well_state.segRates();
        const PhaseUsage& pu = phaseUsage();

        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            // calculate the total rate for each segment
            double total_seg_rate = 0.0;
            const int seg_index = top_segment_index + seg;
            // the segment pressure
            primary_variables_[seg][SPres] = well_state.segPress()[seg_index];
            // TODO: under what kind of circustances, the following will be wrong?
            // the definition of g makes the gas phase is always the last phase
            for (int p = 0; p < number_of_phases_; p++) {
                total_seg_rate += scalingFactor(p) * segment_rates[number_of_phases_ * seg_index + p];
            }

            primary_variables_[seg][GTotal] = total_seg_rate;
            if (std::abs(total_seg_rate) > 0.) {
                if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                    const int water_pos = pu.phase_pos[Water];
                    primary_variables_[seg][WFrac] = scalingFactor(water_pos) * segment_rates[number_of_phases_ * seg_index + water_pos] / total_seg_rate;
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    const int gas_pos = pu.phase_pos[Gas];
                    primary_variables_[seg][GFrac] = scalingFactor(gas_pos) * segment_rates[number_of_phases_ * seg_index + gas_pos] / total_seg_rate;
                }
            } else { // total_seg_rate == 0
                if (this->isInjector()) {
                    // only single phase injection handled
                    auto phase = well.getInjectionProperties().injectorType;

                    if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                        if (phase == Well::InjectorType::WATER) {
                            primary_variables_[seg][WFrac] = 1.0;
                        } else {
                            primary_variables_[seg][WFrac] = 0.0;
                        }
                    }

                    if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                        if (phase == Well::InjectorType::GAS) {
                            primary_variables_[seg][GFrac] = 1.0;
                        } else {
                            primary_variables_[seg][GFrac] = 0.0;
                        }
                    }

                } else if (this->isProducer()) { // producers
                    if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                        primary_variables_[seg][WFrac] = 1.0 / number_of_phases_;
                    }

                    if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                        primary_variables_[seg][GFrac] = 1.0 / number_of_phases_;
                    }
                }
            }
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    recoverSolutionWell(const BVector& x, BVectorWell& xw) const
    {
        BVectorWell resWell = resWell_;
        // resWell = resWell - B * x
        duneB_.mmv(x, resWell);
        // xw = D^-1 * resWell
        xw = mswellhelpers::invDXDirect(duneD_, resWell);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    solveEqAndUpdateWellState(WellState& well_state, Opm::DeferredLogger& deferred_logger)
    {
        // We assemble the well equations, then we check the convergence,
        // which is why we do not put the assembleWellEq here.
        const BVectorWell dx_well = mswellhelpers::invDXDirect(duneD_, resWell_);

        updateWellState(dx_well, well_state, deferred_logger);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computePerfCellPressDiffs(const Simulator& ebosSimulator)
    {
        for (int perf = 0; perf < number_of_perforations_; ++perf) {

            std::vector<double> kr(number_of_phases_, 0.0);
            std::vector<double> density(number_of_phases_, 0.0);

            const int cell_idx = well_cells_[perf];
            const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
            const auto& fs = intQuants.fluidState();

            double sum_kr = 0.;

            const PhaseUsage& pu = phaseUsage();
            if (pu.phase_used[Water]) {
                const int water_pos = pu.phase_pos[Water];
                kr[water_pos] = intQuants.relativePermeability(FluidSystem::waterPhaseIdx).value();
                sum_kr += kr[water_pos];
                density[water_pos] = fs.density(FluidSystem::waterPhaseIdx).value();
            }

            if (pu.phase_used[Oil]) {
                const int oil_pos = pu.phase_pos[Oil];
                kr[oil_pos] = intQuants.relativePermeability(FluidSystem::oilPhaseIdx).value();
                sum_kr += kr[oil_pos];
                density[oil_pos] = fs.density(FluidSystem::oilPhaseIdx).value();
            }

            if (pu.phase_used[Gas]) {
                const int gas_pos = pu.phase_pos[Gas];
                kr[gas_pos] = intQuants.relativePermeability(FluidSystem::gasPhaseIdx).value();
                sum_kr += kr[gas_pos];
                density[gas_pos] = fs.density(FluidSystem::gasPhaseIdx).value();
            }

            assert(sum_kr != 0.);

            // calculate the average density
            double average_density = 0.;
            for (int p = 0; p < number_of_phases_; ++p) {
                average_density += kr[p] * density[p];
            }
            average_density /= sum_kr;

            cell_perforation_pressure_diffs_[perf] = gravity_ * average_density * cell_perforation_depth_diffs_[perf];
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computeInitialSegmentFluids(const Simulator& ebos_simulator)
    {
        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            // TODO: trying to reduce the times for the surfaceVolumeFraction calculation
            const double surface_volume = getSegmentSurfaceVolume(ebos_simulator, seg).value();
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                segment_fluid_initial_[seg][comp_idx] = surface_volume * surfaceVolumeFraction(seg, comp_idx).value();
            }
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updateWellState(const BVectorWell& dwells,
                    WellState& well_state,
                    Opm::DeferredLogger& deferred_logger,
                    const double relaxation_factor) const
    {
        const double dFLimit = param_.dwell_fraction_max_;
        const double max_pressure_change = param_.max_pressure_change_ms_wells_;
        const std::vector<std::array<double, numWellEq> > old_primary_variables = primary_variables_;

        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                const int sign = dwells[seg][WFrac] > 0. ? 1 : -1;
                // const double dx_limited = sign * std::min(std::abs(dwells[seg][WFrac]), relaxation_factor * dFLimit);
                const double dx_limited = sign * std::min(std::abs(dwells[seg][WFrac]) * relaxation_factor, dFLimit);
                primary_variables_[seg][WFrac] = old_primary_variables[seg][WFrac] - dx_limited;
            }

            if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const int sign = dwells[seg][GFrac] > 0. ? 1 : -1;
                // const double dx_limited = sign * std::min(std::abs(dwells[seg][GFrac]), relaxation_factor * dFLimit);
                const double dx_limited = sign * std::min(std::abs(dwells[seg][GFrac]) * relaxation_factor, dFLimit);
                primary_variables_[seg][GFrac] = old_primary_variables[seg][GFrac] - dx_limited;
            }

            // handling the overshooting or undershooting of the fractions
            processFractions(seg);

            // update the segment pressure
            {
                const int sign = dwells[seg][SPres] > 0.? 1 : -1;
                const double dx_limited = sign * std::min(std::abs(dwells[seg][SPres]), relaxation_factor * max_pressure_change);
                // const double dx_limited = sign * std::min(std::abs(dwells[seg][SPres]) * relaxation_factor, max_pressure_change);
                primary_variables_[seg][SPres] = std::max( old_primary_variables[seg][SPres] - dx_limited, 1e5);
            }

            // update the total rate // TODO: should we have a limitation of the total rate change?
            {
                primary_variables_[seg][GTotal] = old_primary_variables[seg][GTotal] - relaxation_factor * dwells[seg][GTotal];

                // make sure that no injector produce and no producer inject
                if (seg == 0) {
                    if (this->isInjector()) {
                        primary_variables_[seg][GTotal] = std::max( primary_variables_[seg][GTotal], 0.0);
                    } else {
                        primary_variables_[seg][GTotal] = std::min( primary_variables_[seg][GTotal], 0.0);
                    }
                }
            }

        }

        updateWellStateFromPrimaryVariables(well_state, deferred_logger);
        Base::calculateReservoirRates(well_state);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    calculateExplicitQuantities(const Simulator& ebosSimulator,
                                const WellState& well_state,
                                Opm::DeferredLogger& deferred_logger)
    {
        updatePrimaryVariables(well_state, deferred_logger);
        initPrimaryVariablesEvaluation();
        computePerfCellPressDiffs(ebosSimulator);
        computeInitialSegmentFluids(ebosSimulator);
    }





    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    addWellContributions(SparseMatrixAdapter& /* jacobian */) const
    {
        OPM_THROW(std::runtime_error, "addWellContributions is not supported by multisegment well yet");
    }





    template <typename TypeTag>
    const WellSegments&
    MultisegmentWell<TypeTag>::
    segmentSet() const
    {
        return well_ecl_.getSegments();
    }





    template <typename TypeTag>
    int
    MultisegmentWell<TypeTag>::
    numberOfSegments() const
    {
        return segmentSet().size();
    }





    template <typename TypeTag>
    int
    MultisegmentWell<TypeTag>::
    numberOfPerforations() const
    {
        return segmentSet().number_of_perforations_;
    }





    template <typename TypeTag>
    WellSegments::CompPressureDrop
    MultisegmentWell<TypeTag>::
    compPressureDrop() const
    {
        return segmentSet().compPressureDrop();
    }





    template <typename TypeTag>
    WellSegments::MultiPhaseModel
    MultisegmentWell<TypeTag>::
    multiphaseModel() const
    {
        return segmentSet().multiPhaseModel();
    }





    template <typename TypeTag>
    int
    MultisegmentWell<TypeTag>::
    segmentNumberToIndex(const int segment_number) const
    {
        return segmentSet().segmentNumberToIndex(segment_number);
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    volumeFraction(const int seg, const unsigned compIdx) const
    {

        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) && compIdx == Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx)) {
            return primary_variables_evaluation_[seg][WFrac];
        }

        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) && compIdx == Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx)) {
            return primary_variables_evaluation_[seg][GFrac];
        }

        // Oil fraction
        EvalWell oil_fraction = 1.0;
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
            oil_fraction -= primary_variables_evaluation_[seg][WFrac];
        }

        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            oil_fraction -= primary_variables_evaluation_[seg][GFrac];
        }
        /* if (has_solvent) {
            oil_fraction -= primary_variables_evaluation_[seg][SFrac];
        } */
        return oil_fraction;
    }




    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    volumeFractionScaled(const int seg, const int comp_idx) const
    {
        // For reservoir rate control, the distr in well control is used for the
        // rate conversion coefficients. For the injection well, only the distr of the injection
        // phase is not zero.
        const double scale = scalingFactor(ebosCompIdxToFlowCompIdx(comp_idx));
        if (scale > 0.) {
            return volumeFraction(seg, comp_idx) / scale;
        }

        return volumeFraction(seg, comp_idx);
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    surfaceVolumeFraction(const int seg, const int comp_idx) const
    {
        EvalWell sum_volume_fraction_scaled = 0.;
        for (int idx = 0; idx < num_components_; ++idx) {
            sum_volume_fraction_scaled += volumeFractionScaled(seg, idx);
        }

        assert(sum_volume_fraction_scaled.value() != 0.);

        return volumeFractionScaled(seg, comp_idx) / sum_volume_fraction_scaled;
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computePerfRatePressure(const IntensiveQuantities& int_quants,
                            const std::vector<EvalWell>& mob_perfcells,
                            const int seg,
                            const int perf,
                            const EvalWell& segment_pressure,
                            const bool& allow_cf,
                            std::vector<EvalWell>& cq_s,
                            EvalWell& perf_press,
                            double& perf_dis_gas_rate,
                            double& perf_vap_oil_rate,
                            Opm::DeferredLogger& deferred_logger) const

    {
        std::vector<EvalWell> cmix_s(num_components_, 0.0);

        // the composition of the components inside wellbore
        for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
            cmix_s[comp_idx] = surfaceVolumeFraction(seg, comp_idx);
        }

        const auto& fs = int_quants.fluidState();

        const EvalWell pressure_cell = extendEval(fs.pressure(FluidSystem::oilPhaseIdx));
        const EvalWell rs = extendEval(fs.Rs());
        const EvalWell rv = extendEval(fs.Rv());

        // not using number_of_phases_ because of solvent
        std::vector<EvalWell> b_perfcells(num_components_, 0.0);

        for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }

            const unsigned compIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
            b_perfcells[compIdx] = extendEval(fs.invB(phaseIdx));
        }

        // pressure difference between the segment and the perforation
        const EvalWell perf_seg_press_diff = gravity_ * segment_densities_[seg] * perforation_segment_depth_diffs_[perf];
        // pressure difference between the perforation and the grid cell
        const double cell_perf_press_diff = cell_perforation_pressure_diffs_[perf];

        perf_press = pressure_cell - cell_perf_press_diff;
        // Pressure drawdown (also used to determine direction of flow)
        // TODO: not 100% sure about the sign of the seg_perf_press_diff
        const EvalWell drawdown = perf_press - (segment_pressure + perf_seg_press_diff);

        // producing perforations
        if ( drawdown > 0.0) {
            // Do nothing is crossflow is not allowed
            if (!allow_cf && this->isInjector()) {
                return;
            }

            // compute component volumetric rates at standard conditions
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                const EvalWell cq_p = - well_index_[perf] * (mob_perfcells[comp_idx] * drawdown);
                cq_s[comp_idx] = b_perfcells[comp_idx] * cq_p;
            }

            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                const EvalWell cq_s_oil = cq_s[oilCompIdx];
                const EvalWell cq_s_gas = cq_s[gasCompIdx];
                cq_s[gasCompIdx] += rs * cq_s_oil;
                cq_s[oilCompIdx] += rv * cq_s_gas;
            }
        } else { // injecting perforations
            // Do nothing if crossflow is not allowed
            if (!allow_cf && this->isProducer()) {
                return;
            }

            // for injecting perforations, we use total mobility
            EvalWell total_mob = mob_perfcells[0];
            for (int comp_idx = 1; comp_idx < num_components_; ++comp_idx) {
                total_mob += mob_perfcells[comp_idx];
            }

            // injection perforations total volume rates
            const EvalWell cqt_i = - well_index_[perf] * (total_mob * drawdown);

            // compute volume ratio between connection and at standard conditions
            EvalWell volume_ratio = 0.0;
            if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
                volume_ratio += cmix_s[waterCompIdx] / b_perfcells[waterCompIdx];
            }

            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);

                // Incorporate RS/RV factors if both oil and gas active
                // TODO: not sure we use rs rv from the perforation cells when handling injecting perforations
                // basically, for injecting perforations, the wellbore is the upstreaming side.
                const EvalWell d = 1.0 - rv * rs;

                if (d.value() == 0.0) {
                    OPM_DEFLOG_THROW(Opm::NumericalIssue, "Zero d value obtained for well " << name() << " during flux calcuation"
                                                  << " with rs " << rs << " and rv " << rv, deferred_logger);
                }

                const EvalWell tmp_oil = (cmix_s[oilCompIdx] - rv * cmix_s[gasCompIdx]) / d;
                volume_ratio += tmp_oil / b_perfcells[oilCompIdx];

                const EvalWell tmp_gas = (cmix_s[gasCompIdx] - rs * cmix_s[oilCompIdx]) / d;
                volume_ratio += tmp_gas / b_perfcells[gasCompIdx];
            } else { // not having gas and oil at the same time
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                    volume_ratio += cmix_s[oilCompIdx] / b_perfcells[oilCompIdx];
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                    volume_ratio += cmix_s[gasCompIdx] / b_perfcells[gasCompIdx];
                }
            }
            // injecting connections total volumerates at standard conditions
            EvalWell cqt_is = cqt_i / volume_ratio;
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                cq_s[comp_idx] = cmix_s[comp_idx] * cqt_is;
            }
        } // end for injection perforations

        // calculating the perforation solution gas rate and solution oil rates
        if (this->isProducer()) {
            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                // TODO: the formulations here remain to be tested with cases with strong crossflow through production wells
                // s means standard condition, r means reservoir condition
                // q_os = q_or * b_o + rv * q_gr * b_g
                // q_gs = q_gr * g_g + rs * q_or * b_o
                // d = 1.0 - rs * rv
                // q_or = 1 / (b_o * d) * (q_os - rv * q_gs)
                // q_gr = 1 / (b_g * d) * (q_gs - rs * q_os)

                const double d = 1.0 - rv.value() * rs.value();
                // vaporized oil into gas
                // rv * q_gr * b_g = rv * (q_gs - rs * q_os) / d
                perf_vap_oil_rate = rv.value() * (cq_s[gasCompIdx].value() - rs.value() * cq_s[oilCompIdx].value()) / d;
                // dissolved of gas in oil
                // rs * q_or * b_o = rs * (q_os - rv * q_gs) / d
                perf_dis_gas_rate = rs.value() * (cq_s[oilCompIdx].value() - rv.value() * cq_s[gasCompIdx].value()) / d;
            }
        }
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    extendEval(const Eval& in) const
    {
        EvalWell out = 0.0;
        out.setValue(in.value());
        for(int eq_idx = 0; eq_idx < numEq;++eq_idx) {
            out.setDerivative(eq_idx, in.derivative(eq_idx));
        }
        return out;
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    computeSegmentFluidProperties(const Simulator& ebosSimulator)
    {
        // TODO: the concept of phases and components are rather confusing in this function.
        // needs to be addressed sooner or later.

        // get the temperature for later use. It is only useful when we are not handling
        // thermal related simulation
        // basically, it is a single value for all the segments

        EvalWell temperature;
        // not sure how to handle the pvt region related to segment
        // for the current approach, we use the pvt region of the first perforated cell
        // although there are some text indicating using the pvt region of the lowest
        // perforated cell
        // TODO: later to investigate how to handle the pvt region
        int pvt_region_index;
        {
            // using the first perforated cell
            const int cell_idx = well_cells_[0];
            const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
            const auto& fs = intQuants.fluidState();
            temperature.setValue(fs.temperature(FluidSystem::oilPhaseIdx).value());
            pvt_region_index = fs.pvtRegionIndex();
        }

        std::vector<double> surf_dens(num_components_);
        // Surface density.
        for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
            if (!FluidSystem::phaseIsActive(phaseIdx)) {
                continue;
            }

            const unsigned compIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
            surf_dens[compIdx] = FluidSystem::referenceDensity( phaseIdx, pvt_region_index );
        }

        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            // the compostion of the components inside wellbore under surface condition
            std::vector<EvalWell> mix_s(num_components_, 0.0);
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                mix_s[comp_idx] = surfaceVolumeFraction(seg, comp_idx);
            }

            std::vector<EvalWell> b(num_components_, 0.0);
            std::vector<EvalWell> visc(num_components_, 0.0);

            const EvalWell seg_pressure = getSegmentPressure(seg);
            if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
                b[waterCompIdx] =
                    FluidSystem::waterPvt().inverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                visc[waterCompIdx] =
                    FluidSystem::waterPvt().viscosity(pvt_region_index, temperature, seg_pressure);
            }

            EvalWell rv(0.0);
            // gas phase
            if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                    const EvalWell rvmax = FluidSystem::gasPvt().saturatedOilVaporizationFactor(pvt_region_index, temperature, seg_pressure);
                    if (mix_s[oilCompIdx] > 0.0) {
                        if (mix_s[gasCompIdx] > 0.0) {
                            rv = mix_s[oilCompIdx] / mix_s[gasCompIdx];
                        }

                        if (rv > rvmax) {
                            rv = rvmax;
                        }
                        b[gasCompIdx] =
                            FluidSystem::gasPvt().inverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure, rv);
                        visc[gasCompIdx] =
                            FluidSystem::gasPvt().viscosity(pvt_region_index, temperature, seg_pressure, rv);
                    } else { // no oil exists
                        b[gasCompIdx] =
                            FluidSystem::gasPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                        visc[gasCompIdx] =
                            FluidSystem::gasPvt().saturatedViscosity(pvt_region_index, temperature, seg_pressure);
                    }
                } else { // no Liquid phase
                    // it is the same with zero mix_s[Oil]
                    b[gasCompIdx] =
                        FluidSystem::gasPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                    visc[gasCompIdx] =
                        FluidSystem::gasPvt().saturatedViscosity(pvt_region_index, temperature, seg_pressure);
                }
            }

            EvalWell rs(0.0);
            // oil phase
            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                    const EvalWell rsmax = FluidSystem::oilPvt().saturatedGasDissolutionFactor(pvt_region_index, temperature, seg_pressure);
                    if (mix_s[gasCompIdx] > 0.0) {
                        if (mix_s[oilCompIdx] > 0.0) {
                            rs = mix_s[gasCompIdx] / mix_s[oilCompIdx];
                        }

                        if (rs > rsmax) {
                            rs = rsmax;
                        }
                        b[oilCompIdx] =
                            FluidSystem::oilPvt().inverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure, rs);
                        visc[oilCompIdx] =
                            FluidSystem::oilPvt().viscosity(pvt_region_index, temperature, seg_pressure, rs);
                    } else { // no oil exists
                        b[oilCompIdx] =
                            FluidSystem::oilPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                        visc[oilCompIdx] =
                            FluidSystem::oilPvt().saturatedViscosity(pvt_region_index, temperature, seg_pressure);
                    }
                } else { // no Liquid phase
                    // it is the same with zero mix_s[Oil]
                    b[oilCompIdx] =
                        FluidSystem::oilPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                    visc[oilCompIdx] =
                        FluidSystem::oilPvt().saturatedViscosity(pvt_region_index, temperature, seg_pressure);
                }
            }

            segment_phase_viscosities_[seg] = visc;

            std::vector<EvalWell> mix(mix_s);
            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);

                const EvalWell d = 1.0 - rs * rv;

                if (rs != 0.0) { // rs > 0.0?
                    mix[gasCompIdx] = (mix_s[gasCompIdx] - mix_s[oilCompIdx] * rs) / d;
                }
                if (rv != 0.0) { // rv > 0.0?
                    mix[oilCompIdx] = (mix_s[oilCompIdx] - mix_s[gasCompIdx] * rv) / d;
                }
            }

            EvalWell volrat(0.0);
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                volrat += mix[comp_idx] / b[comp_idx];
            }

            segment_viscosities_[seg] = 0.;
            // calculate the average viscosity
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                const EvalWell fraction =  mix[comp_idx] / b[comp_idx] / volrat;
                // TODO: a little more work needs to be done to handle the negative fractions here
                segment_phase_fractions_[seg][comp_idx] = fraction; // >= 0.0 ? fraction : 0.0;
                segment_viscosities_[seg] += visc[comp_idx] * segment_phase_fractions_[seg][comp_idx];
            }

            EvalWell density(0.0);
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                density += surf_dens[comp_idx] * mix_s[comp_idx];
            }
            segment_densities_[seg] = density / volrat;

            // calculate the mass rates
            // TODO: for now, we are not considering the upwinding for this amount
            // since how to address the fact that the derivatives is not trivial for now
            // and segment_mass_rates_ goes a long way with the frictional pressure loss
            // and accelerational pressure loss, which needs some work to handle
            segment_mass_rates_[seg] = 0.;
            for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                const EvalWell rate = getSegmentRate(seg, comp_idx);
                segment_mass_rates_[seg] += rate * surf_dens[comp_idx];
            }

            segment_reservoir_volume_rates_[seg] = segment_mass_rates_[seg] / segment_densities_[seg];
        }
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getSegmentPressure(const int seg) const
    {
        return primary_variables_evaluation_[seg][SPres];
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getSegmentRate(const int seg,
                   const int comp_idx) const
    {
        return primary_variables_evaluation_[seg][GTotal] * volumeFractionScaled(seg, comp_idx);
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getSegmentRateUpwinding(const int seg,
                            const size_t comp_idx) const
    {
        const int seg_upwind = upwinding_segments_[seg];
        // the result will contain the derivative with resepct to GTotal in segment seg,
        // and the derivatives with respect to WFrac GFrac in segment seg_upwind.
        // the derivative with respect to SPres should be zero.
        if (seg == 0 && this->isInjector()) {
            const Well& well = Base::wellEcl();
            auto phase = well.getInjectionProperties().injectorType;

            if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)
                    && Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx) == comp_idx
                    && phase == Well::InjectorType::WATER)
                return primary_variables_evaluation_[seg][GTotal] / scalingFactor(ebosCompIdxToFlowCompIdx(comp_idx));


            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)
                    && Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx) == comp_idx
                    && phase == Well::InjectorType::OIL)
                return primary_variables_evaluation_[seg][GTotal] / scalingFactor(ebosCompIdxToFlowCompIdx(comp_idx));

            if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)
                    && Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx) == comp_idx
                    && phase == Well::InjectorType::GAS)
                return primary_variables_evaluation_[seg][GTotal] / scalingFactor(ebosCompIdxToFlowCompIdx(comp_idx));

            return 0.0;
        }

        const EvalWell segment_rate = primary_variables_evaluation_[seg][GTotal] * volumeFractionScaled(seg_upwind, comp_idx);

        assert(segment_rate.derivative(SPres + numEq) == 0.);

        return segment_rate;
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getSegmentGTotal(const int seg) const
    {
        return primary_variables_evaluation_[seg][GTotal];
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    getMobility(const Simulator& ebosSimulator,
                const int perf,
                std::vector<EvalWell>& mob) const
    {
        // TODO: most of this function, if not the whole function, can be moved to the base class
        const int cell_idx = well_cells_[perf];
        assert (int(mob.size()) == num_components_);
        const auto& intQuants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
        const auto& materialLawManager = ebosSimulator.problem().materialLawManager();

        // either use mobility of the perforation cell or calcualte its own
        // based on passing the saturation table index
        const int satid = saturation_table_number_[perf] - 1;
        const int satid_elem = materialLawManager->satnumRegionIdx(cell_idx);
        if( satid == satid_elem ) { // the same saturation number is used. i.e. just use the mobilty from the cell

            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                mob[activeCompIdx] = extendEval(intQuants.mobility(phaseIdx));
            }
            // if (has_solvent) {
            //     mob[contiSolventEqIdx] = extendEval(intQuants.solventMobility());
            // }
        } else {

            const auto& paramsCell = materialLawManager->connectionMaterialLawParams(satid, cell_idx);
            Eval relativePerms[3] = { 0.0, 0.0, 0.0 };
            MaterialLaw::relativePermeabilities(relativePerms, paramsCell, intQuants.fluidState());

            // reset the satnumvalue back to original
            materialLawManager->connectionMaterialLawParams(satid_elem, cell_idx);

            // compute the mobility
            for (unsigned phaseIdx = 0; phaseIdx < FluidSystem::numPhases; ++phaseIdx) {
                if (!FluidSystem::phaseIsActive(phaseIdx)) {
                    continue;
                }

                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                mob[activeCompIdx] = extendEval(relativePerms[phaseIdx] / intQuants.fluidState().viscosity(phaseIdx));
            }

        }
    }




    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleControlEq(const WellState& well_state,
                      const Opm::Schedule& schedule,
                      const SummaryState& summaryState,
                      const Well::InjectionControls& inj_controls,
                      const Well::ProductionControls& prod_controls,
                      Opm::DeferredLogger& deferred_logger)
    {

        EvalWell control_eq(0.0);

        const auto& well = well_ecl_;
        const int well_index = index_of_well_;
        double efficiencyFactor = well.getEfficiencyFactor();

        if (wellIsStopped_) {
            control_eq = getSegmentGTotal(0);
        } else if (this->isInjector() ) {
            const Opm::Well::InjectorCMode& current = well_state.currentInjectionControls()[well_index];
            const auto& controls = inj_controls;

            Well::InjectorType injectorType = controls.injector_type;
            double scaling = 1.0;

            const auto& pu = phaseUsage();

            switch (injectorType) {
            case Well::InjectorType::WATER:
            {
                scaling = scalingFactor(pu.phase_pos[BlackoilPhases::Aqua]);
                break;
            }
            case Well::InjectorType::OIL:
            {
                scaling = scalingFactor(pu.phase_pos[BlackoilPhases::Liquid]);
                break;
            }
            case Well::InjectorType::GAS:
            {
                scaling = scalingFactor(pu.phase_pos[BlackoilPhases::Vapour]);
                break;
            }
            default:
                throw("Expected WATER, OIL or GAS as type for injectors " + well.name());
            }

            switch(current) {
            case Well::InjectorCMode::RATE:
            {
                control_eq = getSegmentGTotal(0) * efficiencyFactor / scaling - controls.surface_rate;
                break;
            }

            case Well::InjectorCMode::RESV:
            {
                std::vector<double> convert_coeff(number_of_phases_, 1.0);
                Base::rateConverter_.calcCoeff(/*fipreg*/ 0, Base::pvtRegionIdx_, convert_coeff);

                double coeff = 1.0;

                switch (injectorType) {
                case Well::InjectorType::WATER:
                {
                    coeff = convert_coeff[pu.phase_pos[BlackoilPhases::Aqua]];
                    break;
                }
                case Well::InjectorType::OIL:
                {
                    coeff = convert_coeff[pu.phase_pos[BlackoilPhases::Liquid]];
                    break;
                }
                case Well::InjectorType::GAS:
                {
                    coeff = convert_coeff[pu.phase_pos[BlackoilPhases::Vapour]];
                    break;
                }
                default:
                    throw("Expected WATER, OIL or GAS as type for injectors " + well.name());

                }

                control_eq = coeff*getSegmentGTotal(0)*efficiencyFactor / scaling - controls.reservoir_rate;
                break;
            }

            case Well::InjectorCMode::THP:
            {
                std::vector<EvalWell> rates(3, 0.);
                if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                    rates[ Water ] = getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx));
                }
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    rates[ Oil ] = getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    rates[ Gas ] = getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx));
                }
                control_eq = getSegmentPressure(0) - calculateBhpFromThp(rates, well, summaryState, deferred_logger);
                break;
            }

            case Well::InjectorCMode::BHP:
            {
                const auto& bhp = controls.bhp_limit;
                control_eq = getSegmentPressure(0) - bhp;
                break;
            }

            case Well::InjectorCMode::GRUP:
            {
                assert(well.isAvailableForGroupControl());
                const auto& group = schedule.getGroup( well.groupName(), current_step_ );
                assembleGroupInjectionControl(group, well_state, schedule, summaryState, controls.injector_type, control_eq, efficiencyFactor, deferred_logger);
                break;
            }

            case Well::InjectorCMode::CMODE_UNDEFINED:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "Well control must be specified for well "  + name(), deferred_logger);
            }
            }


        }
        //Producer
        else
        {
            const Well::ProducerCMode& current = well_state.currentProductionControls()[well_index];
            const auto& controls = prod_controls;

            switch (current) {
            case Well::ProducerCMode::ORAT:
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
                const EvalWell& rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
                control_eq = rate * efficiencyFactor - controls.oil_rate;
                break;
            }
            case Well::ProducerCMode::WRAT:
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
                const EvalWell& rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx));
                control_eq = rate * efficiencyFactor - controls.water_rate;
                break;
            }
            case Well::ProducerCMode::GRAT:
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));
                const EvalWell& rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx));
                control_eq = rate * efficiencyFactor - controls.gas_rate;
                break;

            }
            case Well::ProducerCMode::LRAT:
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
                assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
                EvalWell rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx))
                        -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
                control_eq =  rate * efficiencyFactor - controls.liquid_rate;
                break;
            }
            case Well::ProducerCMode::CRAT:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "CRAT control not supported " << name(), deferred_logger);
            }
            case Well::ProducerCMode::RESV:
            {
                EvalWell total_rate(0.); // reservoir rate
                std::vector<double> convert_coeff(number_of_phases_, 1.0);
                Base::rateConverter_.calcCoeff(/*fipreg*/ 0, Base::pvtRegionIdx_, convert_coeff);
                for (int phase = 0; phase < number_of_phases_; ++phase) {
                    total_rate += getSegmentRate(0, flowPhaseToEbosCompIdx(phase) ) * convert_coeff[phase];
                }

                if (controls.prediction_mode) {
                    control_eq = total_rate - controls.resv_rate;
                } else {
                    std::vector<double> hrates(number_of_phases_,0.);
                    const PhaseUsage& pu = phaseUsage();
                    if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                        hrates[pu.phase_pos[Water]] = controls.water_rate;
                    }
                    if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                        hrates[pu.phase_pos[Oil]] = controls.oil_rate;
                    }
                    if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                        hrates[pu.phase_pos[Gas]] = controls.gas_rate;
                    }
                    std::vector<double> hrates_resv(number_of_phases_,0.);
                    Base::rateConverter_.calcReservoirVoidageRates(/*fipreg*/ 0, Base::pvtRegionIdx_, hrates, hrates_resv);
                    double target = -std::accumulate(hrates_resv.begin(), hrates_resv.end(), 0.0);
                    control_eq = total_rate * efficiencyFactor - target;
                }
                break;
            }
            case Well::ProducerCMode::BHP:
            {
                control_eq = getSegmentPressure(0) - controls.bhp_limit;
                break;
            }
            case Well::ProducerCMode::THP:
            {
                std::vector<EvalWell> rates(3, 0.);
                if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                    rates[ Water ] = getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx));
                }
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    rates[ Oil ] = getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    rates[ Gas ] = getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx));
                }
                control_eq = getSegmentPressure(0) - calculateBhpFromThp(rates, well, summaryState, deferred_logger);
                break;
            }
            case Well::ProducerCMode::GRUP:
            {
                assert(well.isAvailableForGroupControl());

                const auto& group = schedule.getGroup( well.groupName(), current_step_ );
                assembleGroupProductionControl(group, well_state, schedule, summaryState, control_eq, efficiencyFactor, deferred_logger);
                break;
            }
            case Well::ProducerCMode::CMODE_UNDEFINED:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "Well control must be specified for well "  + name(), deferred_logger );
            }
            case Well::ProducerCMode::NONE:
            {
                OPM_DEFLOG_THROW(std::runtime_error, "Well control must be specified for well "  + name(), deferred_logger );
            }

            }
        }

        // using control_eq to update the matrix and residuals
        resWell_[0][SPres] = control_eq.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[0][0][SPres][pv_idx] = control_eq.derivative(pv_idx + numEq);
        }
    }


    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updateThp(WellState& well_state, Opm::DeferredLogger& deferred_logger) const
    {
        // When there is no vaild VFP table provided, we set the thp to be zero.
        if (!this->isVFPActive(deferred_logger) || this->wellIsStopped()) {
            well_state.thp()[index_of_well_] = 0.;
            return;
        }

        // the well is under other control types, we calculate the thp based on bhp and rates
        std::vector<double> rates(3, 0.0);

        const Opm::PhaseUsage& pu = phaseUsage();
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
            rates[ Water ] = well_state.wellRates()[index_of_well_ * number_of_phases_ + pu.phase_pos[ Water ] ];
        }
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
            rates[ Oil ] = well_state.wellRates()[index_of_well_ * number_of_phases_ + pu.phase_pos[ Oil ] ];
        }
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            rates[ Gas ] = well_state.wellRates()[index_of_well_ * number_of_phases_ + pu.phase_pos[ Gas ] ];
        }

        const double bhp = well_state.bhp()[index_of_well_];

        well_state.thp()[index_of_well_] = calculateThpFromBhp(rates, bhp, deferred_logger);

    }



    template<typename TypeTag>
    double
    MultisegmentWell<TypeTag>::
    calculateThpFromBhp(const std::vector<double>& rates,
                        const double bhp,
                        Opm::DeferredLogger& deferred_logger) const
    {
        assert(int(rates.size()) == 3); // the vfp related only supports three phases now.

        const double aqua = rates[Water];
        const double liquid = rates[Oil];
        const double vapour = rates[Gas];

        // pick the density in the top segment
        const double rho = segment_densities_[0].value();

        double thp = 0.0;
        if (this->isInjector()) {
            const int table_id = well_ecl_.vfp_table_number();
            const double vfp_ref_depth = vfp_properties_->getInj()->getTable(table_id)->getDatumDepth();
            const double dp = wellhelpers::computeHydrostaticCorrection(ref_depth_, vfp_ref_depth, rho, gravity_);

            thp = vfp_properties_->getInj()->thp(table_id, aqua, liquid, vapour, bhp + dp);
        }
        else if (this->isProducer()) {
            const int table_id = well_ecl_.vfp_table_number();
            const double alq = well_ecl_.alq_value();
            const double vfp_ref_depth = vfp_properties_->getProd()->getTable(table_id)->getDatumDepth();
            const double dp = wellhelpers::computeHydrostaticCorrection(ref_depth_, vfp_ref_depth, rho, gravity_);

            thp = vfp_properties_->getProd()->thp(table_id, aqua, liquid, vapour, bhp + dp, alq);
        }
        else {
            OPM_DEFLOG_THROW(std::logic_error, "Expected INJECTOR or PRODUCER well", deferred_logger);
        }

        return thp;
    }


    template<typename TypeTag>
    template<class ValueType>
    ValueType
    MultisegmentWell<TypeTag>::
    calculateBhpFromThp(const std::vector<ValueType>& rates,
                        const Well& well,
                        const SummaryState& summaryState,
                        Opm::DeferredLogger& deferred_logger) const
    {
        // TODO: when well is under THP control, the BHP is dependent on the rates,
        // the well rates is also dependent on the BHP, so it might need to do some iteration.
        // However, when group control is involved, change of the rates might impacts other wells
        // so iterations on a higher level will be required. Some investigation might be needed when
        // we face problems under THP control.

        assert(int(rates.size()) == 3); // the vfp related only supports three phases now.

        const ValueType aqua = rates[Water];
        const ValueType liquid = rates[Oil];
        const ValueType vapour = rates[Gas];

        // pick the density in the top layer
        // TODO: it is possible it should be a Evaluation
        const double rho = segment_densities_[0].value();

        if (well.isInjector() )
        {
            const auto& controls = well.injectionControls(summaryState);
            const double vfp_ref_depth = vfp_properties_->getInj()->getTable(controls.vfp_table_number)->getDatumDepth();
            const double dp = wellhelpers::computeHydrostaticCorrection(ref_depth_, vfp_ref_depth, rho, gravity_);
            return vfp_properties_->getInj()->bhp(controls.vfp_table_number, aqua, liquid, vapour, controls.thp_limit) - dp;
         }
         else if (well.isProducer()) {
             const auto& controls = well.productionControls(summaryState);
             const double vfp_ref_depth = vfp_properties_->getProd()->getTable(controls.vfp_table_number)->getDatumDepth();
             const double dp = wellhelpers::computeHydrostaticCorrection(ref_depth_, vfp_ref_depth, rho, gravity_);
             return vfp_properties_->getProd()->bhp(controls.vfp_table_number, aqua, liquid, vapour, controls.thp_limit, controls.alq_value) - dp;
         }
         else {
             OPM_DEFLOG_THROW(std::logic_error, "Expected INJECTOR or PRODUCER well", deferred_logger);
         }



    }

    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleGroupInjectionControl(const Group& group, const WellState& well_state, const Opm::Schedule& schedule, const SummaryState& summaryState, const Well::InjectorType& injectorType, EvalWell& control_eq, double efficiencyFactor, Opm::DeferredLogger& deferred_logger)
    {
        const auto& well = well_ecl_;
        const auto pu = phaseUsage();
        const Group::InjectionCMode& currentGroupControl = well_state.currentInjectionGroupControl(group.name());
        if (currentGroupControl == Group::InjectionCMode::FLD) {
            // Inject share of parents control
            const auto& parent = schedule.getGroup( group.parent(), current_step_ );
            if (group.getTransferGroupEfficiencyFactor())
                efficiencyFactor *= group.getGroupEfficiencyFactor();

            assembleGroupInjectionControl(parent, well_state, schedule, summaryState, injectorType, control_eq, efficiencyFactor, deferred_logger);
            return;
        }

        if (!group.isInjectionGroup() || currentGroupControl == Group::InjectionCMode::NONE) {
            // use bhp as control eq and let the updateControl code find a valid control
            const auto& controls = well.injectionControls(summaryState);
            control_eq = getSegmentPressure(0) - controls.bhp_limit;
            return;
        }

        const auto& groupcontrols = group.injectionControls(summaryState);

        int phasePos;
        Well::GuideRateTarget wellTarget;
        Group::GuideRateTarget groupTarget;
        double scaling = 1.0;

        switch (injectorType) {
        case Well::InjectorType::WATER:
        {
            phasePos = pu.phase_pos[BlackoilPhases::Aqua];
            wellTarget = Well::GuideRateTarget::WAT;
            groupTarget = Group::GuideRateTarget::WAT;
            scaling = scalingFactor(pu.phase_pos[BlackoilPhases::Aqua]);
            break;
        }
        case Well::InjectorType::OIL:
        {
            phasePos = pu.phase_pos[BlackoilPhases::Liquid];
            wellTarget = Well::GuideRateTarget::OIL;
            groupTarget = Group::GuideRateTarget::OIL;
            scaling = scalingFactor(pu.phase_pos[BlackoilPhases::Liquid]);
            break;
        }
        case Well::InjectorType::GAS:
        {
            phasePos = pu.phase_pos[BlackoilPhases::Vapour];
            wellTarget = Well::GuideRateTarget::GAS;
            groupTarget = Group::GuideRateTarget::GAS;
            scaling = scalingFactor(pu.phase_pos[BlackoilPhases::Vapour]);
            break;
        }
        default:
            throw("Expected WATER, OIL or GAS as type for injectors " + well.name());
        }

        const std::vector<double>& groupInjectionReductions = well_state.currentInjectionGroupReductionRates(group.name());
        double groupTargetReduction = groupInjectionReductions[phasePos];
        double fraction = wellGroupHelpers::wellFractionFromGuideRates(well, schedule, well_state, current_step_, Base::guide_rate_, wellTarget, /*isInjector*/true);
        wellGroupHelpers::accumulateGroupPotentialFractions(well.groupName(), group.name(), schedule, well_state, current_step_, phasePos, /*isInjector*/true, fraction);

        switch(currentGroupControl) {
        case Group::InjectionCMode::NONE:
        {
            // The NONE case is handled earlier
            assert(false);
            break;
        }
        case Group::InjectionCMode::RATE:
        {

            control_eq = getSegmentGTotal(0) / scaling - fraction * (groupcontrols.surface_max_rate / efficiencyFactor - groupTargetReduction);
            break;
        }
        case Group::InjectionCMode::RESV:
        {
            std::vector<double> convert_coeff(number_of_phases_, 1.0);
            Base::rateConverter_.calcCoeff(/*fipreg*/ 0, Base::pvtRegionIdx_, convert_coeff);
            double coeff = convert_coeff[phasePos];
            double target = std::max(0.0, (groupcontrols.resv_max_rate/coeff/efficiencyFactor - groupTargetReduction));
            control_eq = getSegmentGTotal(0) / scaling - fraction * target;
            break;
        }
        case Group::InjectionCMode::REIN:
        {
            double productionRate = well_state.currentInjectionREINRates(groupcontrols.reinj_group)[phasePos];
            productionRate /= efficiencyFactor;
            double target = std::max(0.0, (groupcontrols.target_reinj_fraction*productionRate - groupTargetReduction));
            control_eq = getSegmentGTotal(0) / scaling - fraction * target;
            break;
        }
        case Group::InjectionCMode::VREP:
        {
            std::vector<double> convert_coeff(number_of_phases_, 1.0);
            Base::rateConverter_.calcCoeff(/*fipreg*/ 0, Base::pvtRegionIdx_, convert_coeff);
            double coeff = convert_coeff[phasePos];
            double voidageRate = well_state.currentInjectionVREPRates(groupcontrols.voidage_group)*groupcontrols.target_void_fraction;

            double injReduction = 0.0;

            if (groupcontrols.phase != Phase::WATER)
                injReduction += groupInjectionReductions[pu.phase_pos[BlackoilPhases::Aqua]]*convert_coeff[pu.phase_pos[BlackoilPhases::Aqua]];

            if (groupcontrols.phase != Phase::OIL)
                injReduction += groupInjectionReductions[pu.phase_pos[BlackoilPhases::Liquid]]*convert_coeff[pu.phase_pos[BlackoilPhases::Liquid]];

            if (groupcontrols.phase != Phase::GAS)
                injReduction += groupInjectionReductions[pu.phase_pos[BlackoilPhases::Vapour]]*convert_coeff[pu.phase_pos[BlackoilPhases::Vapour]];

            voidageRate -= injReduction;

            voidageRate /= efficiencyFactor;

            double target = std::max(0.0, ( voidageRate/coeff - groupTargetReduction));
            control_eq = getSegmentGTotal(0) / scaling  - fraction * target;
            break;
        }
        case Group::InjectionCMode::FLD:
        {
            // The FLD case is handled earlier
            assert(false);
            break;
        }
        case Group::InjectionCMode::SALE:
        {
            // only for gas injectors
            assert (phasePos == pu.phase_pos[BlackoilPhases::Vapour]);

            // Gas injection rate = Total gas production rate + gas import rate - gas consumption rate - sales rate;
            double inj_rate = well_state.currentInjectionREINRates(group.name())[phasePos];
            if (schedule.gConSump(current_step_).has(group.name())) {
                const auto& gconsump = schedule.gConSump(current_step_).get(group.name(), summaryState);
                if (pu.phase_used[BlackoilPhases::Vapour]) {
                    inj_rate += gconsump.import_rate;
                    inj_rate -= gconsump.consumption_rate;
                }
            }
            const auto& gconsale = schedule.gConSale(current_step_).get(group.name(), summaryState);
            inj_rate -= gconsale.sales_target;

            inj_rate /= efficiencyFactor;
            double target = std::max(0.0, (inj_rate - groupTargetReduction));
            control_eq = getSegmentGTotal(0) /scaling - fraction * target;
            break;
        }

        default:
            OPM_DEFLOG_THROW(std::runtime_error, "Unvalid group control specified for group "  + well.groupName(), deferred_logger );
        }


    }




    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleGroupProductionControl(const Group& group, const WellState& well_state, const Opm::Schedule& schedule, const SummaryState& summaryState, EvalWell& control_eq, double efficiencyFactor, Opm::DeferredLogger& deferred_logger)
    {

        const auto& well = well_ecl_;
        const auto pu = phaseUsage();

        const Group::ProductionCMode& currentGroupControl = well_state.currentProductionGroupControl(group.name());

        if (currentGroupControl == Group::ProductionCMode::FLD ) {
            // Produce share of parents control
            const auto& parent = schedule.getGroup( group.parent(), current_step_ );
            if (group.getTransferGroupEfficiencyFactor())
                efficiencyFactor *= group.getGroupEfficiencyFactor();

            assembleGroupProductionControl(parent, well_state, schedule, summaryState, control_eq, efficiencyFactor, deferred_logger);
            return;
        }
        if (!group.isProductionGroup() || currentGroupControl == Group::ProductionCMode::NONE) {
            // use bhp as control eq and let the updateControl code find a vallied control
            const auto& controls = well.productionControls(summaryState);
            control_eq = getSegmentPressure(0) - controls.bhp_limit;
            return;
        }

        const auto& groupcontrols = group.productionControls(summaryState);
        const std::vector<double>& groupTargetReductions = well_state.currentProductionGroupReductionRates(group.name());

        switch(currentGroupControl) {
        case Group::ProductionCMode::NONE:
        {
            // The NONE case is handled earlier
            assert(false);
            break;
        }
        case Group::ProductionCMode::ORAT:
        {
            double groupTargetReduction = groupTargetReductions[pu.phase_pos[Oil]];
            double fraction = wellGroupHelpers::wellFractionFromGuideRates(well, schedule, well_state, current_step_, Base::guide_rate_, Well::GuideRateTarget::OIL, /*isInjector*/false);
            wellGroupHelpers::accumulateGroupFractions(well.groupName(), group.name(), schedule, well_state, current_step_, Base::guide_rate_, Group::GuideRateTarget::OIL, /*isInjector*/false, fraction);

            const double rate_target = std::max(0.0, groupcontrols.oil_target / efficiencyFactor - groupTargetReduction);
            assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
            const EvalWell& rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
            control_eq = rate - fraction * rate_target;
            break;
        }
        case Group::ProductionCMode::WRAT:
        {
            double groupTargetReduction = groupTargetReductions[pu.phase_pos[Water]];
            double fraction = wellGroupHelpers::wellFractionFromGuideRates(well, schedule, well_state, current_step_, Base::guide_rate_, Well::GuideRateTarget::WAT, /*isInjector*/false);
            wellGroupHelpers::accumulateGroupFractions(well.groupName(), group.name(), schedule, well_state, current_step_, Base::guide_rate_, Group::GuideRateTarget::WAT, /*isInjector*/false, fraction);

            const double rate_target = std::max(0.0, groupcontrols.water_target / efficiencyFactor - groupTargetReduction);
            assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
            const EvalWell& rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx));
            control_eq = rate - fraction * rate_target;
            break;
        }
        case Group::ProductionCMode::GRAT:
        {
            double groupTargetReduction = groupTargetReductions[pu.phase_pos[Gas]];
            double fraction = wellGroupHelpers::wellFractionFromGuideRates(well, schedule, well_state, current_step_, Base::guide_rate_, Well::GuideRateTarget::GAS, /*isInjector*/false);
            wellGroupHelpers::accumulateGroupFractions(well.groupName(), group.name(), schedule, well_state, current_step_, Base::guide_rate_, Group::GuideRateTarget::GAS, /*isInjector*/false, fraction);
            const double rate_target = std::max(0.0, groupcontrols.gas_target / efficiencyFactor - groupTargetReduction);
            assert(FluidSystem::phaseIsActive(FluidSystem::gasCompIdx));
            const EvalWell& rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx));
            control_eq = rate - fraction * rate_target;
            break;
        }
        case Group::ProductionCMode::LRAT:
        {
            double groupTargetReduction = groupTargetReductions[pu.phase_pos[Oil]] + groupTargetReductions[pu.phase_pos[Water]];
            double fraction = wellGroupHelpers::wellFractionFromGuideRates(well, schedule, well_state, current_step_, Base::guide_rate_, Well::GuideRateTarget::LIQ, /*isInjector*/false);
            wellGroupHelpers::accumulateGroupFractions(well.groupName(), group.name(), schedule, well_state, current_step_, Base::guide_rate_, Group::GuideRateTarget::LIQ, /*isInjector*/false, fraction);

            const double rate_target = std::max(0.0, groupcontrols.liquid_target / efficiencyFactor - groupTargetReduction);
            assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));

            EvalWell rate = -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx))
                    -getSegmentRate(0, Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx));
            control_eq = rate - fraction * rate_target;
            break;
        }
        case Group::ProductionCMode::CRAT:
        {
            OPM_DEFLOG_THROW(std::runtime_error, "CRAT group control not implemented for producers", deferred_logger );
            break;
        }
        case Group::ProductionCMode::RESV:
        {
            OPM_DEFLOG_THROW(std::runtime_error, "RESV group control not implemented for producers", deferred_logger );
            break;
        }
        case Group::ProductionCMode::PRBL:
        {
            OPM_DEFLOG_THROW(std::runtime_error, "PRBL group control not implemented for producers", deferred_logger );
            break;
        }
        case Group::ProductionCMode::FLD:
        {
            // The FLD case is handled earlier
            assert(false);
            break;
        }
        default:
            OPM_DEFLOG_THROW(std::runtime_error, "Unvallied group control specified for group "  + well.groupName(), deferred_logger );
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assemblePressureEq(const int seg) const
    {
        assert(seg != 0); // not top segment

        // for top segment, the well control equation will be used.
        EvalWell pressure_equation = getSegmentPressure(seg);

        // we need to handle the pressure difference between the two segments
        // we only consider the hydrostatic pressure loss first
        pressure_equation -= getHydroPressureLoss(seg);

        if (frictionalPressureLossConsidered()) {
            pressure_equation -= getFrictionPressureLoss(seg);
        }

        resWell_[seg][SPres] = pressure_equation.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][seg][SPres][pv_idx] = pressure_equation.derivative(pv_idx + numEq);
        }

        // contribution from the outlet segment
        const int outlet_segment_index = segmentNumberToIndex(segmentSet()[seg].outletSegment());
        const EvalWell outlet_pressure = getSegmentPressure(outlet_segment_index);

        resWell_[seg][SPres] -= outlet_pressure.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][outlet_segment_index][SPres][pv_idx] = -outlet_pressure.derivative(pv_idx + numEq);
        }

        if (accelerationalPressureLossConsidered()) {
            handleAccelerationPressureLoss(seg);
        }
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getHydroPressureLoss(const int seg) const
    {
        return segment_densities_[seg] * gravity_ * segment_depth_diffs_[seg];
    }





    template <typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getFrictionPressureLoss(const int seg) const
    {
        const EvalWell mass_rate = segment_mass_rates_[seg];
        const EvalWell density = segment_densities_[seg];
        const EvalWell visc = segment_viscosities_[seg];
        const int outlet_segment_index = segmentNumberToIndex(segmentSet()[seg].outletSegment());
        const double length = segmentSet()[seg].totalLength() - segmentSet()[outlet_segment_index].totalLength();
        assert(length > 0.);
        const double roughness = segmentSet()[seg].roughness();
        const double area = segmentSet()[seg].crossArea();
        const double diameter = segmentSet()[seg].internalDiameter();

        const double sign = mass_rate < 0. ? 1.0 : - 1.0;

        return sign * mswellhelpers::frictionPressureLoss(length, diameter, area, roughness, density, mass_rate, visc);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    handleAccelerationPressureLoss(const int seg) const
    {
        // TODO: this pressure loss is not significant enough to be well tested yet.
        // handle the out velcocity head
        const double area = segmentSet()[seg].crossArea();
        const EvalWell mass_rate = segment_mass_rates_[seg];
        const EvalWell density = segment_densities_[seg];
        const EvalWell out_velocity_head = mswellhelpers::velocityHead(area, mass_rate, density);

        resWell_[seg][SPres] -= out_velocity_head.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][seg][SPres][pv_idx] -= out_velocity_head.derivative(pv_idx + numEq);
        }

        // calcuate the maximum cross-area among the segment and its inlet segments
        double max_area = area;
        for (const int inlet : segment_inlets_[seg]) {
            const double inlet_area = segmentSet()[inlet].crossArea();
            if (inlet_area > max_area) {
                max_area = inlet_area;
            }
        }

        // handling the velocity head of intlet segments
        for (const int inlet : segment_inlets_[seg]) {
            const EvalWell inlet_density = segment_densities_[inlet];
            const EvalWell inlet_mass_rate = segment_mass_rates_[inlet];
            const EvalWell inlet_velocity_head = mswellhelpers::velocityHead(area, inlet_mass_rate, inlet_density);
            resWell_[seg][SPres] += inlet_velocity_head.value();
            for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
                duneD_[seg][inlet][SPres][pv_idx] += inlet_velocity_head.derivative(pv_idx + numEq);
            }
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    processFractions(const int seg) const
    {
        const PhaseUsage& pu = phaseUsage();

        std::vector<double> fractions(number_of_phases_, 0.0);

        assert( FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) );
        const int oil_pos = pu.phase_pos[Oil];
        fractions[oil_pos] = 1.0;

        if ( FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) ) {
            const int water_pos = pu.phase_pos[Water];
            fractions[water_pos] = primary_variables_[seg][WFrac];
            fractions[oil_pos] -= fractions[water_pos];
        }

        if ( FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) ) {
            const int gas_pos = pu.phase_pos[Gas];
            fractions[gas_pos] = primary_variables_[seg][GFrac];
            fractions[oil_pos] -= fractions[gas_pos];
        }

        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
            const int water_pos = pu.phase_pos[Water];
            if (fractions[water_pos] < 0.0) {
                if ( FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) ) {
                    fractions[pu.phase_pos[Gas]] /= (1.0 - fractions[water_pos]);
                }
                fractions[oil_pos] /= (1.0 - fractions[water_pos]);
                fractions[water_pos] = 0.0;
            }
        }

        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            const int gas_pos = pu.phase_pos[Gas];
            if (fractions[gas_pos] < 0.0) {
                if ( FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) ) {
                    fractions[pu.phase_pos[Water]] /= (1.0 - fractions[gas_pos]);
                }
                fractions[oil_pos] /= (1.0 - fractions[gas_pos]);
                fractions[gas_pos] = 0.0;
            }
        }

        if (fractions[oil_pos] < 0.0) {
            if ( FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) ) {
                fractions[pu.phase_pos[Water]] /= (1.0 - fractions[oil_pos]);
            }
            if ( FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) ) {
                fractions[pu.phase_pos[Gas]] /= (1.0 - fractions[oil_pos]);
            }
            fractions[oil_pos] = 0.0;
        }

        if ( FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) ) {
            primary_variables_[seg][WFrac] = fractions[pu.phase_pos[Water]];
        }

        if ( FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) ) {
            primary_variables_[seg][GFrac] = fractions[pu.phase_pos[Gas]];
        }
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    checkWellOperability(const Simulator& /* ebos_simulator */,
                         const WellState& /* well_state */,
                         Opm::DeferredLogger& deferred_logger)
    {
        const bool checkOperability = EWOMS_GET_PARAM(TypeTag, bool, EnableWellOperabilityCheck);
        if (!checkOperability) {
            return;
        }

        // focusing on PRODUCER for now
        if (this->isInjector()) {
            return;
        }

        if (!this->underPredictionMode() ) {
            return;
        }

        const std::string msg = "Support of well operability checking for multisegment wells is not implemented "
                                "yet, checkWellOperability() for " + name() + " will do nothing";
        deferred_logger.warning("NO_OPERATABILITY_CHECKING_MS_WELLS", msg);
    }





    template <typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updateWellStateFromPrimaryVariables(WellState& well_state, Opm::DeferredLogger& deferred_logger) const
    {
        const PhaseUsage& pu = phaseUsage();
        assert( FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) );
        const int oil_pos = pu.phase_pos[Oil];

        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            std::vector<double> fractions(number_of_phases_, 0.0);
            fractions[oil_pos] = 1.0;

            if ( FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) ) {
                const int water_pos = pu.phase_pos[Water];
                fractions[water_pos] = primary_variables_[seg][WFrac];
                fractions[oil_pos] -= fractions[water_pos];
            }

            if ( FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) ) {
                const int gas_pos = pu.phase_pos[Gas];
                fractions[gas_pos] = primary_variables_[seg][GFrac];
                fractions[oil_pos] -= fractions[gas_pos];
            }

            // convert the fractions to be Q_p / G_total to calculate the phase rates
            for (int p = 0; p < number_of_phases_; ++p) {
                const double scale = scalingFactor(p);
                // for injection wells, there should only one non-zero scaling factor
                if (scale > 0.) {
                    fractions[p] /= scale;
                } else {
                    // this should only happens to injection wells
                    fractions[p] = 0.;
                }
            }

            // calculate the phase rates based on the primary variables
            const double g_total = primary_variables_[seg][GTotal];
            const int top_segment_index = well_state.topSegmentIndex(index_of_well_);
            for (int p = 0; p < number_of_phases_; ++p) {
                const double phase_rate = g_total * fractions[p];
                well_state.segRates()[(seg + top_segment_index) * number_of_phases_ + p] = phase_rate;
                if (seg == 0) { // top segment
                    well_state.wellRates()[index_of_well_ * number_of_phases_ + p] = phase_rate;
                }
            }

            // update the segment pressure
            well_state.segPress()[seg + top_segment_index] = primary_variables_[seg][SPres];
            if (seg == 0) { // top segment
                well_state.bhp()[index_of_well_] = well_state.segPress()[seg + top_segment_index];
            }
        }
        updateThp(well_state, deferred_logger);
    }




    template <typename TypeTag>
    bool
    MultisegmentWell<TypeTag>::
    frictionalPressureLossConsidered() const
    {
        // HF- and HFA needs to consider frictional pressure loss
        return (segmentSet().compPressureDrop() != WellSegments::CompPressureDrop::H__);
    }





    template <typename TypeTag>
    bool
    MultisegmentWell<TypeTag>::
    accelerationalPressureLossConsidered() const
    {
        return (segmentSet().compPressureDrop() == WellSegments::CompPressureDrop::HFA);
    }





    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    iterateWellEquations(const Simulator& ebosSimulator,
                         const std::vector<Scalar>& B_avg,
                         const double dt,
                         const Well::InjectionControls& inj_controls,
                         const Well::ProductionControls& prod_controls,
                         WellState& well_state,
                         Opm::DeferredLogger& deferred_logger)
    {
        const int max_iter_number = param_.max_inner_iter_ms_wells_;
        const WellState well_state0 = well_state;
        const std::vector<Scalar> residuals0 = getWellResiduals(B_avg);
        std::vector<std::vector<Scalar> > residual_history;
        std::vector<double> measure_history;
        int it = 0;
        // relaxation factor
        double relaxation_factor = 1.;
        const double min_relaxation_factor = 0.2;
        bool converged = false;
        int stagnate_count = 0;
        for (; it < max_iter_number; ++it, ++debug_cost_counter_) {

            assembleWellEqWithoutIteration(ebosSimulator, dt, inj_controls, prod_controls, well_state, deferred_logger);

            const BVectorWell dx_well = mswellhelpers::invDXDirect(duneD_, resWell_);


            const auto report = getWellConvergence(well_state, B_avg, deferred_logger);
            if (report.converged()) {
                converged = true;
                break;
            }

            residual_history.push_back(getWellResiduals(B_avg));
            measure_history.push_back(getResidualMeasureValue(well_state, residual_history[it], deferred_logger) );

            bool is_oscillate = false;
            bool is_stagnate = false;

            detectOscillations(measure_history, it, is_oscillate, is_stagnate);
            // TODO: maybe we should have more sophiscated strategy to recover the relaxation factor,
            // for example, to recover it to be bigger

            if (!is_stagnate) {
                stagnate_count = 0;
            }
            if (is_oscillate || is_stagnate) {
                // HACK!
                if (is_stagnate && relaxation_factor == min_relaxation_factor) {
                    // Still stagnating, terminate iterations if 5 iterations pass.
                    ++stagnate_count;
                    if (stagnate_count == 5) {
                        // break;
                    }
                } else {
                    stagnate_count = 0;
                }

                // a factor value to reduce the relaxation_factor
                const double reduction_mutliplier = 0.9;
                relaxation_factor = std::max(relaxation_factor * reduction_mutliplier, min_relaxation_factor);

                // debug output
                std::ostringstream sstr;
                if (is_stagnate) {
                    sstr << " well " << name() << " observes stagnation in inner iteration " << it << "\n";

                }
                if (is_oscillate) {
                    sstr << " well " << name() << " observes oscillation in inner iteration " << it << "\n";
                }
                sstr << " relaxation_factor is " << relaxation_factor << " now\n";
                deferred_logger.debug(sstr.str());
            }
            updateWellState(dx_well, well_state, deferred_logger, relaxation_factor);
            initPrimaryVariablesEvaluation();
        }

        // TODO: we should decide whether to keep the updated well_state, or recover to use the old well_state
        if (converged) {
            std::ostringstream sstr;
            sstr << " well " << name() << " manage to get converged within " << it << " inner iterations";
            deferred_logger.debug(sstr.str());
        } else {
            std::ostringstream sstr;
            sstr << " well " << name() << " did not get converged within " << it << " inner iterations \n";
            sstr << " outputting the residual history for well " << name() << " during inner iterations \n";
            for (int i = 0; i < it; ++i) {
                const auto& residual = residual_history[i];
                sstr << " residual at " << i << "th iteration ";
                for (const auto& res : residual) {
                    sstr << " " << res;
                }
                sstr << " " << measure_history[i] << " \n";
            }
            deferred_logger.debug(sstr.str());
        }
    }





    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleWellEqWithoutIteration(const Simulator& ebosSimulator,
                                   const double dt,
                                   const Well::InjectionControls& inj_controls,
                                   const Well::ProductionControls& prod_controls,
                                   WellState& well_state,
                                   Opm::DeferredLogger& deferred_logger)
    {
        // calculate the fluid properties needed.
        computeSegmentFluidProperties(ebosSimulator);

        // update the upwinding segments
        updateUpwindingSegments();

        // clear all entries
        duneB_ = 0.0;
        duneC_ = 0.0;

        duneD_ = 0.0;
        resWell_ = 0.0;

        well_state.wellVaporizedOilRates()[index_of_well_] = 0.;
        well_state.wellDissolvedGasRates()[index_of_well_] = 0.;

        // for the black oil cases, there will be four equations,
        // the first three of them are the mass balance equations, the last one is the pressure equations.
        //
        // but for the top segment, the pressure equation will be the well control equation, and the other three will be the same.

        const bool allow_cf = getAllowCrossFlow() || openCrossFlowAvoidSingularity(ebosSimulator);

        const int nseg = numberOfSegments();

        for (int seg = 0; seg < nseg; ++seg) {
            // calculating the accumulation term
            // TODO: without considering the efficiencty factor for now
            {
                const EvalWell segment_surface_volume = getSegmentSurfaceVolume(ebosSimulator, seg);
                // for each component
                for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                    const EvalWell accumulation_term = (segment_surface_volume * surfaceVolumeFraction(seg, comp_idx)
                                                     - segment_fluid_initial_[seg][comp_idx]) / dt;

                    resWell_[seg][comp_idx] += accumulation_term.value();
                    for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
                        duneD_[seg][seg][comp_idx][pv_idx] += accumulation_term.derivative(pv_idx + numEq);
                    }
                }
            }
            // considering the contributions due to flowing out from the segment
            {
                for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                    const EvalWell segment_rate = getSegmentRateUpwinding(seg, comp_idx);

                    const int seg_upwind = upwinding_segments_[seg];
                    // segment_rate contains the derivatives with respect to GTotal in seg,
                    // and WFrac and GFrac in seg_upwind
                    resWell_[seg][comp_idx] -= segment_rate.value();
                    duneD_[seg][seg][comp_idx][GTotal] -= segment_rate.derivative(GTotal + numEq);
                    duneD_[seg][seg_upwind][comp_idx][WFrac] -= segment_rate.derivative(WFrac + numEq);
                    duneD_[seg][seg_upwind][comp_idx][GFrac] -= segment_rate.derivative(GFrac + numEq);
                    // pressure derivative should be zero
                }
            }

            // considering the contributions from the inlet segments
            {
                for (const int inlet : segment_inlets_[seg]) {
                    for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                        const EvalWell inlet_rate = getSegmentRateUpwinding(inlet, comp_idx);

                        const int inlet_upwind = upwinding_segments_[inlet];
                        // inlet_rate contains the derivatives with respect to GTotal in inlet,
                        // and WFrac and GFrac in inlet_upwind
                        resWell_[seg][comp_idx] += inlet_rate.value();
                        duneD_[seg][inlet][comp_idx][GTotal] += inlet_rate.derivative(GTotal + numEq);
                        duneD_[seg][inlet_upwind][comp_idx][WFrac] += inlet_rate.derivative(WFrac + numEq);
                        duneD_[seg][inlet_upwind][comp_idx][GFrac] += inlet_rate.derivative(GFrac + numEq);
                        // pressure derivative should be zero
                    }
                }
            }

            // calculating the perforation rate for each perforation that belongs to this segment
            const EvalWell seg_pressure = getSegmentPressure(seg);
            for (const int perf : segment_perforations_[seg]) {
                const int cell_idx = well_cells_[perf];
                const auto& int_quants = *(ebosSimulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
                std::vector<EvalWell> mob(num_components_, 0.0);
                getMobility(ebosSimulator, perf, mob);
                std::vector<EvalWell> cq_s(num_components_, 0.0);
                EvalWell perf_press;
                double perf_dis_gas_rate = 0.;
                double perf_vap_oil_rate = 0.;

                computePerfRatePressure(int_quants, mob, seg, perf, seg_pressure, allow_cf, cq_s, perf_press, perf_dis_gas_rate, perf_vap_oil_rate, deferred_logger);

                // updating the solution gas rate and solution oil rate
                if (this->isProducer()) {
                    well_state.wellDissolvedGasRates()[index_of_well_] += perf_dis_gas_rate;
                    well_state.wellVaporizedOilRates()[index_of_well_] += perf_vap_oil_rate;
                }

                // store the perf pressure and rates
                const int rate_start_offset = (first_perf_ + perf) * number_of_phases_;
                for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                    well_state.perfPhaseRates()[rate_start_offset + ebosCompIdxToFlowCompIdx(comp_idx)] = cq_s[comp_idx].value();
                }
                well_state.perfPress()[first_perf_ + perf] = perf_press.value();

                for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
                    // the cq_s entering mass balance equations need to consider the efficiency factors.
                    const EvalWell cq_s_effective = cq_s[comp_idx] * well_efficiency_factor_;

                    connectionRates_[perf][comp_idx] = Base::restrictEval(cq_s_effective);

                    // subtract sum of phase fluxes in the well equations.
                    resWell_[seg][comp_idx] += cq_s_effective.value();

                    // assemble the jacobians
                    for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {

                        // also need to consider the efficiency factor when manipulating the jacobians.
                        duneC_[seg][cell_idx][pv_idx][comp_idx] -= cq_s_effective.derivative(pv_idx + numEq); // intput in transformed matrix

                        // the index name for the D should be eq_idx / pv_idx
                        duneD_[seg][seg][comp_idx][pv_idx] += cq_s_effective.derivative(pv_idx + numEq);
                    }

                    for (int pv_idx = 0; pv_idx < numEq; ++pv_idx) {
                        // also need to consider the efficiency factor when manipulating the jacobians.
                        duneB_[seg][cell_idx][comp_idx][pv_idx] += cq_s_effective.derivative(pv_idx);
                    }
                }
            }

            // the fourth dequation, the pressure drop equation
            if (seg == 0) { // top segment, pressure equation is the control equation
                const auto& summaryState = ebosSimulator.vanguard().summaryState();
                const Opm::Schedule& schedule = ebosSimulator.vanguard().schedule();
                assembleControlEq(well_state, schedule, summaryState, inj_controls, prod_controls, deferred_logger);
            } else {
                // TODO: maybe the following should go to the function assemblePressureEq()
		switch(segmentSet()[seg].segmentType()) {
		    case Segment::SegmentType::SICD :
		        assembleSICDPressureEq(seg);
			break;
		    case Segment::SegmentType::VALVE :
		        assembleValvePressureEq(seg);
			break;
		    default :
		        assemblePressureEq(seg);
		}
            }
        }
    }



    template<typename TypeTag>
    bool
    MultisegmentWell<TypeTag>::
    openCrossFlowAvoidSingularity(const Simulator& ebos_simulator) const
    {
        return !getAllowCrossFlow() && allDrawDownWrongDirection(ebos_simulator);
    }


    template<typename TypeTag>
    bool
    MultisegmentWell<TypeTag>::
    allDrawDownWrongDirection(const Simulator& ebos_simulator) const
    {
        bool all_drawdown_wrong_direction = true;
        const int nseg = numberOfSegments();

        for (int seg = 0; seg < nseg; ++seg) {
            const EvalWell segment_pressure = getSegmentPressure(seg);
            for (const int perf : segment_perforations_[seg]) {

                const int cell_idx = well_cells_[perf];
                const auto& intQuants = *(ebos_simulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
                const auto& fs = intQuants.fluidState();

                // pressure difference between the segment and the perforation
                const EvalWell perf_seg_press_diff = gravity_ * segment_densities_[seg] * perforation_segment_depth_diffs_[perf];
                // pressure difference between the perforation and the grid cell
                const double cell_perf_press_diff = cell_perforation_pressure_diffs_[perf];

                const double pressure_cell = (fs.pressure(FluidSystem::oilPhaseIdx)).value();
                const double perf_press = pressure_cell - cell_perf_press_diff;
                // Pressure drawdown (also used to determine direction of flow)
                // TODO: not 100% sure about the sign of the seg_perf_press_diff
                const EvalWell drawdown = perf_press - (segment_pressure + perf_seg_press_diff);

                // for now, if there is one perforation can produce/inject in the correct
                // direction, we consider this well can still produce/inject.
                // TODO: it can be more complicated than this to cause wrong-signed rates
                if ( (drawdown < 0. && this->isInjector()) ||
                     (drawdown > 0. && this->isProducer()) )  {
                    all_drawdown_wrong_direction = false;
                    break;
                }
            }
        }

        return all_drawdown_wrong_direction;
    }


    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    wellTestingPhysical(const Simulator& /* simulator */, const std::vector<double>& /* B_avg */,
                        const double /* simulation_time */, const int /* report_step */,
                        WellState& /* well_state */, WellTestState& /* welltest_state */, Opm::DeferredLogger& deferred_logger)
    {
        const std::string msg = "Support of well testing for physical limits for multisegment wells is not "
                                "implemented yet, wellTestingPhysical() for " + name() + " will do nothing";
        deferred_logger.warning("NO_WELLTESTPHYSICAL_CHECKING_MS_WELLS", msg);
    }





    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updateWaterThroughput(const double dt OPM_UNUSED, WellState& well_state OPM_UNUSED) const
    {
    }





    template<typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    getSegmentSurfaceVolume(const Simulator& ebos_simulator, const int seg_idx) const
    {
        EvalWell temperature;
        int pvt_region_index;
        {
            // using the pvt region of first perforated cell
            // TODO: it should be a member of the WellInterface, initialized properly
            const int cell_idx = well_cells_[0];
            const auto& intQuants = *(ebos_simulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/0));
            const auto& fs = intQuants.fluidState();
            temperature.setValue(fs.temperature(FluidSystem::oilPhaseIdx).value());
            pvt_region_index = fs.pvtRegionIndex();
        }

        const EvalWell seg_pressure = getSegmentPressure(seg_idx);

        std::vector<EvalWell> mix_s(num_components_, 0.0);
        for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
            mix_s[comp_idx] = surfaceVolumeFraction(seg_idx, comp_idx);
        }

        std::vector<EvalWell> b(num_components_, 0.);
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
            const unsigned waterCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
            b[waterCompIdx] =
                FluidSystem::waterPvt().inverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
        }

        EvalWell rv(0.0);
        // gas phase
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
            if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
                EvalWell rvmax = FluidSystem::gasPvt().saturatedOilVaporizationFactor(pvt_region_index, temperature, seg_pressure);
                if (rvmax < 0.0) { // negative rvmax can happen if the seg_pressure is outside the range of the table
                    rvmax = 0.0;
                }
                if (mix_s[oilCompIdx] > 0.0) {
                    if (mix_s[gasCompIdx] > 0.0) {
                        rv = mix_s[oilCompIdx] / mix_s[gasCompIdx];
                    }

                    if (rv > rvmax) {
                        rv = rvmax;
                    }
                    b[gasCompIdx] =
                        FluidSystem::gasPvt().inverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure, rv);
                } else { // no oil exists
                    b[gasCompIdx] =
                        FluidSystem::gasPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                }
            } else { // no Liquid phase
                // it is the same with zero mix_s[Oil]
                b[gasCompIdx] =
                    FluidSystem::gasPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
            }
        }

        EvalWell rs(0.0);
        // oil phase
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
            const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
            if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                EvalWell rsmax = FluidSystem::oilPvt().saturatedGasDissolutionFactor(pvt_region_index, temperature, seg_pressure);
                if (rsmax < 0.0) { // negative rsmax can happen if the seg_pressure is outside the range of the table
                    rsmax = 0.0;
                }
                if (mix_s[gasCompIdx] > 0.0) {
                    if (mix_s[oilCompIdx] > 0.0) {
                        rs = mix_s[gasCompIdx] / mix_s[oilCompIdx];
                    }
                    // std::cout << " rs " << rs.value() << " rsmax " << rsmax.value() << std::endl;

                    if (rs > rsmax) {
                        rs = rsmax;
                    }
                    b[oilCompIdx] =
                        FluidSystem::oilPvt().inverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure, rs);
                } else { // no oil exists
                    b[oilCompIdx] =
                        FluidSystem::oilPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
                }
            } else { // no gas phase
                // it is the same with zero mix_s[Gas]
                b[oilCompIdx] =
                    FluidSystem::oilPvt().saturatedInverseFormationVolumeFactor(pvt_region_index, temperature, seg_pressure);
            }
        }

        std::vector<EvalWell> mix(mix_s);
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            const unsigned gasCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
            const unsigned oilCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);

            const EvalWell d = 1.0 - rs * rv;
            if (d <= 0.0 || d > 1.0) {
                OPM_THROW(Opm::NumericalIssue, "Problematic d value " << d << " obtained for well " << name()
                                               << " during convertion to surface volume with rs " << rs
                                               << ", rv " << rv << " and pressure " << seg_pressure
                                               << " obtaining d " << d);
            }

            if (rs > 0.0) { // rs > 0.0?
                mix[gasCompIdx] = (mix_s[gasCompIdx] - mix_s[oilCompIdx] * rs) / d;
            }
            if (rv > 0.0) { // rv > 0.0?
                mix[oilCompIdx] = (mix_s[oilCompIdx] - mix_s[gasCompIdx] * rv) / d;
            }
        }

        EvalWell vol_ratio(0.0);
        for (int comp_idx = 0; comp_idx < num_components_; ++comp_idx) {
            vol_ratio += mix[comp_idx] / b[comp_idx];
        }

        // segment volume
        const double volume = segmentSet()[seg_idx].volume();

        return volume / vol_ratio;
    }





    template<typename TypeTag>
    std::vector<typename MultisegmentWell<TypeTag>::Scalar>
    MultisegmentWell<TypeTag>::
    getWellResiduals(const std::vector<Scalar>& B_avg) const
    {
        assert(int(B_avg.size() ) == num_components_);
        std::vector<Scalar> residuals(numWellEq + 1, 0.0);

        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            for (int eq_idx = 0; eq_idx < numWellEq; ++eq_idx) {
                double residual = 0.;
                if (eq_idx < num_components_) {
                    residual = std::abs(resWell_[seg][eq_idx]) * B_avg[eq_idx];
                } else {
                    if (seg > 0) {
                        residual = std::abs(resWell_[seg][eq_idx]);
                    }
                }
                if (std::isnan(residual) || std::isinf(residual)) {
                    OPM_THROW(Opm::NumericalIssue, "nan or inf value for residal get for well " << name()
                                                    << " segment " << seg << " eq_idx " << eq_idx);
                }

                if (residual > residuals[eq_idx]) {
                    residuals[eq_idx] = residual;
                }
            }
        }

        // handling the control equation residual
        {
            const double control_residual = std::abs(resWell_[0][numWellEq - 1]);
            if (std::isnan(control_residual) || std::isinf(control_residual)) {
               OPM_THROW(Opm::NumericalIssue, "nan or inf value for control residal get for well " << name());
            }
            residuals[numWellEq] = control_residual;
        }

        return residuals;
    }





    /// Detect oscillation or stagnation based on the residual measure history
    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    detectOscillations(const std::vector<double>& measure_history,
                       const int it, bool& oscillate, bool& stagnate) const
    {
        if ( it < 2 ) {
            oscillate = false;
            stagnate = false;
            return;
        }

        stagnate = true;
        const double F0 = measure_history[it];
        const double F1 = measure_history[it - 1];
        const double F2 = measure_history[it - 2];
        const double d1 = std::abs((F0 - F2) / F0);
        const double d2 = std::abs((F0 - F1) / F0);

        const double oscillaton_rel_tol = 0.2;
        oscillate = (d1 < oscillaton_rel_tol) && (oscillaton_rel_tol < d2);

        const double stagnation_rel_tol = 1.e-2;
        stagnate = std::abs((F1 - F2) / F2) <= stagnation_rel_tol;
    }





    template<typename TypeTag>
    double
    MultisegmentWell<TypeTag>::
    getResidualMeasureValue(const WellState& well_state,
                            const std::vector<double>& residuals,
                            DeferredLogger& deferred_logger) const
    {
        assert(int(residuals.size()) == numWellEq + 1);

        const double rate_tolerance = param_.tolerance_wells_;
        int count = 0;
        double sum = 0;
        for (int eq_idx = 0; eq_idx < numWellEq - 1; ++eq_idx) {
            if (residuals[eq_idx] > rate_tolerance) {
                sum += residuals[eq_idx] / rate_tolerance;
                ++count;
            }
        }

        const double pressure_tolerance = param_.tolerance_pressure_ms_wells_;
        if (residuals[SPres] > pressure_tolerance) {
            sum += residuals[SPres] / pressure_tolerance;
            ++count;
        }

        const double control_tolerance = getControlTolerance(well_state, deferred_logger);
        if (residuals[SPres + 1] > control_tolerance) {
            sum += residuals[SPres + 1] / control_tolerance;
            ++count;
        }

        // if (count == 0), it should be converged.
        assert(count != 0);

        return sum;
    }





    template<typename TypeTag>
    double
    MultisegmentWell<TypeTag>::
    getControlTolerance(const WellState& well_state,
                        DeferredLogger& deferred_logger) const
    {
        double control_tolerance = 0.;

        const int well_index = index_of_well_;
        if (this->isInjector() )
        {
            const Opm::Well::InjectorCMode& current = well_state.currentInjectionControls()[well_index];
            switch(current) {
            case Well::InjectorCMode::THP:
                control_tolerance = param_.tolerance_pressure_ms_wells_;
                break;
            case Well::InjectorCMode::BHP:
                control_tolerance = param_.tolerance_wells_;
                break;
            case Well::InjectorCMode::RATE:
            case Well::InjectorCMode::RESV:
                control_tolerance = param_.tolerance_wells_;
                break;
            case Well::InjectorCMode::GRUP:
                control_tolerance = param_.tolerance_wells_;
                break;
            default:
                OPM_DEFLOG_THROW(std::runtime_error, "Unknown well control control types for well " << name(), deferred_logger);
            }
        }

        if (this->isProducer() )
        {
            const Well::ProducerCMode& current = well_state.currentProductionControls()[well_index];
            switch(current) {
            case Well::ProducerCMode::THP:
                control_tolerance = param_.tolerance_pressure_ms_wells_; // 0.1 bar
                break;
            case Well::ProducerCMode::BHP:
                control_tolerance = param_.tolerance_wells_; // 0.01 bar
                break;
            case Well::ProducerCMode::ORAT:
            case Well::ProducerCMode::WRAT:
            case Well::ProducerCMode::GRAT:
            case Well::ProducerCMode::LRAT:
            case Well::ProducerCMode::RESV:
            case Well::ProducerCMode::CRAT:
                control_tolerance = param_.tolerance_wells_; // smaller tolerance for rate control
                break;
            case Well::ProducerCMode::GRUP:
                control_tolerance = param_.tolerance_wells_; // smaller tolerance for rate control
                break;
            default:
                OPM_DEFLOG_THROW(std::runtime_error, "Unknown well control control types for well " << name(), deferred_logger);
            }
        }

        return control_tolerance;
    }





    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    checkConvergenceControlEq(const WellState& well_state,
                              ConvergenceReport& report,
                              DeferredLogger& deferred_logger) const
    {
        double control_tolerance = 0.;
        using CR = ConvergenceReport;
        CR::WellFailure::Type ctrltype = CR::WellFailure::Type::Invalid;

        const int well_index = index_of_well_;
        if (this->isInjector() )
        {
            const Opm::Well::InjectorCMode& current = well_state.currentInjectionControls()[well_index];
            switch(current) {
            case Well::InjectorCMode::THP:
                ctrltype = CR::WellFailure::Type::ControlTHP;
                control_tolerance = param_.tolerance_pressure_ms_wells_;
                break;
            case Well::InjectorCMode::BHP:
                ctrltype = CR::WellFailure::Type::ControlBHP;
                control_tolerance = param_.tolerance_pressure_ms_wells_;
                break;
            case Well::InjectorCMode::RATE:
            case Well::InjectorCMode::RESV:
                ctrltype = CR::WellFailure::Type::ControlRate;
                control_tolerance = param_.tolerance_wells_;
                break;
            case Well::InjectorCMode::GRUP:
                ctrltype = CR::WellFailure::Type::ControlRate;
                control_tolerance = param_.tolerance_wells_;
                break;
            default:
                OPM_DEFLOG_THROW(std::runtime_error, "Unknown well control control types for well " << name(), deferred_logger);
            }
        }

        if (this->isProducer() )
        {
            const Well::ProducerCMode& current = well_state.currentProductionControls()[well_index];
            switch(current) {
            case Well::ProducerCMode::THP:
                ctrltype = CR::WellFailure::Type::ControlTHP;
                control_tolerance = param_.tolerance_pressure_ms_wells_;
                break;
            case Well::ProducerCMode::BHP:
                ctrltype = CR::WellFailure::Type::ControlBHP;
                control_tolerance = param_.tolerance_pressure_ms_wells_;
                break;
            case Well::ProducerCMode::ORAT:
            case Well::ProducerCMode::WRAT:
            case Well::ProducerCMode::GRAT:
            case Well::ProducerCMode::LRAT:
            case Well::ProducerCMode::RESV:
            case Well::ProducerCMode::CRAT:
                ctrltype = CR::WellFailure::Type::ControlRate;
                control_tolerance = param_.tolerance_wells_;
                break;
            case Well::ProducerCMode::GRUP:
                ctrltype = CR::WellFailure::Type::ControlRate;
                control_tolerance = param_.tolerance_wells_;
                break;
            default:
                OPM_DEFLOG_THROW(std::runtime_error, "Unknown well control control types for well " << name(), deferred_logger);
            }
        }

        const double well_control_residual = std::abs(resWell_[0][SPres]);
        const int dummy_component = -1;
        const double max_residual_allowed = param_.max_residual_allowed_;
        if (std::isnan(well_control_residual)) {
            report.setWellFailed({ctrltype, CR::Severity::NotANumber, dummy_component, name()});
        } else if (well_control_residual > max_residual_allowed * 10.) {
            report.setWellFailed({ctrltype, CR::Severity::TooLarge, dummy_component, name()});
        } else if ( well_control_residual > control_tolerance) {
            report.setWellFailed({ctrltype, CR::Severity::Normal, dummy_component, name()});
        }
    }






    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    updateUpwindingSegments()
    {
        for (int seg = 0; seg < numberOfSegments(); ++seg) {
            // special treatment is needed for segment 0
            if (seg == 0) {
                // we are not supposed to have injecting producers and producing injectors
                assert( ! (this->isProducer() && primary_variables_evaluation_[seg][GTotal] > 0.) );
                assert( ! (this->isInjector() && primary_variables_evaluation_[seg][GTotal] < 0.) );
                upwinding_segments_[seg] = seg;
                continue;
            }

            // for other normal segments
            if (primary_variables_evaluation_[seg][GTotal] <= 0.) {
                upwinding_segments_[seg] = seg;
            } else {
                const int outlet_segment_index = segmentNumberToIndex(segmentSet()[seg].outletSegment());
                upwinding_segments_[seg] = outlet_segment_index;
            }
        }
    }












    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleSICDPressureEq(const int seg) const
    {
        // TODO: upwinding needs to be taken care of
        // top segment can not be a spiral ICD device
        assert(seg != 0);

        // the pressure equation is something like
        // p_seg - deltaP - p_outlet = 0.
        // the major part is how to calculate the deltaP

        EvalWell pressure_equation = getSegmentPressure(seg);

        pressure_equation = pressure_equation - pressureDropSpiralICD(seg);

        resWell_[seg][SPres] = pressure_equation.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][seg][SPres][pv_idx] = pressure_equation.derivative(pv_idx + numEq);
        }

        // contribution from the outlet segment
        const int outlet_segment_index = segmentNumberToIndex(segmentSet()[seg].outletSegment());
        const EvalWell outlet_pressure = getSegmentPressure(outlet_segment_index);

        resWell_[seg][SPres] -= outlet_pressure.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][outlet_segment_index][SPres][pv_idx] = -outlet_pressure.derivative(pv_idx + numEq);

        }
    }




    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    assembleValvePressureEq(const int seg) const
    {
        // TODO: upwinding needs to be taken care of
        // top segment can not be a spiral ICD device
        assert(seg != 0);

        // const Valve& valve = *segmentSet()[seg].Valve();

        // the pressure equation is something like
        // p_seg - deltaP - p_outlet = 0.
        // the major part is how to calculate the deltaP

        EvalWell pressure_equation = getSegmentPressure(seg);

        // const int seg_upwind = upwinding_segments_[seg];

        pressure_equation = pressure_equation - pressureDropValve(seg);

        resWell_[seg][SPres] = pressure_equation.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][seg][SPres][pv_idx] = pressure_equation.derivative(pv_idx + numEq);
        }

        // contribution from the outlet segment
        const int outlet_segment_index = segmentNumberToIndex(segmentSet()[seg].outletSegment());
        const EvalWell outlet_pressure = getSegmentPressure(outlet_segment_index);

        resWell_[seg][SPres] -= outlet_pressure.value();
        for (int pv_idx = 0; pv_idx < numWellEq; ++pv_idx) {
            duneD_[seg][outlet_segment_index][SPres][pv_idx] = -outlet_pressure.derivative(pv_idx + numEq);
        }
    }





    template<typename TypeTag>
    boost::optional<double>
    MultisegmentWell<TypeTag>::
    computeBhpAtThpLimitProd(const Simulator& ebos_simulator,
                             const std::vector<Scalar>& B_avg,
                             const SummaryState& summary_state,
                             DeferredLogger& deferred_logger) const
    {
        // Given a VFP function returning bhp as a function of phase
        // rates and thp:
        //     fbhp(rates, thp),
        // a function extracting the particular flow rate used for VFP
        // lookups:
        //     flo(rates)
        // and the inflow function (assuming the reservoir is fixed):
        //     frates(bhp)
        // we want to solve the equation:
        //     fbhp(frates(bhp, thplimit)) - bhp = 0
        // for bhp.
        //
        // This may result in 0, 1 or 2 solutions. If two solutions,
        // the one corresponding to the lowest bhp (and therefore
        // highest rate) should be returned.

        // Make the fbhp() function.
        const auto& controls = well_ecl_.productionControls(summary_state);
        const auto& table = *(vfp_properties_->getProd()->getTable(controls.vfp_table_number));
        const double vfp_ref_depth = table.getDatumDepth();
        const double rho = segment_densities_[0].value(); // Use the density at the top perforation.
        const double dp = wellhelpers::computeHydrostaticCorrection(ref_depth_, vfp_ref_depth, rho, gravity_);
        auto fbhp = [this, &controls, dp](const std::vector<double>& rates) {
            assert(rates.size() == 3);
            return this->vfp_properties_->getProd()
            ->bhp(controls.vfp_table_number, rates[Water], rates[Oil], rates[Gas], controls.thp_limit, controls.alq_value) - dp;
        };

        // Make the flo() function.
        auto flo_type = table.getFloType();
        auto flo = [flo_type](const std::vector<double>& rates) {
            return detail::getFlo(rates[Water], rates[Oil], rates[Gas], flo_type);
        };

        // Make the frates() function.
        auto frates = [this, &ebos_simulator, &B_avg, &deferred_logger](const double bhp) {
            // Not solving the well equations here, which means we are
            // calculating at the current Fg/Fw values of the
            // well. This does not matter unless the well is
            // crossflowing, and then it is likely still a good
            // approximation.
            std::vector<double> rates(3);
            computeWellRatesWithBhp(ebos_simulator, B_avg, bhp, rates, deferred_logger);
            return rates;
        };

        // Find the bhp-point where production becomes nonzero.
        double bhp_max = 0.0;
        {
            auto fflo = [&flo, &frates](double bhp) { return flo(frates(bhp)); };
            double low = controls.bhp_limit;
            double high = maxPerfPress(ebos_simulator) + 1.0 * unit::barsa;
            double f_low = fflo(low);
            double f_high = fflo(high);
            deferred_logger.debug("computeBhpAtThpLimitProd(): well = " + name() +
                                  "  low = " + std::to_string(low) +
                                  "  high = " + std::to_string(high) +
                                  "  f(low) = " + std::to_string(f_low) +
                                  "  f(high) = " + std::to_string(f_high));
            int adjustments = 0;
            const int max_adjustments = 10;
            const double adjust_amount = 5.0 * unit::barsa;
            while (f_low * f_high > 0.0 && adjustments < max_adjustments) {
                // Same sign, adjust high to see if we can flip it.
                high += adjust_amount;
                f_high = fflo(high);
                ++adjustments;
            }
            if (f_low * f_high > 0.0) {
                if (f_low > 0.0) {
                    // Even at the BHP limit, we are injecting.
                    // There will be no solution here, return an
                    // empty optional.
                    deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE_INOPERABLE",
                                            "Robust bhp(thp) solve failed due to inoperability for well " + name());
                    return boost::optional<double>();
                } else {
                    // Still producing, even at high bhp.
                    assert(f_high < 0.0);
                    bhp_max = high;
                }
            } else {
                // Bisect to find a bhp point where we produce, but
                // not a large amount ('eps' below).
                const double eps = 0.1 * std::fabs(table.getFloAxis().front());
                const int maxit = 50;
                int it = 0;
                while (std::fabs(f_low) > eps && it < maxit) {
                    const double curr = 0.5*(low + high);
                    const double f_curr = fflo(curr);
                    if (f_curr * f_low > 0.0) {
                        low = curr;
                        f_low = f_curr;
                    } else {
                        high = curr;
                        f_high = f_curr;
                    }
                    ++it;
                }
                bhp_max = low;
            }
            deferred_logger.debug("computeBhpAtThpLimitProd(): well = " + name() +
                                  "  low = " + std::to_string(low) +
                                  "  high = " + std::to_string(high) +
                                  "  f(low) = " + std::to_string(f_low) +
                                  "  f(high) = " + std::to_string(f_high) +
                                  "  bhp_max = " + std::to_string(bhp_max));
        }

        // Define the equation we want to solve.
        auto eq = [&fbhp, &frates](double bhp) {
            return fbhp(frates(bhp)) - bhp;
        };

        // Find appropriate brackets for the solution.
        double low = controls.bhp_limit;
        double high = bhp_max;
        {
            double eq_high = eq(high);
            double eq_low = eq(low);
            const double eq_bhplimit = eq_low;
            deferred_logger.debug("computeBhpAtThpLimitProd(): well = " + name() +
                                  "  low = " + std::to_string(low) +
                                  "  high = " + std::to_string(high) +
                                  "  eq(low) = " + std::to_string(eq_low) +
                                  "  eq(high) = " + std::to_string(eq_high));
            if (eq_low * eq_high > 0.0) {
                // Failed to bracket the zero.
                // If this is due to having two solutions, bisect until bracketed.
                double abs_low = std::fabs(eq_low);
                double abs_high = std::fabs(eq_high);
                int bracket_attempts = 0;
                const int max_bracket_attempts = 20;
                double interval = high - low;
                const double min_interval = 1.0 * unit::barsa;
                while (eq_low * eq_high > 0.0 && bracket_attempts < max_bracket_attempts && interval > min_interval) {
                    if (abs_high < abs_low) {
                        low = 0.5 * (low + high);
                        eq_low = eq(low);
                        abs_low = std::fabs(eq_low);
                    } else {
                        high = 0.5 * (low + high);
                        eq_high = eq(high);
                        abs_high = std::fabs(eq_high);
                    }
                    ++bracket_attempts;
                }
                if (eq_low * eq_high > 0.0) {
                    // Still failed bracketing!
                    const double limit = 3.0 * unit::barsa;
                    if (std::min(abs_low, abs_high) < limit) {
                        // Return the least bad solution if less off than 3 bar.
                        deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE_BRACKETING_FAILURE",
                                                "Robust bhp(thp) not solved precisely for well " + name());
                        return abs_low < abs_high ? low : high;
                    } else {
                        // Return failure.
                        deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE_BRACKETING_FAILURE",
                                                "Robust bhp(thp) solve failed due to bracketing failure for well " + name());
                        return boost::optional<double>();
                    }
                }
            }
            // We have a bracket!
            // Now, see if (bhplimit, low) is a bracket in addition to (low, high).
            // If so, that is the bracket we shall use, choosing the solution with the
            // highest flow.
            if (eq_low * eq_bhplimit <= 0.0) {
                high = low;
                low = controls.bhp_limit;
            }
        }

        // Solve for the proper solution in the given interval.
        const int max_iteration = 100;
        const double bhp_tolerance = 0.01 * unit::barsa;
        int iteration = 0;
        try {
            const double solved_bhp = RegulaFalsiBisection<ThrowOnError>::
                solve(eq, low, high, max_iteration, bhp_tolerance, iteration);
            return solved_bhp;
        }
        catch (...) {
            deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE",
                                    "Robust bhp(thp) solve failed for well " + name());
            return boost::optional<double>();
	}
    }




    template<typename TypeTag>
    boost::optional<double>
    MultisegmentWell<TypeTag>::
    computeBhpAtThpLimitInj(const Simulator& ebos_simulator,
                            const std::vector<Scalar>& B_avg,
                            const SummaryState& summary_state,
                            DeferredLogger& deferred_logger) const
    {
        // Given a VFP function returning bhp as a function of phase
        // rates and thp:
        //     fbhp(rates, thp),
        // a function extracting the particular flow rate used for VFP
        // lookups:
        //     flo(rates)
        // and the inflow function (assuming the reservoir is fixed):
        //     frates(bhp)
        // we want to solve the equation:
        //     fbhp(frates(bhp, thplimit)) - bhp = 0
        // for bhp.
        //
        // This may result in 0, 1 or 2 solutions. If two solutions,
        // the one corresponding to the lowest bhp (and therefore
        // highest rate) is returned.
        //
        // In order to detect these situations, we will find piecewise
        // linear approximations both to the inverse of the frates
        // function and to the fbhp function.
        //
        // We first take the FLO sample points of the VFP curve, and
        // find the corresponding bhp values by solving the equation:
        //     flo(frates(bhp)) - flo_sample = 0
        // for bhp, for each flo_sample. The resulting (flo_sample,
        // bhp_sample) values give a piecewise linear approximation to
        // the true inverse inflow function, at the same flo values as
        // the VFP data.
        //
        // Then we extract a piecewise linear approximation from the
        // multilinear fbhp() by evaluating it at the flo_sample
        // points, with fractions given by the frates(bhp_sample)
        // values.
        //
        // When we have both piecewise linear curves defined on the
        // same flo_sample points, it is easy to distinguish between
        // the 0, 1 or 2 solution cases, and obtain the right interval
        // in which to solve for the solution we want (with highest
        // flow in case of 2 solutions).

        // Make the fbhp() function.
        const auto& controls = well_ecl_.injectionControls(summary_state);
        const auto& table = *(vfp_properties_->getInj()->getTable(controls.vfp_table_number));
        const double vfp_ref_depth = table.getDatumDepth();
        const double rho = segment_densities_[0].value(); // Use the density at the top perforation.
        const double dp = wellhelpers::computeHydrostaticCorrection(ref_depth_, vfp_ref_depth, rho, gravity_);
        auto fbhp = [this, &controls, dp](const std::vector<double>& rates) {
            assert(rates.size() == 3);
            return this->vfp_properties_->getInj()
                    ->bhp(controls.vfp_table_number, rates[Water], rates[Oil], rates[Gas], controls.thp_limit) - dp;
        };

        // Make the flo() function.
        auto flo_type = table.getFloType();
        auto flo = [flo_type](const std::vector<double>& rates) {
            return detail::getFlo(rates[Water], rates[Oil], rates[Gas], flo_type);
        };

        // Make the frates() function.
        auto frates = [this, &ebos_simulator, &B_avg, &deferred_logger](const double bhp) {
            // Not solving the well equations here, which means we are
            // calculating at the current Fg/Fw values of the
            // well. This does not matter unless the well is
            // crossflowing, and then it is likely still a good
            // approximation.
            std::vector<double> rates(3);
            computeWellRatesWithBhp(ebos_simulator, B_avg, bhp, rates, deferred_logger);
            return rates;
        };

        // Get the flo samples, add extra samples at low rates and bhp
        // limit point if necessary.
        std::vector<double> flo_samples = table.getFloAxis();
        if (flo_samples[0] > 0.0) {
            const double f0 = flo_samples[0];
            flo_samples.insert(flo_samples.begin(), { f0/20.0, f0/10.0, f0/5.0, f0/2.0 });
        }
        const double flo_bhp_limit = flo(frates(controls.bhp_limit));
        if (flo_samples.back() < flo_bhp_limit) {
            flo_samples.push_back(flo_bhp_limit);
        }

        // Find bhp values for inflow relation corresponding to flo samples.
        std::vector<double> bhp_samples;
        for (double flo_sample : flo_samples) {
            if (flo_sample > flo_bhp_limit) {
                // We would have to go over the bhp limit to obtain a
                // flow of this magnitude. We associate all such flows
                // with simply the bhp limit. The first one
                // encountered is considered valid, the rest not. They
                // are therefore skipped.
                bhp_samples.push_back(controls.bhp_limit);
                break;
            }
            auto eq = [&flo, &frates, flo_sample](double bhp) {
                return flo(frates(bhp)) - flo_sample;
            };
            // TODO: replace hardcoded low/high limits.
            const double low = 10.0 * unit::barsa;
            const double high = 800.0 * unit::barsa;
            const int max_iteration = 100;
            const double flo_tolerance = 0.05 * std::fabs(flo_samples.back());
            int iteration = 0;
            try {
                const double solved_bhp = RegulaFalsiBisection<WarnAndContinueOnError>::
                        solve(eq, low, high, max_iteration, flo_tolerance, iteration);
                bhp_samples.push_back(solved_bhp);
            }
            catch (...) {
                // Use previous value (or max value if at start) if we failed.
                bhp_samples.push_back(bhp_samples.empty() ? low : bhp_samples.back());
                deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE_EXTRACT_SAMPLES",
                                        "Robust bhp(thp) solve failed extracting bhp values at flo samples for well " + name());
            }
        }

        // Find bhp values for VFP relation corresponding to flo samples.
        const int num_samples = bhp_samples.size(); // Note that this can be smaller than flo_samples.size()
        std::vector<double> fbhp_samples(num_samples);
        for (int ii = 0; ii < num_samples; ++ii) {
            fbhp_samples[ii] = fbhp(frates(bhp_samples[ii]));
        }
// #define EXTRA_THP_DEBUGGING
#ifdef EXTRA_THP_DEBUGGING
        std::string dbgmsg;
        dbgmsg += "flo: ";
        for (int ii = 0; ii < num_samples; ++ii) {
            dbgmsg += "  " + std::to_string(flo_samples[ii]);
        }
        dbgmsg += "\nbhp: ";
        for (int ii = 0; ii < num_samples; ++ii) {
            dbgmsg += "  " + std::to_string(bhp_samples[ii]);
        }
        dbgmsg += "\nfbhp: ";
        for (int ii = 0; ii < num_samples; ++ii) {
            dbgmsg += "  " + std::to_string(fbhp_samples[ii]);
        }
        OpmLog::debug(dbgmsg);
#endif // EXTRA_THP_DEBUGGING

        // Look for sign changes for the (fbhp_samples - bhp_samples) piecewise linear curve.
        // We only look at the valid
        int sign_change_index = -1;
        for (int ii = 0; ii < num_samples - 1; ++ii) {
            const double curr = fbhp_samples[ii] - bhp_samples[ii];
            const double next = fbhp_samples[ii + 1] - bhp_samples[ii + 1];
            if (curr * next < 0.0) {
                // Sign change in the [ii, ii + 1] interval.
                sign_change_index = ii; // May overwrite, thereby choosing the highest-flo solution.
            }
        }

        // Handle the no solution case.
        if (sign_change_index == -1) {
            return boost::optional<double>();
        }

        // Solve for the proper solution in the given interval.
        auto eq = [&fbhp, &frates](double bhp) {
            return fbhp(frates(bhp)) - bhp;
        };
        // TODO: replace hardcoded low/high limits.
        const double low = bhp_samples[sign_change_index + 1];
        const double high = bhp_samples[sign_change_index];
        const int max_iteration = 100;
        const double bhp_tolerance = 0.01 * unit::barsa;
        int iteration = 0;
        if (low == high) {
            // We are in the high flow regime where the bhp_samples
            // are all equal to the bhp_limit.
            assert(low == controls.bhp_limit);
            deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE",
                                    "Robust bhp(thp) solve failed for well " + name());
            return boost::optional<double>();
        }
        try {
            const double solved_bhp = RegulaFalsiBisection<WarnAndContinueOnError>::
                    solve(eq, low, high, max_iteration, bhp_tolerance, iteration);
#ifdef EXTRA_THP_DEBUGGING
            OpmLog::debug("*****    " + name() + "    solved_bhp = " + std::to_string(solved_bhp)
                          + "    flo_bhp_limit = " + std::to_string(flo_bhp_limit));
#endif // EXTRA_THP_DEBUGGING
            return solved_bhp;
        }
        catch (...) {
            deferred_logger.warning("FAILED_ROBUST_BHP_THP_SOLVE",
                                    "Robust bhp(thp) solve failed for well " + name());
            return boost::optional<double>();
        }

    }





    template<typename TypeTag>
    void
    MultisegmentWell<TypeTag>::
    calculateSICDFlowScalingFactors()
    {
        // top segment will not be spiral ICD segment
        for (int seg = 1; seg < numberOfSegments(); ++seg) {
            const Segment& segment = segmentSet()[seg];
            if (segment.segmentType() == Segment::SegmentType::SICD) {
                // getting the segment length related to this ICD
                const int parental_segment_number = segmentSet()[seg].outletSegment();
                const double segment_length = segmentSet().segmentLength(parental_segment_number);

                // getting the total completion length related to this ICD
                // it should be connections
                const auto& connections = well_ecl_.getConnections();
                double total_connection_length = 0.;
                for (const int conn : segment_perforations_[seg]) {
                    const auto& connection = connections.get(conn);
                    const double connection_length = connection.getSegDistEnd() - connection.getSegDistStart();
                    assert(connection_length > 0.);
                    total_connection_length += connection_length;
                }

                SpiralICD& sicd = *segment.spiralICD();
                sicd.updateScalingFactor(segment_length, total_connection_length);
            }
        }
    }




    template<typename TypeTag>
    double
    MultisegmentWell<TypeTag>::
    maxPerfPress(const Simulator& ebos_simulator) const
    {
        double max_pressure = 0.0;
        const int nseg = numberOfSegments();
        for (int seg = 0; seg < nseg; ++seg) {
            for (const int perf : segment_perforations_[seg]) {
                const int cell_idx = well_cells_[perf];
                const auto& int_quants = *(ebos_simulator.model().cachedIntensiveQuantities(cell_idx, /*timeIdx=*/ 0));
                const auto& fs = int_quants.fluidState();
                double pressure_cell = fs.pressure(FluidSystem::oilPhaseIdx).value();
                max_pressure = std::max(max_pressure, pressure_cell);
            }
        }
        return max_pressure;
    }





    template<typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    pressureDropSpiralICD(const int seg) const
    {
        // TODO: We have to consider the upwinding here
        const SpiralICD& sicd = *segmentSet()[seg].spiralICD();

        const std::vector<EvalWell>& phase_fractions = segment_phase_fractions_[seg];
        const std::vector<EvalWell>& phase_viscosities = segment_phase_viscosities_[seg];

        EvalWell water_fraction = 0.;
        EvalWell water_viscosity = 0.;
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
            const int water_pos = Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
            water_fraction = phase_fractions[water_pos];
            water_viscosity = phase_viscosities[water_pos];
        }

        EvalWell oil_fraction = 0.;
        EvalWell oil_viscosity = 0.;
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
            const int oil_pos = Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
            oil_fraction = phase_fractions[oil_pos];
            oil_viscosity = phase_viscosities[oil_pos];
        }

        EvalWell gas_fraction = 0.;
        EvalWell gas_viscosities = 0.;
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
            const int gas_pos = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
            gas_fraction = phase_fractions[gas_pos];
            gas_viscosities = phase_viscosities[gas_pos];
        }

        const EvalWell liquid_emulsion_viscosity = mswellhelpers::emulsionViscosity(water_fraction, water_viscosity,
                                                     oil_fraction, oil_viscosity, sicd);
        const EvalWell mixture_viscosity = (water_fraction + oil_fraction) * liquid_emulsion_viscosity + gas_fraction * gas_viscosities;

        const EvalWell& reservoir_rate = segment_reservoir_volume_rates_[seg];

        const EvalWell reservoir_rate_icd = reservoir_rate * sicd.scalingFactor();

        const double viscosity_cali = sicd.viscosityCalibration();

        using MathTool = MathToolbox<EvalWell>;

        const EvalWell& density = segment_densities_[seg];
        const double density_cali = sicd.densityCalibration();
        const EvalWell temp_value1 = MathTool::pow(density / density_cali, 0.75);
        const EvalWell temp_value2 = MathTool::pow(mixture_viscosity / viscosity_cali, 0.25);

        // formulation before 2016, base_strength is used
        // const double base_strength = sicd.strength() / density_cali;
        // formulation since 2016, strength is used instead
        const double strength = sicd.strength();

        const double sign = reservoir_rate_icd <= 0. ? 1.0 : -1.0;

        return sign * temp_value1 * temp_value2 * strength * reservoir_rate_icd * reservoir_rate_icd;
    }




    template<typename TypeTag>
    typename MultisegmentWell<TypeTag>::EvalWell
    MultisegmentWell<TypeTag>::
    pressureDropValve(const int seg) const
    {
        const Valve& valve = *segmentSet()[seg].valve();

        const EvalWell& mass_rate = segment_mass_rates_[seg];
        const EvalWell& visc = segment_viscosities_[seg];
        const EvalWell& density = segment_densities_[seg];
        const double additional_length = valve.pipeAdditionalLength();
        const double roughness = valve.pipeRoughness();
        const double diameter = valve.pipeDiameter();
        const double area = valve.pipeCrossArea();

        const EvalWell friction_pressure_loss =
            mswellhelpers::frictionPressureLoss(additional_length, diameter, area, roughness, density, mass_rate, visc);

        const double area_con = valve.conCrossArea();
        const double cv = valve.conFlowCoefficient();

        const EvalWell constriction_pressure_loss =
            mswellhelpers::valveContrictionPressureLoss(mass_rate, density, area_con, cv);

        const double sign = mass_rate <= 0. ? 1.0 : -1.0;
        return sign * (friction_pressure_loss + constriction_pressure_loss);
    }

}
