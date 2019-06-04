/*
  Copyright 2012 SINTEF ICT, Applied Mathematics.
  Copyright 2016 IRIS AS

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

#include "config.h"


#include <opm/core/wells/WellsManager.hpp>
#include <opm/grid/UnstructuredGrid.h>
#include <opm/core/wells.h>
#include <opm/core/well_controls.h>
#include <opm/common/ErrorMacros.hpp>
#include <opm/core/wells/WellCollection.hpp>
#include <opm/core/wells/WellsGroup.hpp>
#include <opm/core/props/phaseUsageFromDeck.hpp>

#include <opm/parser/eclipse/EclipseState/Schedule/ScheduleEnums.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <iostream>

namespace
{
    static double invalid_alq = -1e100;
    static double invalid_vfp = -2147483647;
} //Namespace

// Helper structs and functions for the implementation.
namespace WellsManagerDetail
{



    namespace ProductionControl
    {
        namespace Details {
            std::map<std::string, Mode>
            init_mode_map() {
                std::map<std::string, Mode> m;

                m.insert(std::make_pair("ORAT", ORAT));
                m.insert(std::make_pair("WRAT", WRAT));
                m.insert(std::make_pair("GRAT", GRAT));
                m.insert(std::make_pair("LRAT", LRAT));
                m.insert(std::make_pair("CRAT", CRAT));
                m.insert(std::make_pair("RESV", RESV));
                m.insert(std::make_pair("BHP" , BHP ));
                m.insert(std::make_pair("THP" , THP ));
                m.insert(std::make_pair("GRUP", GRUP));

                return m;
            }
        } // namespace Details

        Mode mode(const std::string& control)
        {
            static std::map<std::string, Mode>
                mode_map = Details::init_mode_map();

            std::map<std::string, Mode>::iterator
                p = mode_map.find(control);

            if (p != mode_map.end()) {
                return p->second;
            }
            else {
                OPM_THROW(std::runtime_error, "Unknown well control mode = "
                      << control << " in input file");
            }
        }


        Mode mode(Opm::WellProducer::ControlModeEnum controlMode)
        {
            switch( controlMode ) {
            case Opm::WellProducer::ORAT:
                return ORAT;
            case Opm::WellProducer::WRAT:
                return WRAT;
            case Opm::WellProducer::GRAT:
                return GRAT;
            case Opm::WellProducer::LRAT:
                return LRAT;
            case Opm::WellProducer::CRAT:
                return CRAT;
            case Opm::WellProducer::RESV:
                return RESV;
            case Opm::WellProducer::BHP:
                return BHP;
            case Opm::WellProducer::THP:
                return THP;
            case Opm::WellProducer::GRUP:
                return GRUP;
            default:
                throw std::invalid_argument("unhandled enum value");
            }
        }
    } // namespace ProductionControl


    namespace InjectionControl
    {

        namespace Details {
            std::map<std::string, Mode>
            init_mode_map() {
                std::map<std::string, Mode> m;

                m.insert(std::make_pair("RATE", RATE));
                m.insert(std::make_pair("RESV", RESV));
                m.insert(std::make_pair("BHP" , BHP ));
                m.insert(std::make_pair("THP" , THP ));
                m.insert(std::make_pair("GRUP", GRUP));

                return m;
            }
        } // namespace Details

        Mode mode(const std::string& control)
        {
            static std::map<std::string, Mode>
                mode_map = Details::init_mode_map();

            std::map<std::string, Mode>::iterator
                p = mode_map.find(control);

            if (p != mode_map.end()) {
                return p->second;
            }
            else {
                OPM_THROW(std::runtime_error, "Unknown well control mode = "
                      << control << " in input file");
            }
        }

        Mode mode(Opm::WellInjector::ControlModeEnum controlMode)
        {
            switch ( controlMode  ) {
            case Opm::WellInjector::GRUP:
                return GRUP;
            case Opm::WellInjector::RESV:
                return RESV;
            case Opm::WellInjector::RATE:
                return RATE;
            case Opm::WellInjector::THP:
                return THP;
            case Opm::WellInjector::BHP:
                return BHP;
            default:
                throw std::invalid_argument("unhandled enum value");
            }
        }

    } // namespace InjectionControl

} // anonymous namespace





namespace Opm
{


    /// Default constructor.
    WellsManager::WellsManager()
        : w_(create_wells(0,0,0)), is_parallel_run_(false)
    {
    }

    /// Construct from existing wells object.
    WellsManager::WellsManager(struct Wells* W)
        : w_(clone_wells(W)), is_parallel_run_(false)
    {
    }

    /// Construct wells from deck.
    WellsManager::WellsManager(const Opm::EclipseState& eclipseState,
                               const Opm::Schedule& schedule,
                               const SummaryState& summaryState,
                               const size_t timeStep,
                               const UnstructuredGrid& grid)
        : w_(create_wells(0,0,0)), is_parallel_run_(false)
    {
        init(eclipseState, schedule, timeStep, UgGridHelpers::numCells(grid),
             UgGridHelpers::globalCell(grid), UgGridHelpers::cartDims(grid),
             UgGridHelpers::dimensions(grid),
             UgGridHelpers::cell2Faces(grid), UgGridHelpers::beginFaceCentroids(grid),
             std::unordered_set<std::string>());

    }

    /// Destructor.
    WellsManager::~WellsManager()
    {
        destroy_wells(w_);
    }


    /// Does the "deck" define any wells?
    bool WellsManager::empty() const
    {
        return (w_ == 0) || (w_->number_of_wells == 0);
    }



    /// Access the managed Wells.
    /// The method is named similarly to c_str() in std::string,
    /// to make it clear that we are returning a C-compatible struct.
    const Wells* WellsManager::c_wells() const
    {
        return w_;
    }

    const WellCollection& WellsManager::wellCollection() const
    {
        return well_collection_;
    }

    WellCollection& WellsManager::wellCollection() {
        return well_collection_;

    }


    bool WellsManager::conditionsMet(const std::vector<double>& well_bhp,
                                     const std::vector<double>& well_reservoirrates_phase,
                                     const std::vector<double>& well_surfacerates_phase)
    {
        return well_collection_.conditionsMet(well_bhp,
                                              well_reservoirrates_phase,
                                              well_surfacerates_phase);
    }

    /// Applies explicit reinjection controls. This must be called at each timestep to be correct.
    /// \param[in]    well_reservoirrates_phase
    ///                         A vector containing reservoir rates by phase for each well.
    ///                         Is assumed to be ordered the same way as the related Wells-struct,
    ///                         with all phase rates of a single well adjacent in the array.
    /// \param[in]    well_surfacerates_phase
    ///                         A vector containing surface rates by phase for each well.
    ///                         Is assumed to be ordered the same way as the related Wells-struct,
    ///                         with all phase rates of a single well adjacent in the array.

    void WellsManager::applyExplicitReinjectionControls(const std::vector<double>& well_reservoirrates_phase,
                                                        const std::vector<double>& well_surfacerates_phase)
    {
        well_collection_.applyExplicitReinjectionControls(well_reservoirrates_phase, well_surfacerates_phase);
    }

    void WellsManager::setupCompressedToCartesian(const int* global_cell, int number_of_cells,
                                                  std::map<int,int>& cartesian_to_compressed ) {
        // global_cell is a map from compressed cells to Cartesian grid cells.
        // We must make the inverse lookup.

        if (global_cell) {
            for (int i = 0; i < number_of_cells; ++i) {
                cartesian_to_compressed.insert(std::make_pair(global_cell[i], i));
            }
        }
        else {
            for (int i = 0; i < number_of_cells; ++i) {
                cartesian_to_compressed.insert(std::make_pair(i, i));
            }
        }

    }



    void WellsManager::setupWellControls(const std::vector<Well2>& wells, size_t timeStep,
                                         std::vector<std::string>& well_names, const PhaseUsage& phaseUsage,
                                         const std::vector<int>& wells_on_proc) {
        int well_index = 0;
        auto well_on_proc = wells_on_proc.begin();
        SummaryState summaryState;

        for (auto wellIter = wells.begin(); wellIter != wells.end(); ++wellIter, ++well_on_proc) {
            if( ! *well_on_proc )
            {
                // Wells not stored on the process are not in the list
                continue;
            }

            const auto& well = (*wellIter);

            if (well.getStatus() == WellCommon::SHUT) {
                //SHUT wells are not added to the well list
                continue;
            }

            if (well.getStatus() == WellCommon::STOP) {
                // Stopped wells are kept in the well list but marked as stopped.
                well_controls_stop_well(w_->ctrls[well_index]);
            }


            if (well.isInjector()) {
                const auto controls = well.injectionControls(summaryState);
                int ok = 1;
                int control_pos[5] = { -1, -1, -1, -1, -1 };

                clear_well_controls(well_index, w_);
                if (controls.hasControl(WellInjector::RATE)) {
                    control_pos[WellsManagerDetail::InjectionControl::RATE] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 0.0, 0.0, 0.0 };
                    WellInjector::TypeEnum injectorType = controls.injector_type;

                    if (injectorType == WellInjector::TypeEnum::WATER) {
                        distr[phaseUsage.phase_pos[BlackoilPhases::Aqua]] = 1.0;
                    } else if (injectorType == WellInjector::TypeEnum::OIL) {
                        distr[phaseUsage.phase_pos[BlackoilPhases::Liquid]] = 1.0;
                    } else if (injectorType == WellInjector::TypeEnum::GAS) {
                        distr[phaseUsage.phase_pos[BlackoilPhases::Vapour]] = 1.0;
                    }

                    ok = append_well_controls(SURFACE_RATE,
                                              controls.surface_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellInjector::RESV)) {
                    control_pos[WellsManagerDetail::InjectionControl::RESV] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 0.0, 0.0, 0.0 };
                    WellInjector::TypeEnum injectorType = controls.injector_type;

                    if (injectorType == WellInjector::TypeEnum::WATER) {
                        distr[phaseUsage.phase_pos[BlackoilPhases::Aqua]] = 1.0;
                    } else if (injectorType == WellInjector::TypeEnum::OIL) {
                        distr[phaseUsage.phase_pos[BlackoilPhases::Liquid]] = 1.0;
                    } else if (injectorType == WellInjector::TypeEnum::GAS) {
                        distr[phaseUsage.phase_pos[BlackoilPhases::Vapour]] = 1.0;
                    }

                    ok = append_well_controls(RESERVOIR_RATE,
                                              controls.reservoir_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellInjector::BHP)) {
                    control_pos[WellsManagerDetail::InjectionControl::BHP] = well_controls_get_num(w_->ctrls[well_index]);
                    ok = append_well_controls(BHP,
                                              controls.bhp_limit,
                                              invalid_alq,
                                              invalid_vfp,
                                              NULL,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellInjector::THP)) {
                    control_pos[WellsManagerDetail::InjectionControl::THP] = well_controls_get_num(w_->ctrls[well_index]);
                    const double thp_limit  = controls.thp_limit;
                    const int    vfp_number = controls.vfp_table_number;
                    ok = append_well_controls(THP,
                                              thp_limit,
                                              invalid_alq,
                                              vfp_number,
                                              NULL,
                                              well_index,
                                              w_);
                }

                if (!ok) {
                    OPM_THROW(std::runtime_error, "Failure occured appending controls for well " << well_names[well_index]);
                }

                if (controls.cmode != WellInjector::CMODE_UNDEFINED) {
                    WellsManagerDetail::InjectionControl::Mode mode = WellsManagerDetail::InjectionControl::mode(controls.cmode);
                    int cpos = control_pos[mode];
                    if (cpos == -1 && mode != WellsManagerDetail::InjectionControl::GRUP) {
                        OPM_THROW(std::runtime_error, "Control not specified in well " << well_names[well_index]);
                    }

                    set_current_control(well_index, cpos, w_);
                }

                // Set well component fraction.
                double cf[3] = { 0.0, 0.0, 0.0 };
                {
                    WellInjector::TypeEnum injectorType = controls.injector_type;

                    if (injectorType == WellInjector::WATER) {
                        if (!phaseUsage.phase_used[BlackoilPhases::Aqua]) {
                            OPM_THROW(std::runtime_error, "Water phase not used, yet found water-injecting well.");
                        }
                        cf[phaseUsage.phase_pos[BlackoilPhases::Aqua]] = 1.0;
                    } else if (injectorType == WellInjector::OIL) {
                        if (!phaseUsage.phase_used[BlackoilPhases::Liquid]) {
                            OPM_THROW(std::runtime_error, "Oil phase not used, yet found oil-injecting well.");
                        }
                        cf[phaseUsage.phase_pos[BlackoilPhases::Liquid]] = 1.0;
                    } else if (injectorType == WellInjector::GAS) {
                        if (!phaseUsage.phase_used[BlackoilPhases::Vapour]) {
                            OPM_THROW(std::runtime_error, "Gas phase not used, yet found gas-injecting well.");
                        }
                        cf[phaseUsage.phase_pos[BlackoilPhases::Vapour]] = 1.0;
                    }
                    std::copy(cf, cf + phaseUsage.num_phases, w_->comp_frac + well_index*phaseUsage.num_phases);
                }
            }

            if (well.isProducer( )) {
                // Add all controls that are present in well.
                // First we must clear existing controls, in case the
                // current WCONPROD line is modifying earlier controls.
                const auto controls = well.productionControls(summaryState);
                int control_pos[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
                int ok = 1;

                clear_well_controls(well_index, w_);
                if (ok && controls.hasControl(WellProducer::ORAT)) {
                    if (!phaseUsage.phase_used[BlackoilPhases::Liquid]) {
                        OPM_THROW(std::runtime_error, "Oil phase not active and ORAT control specified.");
                    }

                    control_pos[WellsManagerDetail::ProductionControl::ORAT] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 0.0, 0.0, 0.0 };
                    distr[phaseUsage.phase_pos[BlackoilPhases::Liquid]] = 1.0;
                    ok = append_well_controls(SURFACE_RATE,
                                              -controls.oil_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellProducer::WRAT)) {
                    if (!phaseUsage.phase_used[BlackoilPhases::Aqua]) {
                        OPM_THROW(std::runtime_error, "Water phase not active and WRAT control specified.");
                    }
                    control_pos[WellsManagerDetail::ProductionControl::WRAT] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 0.0, 0.0, 0.0 };
                    distr[phaseUsage.phase_pos[BlackoilPhases::Aqua]] = 1.0;
                    ok = append_well_controls(SURFACE_RATE,
                                              -controls.water_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellProducer::GRAT)) {
                    if (!phaseUsage.phase_used[BlackoilPhases::Vapour]) {
                        OPM_THROW(std::runtime_error, "Gas phase not active and GRAT control specified.");
                    }
                    control_pos[WellsManagerDetail::ProductionControl::GRAT] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 0.0, 0.0, 0.0 };
                    distr[phaseUsage.phase_pos[BlackoilPhases::Vapour]] = 1.0;
                    ok = append_well_controls(SURFACE_RATE,
                                              -controls.gas_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellProducer::LRAT)) {
                    if (!phaseUsage.phase_used[BlackoilPhases::Aqua]) {
                        OPM_THROW(std::runtime_error, "Water phase not active and LRAT control specified.");
                    }
                    if (!phaseUsage.phase_used[BlackoilPhases::Liquid]) {
                        OPM_THROW(std::runtime_error, "Oil phase not active and LRAT control specified.");
                    }
                    control_pos[WellsManagerDetail::ProductionControl::LRAT] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 0.0, 0.0, 0.0 };
                    distr[phaseUsage.phase_pos[BlackoilPhases::Aqua]] = 1.0;
                    distr[phaseUsage.phase_pos[BlackoilPhases::Liquid]] = 1.0;
                    ok = append_well_controls(SURFACE_RATE,
                                              -controls.liquid_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellProducer::RESV)) {
                    control_pos[WellsManagerDetail::ProductionControl::RESV] = well_controls_get_num(w_->ctrls[well_index]);
                    double distr[3] = { 1.0, 1.0, 1.0 };
                    ok = append_well_controls(RESERVOIR_RATE,
                                              -controls.resv_rate,
                                              invalid_alq,
                                              invalid_vfp,
                                              distr,
                                              well_index,
                                              w_);
                }

                if (ok && controls.hasControl(WellProducer::THP)) {
                    const double thp_limit  = controls.thp_limit;
                    const double alq_value  = controls.alq_value;
                    const int    vfp_number = controls.vfp_table_number;
                    control_pos[WellsManagerDetail::ProductionControl::THP] = well_controls_get_num(w_->ctrls[well_index]);
                    ok = append_well_controls(THP,
                                              thp_limit,
                                              alq_value,
                                              vfp_number,
                                              NULL,
                                              well_index,
                                              w_);
                }

                if (ok) {
                    const double bhp_limit = controls.bhp_limit;
                    control_pos[WellsManagerDetail::ProductionControl::BHP] = well_controls_get_num(w_->ctrls[well_index]);
                    ok = append_well_controls(BHP,
                                              bhp_limit,
                                              invalid_alq,
                                              invalid_vfp,
                                              NULL,
                                              well_index,
                                              w_);
                }

                if (!ok) {
                    OPM_THROW(std::runtime_error, "Failure occured appending controls for well " << well_names[well_index]);
                }

                if (controls.cmode != WellProducer::CMODE_UNDEFINED) {
                    WellsManagerDetail::ProductionControl::Mode mode = WellsManagerDetail::ProductionControl::mode(controls.cmode);
                    int cpos = control_pos[mode];
                    if (cpos == -1 && mode != WellsManagerDetail::ProductionControl::GRUP) {
                        OPM_THROW(std::runtime_error, "Control mode type " << mode << " not present in well " << well_names[well_index]);
                    }
                    else {
                        set_current_control(well_index, cpos, w_);
                    }
                }

                // Set well component fraction to match preferred phase for the well.
                double cf[3] = { 0.0, 0.0, 0.0 };
                {
                    switch (well.getPreferredPhase()) {
                    case Phase::WATER:
                        if (phaseUsage.phase_used[BlackoilPhases::Aqua]) {
                            cf[phaseUsage.phase_pos[BlackoilPhases::Aqua]] = 1.0;
                        }
                        break;
                    case Phase::OIL:
                        if (phaseUsage.phase_used[BlackoilPhases::Liquid]) {
                            cf[phaseUsage.phase_pos[BlackoilPhases::Liquid]] = 1.0;
                        }
                        break;
                    case Phase::GAS:
                        if (phaseUsage.phase_used[BlackoilPhases::Vapour]) {
                            cf[phaseUsage.phase_pos[BlackoilPhases::Vapour]] = 1.0;
                        }
                        break;
                    default:
                        OPM_THROW(std::logic_error, "Unknown preferred phase: " << well.getPreferredPhase());
                    }
                    std::copy(cf, cf + phaseUsage.num_phases, w_->comp_frac + well_index*phaseUsage.num_phases);
                }
            }
            well_index++;
        }

    }

    // only handle the guide rates from the keyword WGRUPCON
    void WellsManager::setupGuideRates(const std::vector<Well2>& wells, const size_t timeStep, std::vector<WellData>& well_data, std::map<std::string, int>& well_names_to_index)
    {
        for (auto wellIter = wells.begin(); wellIter != wells.end(); ++wellIter ) {
            const auto& well = *wellIter;

            if (well.getStatus() == WellCommon::SHUT) {
                //SHUT wells does not need guide rates
                continue;
            }

            const int wix = well_names_to_index[well.name()];
            WellNode& wellnode = *well_collection_.getLeafNodes()[wix];

            // TODO: looks like only handling OIL phase guide rate for producers
            if (well.getGuideRatePhase() != GuideRate::UNDEFINED && well.getGuideRate() >= 0.) {
                if (well_data[wix].type == PRODUCER) {
                    wellnode.prodSpec().guide_rate_ = well.getGuideRate();
                    if (well.getGuideRatePhase() == GuideRate::OIL) {
                        wellnode.prodSpec().guide_rate_type_ = ProductionSpecification::OIL;
                    } else {
                        OPM_THROW(std::runtime_error, "Guide rate type " << GuideRate::GuideRatePhaseEnum2String(well.getGuideRatePhase()) << " specified for producer "
                                  << well.name() << " in WGRUPCON, cannot handle.");
                    }
                } else if (well_data[wix].type == INJECTOR) {
                    wellnode.injSpec().guide_rate_ = well.getGuideRate();
                    if (well.getGuideRatePhase() == GuideRate::RAT) {
                        wellnode.injSpec().guide_rate_type_ = InjectionSpecification::RAT;
                    } else {
                        OPM_THROW(std::runtime_error, "Guide rate type " << GuideRate::GuideRatePhaseEnum2String(well.getGuideRatePhase()) << " specified for injector "
                                  << well.name() << " in WGRUPCON, cannot handle.");
                    }
                } else {
                    OPM_THROW(std::runtime_error, "Unknown well type " << well_data[wix].type << " for well " << well.name());
                }
            } else {
                wellnode.setIsGuideRateWellPotential(true);
            }
        }
    }

} // namespace Opm
