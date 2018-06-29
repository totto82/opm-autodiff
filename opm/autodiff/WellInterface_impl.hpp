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


namespace Opm
{


    template<typename TypeTag>
    WellInterface<TypeTag>::
    WellInterface(const Well* well, const Group& group, const int time_step, const Wells* wells,
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
        if (!well) {
            OPM_THROW(std::invalid_argument, "Null pointer of Well is used to construct WellInterface");
        }

        if (time_step < 0) {
            OPM_THROW(std::invalid_argument, "Negtive time step is used to construct WellInterface");
        }

        if (!wells) {
            OPM_THROW(std::invalid_argument, "Null pointer of Wells is used to construct WellInterface");
        }

        const std::string& well_name = well->name();

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

        well_controls_ = wells->ctrls[index_well];
        if (well_ecl_->isInjector(current_step_))
        {
            const WellInjectionProperties& injectionProperties = well_ecl_->getInjectionProperties(current_step_);
            current_control_ = WellInjector::ControlMode2String(injectionProperties.controlMode);
        } else {
            const WellProductionProperties& productionProperties = well_ecl_->getProductionProperties(current_step_);
            current_control_ = WellProducer::ControlMode2String(productionProperties.controlMode);
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

        well_efficiency_factor_ = 1.0;


        // Implement group control

        guideRate_ = well_ecl_->getGuideRate(time_step);
        //update guideRate based on well_potensial

        //control_eq inj
        if (well_type_ == INJECTOR)
        {
            switch (group.getInjectionPhase(time_step)) {
            case Phase::OIL:
                groupCompIdx_ = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                break;
            case Phase::GAS:
                groupCompIdx_ = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                break;
            case Phase::WATER:
                groupCompIdx_ = Indices::canonicalToActiveComponentIndex(FluidSystem::gasCompIdx);
                break;
            default:
                OPM_THROW(std::runtime_error, "Only OIL, GAS and WATER phase is supported for group injection. Issue in well "  + name() );
                break;
            }
            groupTarget_ = group.getInjectionRate(time_step) * group.getGroupEfficiencyFactor(time_step);
        } else {

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
    setVFPProperties(const VFPProperties* vfp_properties_arg)
    {
        vfp_properties_ = vfp_properties_arg;
    }





    template<typename TypeTag>
    const std::string&
    WellInterface<TypeTag>::
    name() const
    {
        return well_ecl_->name();
    }





    template<typename TypeTag>
    WellType
    WellInterface<TypeTag>::
    wellType() const
    {
        return well_type_;
    }





    template<typename TypeTag>
    WellControls*
    WellInterface<TypeTag>::
    wellControls() const
    {
        return well_controls_;
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    getAllowCrossFlow() const
    {
        return well_ecl_->getAllowCrossFlow();
    }




    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    setWellEfficiencyFactor(const double efficiency_factor)
    {
        well_efficiency_factor_ = efficiency_factor;
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
        if (!has_solvent) {
            return 0.0;
        }

        WellInjectionProperties injection = well_ecl_->getInjectionProperties(current_step_);
        if (injection.injectorType == WellInjector::GAS) {
            double solvent_fraction = well_ecl_->getSolventFraction(current_step_);
            return solvent_fraction;
        } else {
            // Not a gas injection well => no solvent.
            return 0.0;
        }
    }





    template<typename TypeTag>
    double
    WellInterface<TypeTag>::
    wpolymer() const
    {
        if (!has_polymer) {
            return 0.0;
        }

        WellInjectionProperties injection = well_ecl_->getInjectionProperties(current_step_);
        WellPolymerProperties polymer = well_ecl_->getPolymerProperties(current_step_);

        if (injection.injectorType == WellInjector::WATER) {
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
    mostStrictBhpFromBhpLimits() const
    {
        if (well_ecl_->isInjector(current_step_))
        {
            const WellInjectionProperties& injectionProperties = well_ecl_->getInjectionProperties(current_step_);
            return std::min(std::numeric_limits<double>::max(),injectionProperties.BHPLimit);
        }
        // else producer
        const WellProductionProperties& productionProperties = well_ecl_->getProductionProperties(current_step_);
        return std::max(-std::numeric_limits<double>::max(),productionProperties.BHPLimit);
    }




    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    wellHasTHPConstraints() const
    {
        if (well_ecl_->isInjector(current_step_))
        {
            const WellInjectionProperties& injectionProperties = well_ecl_->getInjectionProperties(current_step_);
            return injectionProperties.hasInjectionControl(WellInjector::THP);
        }
        // else producer
        const WellProductionProperties& productionProperties = well_ecl_->getProductionProperties(current_step_);
        return productionProperties.hasProductionControl(WellProducer::THP);
    }



    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updateWellControl(WellState& well_state,
                      wellhelpers::WellSwitchingLogger& logger)
    {

        const PhaseUsage& pu = phaseUsage();
        if (well_ecl_->isInjector(current_step_))
        {
            const WellInjectionProperties& injectionProperties = well_ecl_->getInjectionProperties(current_step_);
            WellInjector::ControlModeEnum current = WellInjector::ControlModeFromString(current_control_);

            if (injectionProperties.hasInjectionControl(WellInjector::RATE) && current != WellInjector::RATE)
            {
                // this can be simplified when well_state is refactored
                double rate = 0.;
                switch(injectionProperties.injectorType) {
                case(WellInjector::WATER): {
                    assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
                    rate = well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Water ] ];
                    break;
                }
                case(WellInjector::OIL): {
                    assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
                    rate = well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Oil ] ];
                    break;
                }
                case(WellInjector::GAS): {
                    assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));
                    rate = well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Gas ] ];
                    break;
                }
                case(WellInjector::MULTI): {
                    OPM_THROW(std::runtime_error, "MULTI control for injector not supported " << name());
                }

                }

                if (rate > injectionProperties.surfaceInjectionRate) {
#warning TODO make a logger to avoid copy
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellInjector::ControlMode2String(current)
                       << " to " << WellInjector::ControlMode2String(WellInjector::RATE);
                    OpmLog::info(ss.str());
                    current = WellInjector::RATE;
                }
            }
            if (injectionProperties.hasInjectionControl(WellInjector::RESV) && current != WellInjector::RESV)
            {
                std::vector<double> rates(number_of_phases_, 0.);
                if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                    rates[ Water ] = well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Water ] ];
                }
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    rates[ Oil ] =  well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Oil ] ];
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    rates[ Gas ] =  well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Gas ] ];
                }

                std::vector<double> rates_resv(number_of_phases_);
                rateConverter_.calcReservoirVoidageRates(/*fipreg*/ 0, pvtRegionIdx_, rates, rates_resv);
                double rate = std::accumulate(rates_resv.begin(), rates_resv.end(), 0.0);


                if (rate > injectionProperties.reservoirInjectionRate) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellInjector::ControlMode2String(current)
                       << " to " << WellInjector::ControlMode2String(WellInjector::RESV);
                    OpmLog::info(ss.str());
                    current = WellInjector::RESV;
                }
            }

            if (injectionProperties.hasInjectionControl(WellInjector::THP) && current != WellInjector::THP)
            {
                if (well_state.thp()[index_of_well_] > injectionProperties.THPLimit) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellInjector::ControlMode2String(current)
                       << " to " << WellInjector::ControlMode2String(WellInjector::THP);
                    OpmLog::info(ss.str());
                    current = WellInjector::THP;
                }
            }
            if (injectionProperties.hasInjectionControl(WellInjector::BHP) && current != WellInjector::BHP)
            {
                if (well_state.bhp()[index_of_well_] > injectionProperties.BHPLimit) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellInjector::ControlMode2String(current)
                       << " to " << WellInjector::ControlMode2String(WellInjector::BHP);
                    OpmLog::info(ss.str());
                    current = WellInjector::BHP;
                }
            }
            if (injectionProperties.hasInjectionControl(WellInjector::GRUP) && current != WellInjector::GRUP)
            {
#warning need to think about this
            }
            // update the current control
            current_control_ = WellInjector::ControlMode2String(current);
        }
        //Producer
        else
        {
            const WellProductionProperties& productionProperties = well_ecl_->getProductionProperties(current_step_);
            WellProducer::ControlModeEnum current = WellProducer::ControlModeFromString(current_control_);

            if (productionProperties.hasProductionControl(WellProducer::ORAT) && current != WellProducer::ORAT)
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
                double rate = - well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Oil ] ];
                if (rate > productionProperties.OilRate) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::ORAT);
                    OpmLog::info(ss.str());
                    current = WellProducer::ORAT;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::WRAT) && current != WellProducer::WRAT)
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
                double rate = - well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[Water] ];
                if (rate > productionProperties.WaterRate) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::WRAT);
                    OpmLog::info(ss.str());
                    current = WellProducer::WRAT;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::GRAT) && current != WellProducer::GRAT)
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx));
                double rate =  - well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[Gas] ];

                if (rate > productionProperties.GasRate) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::GRAT);
                    OpmLog::info(ss.str());
                    current = WellProducer::GRAT;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::LRAT) && current != WellProducer::LRAT)
            {
                assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));
                assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
                double rate = - (well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[Water] ] + well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[Oil] ]);
                if (rate > productionProperties.LiquidRate) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::LRAT);
                    OpmLog::info(ss.str());
                    current = WellProducer::LRAT;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::CRAT) && current != WellProducer::CRAT)
            {
                OPM_THROW(std::runtime_error, "CRAT control not supported " << name());
            }
            if (productionProperties.hasProductionControl(WellProducer::RESV) && current != WellProducer::RESV)
            {
                std::vector<double> rates(number_of_phases_, 0.);
                if (FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx)) {
                    rates[ Water ] = - well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Water ] ];
                }
                if (FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx)) {
                    rates[ Oil ] =  - well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Oil ] ];
                }
                if (FluidSystem::phaseIsActive(FluidSystem::gasPhaseIdx)) {
                    rates[ Gas ] =  - well_state.wellRates()[index_of_well_*number_of_phases_ + pu.phase_pos[ Gas ] ];
                }
                std::vector<double> rates_resv(number_of_phases_, 0.);
                rateConverter_.calcReservoirVoidageRates(/*fipreg*/ 0, pvtRegionIdx_, rates, rates_resv);
                double rate = std::accumulate(rates_resv.begin(), rates_resv.end(), 0.0);

                if (rate > productionProperties.ResVRate) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::RESV);
                    OpmLog::info(ss.str());
                    current = WellProducer::RESV;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::BHP) && current != WellProducer::BHP)
            {
                if (well_state.bhp()[index_of_well_] < productionProperties.BHPLimit) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::BHP);
                    OpmLog::info(ss.str());
                    current = WellProducer::BHP;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::THP) && current != WellProducer::THP)
            {
                if (well_state.thp()[index_of_well_] < productionProperties.THPLimit) {
                    std::ostringstream ss;
                    ss << "    Switching control mode for well " << name()
                       << " from " << WellProducer::ControlMode2String(current)
                       << " to " << WellProducer::ControlMode2String(WellProducer::THP);
                    OpmLog::info(ss.str());
                    current = WellProducer::THP;
                }
            }
            if (productionProperties.hasProductionControl(WellProducer::GRUP) && current != WellProducer::GRUP)
            {
#warning need to think about this
            }

            current_control_ = WellProducer::ControlMode2String(current);
        }

//        // the new well control indices after all the related updates,
//        const int updated_control_index = well_state.currentControls()[w];

//        // checking whether control changed
//        if (updated_control_index != old_control_index) {
//            logger.wellSwitched(name(),
//                                well_controls_iget_type(wc, old_control_index),
//                                well_controls_iget_type(wc, updated_control_index));
//        }

//        if (updated_control_index != old_control_index) { //  || well_collection_->groupControlActive()) {
//            //updateWellStateWithTarget(well_state);
//        }
    }





    template<typename TypeTag>
    bool
    WellInterface<TypeTag>::
    checkRateEconLimits(const WellEconProductionLimits& econ_production_limits,
                        const WellState& well_state) const
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
            OpmLog::warning("NOT_SUPPORTING_MIN_RESERVOIR_FLUID_RATE", "Minimum reservoir fluid production rate limit is not supported yet");
        }

        return false;
    }






    template<typename TypeTag>
    typename WellInterface<TypeTag>::RatioCheckTuple
    WellInterface<TypeTag>::
    checkMaxWaterCutLimit(const WellEconProductionLimits& econ_production_limits,
                          const WellState& well_state) const
    {
        bool water_cut_limit_violated = false;
        int worst_offending_connection = INVALIDCONNECTION;
        bool last_connection = false;
        double violation_extent = -1.0;

        const int np = number_of_phases_;
        const Opm::PhaseUsage& pu = phaseUsage();
        const int well_number = index_of_well_;

        assert(FluidSystem::phaseIsActive(FluidSystem::oilPhaseIdx));
        assert(FluidSystem::phaseIsActive(FluidSystem::waterPhaseIdx));

        const double oil_rate = well_state.wellRates()[well_number * np + pu.phase_pos[ Oil ] ];
        const double water_rate = well_state.wellRates()[well_number * np + pu.phase_pos[ Water ] ];
        const double liquid_rate = oil_rate + water_rate;
        double water_cut;
        if (std::abs(liquid_rate) != 0.) {
            water_cut = water_rate / liquid_rate;
        } else {
            water_cut = 0.0;
        }

        const double max_water_cut_limit = econ_production_limits.maxWaterCut();
        if (water_cut > max_water_cut_limit) {
            water_cut_limit_violated = true;
        }

        if (water_cut_limit_violated) {
            // need to handle the worst_offending_connection
            const int perf_start = first_perf_;
            const int perf_number = number_of_perforations_;

            std::vector<double> water_cut_perf(perf_number);
            for (int perf = 0; perf < perf_number; ++perf) {
                const int i_perf = perf_start + perf;
                const double oil_perf_rate = well_state.perfPhaseRates()[i_perf * np + pu.phase_pos[ Oil ] ];
                const double water_perf_rate = well_state.perfPhaseRates()[i_perf * np + pu.phase_pos[ Water ] ];
                const double liquid_perf_rate = oil_perf_rate + water_perf_rate;
                if (std::abs(liquid_perf_rate) != 0.) {
                    water_cut_perf[perf] = water_perf_rate / liquid_perf_rate;
                } else {
                    water_cut_perf[perf] = 0.;
                }
            }

            last_connection = (perf_number == 1);
            if (last_connection) {
                worst_offending_connection = 0;
                violation_extent = water_cut_perf[0] / max_water_cut_limit;
                return std::make_tuple(water_cut_limit_violated, last_connection, worst_offending_connection, violation_extent);
            }

            double max_water_cut_perf = 0.;
            for (int perf = 0; perf < perf_number; ++perf) {
                if (water_cut_perf[perf] > max_water_cut_perf) {
                    worst_offending_connection = perf;
                    max_water_cut_perf = water_cut_perf[perf];
                }
            }

            assert(max_water_cut_perf != 0.);
            assert((worst_offending_connection >= 0) && (worst_offending_connection < perf_number));

            violation_extent = max_water_cut_perf / max_water_cut_limit;
        }

        return std::make_tuple(water_cut_limit_violated, last_connection, worst_offending_connection, violation_extent);
    }





    template<typename TypeTag>
    typename WellInterface<TypeTag>::RatioCheckTuple
    WellInterface<TypeTag>::
    checkRatioEconLimits(const WellEconProductionLimits& econ_production_limits,
                         const WellState& well_state) const
    {
        // TODO: not sure how to define the worst-offending connection when more than one
        //       ratio related limit is violated.
        //       The defintion used here is that we define the violation extent based on the
        //       ratio between the value and the corresponding limit.
        //       For each violated limit, we decide the worst-offending connection separately.
        //       Among the worst-offending connections, we use the one has the biggest violation
        //       extent.

        bool any_limit_violated = false;
        bool last_connection = false;
        int worst_offending_connection = INVALIDCONNECTION;
        double violation_extent = -1.0;

        if (econ_production_limits.onMaxWaterCut()) {
            const RatioCheckTuple water_cut_return = checkMaxWaterCutLimit(econ_production_limits, well_state);
            bool water_cut_violated = std::get<0>(water_cut_return);
            if (water_cut_violated) {
                any_limit_violated = true;
                const double violation_extent_water_cut = std::get<3>(water_cut_return);
                if (violation_extent_water_cut > violation_extent) {
                    violation_extent = violation_extent_water_cut;
                    worst_offending_connection = std::get<2>(water_cut_return);
                    last_connection = std::get<1>(water_cut_return);
                }
            }
        }

        if (econ_production_limits.onMaxGasOilRatio()) {
            OpmLog::warning("NOT_SUPPORTING_MAX_GOR", "the support for max Gas-Oil ratio is not implemented yet!");
        }

        if (econ_production_limits.onMaxWaterGasRatio()) {
            OpmLog::warning("NOT_SUPPORTING_MAX_WGR", "the support for max Water-Gas ratio is not implemented yet!");
        }

        if (econ_production_limits.onMaxGasLiquidRatio()) {
            OpmLog::warning("NOT_SUPPORTING_MAX_GLR", "the support for max Gas-Liquid ratio is not implemented yet!");
        }

        if (any_limit_violated) {
            assert(worst_offending_connection >=0);
            assert(violation_extent > 1.);
        }

        return std::make_tuple(any_limit_violated, last_connection, worst_offending_connection, violation_extent);
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    updateListEconLimited(const WellState& well_state,
                          DynamicListEconLimited& list_econ_limited) const
    {
        // economic limits only apply for production wells.
        if (wellType() != PRODUCER) {
            return;
        }

        // flag to check if the mim oil/gas rate limit is violated
        bool rate_limit_violated = false;
        const WellEconProductionLimits& econ_production_limits = well_ecl_->getEconProductionLimits(current_step_);

        // if no limit is effective here, then continue to the next well
        if ( !econ_production_limits.onAnyEffectiveLimit() ) {
            return;
        }

        const std::string well_name = name();

        // for the moment, we only handle rate limits, not handling potential limits
        // the potential limits should not be difficult to add
        const WellEcon::QuantityLimitEnum& quantity_limit = econ_production_limits.quantityLimit();
        if (quantity_limit == WellEcon::POTN) {
            const std::string msg = std::string("POTN limit for well ") + well_name + std::string(" is not supported for the moment. \n")
                                  + std::string("All the limits will be evaluated based on RATE. ");
            OpmLog::warning("NOT_SUPPORTING_POTN", msg);
        }

        if (econ_production_limits.onAnyRateLimit()) {
            rate_limit_violated = checkRateEconLimits(econ_production_limits, well_state);
        }

        if (rate_limit_violated) {
            if (econ_production_limits.endRun()) {
                const std::string warning_message = std::string("ending run after well closed due to economic limits is not supported yet \n")
                                                  + std::string("the program will keep running after ") + well_name + std::string(" is closed");
                OpmLog::warning("NOT_SUPPORTING_ENDRUN", warning_message);
            }

            if (econ_production_limits.validFollowonWell()) {
                OpmLog::warning("NOT_SUPPORTING_FOLLOWONWELL", "opening following on well after well closed is not supported yet");
            }

            if (well_ecl_->getAutomaticShutIn()) {
                list_econ_limited.addShutWell(well_name);
                const std::string msg = std::string("well ") + well_name + std::string(" will be shut in due to rate economic limit");
                    OpmLog::info(msg);
            } else {
                list_econ_limited.addStoppedWell(well_name);
                const std::string msg = std::string("well ") + well_name + std::string(" will be stopped due to rate economic limit");
                OpmLog::info(msg);
            }
            // the well is closed, not need to check other limits
            return;
        }

        // checking for ratio related limits, mostly all kinds of ratio.
        bool ratio_limits_violated = false;
        RatioCheckTuple ratio_check_return;

        if (econ_production_limits.onAnyRatioLimit()) {
            ratio_check_return = checkRatioEconLimits(econ_production_limits, well_state);
            ratio_limits_violated = std::get<0>(ratio_check_return);
        }

        if (ratio_limits_violated) {
            const WellEcon::WorkoverEnum workover = econ_production_limits.workover();
            switch (workover) {
                case WellEcon::CON:
                {
                    const bool last_connection = std::get<1>(ratio_check_return);
                    const int worst_offending_connection = std::get<2>(ratio_check_return);

                    assert((worst_offending_connection >= 0) && (worst_offending_connection < number_of_perforations_));

                    const int cell_worst_offending_connection = well_cells_[worst_offending_connection];
                    list_econ_limited.addClosedConnectionsForWell(well_name, cell_worst_offending_connection);
                    const std::string msg = std::string("Connection ") + std::to_string(worst_offending_connection) + std::string(" for well ")
                                            + well_name + std::string(" will be closed due to economic limit");
                    OpmLog::info(msg);

                    if (last_connection) {
                        // TODO: there is more things to check here
                        list_econ_limited.addShutWell(well_name);
                        const std::string msg2 = well_name + std::string(" will be shut due to the last connection closed");
                        OpmLog::info(msg2);
                    }
                    break;
                }
                case WellEcon::WELL:
                {
                    if (well_ecl_->getAutomaticShutIn()) {
                        list_econ_limited.addShutWell(well_name);
                        const std::string msg = well_name + std::string(" will be shut due to ratio economic limit");
                        OpmLog::info(msg);
                    } else {
                        list_econ_limited.addStoppedWell(well_name);
                        const std::string msg = well_name + std::string(" will be stopped due to ratio economic limit");
                        OpmLog::info(msg);
                    }
                    break;
                }
                case WellEcon::NONE:
                    break;
                default:
                {
                    OpmLog::warning("NOT_SUPPORTED_WORKOVER_TYPE", "not supporting workover type " + WellEcon::WorkoverEnumToString(workover) );
                }
            }
        }
    }





    template<typename TypeTag>
    void
    WellInterface<TypeTag>::
    computeRepRadiusPerfLength(const Grid& grid,
                               const std::map<int, int>& cartesian_to_compressed)
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
        const auto& completionSet = well_ecl_->getConnections(current_step_);
        for (size_t c=0; c<completionSet.size(); c++) {
            const auto& completion = completionSet.get(c);
            if (completion.state == WellCompletion::OPEN) {
                const int i = completion.getI();
                const int j = completion.getJ();
                const int k = completion.getK();

                const int* cpgdim = cart_dims;
                const int cart_grid_indx = i + cpgdim[0]*(j + cpgdim[1]*k);
                const std::map<int, int>::const_iterator cgit = cartesian_to_compressed.find(cart_grid_indx);
                if (cgit == cartesian_to_compressed.end()) {
                    OPM_THROW(std::runtime_error, "Cell with i,j,k indices " << i << ' ' << j << ' '
                              << k << " not found in grid (well = " << name() << ')');
                }
                const int cell = cgit->second;

                {
                    double radius = 0.5*completion.getDiameter();
                    if (radius <= 0.0) {
                        radius = 0.5*unit::feet;
                        OPM_MESSAGE("**** Warning: Well bore internal radius set to " << radius);
                    }

                    const std::array<double, 3> cubical =
                    WellsManagerDetail::getCubeDim<3>(cell_to_faces, begin_face_centroids, cell);

                    double re; // area equivalent radius of the grid block
                    double perf_length; // the length of the well perforation

                    switch (completion.dir) {
                        case Opm::WellCompletion::DirectionEnum::X:
                            re = std::sqrt(cubical[1] * cubical[2] / M_PI);
                            perf_length = cubical[0];
                            break;
                        case Opm::WellCompletion::DirectionEnum::Y:
                            re = std::sqrt(cubical[0] * cubical[2] / M_PI);
                            perf_length = cubical[1];
                            break;
                        case Opm::WellCompletion::DirectionEnum::Z:
                            re = std::sqrt(cubical[0] * cubical[1] / M_PI);
                            perf_length = cubical[2];
                            break;
                        default:
                            OPM_THROW(std::runtime_error, " Dirtecion of well is not supported ");
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
        if (current_control_ == "RESV") {
            if (has_solvent && phaseIdx == contiSolventEqIdx ) {
                typedef Ewoms::BlackOilSolventModule<TypeTag> SolventModule;
                double coeff = 0;
                rateConverter_.template calcCoeffSolvent<SolventModule>(/*fipreg*/ 0, pvtRegionIdx_, coeff);
                return coeff;
            } else {
                std::vector<double> coeff (number_of_phases_);
                rateConverter_.template calcCoeff(/*fipreg*/ 0, pvtRegionIdx_, coeff);
                return coeff[phaseIdx];
            }
        }
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
    int
    WellInterface<TypeTag>::groupComponentIdx() const
    {

        return groupCompIdx_;

    }

    template<typename TypeTag>
    double
    WellInterface<TypeTag>::guideRate() const
    {
        return guideRate_;
    }

    template<typename TypeTag>
    double
    WellInterface<TypeTag>::groupTarget() const
    {
        return groupTarget_;
    }



}
