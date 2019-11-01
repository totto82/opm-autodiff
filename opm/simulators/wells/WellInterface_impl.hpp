/*
  Copyright 2017 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2017 Statoil ASA.
  Copyright 2018 IRIS

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

#include <opm/simulators/utils/DeferredLoggingErrorHelpers.hpp>

namespace Opm
{


    template<typename TypeTag>
    WellInterface<TypeTag>::
    WellInterface(const Well2& well, const int time_step, const Wells* wells,
                  const ModelParameters& param,
                  const RateConverterType& rate_converter,
                  const int pvtRegionIdx,
                  const int num_components)
      : well_ecl_(well)
      , current_step_(time_step)
      , param_(param)
      , rateConverter_(rate_converter)
      , pvtRegionIdx_(pvtRegionIdx)
      , num_components_(num_components)
    {
        if (time_step < 0) {
            OPM_THROW(std::invalid_argument, "Negtive time step is used to construct WellInterface");
        }

        if (!wells) {
            OPM_THROW(std::invalid_argument, "Null pointer of Wells is used to construct WellInterface");
        }

        const std::string& well_name = well.name();

        // looking for the location of the well in the wells struct
        int index_well;
        for (index_well = 0; index_well < wells->number_of_wells; ++index_well) {
            if (well_name == std::string(wells->name[index_well])) {
                break;
            }
        }

        // should not enter the constructor if the well does not exist in the wells struct
        // here, just another assertion.
        assert(index_well != wells->number_of_wells);

        index_of_well_ = index_well;
        well_type_ = wells->type[index_well];
        number_of_phases_ = wells->number_of_phases;

        // copying the comp_frac
        {
            comp_frac_.resize(number_of_phases_);
            const int index_begin = index_well * number_of_phases_;
            std::copy(wells->comp_frac + index_begin,
                      wells->comp_frac + index_begin + number_of_phases_, comp_frac_.begin() );
        }

        ref_depth_ = wells->depth_ref[index_well];

        // perforations related
        {
            const int perf_index_begin = wells->well_connpos[index_well];
            const int perf_index_end = wells->well_connpos[index_well + 1];
            number_of_perforations_ = perf_index_end - perf_index_begin;
            first_perf_ = perf_index_begin;

            well_cells_.resize(number_of_perforations_);
            std::copy(wells->well_cells + perf_index_begin,
                      wells->well_cells + perf_index_end,
                      well_cells_.begin() );

            well_index_.resize(number_of_perforations_);
            std::copy(wells->WI + perf_index_begin,
                      wells->WI + perf_index_end,
                      well_index_.begin() );

            saturation_table_number_.resize(number_of_perforations_);
            std::copy(wells->sat_table_id + perf_index_begin,
                      wells->sat_table_id + perf_index_end,
                      saturation_table_number_.begin() );
        }

        // initialization of the completions mapping
        initCompletions();

        well_efficiency_factor_ = 1.0;

        connectionRates_.resize(number_of_perforations_);

        well_productivity_index_logger_counter_ = 0;

        wellIsStopped_ = false;
        if (well.getStatus() == Well2::Status::STOP) {
            wellIsStopped_ = true;
        }

        wsolvent_ = 0.0;

        if (has_solvent && well.isInjector()) {
            auto injectorType = well_ecl_.injectorType();
            if (injectorType == Well2::InjectorType::GAS) {
                wsolvent_ = well_ecl_.getSolventFraction();
            }
        }
    }

    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updatePerforatedCell(std::vector<bool>& is_cell_perforated)
    {

        for (int perf_idx = 0; perf_idx<number_of_perforations_; ++perf_idx) {
            is_cell_perforated[well_cells_[perf_idx]] = true;
        }
    }



    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    init(const PhaseUsage* phase_usage_arg,
         const std::vector<double>& /* depth_arg */,
         const double gravity_arg,
         const int /* num_cells */)
    {
        phase_usage_ = phase_usage_arg;
        gravity_ = gravity_arg;
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    initCompletions()
    {
        assert(completions_.empty() );

        const WellConnections& connections = well_ecl_.getConnections();
        const int num_conns = connections.size();

        int num_active_connections = 0;
        for (int c = 0; c < num_conns; ++c) {
            if (connections[c].state() == Connection::State::OPEN) {
                completions_[connections[c].complnum()].push_back(num_active_connections++);
            }
        }
        assert(num_active_connections == number_of_perforations_);
    }






    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    setVFPProperties(const VFPProperties<VFPInjProperties,VFPProdProperties>* vfp_properties_arg)
    {
        vfp_properties_ = vfp_properties_arg;
    }

    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    setGuideRate(const GuideRate* guide_rate_arg)
    {
        guide_rate_ = guide_rate_arg;
    }


    template<typename TypeTag>
    const std::string&
    WellInterface<TypeTag>::
    name() const
    {
        return well_ecl_.name();
    }





    template<typename TypeTag>
    WellType
    WellInterface<TypeTag>::
    wellType() const
    {
        return well_type_;
    }




    template<typename TypeTag>
    int
    WellInterface<TypeTag>::
    indexOfWell() const
    {
        return index_of_well_;
    }






    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    getAllowCrossFlow() const
    {
        return well_ecl_.getAllowCrossFlow();
    }




    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    setWellEfficiencyFactor(const double efficiency_factor)
    {
        well_efficiency_factor_ = efficiency_factor;
    }



    template<typename TypeTag>
    const Well2&
    WellInterface<TypeTag>::
    wellEcl() const
    {
      return well_ecl_;
    }



    template<typename TypeTag>
    const PhaseUsage&
    WellInterface<TypeTag>::
    phaseUsage() const
    {
        assert(phase_usage_);

        return *phase_usage_;
    }





    template<typename TypeTag>
    int
    WellInterface<TypeTag>::
    flowPhaseToEbosCompIdx( const int phaseIdx ) const
    {
        const auto& pu = phaseUsage();
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) && pu.phase_pos[Water] == phaseIdx)
            return Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx);
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && pu.phase_pos[Oil] == phaseIdx)
            return Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx);
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) && pu.phase_pos[Gas] == phaseIdx)
            return Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);

        // for other phases return the index
        return phaseIdx;
    }

    template<typename TypeTag>
    int
    WellInterface<TypeTag>::
    ebosCompIdxToFlowCompIdx( const unsigned compIdx ) const
    {
        const auto& pu = phaseUsage();
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) && Indices::canonicalToActiveComponentIndex(FluidSystem::waterCompIdx) == compIdx)
            return pu.phase_pos[Water];
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && Indices::canonicalToActiveComponentIndex(FluidSystem::oilCompIdx) == compIdx)
            return pu.phase_pos[Oil];
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) && Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx) == compIdx)
            return pu.phase_pos[Gas];

        // for other phases return the index
        return compIdx;
    }




    template<typename TypeTag>
    double
    WellInterface<TypeTag>::
    wsolvent() const
    {
        return wsolvent_;
    }



    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    setWsolvent(const double wsolvent)
    {
       wsolvent_ = wsolvent;
    }





    template<typename TypeTag>
    double
    WellInterface<TypeTag>::
    wpolymer() const
    {
        if (!has_polymer) {
            return 0.0;
        }

        auto injectorType = well_ecl_.injectorType();

        if (injectorType == Well2::InjectorType::WATER) {
            WellPolymerProperties polymer = well_ecl_.getPolymerProperties();
            const double polymer_injection_concentration = polymer.m_polymerConcentration;
            return polymer_injection_concentration;
        } else {
            // Not a water injection well => no polymer.
            return 0.0;
        }
    }





    template<typename TypeTag>
    double
    WellInterface<TypeTag>::
    wfoam() const
    {
        if (!has_foam) {
            return 0.0;
        }

        auto injectorType = well_ecl_.injectorType();

        if (injectorType == Well2::InjectorType::GAS) {
            WellFoamProperties fprop = well_ecl_.getFoamProperties();
            return fprop.m_foamConcentration;
        } else {
            // Not a gas injection well => no foam.
            return 0.0;
        }
    }



    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    wellHasTHPConstraints(const SummaryState& summaryState) const
    {
        if (well_ecl_.isInjector()) {
            const auto controls = well_ecl_.injectionControls(summaryState);
            if (controls.hasControl(Well2::InjectorCMode::THP))
                return true;
        }

        if (well_ecl_.isProducer( )) {
            const auto controls = well_ecl_.productionControls(summaryState);
            if (controls.hasControl(Well2::ProducerCMode::THP))
                return true;
        }

        return false;

    }

    template<typename TypeTag>
    double
    WellInterface<TypeTag>::
    mostStrictBhpFromBhpLimits(const SummaryState& summaryState) const
    {
        if (well_ecl_.isInjector()) {
            const auto& controls = well_ecl_.injectionControls(summaryState);
            return controls.bhp_limit;
        }

        if (well_ecl_.isProducer( )) {
            const auto& controls = well_ecl_.productionControls(summaryState);
            return controls.bhp_limit;
        }

        return 0.0;
    }

    template<typename TypeTag>
    double
    WellInterface<TypeTag>::
    getTHPConstraint(const SummaryState& summaryState) const
    {
        if (well_ecl_.isInjector()) {
            const auto& controls = well_ecl_.injectionControls(summaryState);
            return controls.thp_limit;
        }

        if (well_ecl_.isProducer( )) {
            const auto& controls = well_ecl_.productionControls(summaryState);
            return controls.thp_limit;
        }

        return 0.0;

    }






    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updateWellControl(const Simulator& ebos_simulator,
                      WellState& well_state,
                      Opm::DeferredLogger& deferred_logger) /* const */
    {
        if (this->wellIsStopped()) {
            return;
        }

        const auto& summaryState = ebos_simulator.vanguard().summaryState();
        const auto& well = well_ecl_;
        std::string from;
        if (well.isInjector()) {
            from = Well2::InjectorCMode2String(well_state.currentInjectionControls()[index_of_well_]);
        } else {
            from = Well2::ProducerCMode2String(well_state.currentProductionControls()[index_of_well_]);
        }

        bool changed = checkConstraints(well_state, summaryState);

        auto cc = Dune::MPIHelper::getCollectiveCommunication();

        // checking whether control changed
        if (changed) {
            std::string to;
            if (well.isInjector()) {
                to = Well2::InjectorCMode2String(well_state.currentInjectionControls()[index_of_well_]);
            } else {
                to = Well2::ProducerCMode2String(well_state.currentProductionControls()[index_of_well_]);
            }
            std::ostringstream ss;
            ss << "    Switching control mode for well " << name()
               << " from " << from
               << " to " <<  to;
            if (cc.size() > 1) {
               ss << " on rank " << cc.rank();
            }
            deferred_logger.info(ss.str());
            updateWellStateWithTarget(ebos_simulator, well_state, deferred_logger);
            updatePrimaryVariables(well_state, deferred_logger);
        }
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    underPredictionMode() const
    {
        return well_ecl_.predictionMode();
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    checkRateEconLimits(const WellEconProductionLimits& econ_production_limits,
                        const WellState& well_state,
                        Opm::DeferredLogger& deferred_logger) const
    {
        const Opm::PhaseUsage& pu = phaseUsage();
        const int np = number_of_phases_;

        if (econ_production_limits.onMinOilRate()) {
            assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
            const double oil_rate = well_state.wellRates()[index_of_well_ * np + pu.phase_pos[ Oil ] ];
            const double min_oil_rate = econ_production_limits.minOilRate();
            if (std::abs(oil_rate) < min_oil_rate) {
                return true;
            }
        }

        if (econ_production_limits.onMinGasRate() ) {
            assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));
            const double gas_rate = well_state.wellRates()[index_of_well_ * np + pu.phase_pos[ Gas ] ];
            const double min_gas_rate = econ_production_limits.minGasRate();
            if (std::abs(gas_rate) < min_gas_rate) {
                return true;
            }
        }

        if (econ_production_limits.onMinLiquidRate() ) {
            assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
            assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
            const double oil_rate = well_state.wellRates()[index_of_well_ * np + pu.phase_pos[ Oil ] ];
            const double water_rate = well_state.wellRates()[index_of_well_ * np + pu.phase_pos[ Water ] ];
            const double liquid_rate = oil_rate + water_rate;
            const double min_liquid_rate = econ_production_limits.minLiquidRate();
            if (std::abs(liquid_rate) < min_liquid_rate) {
                return true;
            }
        }

        if (econ_production_limits.onMinReservoirFluidRate()) {
            deferred_logger.warning("NOT_SUPPORTING_MIN_RESERVOIR_FLUID_RATE", "Minimum reservoir fluid production rate limit is not supported yet");
        }

        return false;
    }






    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    checkMaxWaterCutLimit(const WellEconProductionLimits& econ_production_limits,
                          const WellState& well_state,
                          RatioLimitCheckReport& report) const
    {

        assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
        assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));

        // function to calculate water cut based on rates
        auto waterCut = [](const std::vector<double>& rates,
                           const PhaseUsage& pu) {

            const double oil_rate = rates[pu.phase_pos[Oil]];
            const double water_rate = rates[pu.phase_pos[Water]];

            // both rate should be in the same direction
            assert(oil_rate * water_rate >= 0.);

            const double liquid_rate = oil_rate + water_rate;
            if (liquid_rate != 0.) {
                return (water_rate / liquid_rate);
            } else {
                return 0.;
            }
        };

        const double max_water_cut_limit = econ_production_limits.maxWaterCut();
        assert(max_water_cut_limit > 0.);

        const bool watercut_limit_violated = checkMaxRatioLimitWell(well_state, max_water_cut_limit, waterCut);

        if (watercut_limit_violated) {
            report.ratio_limit_violated = true;
            checkMaxRatioLimitCompletions(well_state, max_water_cut_limit, waterCut, report);
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    checkMaxGORLimit(const WellEconProductionLimits& econ_production_limits,
                     const WellState& well_state,
                     RatioLimitCheckReport& report) const
    {

        assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
        assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));

        // function to calculate gor based on rates
        auto gor = [](const std::vector<double>& rates,
                      const PhaseUsage& pu) {

            const double oil_rate = rates[pu.phase_pos[Oil]];
            const double gas_rate = rates[pu.phase_pos[Gas]];

            // both rate should be in the same direction
            assert(oil_rate * gas_rate >= 0.);

            double gas_oil_ratio = 0.;

            if (oil_rate != 0.) {
                gas_oil_ratio = gas_rate / oil_rate;
            } else {
                if (gas_rate != 0.) {
                    gas_oil_ratio = 1.e100; // big value to mark it as violated
                } else {
                    gas_oil_ratio = 0.0;
                }
            }

            return gas_oil_ratio;
        };

        const double max_gor_limit = econ_production_limits.maxGasOilRatio();
        assert(max_gor_limit > 0.);

        const bool gor_limit_violated = checkMaxRatioLimitWell(well_state, max_gor_limit, gor);

        if (gor_limit_violated) {
            report.ratio_limit_violated = true;
            checkMaxRatioLimitCompletions(well_state, max_gor_limit, gor, report);
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    checkRatioEconLimits(const WellEconProductionLimits& econ_production_limits,
                         const WellState& well_state,
                         RatioLimitCheckReport& report,
                         Opm::DeferredLogger& deferred_logger) const
    {
        // TODO: not sure how to define the worst-offending completion when more than one
        //       ratio related limit is violated.
        //       The defintion used here is that we define the violation extent based on the
        //       ratio between the value and the corresponding limit.
        //       For each violated limit, we decide the worst-offending completion separately.
        //       Among the worst-offending completions, we use the one has the biggest violation
        //       extent.

        if (econ_production_limits.onMaxWaterCut()) {
            checkMaxWaterCutLimit(econ_production_limits, well_state, report);
        }

        if (econ_production_limits.onMaxGasOilRatio()) {
            checkMaxGORLimit(econ_production_limits, well_state, report);
        }

        if (econ_production_limits.onMaxWaterGasRatio()) {
            deferred_logger.warning("NOT_SUPPORTING_MAX_WGR", "the support for max Water-Gas ratio is not implemented yet!");
        }

        if (econ_production_limits.onMaxGasLiquidRatio()) {
            deferred_logger.warning("NOT_SUPPORTING_MAX_GLR", "the support for max Gas-Liquid ratio is not implemented yet!");
        }

        if (report.ratio_limit_violated) {
            assert(report.worst_offending_completion != INVALIDCOMPLETION);
            assert(report.violation_extent > 1.);
        }
    }




    template<typename TypeTag>
    template<typename RatioFunc>
    bool
    WellInterface<TypeTag>::
    checkMaxRatioLimitWell(const WellState& well_state,
                           const double max_ratio_limit,
                           const RatioFunc& ratioFunc) const
    {
        const int np = number_of_phases_;

        std::vector<double> well_rates(np, 0.0);

        for (int p = 0; p < np; ++p) {
            well_rates[p] = well_state.wellRates()[index_of_well_ * np + p];
        }

        const double well_ratio = ratioFunc(well_rates, phaseUsage());

        return (well_ratio > max_ratio_limit);
    }




    template<typename TypeTag>
    template<typename RatioFunc>
    void
    WellInterface<TypeTag>::
    checkMaxRatioLimitCompletions(const WellState& well_state,
                                  const double max_ratio_limit,
                                  const RatioFunc& ratioFunc,
                                  RatioLimitCheckReport& report) const
    {
        int worst_offending_completion = INVALIDCOMPLETION;

        // the maximum water cut value of the completions
        // it is used to identify the most offending completion
        double max_ratio_completion = 0;

        // look for the worst_offending_completion
        for (const auto& completion : completions_) {

            const int np = number_of_phases_;
            std::vector<double> completion_rates(np, 0.0);

            // looping through the connections associated with the completion
            const std::vector<int>& conns = completion.second;
            for (const int c : conns) {
                const int index_con = c + first_perf_;

                for (int p = 0; p < np; ++p) {
                    const double connection_rate = well_state.perfPhaseRates()[index_con * np + p];
                    completion_rates[p] += connection_rate;
                }
            } // end of for (const int c : conns)

            const double ratio_completion = ratioFunc(completion_rates, phaseUsage());

            if (ratio_completion > max_ratio_completion) {
                worst_offending_completion = completion.first;
                max_ratio_completion = ratio_completion;
            }
        } // end of for (const auto& completion : completions_)

        assert(max_ratio_completion > max_ratio_limit);
        assert(worst_offending_completion != INVALIDCOMPLETION);
        const double violation_extent = max_ratio_completion / max_ratio_limit;
        assert(violation_extent > 1.0);

        if (violation_extent > report.violation_extent) {
            report.worst_offending_completion = worst_offending_completion;
            report.violation_extent = violation_extent;
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updateWellTestState(const WellState& well_state,
                        const double& simulationTime,
                        const bool& writeMessageToOPMLog,
                        WellTestState& wellTestState,
                        Opm::DeferredLogger& deferred_logger) const
    {

        // currently, we only updateWellTestState for producers
        if (wellType() != PRODUCER) {
            return;
        }

        // Based on current understanding, only under prediction mode, we need to shut well due to various
        // reasons or limits. With more knowlage or testing cases later, this might need to be corrected.
        if (!underPredictionMode() ) {
            return;
        }

        // updating well test state based on physical (THP/BHP) limits.
        updateWellTestStatePhysical(well_state, simulationTime, writeMessageToOPMLog, wellTestState, deferred_logger);

        // updating well test state based on Economic limits.
        updateWellTestStateEconomic(well_state, simulationTime, writeMessageToOPMLog, wellTestState, deferred_logger);

        // TODO: well can be shut/closed due to other reasons
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updateWellTestStatePhysical(const WellState& /* well_state */,
                                const double simulation_time,
                                const bool write_message_to_opmlog,
                                WellTestState& well_test_state,
                                Opm::DeferredLogger& deferred_logger) const
    {
        if (!isOperable() || wellIsStopped_) {
            if (well_test_state.hasWellClosed(name(), WellTestConfig::Reason::ECONOMIC) ||
                well_test_state.hasWellClosed(name(), WellTestConfig::Reason::PHYSICAL) ) {
                // Already closed, do nothing.
            } else {
                well_test_state.closeWell(name(), WellTestConfig::Reason::PHYSICAL, simulation_time);
                if (write_message_to_opmlog) {
                    const std::string action = well_ecl_.getAutomaticShutIn() ? "shut" : "stopped";
                    const std::string msg = "Well " + name()
                        + " will be " + action + " as it can not operate under current reservoir conditions.";
                    deferred_logger.info(msg);
                }
            }
        }

    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updateWellTestStateEconomic(const WellState& well_state,
                                const double simulation_time,
                                const bool write_message_to_opmlog,
                                WellTestState& well_test_state,
                                Opm::DeferredLogger& deferred_logger) const
    {
        if (wellIsStopped_)
            return;

        const WellEconProductionLimits& econ_production_limits = well_ecl_.getEconLimits();

        // if no limit is effective here, then continue to the next well
        if ( !econ_production_limits.onAnyEffectiveLimit() ) {
            return;
        }

        // flag to check if the mim oil/gas rate limit is violated
        bool rate_limit_violated = false;

        // for the moment, we only handle rate limits, not handling potential limits
        // the potential limits should not be difficult to add
        const auto& quantity_limit = econ_production_limits.quantityLimit();
        if (quantity_limit == WellEconProductionLimits::QuantityLimit::POTN) {
            const std::string msg = std::string("POTN limit for well ") + name() + std::string(" is not supported for the moment. \n")
                                  + std::string("All the limits will be evaluated based on RATE. ");
            deferred_logger.warning("NOT_SUPPORTING_POTN", msg);
        }

        if (econ_production_limits.onAnyRateLimit()) {
            rate_limit_violated = checkRateEconLimits(econ_production_limits, well_state, deferred_logger);
        }

        if (rate_limit_violated) {
            if (econ_production_limits.endRun()) {
                const std::string warning_message = std::string("ending run after well closed due to economic limits")
                                                  + std::string("is not supported yet \n")
                                                  + std::string("the program will keep running after ") + name()
                                                  + std::string(" is closed");
                deferred_logger.warning("NOT_SUPPORTING_ENDRUN", warning_message);
            }

            if (econ_production_limits.validFollowonWell()) {
                deferred_logger.warning("NOT_SUPPORTING_FOLLOWONWELL", "opening following on well after well closed is not supported yet");
            }

            well_test_state.closeWell(name(), WellTestConfig::Reason::ECONOMIC, simulation_time);
            if (write_message_to_opmlog) {
                if (well_ecl_.getAutomaticShutIn()) {
                    const std::string msg = std::string("well ") + name() + std::string(" will be shut due to rate economic limit");
                    deferred_logger.info(msg);
                } else {
                    const std::string msg = std::string("well ") + name() + std::string(" will be stopped due to rate economic limit");
                    deferred_logger.info(msg);
                }
            }
            // the well is closed, not need to check other limits
            return;
        }


        if ( !econ_production_limits.onAnyRatioLimit() ) {
            // there is no need to check the ratio limits
            return;
        }

        // checking for ratio related limits, mostly all kinds of ratio.
        RatioLimitCheckReport ratio_report;

        checkRatioEconLimits(econ_production_limits, well_state, ratio_report, deferred_logger);

        if (ratio_report.ratio_limit_violated) {
            const auto workover = econ_production_limits.workover();
            switch (workover) {
            case WellEconProductionLimits::EconWorkover::CON:
                {
                    const int worst_offending_completion = ratio_report.worst_offending_completion;

                    well_test_state.addClosedCompletion(name(), worst_offending_completion, simulation_time);
                    if (write_message_to_opmlog) {
                        if (worst_offending_completion < 0) {
                            const std::string msg = std::string("Connection ") + std::to_string(- worst_offending_completion)
                                    + std::string(" for well ") + name() + std::string(" will be closed due to economic limit");
                            deferred_logger.info(msg);
                        } else {
                            const std::string msg = std::string("Completion ") + std::to_string(worst_offending_completion)
                                    + std::string(" for well ") + name() + std::string(" will be closed due to economic limit");
                            deferred_logger.info(msg);
                        }
                    }

                    bool allCompletionsClosed = true;
                    const auto& connections = well_ecl_.getConnections();
                    for (const auto& connection : connections) {
                        if (!well_test_state.hasCompletion(name(), connection.complnum())) {
                            allCompletionsClosed = false;
                        }
                    }

                    if (allCompletionsClosed) {
                        well_test_state.closeWell(name(), WellTestConfig::Reason::ECONOMIC, simulation_time);
                        if (write_message_to_opmlog) {
                            if (well_ecl_.getAutomaticShutIn()) {
                                const std::string msg = name() + std::string(" will be shut due to last completion closed");
                                deferred_logger.info(msg);
                            } else {
                                const std::string msg = name() + std::string(" will be stopped due to last completion closed");
                                deferred_logger.info(msg);
                            }
                        }
                    }
                    break;
                }
            case WellEconProductionLimits::EconWorkover::WELL:
                {
                well_test_state.closeWell(name(), WellTestConfig::Reason::ECONOMIC, simulation_time);
                if (write_message_to_opmlog) {
                    if (well_ecl_.getAutomaticShutIn()) {
                        // tell the control that the well is closed
                        const std::string msg = name() + std::string(" will be shut due to ratio economic limit");
                        deferred_logger.info(msg);
                    } else {
                        const std::string msg = name() + std::string(" will be stopped due to ratio economic limit");
                        deferred_logger.info(msg);
                    }
                }
                    break;
                }
            case WellEconProductionLimits::EconWorkover::NONE:
                break;
                default:
                {
                    deferred_logger.warning("NOT_SUPPORTED_WORKOVER_TYPE",
                                            "not supporting workover type " + WellEconProductionLimits::EconWorkover2String(workover) );
                }
            }
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    wellTesting(const Simulator& simulator, const std::vector<double>& B_avg,
                const double simulation_time, const int report_step,
                const WellTestConfig::Reason testing_reason,
                /* const */ WellState& well_state,
                WellTestState& well_test_state,
                Opm::DeferredLogger& deferred_logger)
    {
        if (testing_reason == WellTestConfig::Reason::PHYSICAL) {
            wellTestingPhysical(simulator, B_avg, simulation_time, report_step,
                                well_state, well_test_state, deferred_logger);
        }

        if (testing_reason == WellTestConfig::Reason::ECONOMIC) {
            wellTestingEconomic(simulator, B_avg, simulation_time,
                                well_state, well_test_state, deferred_logger);
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    wellTestingEconomic(const Simulator& simulator, const std::vector<double>& B_avg,
                        const double simulation_time, const WellState& well_state,
                        WellTestState& welltest_state, Opm::DeferredLogger& deferred_logger)
    {
        deferred_logger.info(" well " + name() + " is being tested for economic limits");

        WellState well_state_copy = well_state;

        updateWellStateWithTarget(simulator, well_state_copy, deferred_logger);
        calculateExplicitQuantities(simulator, well_state_copy, deferred_logger);
        updatePrimaryVariables(well_state_copy, deferred_logger);
        initPrimaryVariablesEvaluation();

        WellTestState welltest_state_temp;

        bool testWell = true;
        // if a well is closed because all completions are closed, we need to check each completion
        // individually. We first open all completions, then we close one by one by calling updateWellTestState
        // untill the number of closed completions do not increase anymore.
        while (testWell) {
            const size_t original_number_closed_completions = welltest_state_temp.sizeCompletions();
            solveWellForTesting(simulator, well_state_copy, B_avg, deferred_logger);
            updateWellTestState(well_state_copy, simulation_time, /*writeMessageToOPMLog=*/ false, welltest_state_temp, deferred_logger);
            closeCompletions(welltest_state_temp);

            // Stop testing if the well is closed or shut due to all completions shut
            // Also check if number of completions has increased. If the number of closed completions do not increased
            // we stop the testing.
            // TODO: it can be tricky here, if the well is shut/closed due to other reasons
            if ( welltest_state_temp.sizeWells() > 0 ||
                (original_number_closed_completions == welltest_state_temp.sizeCompletions()) ) {
                    testWell = false; // this terminates the while loop
            }
        }

        // update wellTestState if the well test succeeds
        if (!welltest_state_temp.hasWellClosed(name(), WellTestConfig::Reason::ECONOMIC)) {
            welltest_state.openWell(name(), WellTestConfig::Reason::ECONOMIC);
            const std::string msg = std::string("well ") + name() + std::string(" is re-opened through ECONOMIC testing");
            deferred_logger.info(msg);

            // also reopen completions
            for (auto& completion : well_ecl_.getCompletions()) {
                if (!welltest_state_temp.hasCompletion(name(), completion.first)) {
                    welltest_state.dropCompletion(name(), completion.first);
                }
            }
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    computeRepRadiusPerfLength(const Grid& grid,
                               const std::vector<int>& cartesian_to_compressed,
                               Opm::DeferredLogger& deferred_logger
                               )
    {
        const int* cart_dims = Opm::UgGridHelpers::cartDims(grid);
        auto cell_to_faces = Opm::UgGridHelpers::cell2Faces(grid);
        auto begin_face_centroids = Opm::UgGridHelpers::beginFaceCentroids(grid);

        const int nperf = number_of_perforations_;

        perf_rep_radius_.clear();
        perf_length_.clear();
        bore_diameters_.clear();

        perf_rep_radius_.reserve(nperf);
        perf_length_.reserve(nperf);
        bore_diameters_.reserve(nperf);

        // COMPDAT handling
        const auto& connectionSet = well_ecl_.getConnections();
        for (size_t c=0; c<connectionSet.size(); c++) {
            const auto& connection = connectionSet.get(c);
            if (connection.state() == Connection::State::OPEN) {
                const int i = connection.getI();
                const int j = connection.getJ();
                const int k = connection.getK();

                const int* cpgdim = cart_dims;
                const int cart_grid_indx = i + cpgdim[0]*(j + cpgdim[1]*k);
                const int cell = cartesian_to_compressed[cart_grid_indx];

                if (cell < 0) {
                    OPM_DEFLOG_THROW(std::runtime_error, "Cell with i,j,k indices " << i << ' ' << j << ' '
                              << k << " not found in grid (well = " << name() << ')', deferred_logger);
                }

                {
                    double radius = connection.rw();
                    const std::array<double, 3> cubical =
                    WellsManagerDetail::getCubeDim<3>(cell_to_faces, begin_face_centroids, cell);

                    double re; // area equivalent radius of the grid block
                    double perf_length; // the length of the well perforation

                    switch (connection.dir()) {
                        case Opm::Connection::Direction::X:
                            re = std::sqrt(cubical[1] * cubical[2] / M_PI);
                            perf_length = cubical[0];
                            break;
                        case Opm::Connection::Direction::Y:
                            re = std::sqrt(cubical[0] * cubical[2] / M_PI);
                            perf_length = cubical[1];
                            break;
                        case Opm::Connection::Direction::Z:
                            re = std::sqrt(cubical[0] * cubical[1] / M_PI);
                            perf_length = cubical[2];
                            break;
                        default:
                            OPM_DEFLOG_THROW(std::runtime_error, " Dirtecion of well is not supported ", deferred_logger);
                    }

                    const double repR = std::sqrt(re * radius);
                    perf_rep_radius_.push_back(repR);
                    perf_length_.push_back(perf_length);
                    bore_diameters_.push_back(2. * radius);
                }
            }
        }
    }

    template<typename TypeTag>
    double
    WellInterface<TypeTag>::scalingFactor(const int phaseIdx) const
    {
        const auto& pu = phaseUsage();
        if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx) && pu.phase_pos[Water] == phaseIdx)
            return 1.0;
        if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx) && pu.phase_pos[Oil] == phaseIdx)
            return 1.0;
        if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx) && pu.phase_pos[Gas] == phaseIdx)
            return 0.01;
        if (has_solvent && phaseIdx == contiSolventEqIdx )
            return 0.01;

        // we should not come this far
        assert(false);
        return 1.0;
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::isVFPActive(Opm::DeferredLogger& deferred_logger) const
    {
        // since the well_controls only handles the VFP number when THP constraint/target is there.
        // we need to get the table number through the parser, in case THP constraint/target is not there.
        // When THP control/limit is not active, if available VFP table is provided, we will still need to
        // update THP value. However, it will only used for output purpose.
        if (well_type_ == PRODUCER) { // producer
            const int table_id = well_ecl_.vfp_table_number();
            if (table_id <= 0) {
                return false;
            } else {
                if (vfp_properties_->getProd()->hasTable(table_id)) {
                    return true;
                } else {
                    OPM_DEFLOG_THROW(std::runtime_error, "VFPPROD table " << std::to_string(table_id) << " is specfied,"
                              << " for well " << name() << ", while we could not access it during simulation", deferred_logger);
                }
            }

        } else { // injector
            const int table_id = well_ecl_.vfp_table_number();
            if (table_id <= 0) {
                return false;
            } else {
                if (vfp_properties_->getInj()->hasTable(table_id)) {
                    return true;
                } else {
                    OPM_DEFLOG_THROW(std::runtime_error, "VFPINJ table " << std::to_string(table_id) << " is specfied,"
                              << " for well " << name() << ", while we could not access it during simulation", deferred_logger);
                }
            }
        }
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    solveWellEqUntilConverged(const Simulator& ebosSimulator,
                              const std::vector<double>& B_avg,
                              WellState& well_state,
                              Opm::DeferredLogger& deferred_logger)
    {
        const int max_iter = param_.max_welleq_iter_;
        int it = 0;
        const double dt = 1.0; //not used for the well tests
        bool converged;
        WellState well_state0 = well_state;
        do {
            assembleWellEq(ebosSimulator, B_avg, dt, well_state, deferred_logger);

            auto report = getWellConvergence(well_state, B_avg, deferred_logger);

            converged = report.converged();
            if (converged) {
                break;
            }

            ++it;
            solveEqAndUpdateWellState(well_state, deferred_logger);

            // We don't allow for switching well controls while computing well potentials and testing wells
            // updateWellControl(ebosSimulator, well_state, deferred_logger);
            initPrimaryVariablesEvaluation();
        } while (it < max_iter);

        return converged;
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::calculateReservoirRates(WellState& well_state) const
    {
        const int fipreg = 0; // not considering the region for now
        const int np = number_of_phases_;

        std::vector<double> surface_rates(np, 0.0);
        const int well_rate_index = np * index_of_well_;
        for (int p = 0; p < np; ++p) {
            surface_rates[p] = well_state.wellRates()[well_rate_index + p];
        }

        std::vector<double> voidage_rates(np, 0.0);
        rateConverter_.calcReservoirVoidageRates(fipreg, pvtRegionIdx_, surface_rates, voidage_rates);

        for (int p = 0; p < np; ++p) {
            well_state.wellReservoirRates()[well_rate_index + p] = voidage_rates[p];
        }
    }

    template<typename TypeTag>
    void
    WellInterface<TypeTag>::closeCompletions(WellTestState& wellTestState)
    {
        const auto& connections = well_ecl_.getConnections();
        int perfIdx = 0;
        for (const auto& connection : connections) {
            if (wellTestState.hasCompletion(name(), connection.complnum())) {
                well_index_[perfIdx] = 0.0;
            }
            perfIdx++;
        }
    }

    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    solveWellForTesting(const Simulator& ebosSimulator, WellState& well_state,
                        const std::vector<double>& B_avg,
                        Opm::DeferredLogger& deferred_logger)
    {
        // keep a copy of the original well state
        const WellState well_state0 = well_state;
        const bool converged = solveWellEqUntilConverged(ebosSimulator, B_avg, well_state, deferred_logger);
        if (converged) {
            deferred_logger.debug("WellTest: Well equation for well " + name() +  " converged");
        } else {
            const int max_iter = param_.max_welleq_iter_;
            deferred_logger.debug("WellTest: Well equation for well " +name() + " failed converging in "
                          + std::to_string(max_iter) + " iterations");
            well_state = well_state0;
        }
    }

    template<typename TypeTag>
    void
    WellInterface<TypeTag>::scaleProductivityIndex(const int perfIdx, double& productivity_index, const bool new_well, Opm::DeferredLogger& deferred_logger)
    {
        const auto& connection = well_ecl_.getConnections()[perfIdx];
        if (well_ecl_.getDrainageRadius() < 0) {
            if (new_well && perfIdx == 0) {
                deferred_logger.warning("PRODUCTIVITY_INDEX_WARNING", "Negative drainage radius not supported. The productivity index is set to zero");
            }
            productivity_index = 0.0;
            return;
        }

        if (connection.r0() > well_ecl_.getDrainageRadius()) {
            if (new_well && well_productivity_index_logger_counter_ < 1) {
                deferred_logger.info("PRODUCTIVITY_INDEX_INFO", "The effective radius is larger than the well drainage radius for well " + name() +
                             " They are set to equal in the well productivity index calculations");
                well_productivity_index_logger_counter_++;
            }
            return;
        }

        // For zero drainage radius the productivity index is just the transmissibility times the mobility
        if (well_ecl_.getDrainageRadius() == 0) {
            return;
        }

        // Scale the productivity index to account for the drainage radius.
        // Assumes steady radial flow only valied for horizontal wells
        productivity_index *=
        (std::log(connection.r0() / connection.rw()) + connection.skinFactor()) /
        (std::log(well_ecl_.getDrainageRadius() / connection.rw()) + connection.skinFactor());
    }

    template<typename TypeTag>
    void
    WellInterface<TypeTag>::addCellRates(RateVector& rates, int cellIdx) const
    {
        for (int perfIdx = 0; perfIdx < number_of_perforations_; ++perfIdx) {
            if (cells()[perfIdx] == cellIdx) {
                for (int i = 0; i < RateVector::dimension; ++i) {
                    rates[i] += connectionRates_[perfIdx][i];
                }
            }
        }
    }

    template<typename TypeTag>
    typename WellInterface<TypeTag>::Scalar
    WellInterface<TypeTag>::volumetricSurfaceRateForConnection(int cellIdx, int phaseIdx) const {
        for (int perfIdx = 0; perfIdx < number_of_perforations_; ++perfIdx) {
            if (cells()[perfIdx] == cellIdx) {
                const unsigned activeCompIdx = Indices::canonicalToActiveComponentIndex(FluidSystem::solventComponentIndex(phaseIdx));
                return connectionRates_[perfIdx][activeCompIdx].value();
            }
        }
        // this is not thread safe
        OPM_THROW(std::invalid_argument, "The well with name " + name()
                  + " does not perforate cell " + std::to_string(cellIdx));
        return 0.0;
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    isOperable() const {
        return operability_status_.isOperable();
    }

    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    checkConstraints(WellState& well_state, const SummaryState& summaryState) {

        const auto& well = well_ecl_;
        const PhaseUsage& pu = phaseUsage();
        const int well_index = index_of_well_;
        const auto wellrate_index = well_index * pu.num_phases;
        // bool changed = false;

//        // Stopped wells can not change control
//        if (currentControl == "STOP")
//            return newControl;

        if (well.isInjector()) {
            const auto controls = well.injectionControls(summaryState);
            Opm::Well2::InjectorCMode& currentControl = well_state.currentInjectionControls()[well_index];

            if (controls.hasControl(Well2::InjectorCMode::BHP) && currentControl != Well2::InjectorCMode::BHP)
            {
                const auto& bhp = controls.bhp_limit;
                double current_bhp = well_state.bhp()[well_index];
                if (bhp < current_bhp) {
                    currentControl = Well2::InjectorCMode::BHP;
                    return true;
                }
            }

            if (controls.hasControl(Well2::InjectorCMode::RATE) && currentControl != Well2::InjectorCMode::RATE)
            {
                Well2::InjectorType injectorType = controls.injector_type;
                double current_rate = 0.0;

                switch (injectorType) {
                case Well2::InjectorType::WATER:
                {
                    current_rate = well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Aqua] ];
                    break;
                }
                case Well2::InjectorType::OIL:
                {
                    current_rate = well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Liquid] ];
                    break;
                }
                case Well2::InjectorType::GAS:
                {
                    current_rate = well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Vapour] ];
                    break;
                }
                default:
                    throw("Expected WATER, OIL or GAS as type for injectors " + well.name());
                }

                if (controls.surface_rate < current_rate) {
                    currentControl = Well2::InjectorCMode::RATE;
                    return true;
                }

            }

            if (controls.hasControl(Well2::InjectorCMode::RESV) && currentControl != Well2::InjectorCMode::RESV)
            {
                double current_rate = 0.0;
                if( pu.phase_used[BlackoilPhases::Aqua] )
                    current_rate += well_state.wellReservoirRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Aqua] ];

                if( pu.phase_used[BlackoilPhases::Liquid] )
                    current_rate += well_state.wellReservoirRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Liquid] ];

                if( pu.phase_used[BlackoilPhases::Vapour] )
                    current_rate += well_state.wellReservoirRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Vapour] ];

                if (controls.reservoir_rate < current_rate) {
                    currentControl = Well2::InjectorCMode::RESV;
                    return true;
                }
            }

            if (controls.hasControl(Well2::InjectorCMode::THP) && currentControl != Well2::InjectorCMode::THP)
            {
                const auto& thp = controls.thp_limit;
                double current_thp = well_state.thp()[well_index];
                if (thp < current_thp) {
                    currentControl = Well2::InjectorCMode::THP;
                    return true;
                }
            }

        }

        if (well.isProducer( )) {
            const auto controls = well.productionControls(summaryState);
            Well2::ProducerCMode& currentControl = well_state.currentProductionControls()[well_index];

            if (controls.hasControl(Well2::ProducerCMode::BHP) && currentControl != Well2::ProducerCMode::BHP )
            {
                const double bhp = controls.bhp_limit;
                double current_bhp = well_state.bhp()[well_index];
                if (bhp > current_bhp) {
                    currentControl = Well2::ProducerCMode::BHP;
                    return true;
                }
            }

            if (controls.hasControl(Well2::ProducerCMode::ORAT) && currentControl != Well2::ProducerCMode::ORAT) {
                double current_rate = -well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Liquid] ];
                if (controls.oil_rate < current_rate  ) {
                    currentControl = Well2::ProducerCMode::ORAT;
                    return true;
                }
            }

            if (controls.hasControl(Well2::ProducerCMode::WRAT) && currentControl != Well2::ProducerCMode::WRAT ) {
                double current_rate = -well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Aqua] ];
                if (controls.water_rate < current_rate  ) {
                    currentControl = Well2::ProducerCMode::WRAT;
                    return true;
                }
            }

            if (controls.hasControl(Well2::ProducerCMode::GRAT) && currentControl != Well2::ProducerCMode::GRAT ) {
                double current_rate = -well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Vapour] ];
                if (controls.gas_rate < current_rate  ) {
                    currentControl = Well2::ProducerCMode::GRAT;
                    return true;
                }
            }

            if (controls.hasControl(Well2::ProducerCMode::LRAT) && currentControl != Well2::ProducerCMode::LRAT) {
                double current_rate = -well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Liquid] ];
                current_rate -= well_state.wellRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Aqua] ];
                if (controls.liquid_rate < current_rate  ) {
                    currentControl = Well2::ProducerCMode::LRAT;
                    return true;
                }
            }

            if (controls.hasControl(Well2::ProducerCMode::RESV) && currentControl != Well2::ProducerCMode::RESV ) {
                double current_rate = 0.0;
                if( pu.phase_used[BlackoilPhases::Aqua] )
                    current_rate -= well_state.wellReservoirRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Aqua] ];

                if( pu.phase_used[BlackoilPhases::Liquid] )
                    current_rate -= well_state.wellReservoirRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Liquid] ];

                if( pu.phase_used[BlackoilPhases::Vapour] )
                    current_rate -= well_state.wellReservoirRates()[ wellrate_index + pu.phase_pos[BlackoilPhases::Vapour] ];

                if (controls.prediction_mode && controls.resv_rate > current_rate) {
                    currentControl = Well2::ProducerCMode::RESV;
                    return true;
                }

                if (!controls.prediction_mode) {
                    const int fipreg = 0; // not considering the region for now
                    const int np = number_of_phases_;

                    std::vector<double> surface_rates(np, 0.0);
                    if( pu.phase_used[BlackoilPhases::Aqua] )
                        surface_rates[pu.phase_pos[BlackoilPhases::Aqua]] = controls.water_rate;
                    if( pu.phase_used[BlackoilPhases::Liquid] )
                        surface_rates[pu.phase_pos[BlackoilPhases::Liquid]] = controls.oil_rate;
                    if( pu.phase_used[BlackoilPhases::Vapour] )
                        surface_rates[pu.phase_pos[BlackoilPhases::Vapour]] = controls.gas_rate;

                    std::vector<double> voidage_rates(np, 0.0);
                    rateConverter_.calcReservoirVoidageRates(fipreg, pvtRegionIdx_, surface_rates, voidage_rates);

                    double resv_rate = 0.0;
                    for (int p = 0; p < np; ++p) {
                        resv_rate += voidage_rates[p];
                    }

                    if (resv_rate < current_rate) {
                        currentControl = Well2::ProducerCMode::RESV;
                        return true;
                    }
                }
            }

            if (controls.hasControl(Well2::ProducerCMode::THP) && currentControl != Well2::ProducerCMode::THP)
            {
                const auto& thp = controls.thp_limit;
                double current_thp =  well_state.thp()[well_index];
                if (thp > current_thp) {
                    currentControl = Well2::ProducerCMode::THP;
                    return true;
                }
            }

        }

        return false;
    }


}
