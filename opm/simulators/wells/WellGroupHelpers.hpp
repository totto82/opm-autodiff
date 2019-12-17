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

#include <vector>

namespace Opm {


    namespace wellGroupHelpers
    {

    inline void setGroupControl(const Group& group, const Schedule& schedule, const int reportStepIdx, const bool injector, WellStateFullyImplicitBlackoil& wellState, std::ostringstream& ss) {

        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            setGroupControl(groupTmp, schedule, reportStepIdx, injector, wellState, ss);
            if (injector)
                wellState.setCurrentInjectionGroupControl(groupName, Group::InjectionCMode::FLD);
            else
                wellState.setCurrentProductionGroupControl(groupName, Group::ProductionCMode::FLD);
        }

        const auto& end = wellState.wellMap().end();
        for (const std::string& wellName : group.wells()) {
            const auto& it = wellState.wellMap().find( wellName );
            if (it == end)  // the well is not found
                continue;

            int well_index = it->second[0];
            const auto& wellEcl = schedule.getWell(wellName, reportStepIdx);

            if (wellEcl.getStatus() == Well::Status::SHUT)
                continue;

            if (!wellEcl.isAvailableForGroupControl())
                continue;

            if (wellEcl.isProducer() && !injector) {
                if (wellState.currentProductionControls()[well_index] != Well::ProducerCMode::GRUP) {
                    wellState.currentProductionControls()[well_index] = Well::ProducerCMode::GRUP;
                    ss <<"\n Producer " << wellName << " switches to GRUP control limit";
                }
            }

            if (wellEcl.isInjector() && injector) {
                // only switch if the well phase is the same as the group phase
                if (group.injection_phase() != wellEcl.getPreferredPhase())
                    continue;

                if (wellState.currentInjectionControls()[well_index] != Well::InjectorCMode::GRUP) {
                    wellState.currentInjectionControls()[well_index] = Well::InjectorCMode::GRUP;
                    ss <<"\n Injector " << wellName << " switches to GRUP control limit";
                }
            }
        }
    }

    inline void setCmodeGroup(const Group& group, const Schedule& schedule, const SummaryState& summaryState, const int reportStepIdx, WellStateFullyImplicitBlackoil& wellState) {

        for (const std::string& groupName : group.groups()) {
            setCmodeGroup( schedule.getGroup(groupName, reportStepIdx), schedule, summaryState, reportStepIdx, wellState);
        }

        // use NONE as default control
        if (!wellState.hasInjectionGroupControl(group.name())) {
            wellState.setCurrentInjectionGroupControl(group.name(), Group::InjectionCMode::NONE);
        }
        if (!wellState.hasProductionGroupControl(group.name())) {
            wellState.setCurrentProductionGroupControl(group.name(), Group::ProductionCMode::NONE);
        }

        if (group.isInjectionGroup() && schedule.hasWellGroupEvent(group.name(),  ScheduleEvents::GROUP_INJECTION_UPDATE, reportStepIdx)) {
            const auto controls = group.injectionControls(summaryState);
            wellState.setCurrentInjectionGroupControl(group.name(), controls.cmode);
        }

        if (group.isProductionGroup() && schedule.hasWellGroupEvent(group.name(),  ScheduleEvents::GROUP_PRODUCTION_UPDATE, reportStepIdx)) {
            const auto controls = group.productionControls(summaryState);
            wellState.setCurrentProductionGroupControl(group.name(), controls.cmode);
        }

        if (schedule.gConSale(reportStepIdx).has(group.name())) {
            wellState.setCurrentInjectionGroupControl(group.name(), Group::InjectionCMode::SALE);
            std::ostringstream ss;
            setGroupControl(group, schedule, reportStepIdx, /*injector*/true, wellState, ss);
        }
    }


    inline void accumulateGroupEfficiencyFactor(const Group& group, const Schedule& schedule, const int reportStepIdx, double& factor) {
        factor *= group.getGroupEfficiencyFactor();
        if (group.parent() != "FIELD")
            accumulateGroupEfficiencyFactor(schedule.getGroup(group.parent(), reportStepIdx), schedule, reportStepIdx, factor);
    }


    inline void computeGroupTargetReduction(const Group& group, const WellStateFullyImplicitBlackoil& wellState, const Schedule& schedule, const int reportStepIdx, const int phasePos, const bool isInjector, double& groupTargetReduction )
    {

        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            computeGroupTargetReduction(groupTmp, wellState, schedule, reportStepIdx, phasePos, isInjector, groupTargetReduction);
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
            // add contributino from wells not under group control
            if (isInjector) {
                if (wellState.currentInjectionControls()[well_index] != Well::InjectorCMode::GRUP)
                    groupTargetReduction += wellState.wellRates()[wellrate_index + phasePos];
            } else {
                if (wellState.currentProductionControls()[well_index] !=  Well::ProducerCMode::GRUP)
                    groupTargetReduction -= wellState.wellRates()[wellrate_index + phasePos];
            }
        }
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

    inline void updateGroupTargetReduction(const Group& group, const Schedule& schedule, const int reportStepIdx, const bool isInjector, const WellStateFullyImplicitBlackoil& wellStateNupcol, WellStateFullyImplicitBlackoil& wellState, std::vector<double>& groupTargetReduction)
    {
        const int np = wellState.numPhases();
        for (const std::string& groupName : group.groups()) {
            std::vector<double> thisGroupTargetReduction(np, 0.0);
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateGroupTargetReduction(groupTmp, schedule, reportStepIdx, isInjector, wellStateNupcol, wellState, thisGroupTargetReduction);

            // accumulate group contribution from sub group
            if (isInjector) {
                const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(groupName);
                if (currentGroupControl != Group::InjectionCMode::FLD) {
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += sumWellRates(groupTmp, schedule, wellStateNupcol, reportStepIdx, phase, isInjector);
                    }
                    continue;
                }
            } else {
                const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
                if (currentGroupControl != Group::ProductionCMode::FLD) {
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += sumWellRates(groupTmp, schedule, wellStateNupcol, reportStepIdx, phase, isInjector);
                    }
                    continue;
                }
            }
            // or accumulate directly from the wells if controled from its parents
            for (int phase = 0; phase < np; phase++) {
                groupTargetReduction[phase] += thisGroupTargetReduction[phase];
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
            // add contributino from wells not under group control
            if (isInjector) {
                if (wellState.currentInjectionControls()[well_index] != Well::InjectorCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] += wellStateNupcol.wellRates()[wellrate_index + phase];
                    }
            } else {
                if (wellState.currentProductionControls()[well_index] !=  Well::ProducerCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        groupTargetReduction[phase] -= wellStateNupcol.wellRates()[wellrate_index + phase];
                    }
            }
        }
        if (isInjector)
            wellState.setCurrentInjectionGroupReductionRates(group.name(), groupTargetReduction);
        else
            wellState.setCurrentProductionGroupReductionRates(group.name(), groupTargetReduction);
    }


    inline void updateGuideRateForGroups(const Group& group, const Schedule& schedule, const PhaseUsage& pu, const int reportStepIdx, const double& simTime, const bool isInjector, WellStateFullyImplicitBlackoil& wellState, GuideRate* guideRate, std::vector<double>& pot)
    {
        const int np = pu.num_phases;
        for (const std::string& groupName : group.groups()) {
            std::vector<double> thisPot(np, 0.0);
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            updateGuideRateForGroups(groupTmp, schedule, pu, reportStepIdx, simTime, isInjector, wellState, guideRate, thisPot);

            // accumulate group contribution from sub group if not FLD
            if (isInjector) {
                const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(groupName);
                if (currentGroupControl != Group::InjectionCMode::FLD) {
                    continue;
                }
            } else {
                const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
                if (currentGroupControl != Group::ProductionCMode::FLD) {
                    continue;
                }
            }
            for (int phase = 0; phase < np; phase++) {
                pot[phase] += thisPot[phase];
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
            // add contribution from wells not under group control
            if (isInjector) {
                if (wellState.currentInjectionControls()[well_index] == Well::InjectorCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        pot[phase] += wellState.wellPotentials()[wellrate_index + phase];
                    }
            } else {
                if (wellState.currentProductionControls()[well_index] ==  Well::ProducerCMode::GRUP)
                    for (int phase = 0; phase < np; phase++) {
                        pot[phase] -= wellState.wellPotentials()[wellrate_index + phase];
                    }
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

        if (isInjector) {
            wellState.setCurrentGroupInjectionPotentials(group.name(), pot);
        } else {
            guideRate->compute(group.name(), reportStepIdx, simTime, oilPot, gasPot, waterPot);
        }
    }


    inline void updateVREPForGroups(const Group& group, const Schedule& schedule, const int reportStepIdx, WellStateFullyImplicitBlackoil& wellState, double& resv) {
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            double thisResv = 0.0;
            updateVREPForGroups(groupTmp, schedule, reportStepIdx, wellState, thisResv);
            resv += thisResv;
        }
        const int np = wellState.numPhases();
        for (int phase = 0; phase < np; ++phase) {
            resv += sumWellPhaseRates(wellState.wellReservoirRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ false);
        }

        wellState.setCurrentInjectionVREPRates(group.name(), resv);
    }

    inline void updateREINForGroups(const Group& group, const Schedule& schedule, const int reportStepIdx, const PhaseUsage& pu, const SummaryState& st, WellStateFullyImplicitBlackoil& wellState, std::vector<double>& rein) {
        const int np = wellState.numPhases();
        for (const std::string& groupName : group.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);
            std::vector<double> thisRein(np, 0.0);
            updateREINForGroups(groupTmp, schedule, reportStepIdx, pu, st, wellState, thisRein);
            for (int phase = 0; phase < np; ++phase) {
                rein[phase] = thisRein[phase];
            }
        }
        for (int phase = 0; phase < np; ++phase) {
            rein[phase] = sumWellPhaseRates(wellState.wellRates(), group, schedule, wellState, reportStepIdx, phase, /*isInjector*/ false);
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

    inline double wellFractionFromGuideRates(const Well& well, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const GuideRate* guideRate, const Well::GuideRateTarget& wellTarget, const bool isInjector) {
        double groupTotalGuideRate = 0.0;
        const Group& groupTmp = schedule.getGroup(well.groupName(), reportStepIdx);
        for (const std::string& wellName : groupTmp.wells()) {
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

            // only count wells under group control
            if (isInjector) {
                if (wellState.currentInjectionControls()[well_index] != Well::InjectorCMode::GRUP)
                    continue;
            } else {
                if (wellState.currentProductionControls()[well_index] !=  Well::ProducerCMode::GRUP)
                    continue;
            }

            groupTotalGuideRate += guideRate->get(wellName, wellTarget);
        }

        if (groupTotalGuideRate == 0.0)
            return 0.0;

        double wellGuideRate = guideRate->get(well.name(), wellTarget);
        return wellGuideRate / groupTotalGuideRate;
    }

    inline double groupFractionFromGuideRates(const Group& group, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const GuideRate* guideRate, const Group::GuideRateTarget& groupTarget, const bool isInjector) {
        double groupTotalGuideRate = 0.0;
        const Group& groupParent = schedule.getGroup(group.parent(), reportStepIdx);
        for (const std::string& groupName : groupParent.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);

            // only count group under group control from its parent
            if (isInjector) {
                const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(groupName);
                if (currentGroupControl != Group::InjectionCMode::FLD)
                    continue;
            } else {
                const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
                if (currentGroupControl != Group::ProductionCMode::FLD)
                    continue;
            }

            if ( (groupTmp.isProductionGroup() && !isInjector)) {
                groupTotalGuideRate += guideRate->get(groupName, groupTarget);
            } else if ((groupTmp.isInjectionGroup() && isInjector)){
                assert(false);
            }

        }
        if (groupTotalGuideRate == 0.0)
            return 1.0;

        double groupGuideRate = guideRate->get(group.name(), groupTarget);
        return groupGuideRate / groupTotalGuideRate;
    }

    inline void accumulateGroupFractions(const std::string& groupName, const std::string& controlGroupName, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState,const int reportStepIdx, const GuideRate* guideRate, const Group::GuideRateTarget& groupTarget, const bool isInjector, double& fraction) {

        const Group& group = schedule.getGroup(groupName, reportStepIdx);
        if (groupName != controlGroupName) {
            fraction *= groupFractionFromGuideRates(group, schedule, wellState, reportStepIdx, guideRate, groupTarget, isInjector);
            accumulateGroupFractions(group.parent(), controlGroupName, schedule, wellState, reportStepIdx, guideRate, groupTarget, isInjector, fraction);
        }

        return;
    }

    inline double groupFractionFromPotentials(const Group& group, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState, const int reportStepIdx, const int phasePos, const bool isInjector) {
        double groupTotalGuideRate = 0.0;
        const Group& groupParent = schedule.getGroup(group.parent(), reportStepIdx);
        for (const std::string& groupName : groupParent.groups()) {
            const Group& groupTmp = schedule.getGroup(groupName, reportStepIdx);

            // only count group under group control from its parent
            if (isInjector) {
                const Group::InjectionCMode& currentGroupControl = wellState.currentInjectionGroupControl(groupName);
                if (currentGroupControl != Group::InjectionCMode::FLD)
                    continue;
            } else {
                const Group::ProductionCMode& currentGroupControl = wellState.currentProductionGroupControl(groupName);
                if (currentGroupControl != Group::ProductionCMode::FLD)
                    continue;
            }

            if ( (groupTmp.isProductionGroup() && !isInjector)) {
                assert(false);
            } else if ((groupTmp.isInjectionGroup() && isInjector)){
                groupTotalGuideRate += wellState.currentGroupInjectionPotentials(groupName)[phasePos];
            }

        }
        if (groupTotalGuideRate == 0.0)
            return 1.0;

        double groupGuideRate = wellState.currentGroupInjectionPotentials(group.name())[phasePos];
        return groupGuideRate / groupTotalGuideRate;
    }

    inline void accumulateGroupPotentialFractions(const std::string& groupName, const std::string& controlGroupName, const Schedule& schedule, const WellStateFullyImplicitBlackoil& wellState,const int reportStepIdx, const int phasePos, const bool isInjector, double& fraction) {

        const Group& group = schedule.getGroup(groupName, reportStepIdx);
        if (groupName != controlGroupName) {
            fraction *= groupFractionFromPotentials(group, schedule, wellState, reportStepIdx, phasePos, isInjector);
            accumulateGroupPotentialFractions(group.parent(), controlGroupName, schedule, wellState, reportStepIdx, phasePos, isInjector, fraction);
        }

        return;
    }



    } // namespace wellGroupHelpers

}

#endif
