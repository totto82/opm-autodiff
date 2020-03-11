/*
  Copyright 2019 Norce.

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


#ifndef OPM_WELLGROUPHELPERS_HEADER_INCLUDED
#define OPM_WELLGROUPHELPERS_HEADER_INCLUDED

#include <opm/simulators/utils/DeferredLogger.hpp>
#include <opm/simulators/utils/DeferredLoggingErrorHelpers.hpp>

#include <vector>
#include <opm/parser/eclipse/EclipseState/Schedule/ScheduleTypes.hpp>

namespace Opm {


    namespace wellGroupHelpers
    {

    inline void setCmodeGroup(const Group& group, const Schedule& schedule, const SummaryState& summaryState, const int reportStepIdx, WellStateFullyImplicitBlackoil& wellState) {

        for (const std::string& groupName : group.groups()) {
            setCmodeGroup( schedule.getGroup(groupName, reportStepIdx), schedule, summaryState, reportStepIdx, wellState);
        }

        // use NONE as default control
        const Phase all[] = {Phase::WATER, Phase::OIL, Phase::GAS};
        for (Phase phase : all) {
            if (!wellState.hasInjectionGroupControl(phase, group.name())) {
                wellState.setCurrentInjectionGroupControl(phase, group.name(), Group::InjectionCMode::NONE);
            }
        }
        if (!wellState.hasProductionGroupControl(group.name())) {
            wellState.setCurrentProductionGroupControl(group.name(), Group::ProductionCMode::NONE);
        }

        if (group.isInjectionGroup() && schedule.hasWellGroupEvent(group.name(),  ScheduleEvents::GROUP_INJECTION_UPDATE, reportStepIdx)) {

            for (Phase phase : all) {
                if (!group.hasInjectionControl(phase))
                    continue;

                const auto& controls = group.injectionControls(phase, summaryState);
                wellState.setCurrentInjectionGroupControl(phase, group.name(), controls.cmode);
            }
        }

        if (group.isProductionGroup() && schedule.hasWellGroupEvent(group.name(),  ScheduleEvents::GROUP_PRODUCTION_UPDATE, reportStepIdx)) {
            const auto controls = group.productionControls(summaryState);
            wellState.setCurrentProductionGroupControl(group.name(), controls.cmode);
        }

        if (schedule.gConSale(reportStepIdx).has(group.name())) {
            wellState.setCurrentInjectionGroupControl(Phase::GAS, group.name(), Group::InjectionCMode::SALE);
        }
    }


    inline void accumulateGroupEfficiencyFactor(const Group& group, const Schedule& schedule, const int reportStepIdx, double& factor) {
        factor *= group.getGroupEfficiencyFactor();
        if (group.parent() != "FIELD")
            accumulateGroupEfficiencyFactor(schedule.getGroup(group.parent(), reportStepIdx), schedule, reportStepIdx, factor);
    }

    inline double sumWellPhaseRates(const std::vector<double>& rates, const Group& group, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const int phasePos,
                                    const bool injector) {

        double rate = 0.0;
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            rate += groupTmp.getGroupEfficiencyFactor()*sumWellPhaseRates(rates, groupTmp, schedule, wellState, reportStepIdx, phasePos, injector);
        }
        const auto& end = wellState.wellMap().end();
        for (const std::string& wellName : group.wells()) {
            const auto& it = wellState.wellMap().find( wellName );
            if (it == end)  // the well is not found
                continue;

            int well_index = it->second[0];

            const auto& wellEcl = schedule.getWell(wellName, reportStepIdx);
            //only count producers or injectors
            if ( (wellEcl.isProducer() && injector) ||  (wellEcl.isInjector() && !injector))
                continue;

            if (wellEcl.getStatus() == Well::Status::SHUT)
                continue;

            double factor = wellEcl.getEfficiencyFactor();
            const auto wellrate_index = well_index * wellState.numPhases();
            if (injector)
                rate += factor * rates[ wellrate_index + phasePos];
            else
                rate -= factor * rates[ wellrate_index + phasePos];
        }
        return rate;
    }

    inline double sumWellRates(const Group& group, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const int phasePos, const bool injector) {
        return sumWellPhaseRates(wellState.wellRates(), group, schedule, wellState, reportStepIdx, phasePos, injector);
    }

    inline double sumWellResRates(const Group& group, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const int phasePos, const bool injector) {
        return sumWellPhaseRates(wellState.wellReservoirRates(), group, schedule, wellState, reportStepIdx, phasePos, injector);
    }

    inline double sumSolventRates(const Group& group, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const bool injector) {

        double rate = 0.0;
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            rate += groupTmp.getGroupEfficiencyFactor()*sumSolventRates(groupTmp, schedule, wellState, reportStepIdx, injector);
        }
        const auto& end = wellState.wellMap().end();
        for (const std::string& wellName : group.wells()) {
            const auto& it = wellState.wellMap().find( wellName );
            if (it == end)  // the well is not found
                continue;

            int well_index = it->second[0];

            const auto& wellEcl = schedule.getWell(wellName, reportStepIdx);
            //only count producers or injectors
            if ( (wellEcl.isProducer() && injector) ||  (wellEcl.isInjector() && !injector))
                continue;

            if (wellEcl.getStatus() == Well::Status::SHUT)
                continue;

            double factor = wellEcl.getEfficiencyFactor();
            if (injector)
                rate += factor * wellState.solventWellRate(well_index);
            else
                rate -= factor * wellState.solventWellRate(well_index);
        }
        return rate;
    }

    inline void updateGroupTargetReduction(const Group& group, const Schedule& schedule, const int reportStepIdx, const bool isInjector, const PhaseUsage& pu, const WellStateFullyImplicitBlackoil& wellStateNupcol, WellStateFullyImplicitBlackoil& wellState, std::vector<double>& groupTargetReduction)
    {
        const int np = wellState.numPhases();
        for (const std::string& subGroupName : group.groups()) {
            std::vector<double> subGroupTargetReduction(np, 0.0);
            const Group& subGroup = schedule.getGroup(subGroupName, reportStepIdx);
            updateGroupTargetReduction(subGroup, schedule, reportStepIdx, isInjector, pu, wellStateNupcol, wellState, subGroupTargetReduction);

            // accumulate group contribution from sub group
            if (isInjector) {
                const Phase all[] = {Phase::WATER, Phase::OIL, Phase::GAS};
                for (Phase phase : all) {
                    const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(phase, subGroupName);
                    int phasePos;
                    if (phase == Phase::GAS && pu.phase_used[BlackoilPhases::Vapour] )
                        phasePos = pu.phase_pos[BlackoilPhases::Vapour];
                    else if (phase == Phase::OIL && pu.phase_used[BlackoilPhases::Liquid])
                        phasePos = pu.phase_pos[BlackoilPhases::Liquid];
                    else if (phase == Phase::WATER && pu.phase_used[BlackoilPhases::Aqua] )
                        phasePos = pu.phase_pos[BlackoilPhases::Aqua];
                    else
                        continue;

                    if (currentGroupControl != Group::InjectionCMode::FLD &&
                        currentGroupControl != Group::InjectionCMode::NONE) {
                        // Subgroup is under individual control.
                        groupTargetReduction[phasePos] += sumWellRates(subGroup, schedule, wellStateNupcol, reportStepIdx, phasePos, isInjector);
                    } else {
                        groupTargetReduction[phasePos] += subGroupTargetReduction[phasePos];
                    }
                }
            } else {
                const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(subGroupName);
                if (currentGroupControl != Group::ProductionCMode::FLD &&
                    currentGroupControl != Group::ProductionCMode::NONE) {
                    // Subgroup is under individual control.
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += sumWellRates(subGroup, schedule, wellStateNupcol, reportStepIdx, phase, isInjector);
                    }
                } else {
                    // or accumulate directly from the wells if controled from its parents
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += subGroupTargetReduction[phase];
                    }
                }
            }
        }
        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (wellTmp.isProducer() && isInjector)
                continue;

            if (wellTmp.isInjector() && !isInjector)
                continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;

            const auto& end = wellState.wellMap().end();
            const auto& it = wellState.wellMap().find( wellName );
            if (it == end)  // the well is not found
                continue;

            int well_index = it->second[0];
            const auto wellrate_index = well_index * wellState.numPhases();
            const double efficiency = wellTmp.getEfficiencyFactor();
            // add contributino from wells not under group control
            if (isInjector) {
                if (wellState.currentInjectionControls()[well_index] != Well::InjectorCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += wellStateNupcol.wellRates()[wellrate_index + phase] * efficiency;
                    }
            } else {
                if (wellState.currentProductionControls()[well_index] !=  Well::ProducerCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] -= wellStateNupcol.wellRates()[wellrate_index + phase] * efficiency;
                    }
            }
        }
        const double groupEfficiency = group.getGroupEfficiencyFactor();
        for (double& elem : groupTargetReduction) {
            elem *= groupEfficiency;
        }
        if (isInjector)
            wellState.setCurrentInjectionGroupReductionRates(group.name(), groupTargetReduction);
        else
            wellState.setCurrentProductionGroupReductionRates(group.name(), groupTargetReduction);
    }

    template <class Comm>
    inline void updateGuideRateForGroups(const Group& group, const Schedule& schedule, const PhaseUsage& pu, const int reportStepIdx, const double& simTime, const bool isInjector, WellStateFullyImplicitBlackoil& wellState, const Comm& comm, GuideRate* guideRate, std::vector<double>& pot)
    {
        const int np = pu.num_phases;
        for (const std::string& groupName : group.groups()) {
            std::vector<double> thisPot(np, 0.0);
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateGuideRateForGroups(groupTmp, schedule, pu, reportStepIdx, simTime, isInjector, wellState, comm, guideRate, thisPot);

            // accumulate group contribution from sub group unconditionally
            if (isInjector) {
                const Phase all[] = {Phase::WATER, Phase::OIL, Phase::GAS};
                for (Phase phase : all) {
                    const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(phase, groupName);
                    int phasePos;
                    if (phase == Phase::GAS && pu.phase_used[BlackoilPhases::Vapour] )
                        phasePos = pu.phase_pos[BlackoilPhases::Vapour];
                    else if (phase == Phase::OIL && pu.phase_used[BlackoilPhases::Liquid])
                        phasePos = pu.phase_pos[BlackoilPhases::Liquid];
                    else if (phase == Phase::WATER && pu.phase_used[BlackoilPhases::Aqua] )
                        phasePos = pu.phase_pos[BlackoilPhases::Aqua];
                    else
                        continue;

                    pot[phasePos] += thisPot[phasePos];
                }
            } else {
                const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
                if (currentGroupControl != Group::ProductionCMode::FLD && currentGroupControl != Group::ProductionCMode::NONE) {
                    continue;
                }
                for (int phase = 0; phase < np; phase++) {
                    pot[phase] += thisPot[phase];
                }
            }

        }
        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (wellTmp.isProducer() && isInjector)
                continue;

            if (wellTmp.isInjector() && !isInjector)
                continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;
            const auto& end = wellState.wellMap().end();
            const auto& it = wellState.wellMap().find( wellName );
            if (it == end)  // the well is not found
                continue;

            int well_index = it->second[0];
            const auto wellrate_index = well_index * wellState.numPhases();
            // add contribution from wells unconditionally
            for (int phase = 0; phase < np; phase++) {
                pot[phase] += wellState.wellPotentials()[wellrate_index + phase];
            }
        }

        double oilPot = 0.0;
        if (pu.phase_used[BlackoilPhases::Liquid])
            oilPot = pot [ pu.phase_pos[BlackoilPhases::Liquid]];

        double gasPot = 0.0;
        if (pu.phase_used[BlackoilPhases::Vapour])
            gasPot = pot [ pu.phase_pos[BlackoilPhases::Vapour]];

        double waterPot = 0.0;
        if (pu.phase_used[BlackoilPhases::Aqua])
            waterPot = pot [pu.phase_pos[BlackoilPhases::Aqua]];

        const double gefac = group.getGroupEfficiencyFactor();

        oilPot = comm.sum(oilPot) * gefac;
        gasPot = comm.sum(gasPot) * gefac;
        waterPot = comm.sum(waterPot) * gefac;

        if (isInjector) {
            wellState.setCurrentGroupInjectionPotentials(group.name(), pot);
        } else {
            guideRate->compute(group.name(), reportStepIdx, simTime, oilPot, gasPot, waterPot);
        }
    }

    template <class Comm>
    inline void updateGuideRatesForWells(const Schedule& schedule, const PhaseUsage& pu, const int reportStepIdx, const double& simTime, const WellStateFullyImplicitBlackoil& wellState, const Comm& comm, GuideRate* guideRate) {

        const auto& end = wellState.wellMap().end();
        for (const auto& well : schedule.getWells(reportStepIdx)) {
            double oilpot = 0.0;
            double gaspot = 0.0;
            double waterpot = 0.0;

            const auto& it = wellState.wellMap().find( well.name());
            if (it != end) {  // the well is found

                int well_index = it->second[0];

                const auto wpot = wellState.wellPotentials().data() + well_index*wellState.numPhases();
                if (pu.phase_used[BlackoilPhases::Liquid] > 0)
                    oilpot = wpot[pu.phase_pos[BlackoilPhases::Liquid]];

                if (pu.phase_used[BlackoilPhases::Vapour] > 0)
                    gaspot = wpot[pu.phase_pos[BlackoilPhases::Vapour]];

                if (pu.phase_used[BlackoilPhases::Aqua] > 0)
                    waterpot = wpot[pu.phase_pos[BlackoilPhases::Aqua]];
            }
            const double wefac = well.getEfficiencyFactor();
            oilpot = comm.sum(oilpot) * wefac;
            gaspot = comm.sum(gaspot) * wefac;
            waterpot = comm.sum(waterpot) * wefac;
            guideRate->compute(well.name(), reportStepIdx, simTime, oilpot, gaspot, waterpot);
        }

    }


    inline void updateVREPForGroups(const Group& group, const Schedule& schedule, const int reportStepIdx, const WellStateFullyImplicitBlackoil& wellStateNupcol, WellStateFullyImplicitBlackoil& wellState) {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateVREPForGroups(groupTmp, schedule, reportStepIdx, wellStateNupcol, wellState);
        }
        const int np = wellState.numPhases();
        double resv = 0.0;
        for (int phase = 0; phase < np; ++phase) {
            resv += sumWellPhaseRates(wellStateNupcol.wellReservoirRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ false);
        }
        wellState.setCurrentInjectionVREPRates(group.name(), resv);
    }

    inline void updateReservoirRatesInjectionGroups(const Group& group, const Schedule& schedule, const int reportStepIdx, const WellStateFullyImplicitBlackoil& wellStateNupcol, WellStateFullyImplicitBlackoil& wellState) {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateReservoirRatesInjectionGroups(groupTmp, schedule, reportStepIdx, wellStateNupcol, wellState);
        }
        const int np = wellState.numPhases();
        std::vector<double> resv(np, 0.0);
        for (int phase = 0; phase < np; ++phase) {
            resv[phase] = sumWellPhaseRates(wellStateNupcol.wellReservoirRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ true);
        }
        wellState.setCurrentInjectionGroupReservoirRates(group.name(), resv);
    }

    inline void updateREINForGroups(const Group& group, const Schedule& schedule, const int reportStepIdx, const PhaseUsage& pu, const SummaryState& st, const WellStateFullyImplicitBlackoil& wellStateNupcol, WellStateFullyImplicitBlackoil& wellState) {
        const int np = wellState.numPhases();
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateREINForGroups(groupTmp, schedule, reportStepIdx, pu, st, wellStateNupcol, wellState);
        }

        std::vector<double> rein(np, 0.0);
        for (int phase = 0; phase < np; ++phase) {
            rein[phase] = sumWellPhaseRates(wellStateNupcol.wellRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ false);
        }

        // add import rate and substract consumption rate for group for gas
        if (schedule.gConSump(reportStepIdx).has(group.name())) {
            const auto& gconsump = schedule.gConSump(reportStepIdx).get(group.name(), st);
            if (pu.phase_used[BlackoilPhases::Vapour]) {
                rein[pu.phase_pos[BlackoilPhases::Vapour]] += gconsump.import_rate;
                rein[pu.phase_pos[BlackoilPhases::Vapour]] -= gconsump.consumption_rate;
            }
        }

        wellState.setCurrentInjectionREINRates(group.name(), rein);
    }



    inline double getGuideRate(const std::string& name,
                               const Schedule& schedule,
                               const WellStateFullyImplicitBlackoil& wellState,
                               const int reportStepIdx,
                               const GuideRate* guideRate,
                               const GuideRateModel::Target target)
    {
        if (schedule.hasWell(name, reportStepIdx) || guideRate->has(name)) {
            return guideRate->get(name, target);
        }

        double totalGuideRate = 0.0;
        const Group& group = schedule.getGroup(name, reportStepIdx);

        for (const std::string& groupName : group.groups()) {
            const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
            if (currentGroupControl == Group::ProductionCMode::FLD || currentGroupControl == Group::ProductionCMode::NONE) {
                // accumulate from sub wells/groups
                totalGuideRate += getGuideRate(groupName, schedule, wellState, reportStepIdx, guideRate, target);
            }
        }

        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (wellTmp.isInjector())
                 continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;

            // Only count wells under group control or the ru
            if (!wellState.isProductionGrup(wellName))
                continue;

            totalGuideRate += guideRate->get(wellName, target);
        }
        return totalGuideRate;
   }


    inline double getGuideRateInj(const std::string& name,
                                  const Schedule& schedule,
                                  const WellStateFullyImplicitBlackoil& wellState,
                                  const int reportStepIdx,
                                  const GuideRate* guideRate,
                                  const GuideRateModel::Target target,
                                  const Phase& injectionPhase)
    {
        if (schedule.hasWell(name, reportStepIdx)) {
            return guideRate->get(name, target);
        }

        double totalGuideRate = 0.0;
        const Group& group = schedule.getGroup(name, reportStepIdx);

        for (const std::string& groupName : group.groups()) {
            const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(injectionPhase, groupName);
            if (currentGroupControl == Group::InjectionCMode::FLD || currentGroupControl == Group::InjectionCMode::NONE) {
                // accumulate from sub wells/groups
                totalGuideRate += getGuideRateInj(groupName, schedule, wellState, reportStepIdx, guideRate, target, injectionPhase);
            }
        }

        for (const std::string& wellName : group.wells()) {
            const auto& wellTmp = schedule.getWell(wellName, reportStepIdx);

            if (!wellTmp.isInjector())
                 continue;

            if (wellTmp.getStatus() == Well::Status::SHUT)
                continue;

            // Only count wells under group control or the ru
            if (!wellState.isInjectionGrup(wellName))
                continue;

            totalGuideRate += guideRate->get(wellName, target);
        }
        return totalGuideRate;
   }


    class FractionCalculator
    {
    public:
        FractionCalculator(const Schedule& schedule,
                           const WellStateFullyImplicitBlackoil& well_state,
                           const int report_step,
                           const GuideRate* guide_rate,
                           const GuideRateModel::Target target)
            : schedule_(schedule)
            , well_state_(well_state)
            , report_step_(report_step)
            , guide_rate_(guide_rate)
            , target_(target)
        {
        }
        double fraction(const std::string& name,
                        const std::string& control_group_name,
                        const bool always_include_this)
        {
            double fraction = 1.0;
            std::string current = name;
            while (current != control_group_name) {
                fraction *= localFraction(current, always_include_this ? name : "");
                current = parent(current);
            }
            return fraction;
        }
    private:
        double localFraction(const std::string& name, const std::string& always_included_child)
        {
            const double my_guide_rate = guideRate(name, always_included_child);
            const Group& parent_group = schedule_.getGroup(parent(name), report_step_);
            const double total_guide_rate = guideRateSum(parent_group, always_included_child);
            return my_guide_rate / total_guide_rate;
        }
        std::string parent(const std::string& name)
        {
            if (schedule_.hasWell(name)) {
                return schedule_.getWell(name, report_step_).groupName();
            } else {
                return schedule_.getGroup(name, report_step_).parent();
            }
        }
        double guideRateSum(const Group& group, const std::string& always_included_child)
        {
            double total_guide_rate = 0.0;
            for (const std::string& child_group : group.groups()) {
                const auto ctrl = well_state_.currentProductionGroupControl(child_group);
                const bool included = (ctrl == Group::ProductionCMode::FLD)
                    || (ctrl == Group::ProductionCMode::NONE)
                    || (child_group == always_included_child);
                if (included) {
                    total_guide_rate += guideRate(child_group, always_included_child);
                }
            }
            for (const std::string& child_well : group.wells()) {
                const bool included = (well_state_.isProductionGrup(child_well))
                    || (child_well == always_included_child);
                if (included) {
                    total_guide_rate += guideRate(child_well, always_included_child);
                }
            }
            return total_guide_rate;
        }
        double guideRate(const std::string& name, const std::string& always_included_child)
        {
            if (schedule_.hasWell(name, report_step_)) {
                return guide_rate_->get(name, target_);
            } else {
                if (groupControlledWells(name, always_included_child) > 0) {
                    if (guide_rate_->has(name)) {
                        return guide_rate_->get(name, target_);
                    } else {
                        // We are a group, with default guide rate.
                        // Compute guide rate by accumulating our children's guide rates.
                        // (only children not under individual control though).
                        const Group& group = schedule_.getGroup(name, report_step_);
                        return guideRateSum(group, always_included_child);
                    }
                } else {
                    // No group-controlled subordinate wells.
                    return 0.0;
                }
            }
        }
        int groupControlledWells(const std::string& group_name, const std::string& always_included_child)
        {
            const Group& group = schedule_.getGroup(group_name, report_step_);
            int num_wells = 0;
            for (const std::string& child_group : group.groups()) {
                const auto ctrl = well_state_.currentProductionGroupControl(child_group);
                const bool included = (ctrl == Group::ProductionCMode::FLD)
                    || (ctrl == Group::ProductionCMode::NONE)
                    || (child_group == always_included_child);
                if (included) {
                    num_wells += groupControlledWells(child_group, always_included_child);
                }
            }
            for (const std::string& child_well : group.wells()) {
                const bool included = (well_state_.isProductionGrup(child_well))
                    || (child_well == always_included_child);
                if (included) {
                    ++num_wells;
                }
            }
            return num_wells;
        }
        const Schedule& schedule_;
        const WellStateFullyImplicitBlackoil& well_state_;
        int report_step_;
        const GuideRate* guide_rate_;
        GuideRateModel::Target target_;
    };


    inline double fractionFromGuideRates(const std::string& name,
                                         const std::string& controlGroupName,
                                         const Schedule& schedule,
                                         const WellStateFullyImplicitBlackoil& wellState,
                                         const int reportStepIdx,
                                         const GuideRate* guideRate,
                                         const GuideRateModel::Target target,
                                         const bool alwaysIncludeThis = false)
    {
        FractionCalculator calc(schedule, wellState, reportStepIdx, guideRate, target);
        return calc.fraction(name, controlGroupName, alwaysIncludeThis);
    }

    inline double fractionFromInjectionPotentials(const std::string& name,
                                                  const std::string& controlGroupName,
                                                  const Schedule& schedule,
                                                  const WellStateFullyImplicitBlackoil& wellState,
                                                  const int reportStepIdx,
                                                  const GuideRate* guideRate,
                                                  const GuideRateModel::Target target,
                                                  const PhaseUsage& pu,
                                                  const Phase& injectionPhase,
                                                  const bool alwaysIncludeThis = false)
    {
        double thisGuideRate = getGuideRateInj(name, schedule, wellState, reportStepIdx, guideRate, target, injectionPhase);
        double controlGroupGuideRate = getGuideRateInj(controlGroupName, schedule, wellState, reportStepIdx, guideRate, target, injectionPhase);
        if (alwaysIncludeThis)
            controlGroupGuideRate += thisGuideRate;

        return thisGuideRate / controlGroupGuideRate;
    }


    template <class RateConverterType>
    inline bool checkGroupConstraintsInj(const std::string& name,
                                         const std::string& parent,
                                         const Group& group,
                                         const WellStateFullyImplicitBlackoil& wellState,
                                         const int reportStepIdx,
                                         const GuideRate* guideRate,
                                         const double* rates,
                                         Phase injectionPhase,
                                         const PhaseUsage& pu,
                                         const double efficiencyFactor,
                                         const Schedule& schedule,
                                         const SummaryState& summaryState,
                                         const RateConverterType& rateConverter,
                                         const int pvtRegionIdx,
                                         DeferredLogger& deferred_logger)
    {
        // When called for a well ('name' is a well name), 'parent'
        // will be the name of 'group'. But if we recurse, 'name' and
        // 'parent' will stay fixed while 'group' will be higher up
        // in the group tree.

        const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(injectionPhase, group.name());
        if (currentGroupControl == Group::InjectionCMode::FLD ||
            currentGroupControl == Group::InjectionCMode::NONE) {
            // Return if we are not available for parent group.
            if (!group.isAvailableForGroupControl()) {
                return false;
            }
            // Otherwise: check injection share of parent's control.
            const auto& parentGroup = schedule.getGroup(group.parent(), reportStepIdx);
            return checkGroupConstraintsInj(name,
                                            parent,
                                            parentGroup,
                                            wellState,
                                            reportStepIdx,
                                            guideRate,
                                            rates,
                                            injectionPhase,
                                            pu,
                                            efficiencyFactor * group.getGroupEfficiencyFactor(),
                                            schedule,
                                            summaryState,
                                            rateConverter,
                                            pvtRegionIdx,
                                            deferred_logger);
        }

        // If we are here, we are at the topmost group to be visited in the recursion.
        // This is the group containing the control we will check against.

        // This can be false for FLD-controlled groups, we must therefore
        // check for FLD first (done above).
        if (!group.isInjectionGroup()) {
            return false;
        }

        int phasePos;
        GuideRateModel::Target target;

        switch (injectionPhase) {
        case Phase::WATER:
        {
            phasePos = pu.phase_pos[BlackoilPhases::Aqua];
            target = GuideRateModel::Target::WAT;
            break;
        }
        case Phase::OIL:
        {
            phasePos = pu.phase_pos[BlackoilPhases::Liquid];
            target = GuideRateModel::Target::OIL;
            break;
        }
        case Phase::GAS:
        {
            phasePos = pu.phase_pos[BlackoilPhases::Vapour];
            target = GuideRateModel::Target::GAS;
            break;
        }
        default:
            OPM_DEFLOG_THROW(std::logic_error, "Expected WATER, OIL or GAS as injecting type for " + name, deferred_logger);
        }

        assert(group.hasInjectionControl(injectionPhase));
        const auto& groupcontrols = group.injectionControls(injectionPhase, summaryState);

        const std::vector<double>& groupInjectionReductions = wellState.currentInjectionGroupReductionRates(group.name());
        const double groupTargetReduction = groupInjectionReductions[phasePos];
        double fraction = wellGroupHelpers::fractionFromInjectionPotentials(name, group.name(), schedule, wellState, reportStepIdx, guideRate, target, pu, injectionPhase, true);

        bool constraint_broken = false;
        switch(currentGroupControl) {
        case Group::InjectionCMode::RATE:
        {
            const double current_rate = rates[phasePos];
            const double target_rate = fraction * std::max(0.0, (groupcontrols.surface_max_rate - groupTargetReduction + current_rate*efficiencyFactor)) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::InjectionCMode::RESV:
        {
            std::vector<double> convert_coeff(pu.num_phases, 1.0);
            rateConverter.calcCoeff(/*fipreg*/ 0, pvtRegionIdx, convert_coeff);
            const double coeff = convert_coeff[phasePos];
            const double current_rate = rates[phasePos];
            const double target_rate = fraction * std::max(0.0, (groupcontrols.resv_max_rate/coeff - groupTargetReduction + current_rate*efficiencyFactor)) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::InjectionCMode::REIN:
        {
            double productionRate = wellState.currentInjectionREINRates(groupcontrols.reinj_group)[phasePos];
            const double current_rate = rates[phasePos];
            const double target_rate = fraction * std::max(0.0, (groupcontrols.target_reinj_fraction*productionRate - groupTargetReduction + current_rate*efficiencyFactor)) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::InjectionCMode::VREP:
        {
            std::vector<double> convert_coeff(pu.num_phases, 1.0);
            rateConverter.calcCoeff(/*fipreg*/ 0, pvtRegionIdx, convert_coeff);
            const double coeff = convert_coeff[phasePos];
            double voidageRate = wellState.currentInjectionVREPRates(groupcontrols.voidage_group)*groupcontrols.target_void_fraction;

            double injReduction = 0.0;
            if (groupcontrols.phase != Phase::WATER)
                injReduction += groupInjectionReductions[pu.phase_pos[BlackoilPhases::Aqua]]*convert_coeff[pu.phase_pos[BlackoilPhases::Aqua]];
            if (groupcontrols.phase != Phase::OIL)
                injReduction += groupInjectionReductions[pu.phase_pos[BlackoilPhases::Liquid]]*convert_coeff[pu.phase_pos[BlackoilPhases::Liquid]];
            if (groupcontrols.phase != Phase::GAS)
                injReduction += groupInjectionReductions[pu.phase_pos[BlackoilPhases::Vapour]]*convert_coeff[pu.phase_pos[BlackoilPhases::Vapour]];
            voidageRate -= injReduction;

            const double current_rate = rates[phasePos];
            const double target_rate = fraction * std::max(0.0, ( voidageRate/coeff - groupTargetReduction + current_rate*efficiencyFactor)) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::InjectionCMode::SALE:
        {
            // only for gas injectors
            assert (phasePos == pu.phase_pos[BlackoilPhases::Vapour]);

            // Gas injection rate = Total gas production rate + gas import rate - gas consumption rate - sales rate;
            double inj_rate = wellState.currentInjectionREINRates(group.name())[phasePos];
            if (schedule.gConSump(reportStepIdx).has(group.name())) {
                const auto& gconsump = schedule.gConSump(reportStepIdx).get(group.name(), summaryState);
                if (pu.phase_used[BlackoilPhases::Vapour]) {
                    inj_rate += gconsump.import_rate;
                    inj_rate -= gconsump.consumption_rate;
                }
            }
            const auto& gconsale = schedule.gConSale(reportStepIdx).get(group.name(), summaryState);
            inj_rate -= gconsale.sales_target;

            const double current_rate = rates[phasePos];
            const double target_rate = fraction * std::max(0.0, (inj_rate - groupTargetReduction + current_rate*efficiencyFactor)) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::InjectionCMode::NONE:
        {
            assert(false); // Should already be handled at the top.
        }
        case Group::InjectionCMode::FLD:
        {
            assert(false); // Should already be handled at the top.
        }
        default:
            OPM_DEFLOG_THROW(std::runtime_error, "Invalid group control specified for group "  + group.name(), deferred_logger );

        }

        return constraint_broken;
    }


    template <class RateConverterType>
    inline bool checkGroupConstraintsProd(const std::string& name,
                                          const std::string& parent,
                                          const Group& group,
                                          const WellStateFullyImplicitBlackoil& wellState,
                                          const int reportStepIdx,
                                          const GuideRate* guideRate,
                                          const double* rates,
                                          const PhaseUsage& pu,
                                          const double efficiencyFactor,
                                          const Schedule& schedule,
                                          const SummaryState& summaryState,
                                          const RateConverterType& rateConverter,
                                          const int pvtRegionIdx,
                                          DeferredLogger& deferred_logger)
    {
        // When called for a well ('name' is a well name), 'parent'
        // will be the name of 'group'. But if we recurse, 'name' and
        // 'parent' will stay fixed while 'group' will be higher up
        // in the group tree.



        const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(group.name());

        if (currentGroupControl == Group::ProductionCMode::FLD ||
            currentGroupControl == Group::ProductionCMode::NONE) {
            // Return if we are not available for parent group.
            if (!group.isAvailableForGroupControl()) {
                return false;
            }
            // Otherwise: check production share of parent's control.
            const auto& parentGroup = schedule.getGroup(group.parent(), reportStepIdx);
            return checkGroupConstraintsProd(name,
                                             parent,
                                             parentGroup,
                                             wellState,
                                             reportStepIdx,
                                             guideRate,
                                             rates,
                                             pu,
                                             efficiencyFactor * group.getGroupEfficiencyFactor(),
                                             schedule,
                                             summaryState,
                                             rateConverter,
                                             pvtRegionIdx,
                                             deferred_logger);
        }

        // If we are here, we are at the topmost group to be visited in the recursion.
        // This is the group containing the control we will check against.

        // This can be false for FLD-controlled groups, we must therefore
        // check for FLD first (done above).
        if (!group.isProductionGroup()) {
            return false;
        }

        auto fractionFunc = [&](const GuideRateModel::Target target) {
            double fraction = fractionFromGuideRates(name,
                                                     group.name(),
                                                     schedule,
                                                     wellState,
                                                     reportStepIdx,
                                                     guideRate,
                                                     target,
                                                     true); // alwaysIncludeThisObject
            return fraction;
        };

        const auto& groupcontrols = group.productionControls(summaryState);
        const std::vector<double>& groupTargetReductions = wellState.currentProductionGroupReductionRates(group.name());

        bool constraint_broken = false;
        switch(currentGroupControl) {
        case Group::ProductionCMode::ORAT:
        {
            assert(pu.phase_used[BlackoilPhases::Liquid]);
            const int pos = pu.phase_pos[BlackoilPhases::Liquid];
            const double groupTargetReduction = groupTargetReductions[pos];
            const double fraction = fractionFunc(GuideRateModel::Target::OIL);
            const double current_rate = -rates[pos];
            const double target_rate = fraction * std::max(0.0, groupcontrols.oil_target - groupTargetReduction + current_rate*efficiencyFactor) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::ProductionCMode::WRAT:
        {
            assert(pu.phase_used[BlackoilPhases::Aqua]);
            const int pos = pu.phase_pos[BlackoilPhases::Aqua];
            const double groupTargetReduction = groupTargetReductions[pos];
            const double fraction = fractionFunc(GuideRateModel::Target::WAT);
            const double current_rate = -rates[pos];
            const double target_rate = fraction * std::max(0.0, groupcontrols.water_target - groupTargetReduction + current_rate*efficiencyFactor) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::ProductionCMode::GRAT:
        {
            assert(pu.phase_used[BlackoilPhases::Vapour]);
            const int pos = pu.phase_pos[BlackoilPhases::Vapour];
            const double groupTargetReduction = groupTargetReductions[pos];
            const double fraction = fractionFunc(GuideRateModel::Target::GAS);
            const double current_rate = -rates[pos];
            const double target_rate = fraction * std::max(0.0, groupcontrols.gas_target - groupTargetReduction + current_rate*efficiencyFactor) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::ProductionCMode::LRAT:
        {
            assert(pu.phase_used[BlackoilPhases::Liquid]);
            assert(pu.phase_used[BlackoilPhases::Aqua]);
            const int opos = pu.phase_pos[BlackoilPhases::Liquid];
            const int wpos = pu.phase_pos[BlackoilPhases::Aqua];
            const double groupTargetReduction = groupTargetReductions[opos] + groupTargetReductions[wpos];
            const double fraction = fractionFunc(GuideRateModel::Target::LIQ);
            const double current_rate = -rates[opos] - rates[wpos];
            const double target_rate = fraction * std::max(0.0, groupcontrols.liquid_target - groupTargetReduction + current_rate*efficiencyFactor) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
            break;
        }
        case Group::ProductionCMode::CRAT:
        {
            OPM_DEFLOG_THROW(std::runtime_error, "CRAT group control not implemented for producers", deferred_logger );
            break;
        }
        case Group::ProductionCMode::RESV:
        {
            assert(pu.phase_used[BlackoilPhases::Liquid]);
            assert(pu.phase_used[BlackoilPhases::Aqua]);
            assert(pu.phase_used[BlackoilPhases::Vapour]);
            const double groupTargetReduction =
                groupTargetReductions[pu.phase_pos[BlackoilPhases::Liquid]]
                + groupTargetReductions[pu.phase_pos[BlackoilPhases::Aqua]]
                + groupTargetReductions[pu.phase_pos[BlackoilPhases::Vapour]];
            const double fraction = fractionFunc(GuideRateModel::Target::RES);
            std::vector<double> convert_coeff(pu.num_phases, 1.0);
            rateConverter.calcCoeff(/*fipreg*/ 0, pvtRegionIdx, convert_coeff);
            double current_rate = 0.0;
            for (int phase = 0; phase < pu.num_phases; ++phase) {
                current_rate -= rates[phase] * convert_coeff[phase];
            }
            const double target_rate = fraction * std::max(0.0, groupcontrols.resv_target - groupTargetReduction + current_rate*efficiencyFactor) / efficiencyFactor;
            if (current_rate > target_rate) {
                constraint_broken = true;
            }
        }
        case Group::ProductionCMode::PRBL:
        {
            OPM_DEFLOG_THROW(std::runtime_error, "PRBL group control not implemented for producers", deferred_logger );
            break;
        }
        case Group::ProductionCMode::FLD:
            // Handled above.
        case Group::ProductionCMode::NONE:
            // Handled above.
        default:
            OPM_DEFLOG_THROW(std::runtime_error, "Invalid group control specified for group "  + group.name(), deferred_logger );
        }

        return constraint_broken;
    }



    } // namespace wellGroupHelpers

}

#endif
