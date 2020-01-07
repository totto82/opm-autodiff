/*
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

// Define making clear that the simulator supports AMG
#define FLOW_SUPPORT_AMG 1

#include <flow/flow_ebos_oilwater_polymer.hpp>

#include <opm/material/common/ResetLocale.hpp>
#include <opm/models/blackoil/blackoiltwophaseindices.hh>

#include <opm/grid/CpGrid.hpp>
#include <opm/simulators/flow/SimulatorFullyImplicitBlackoilEbos.hpp>
#include <opm/simulators/flow/FlowMainEbos.hpp>

#if HAVE_DUNE_FEM
#include <dune/fem/misc/mpimanager.hh>
#else
#include <dune/common/parallel/mpihelper.hh>
#endif

namespace Opm {
namespace Properties {
NEW_TYPE_TAG(EclFlowOilWaterPolymerProblem, INHERITS_FROM(EclFlowProblem));
SET_BOOL_PROP(EclFlowOilWaterPolymerProblem, EnablePolymer, true);
//! The indices required by the model
//! The indices required by the model
SET_PROP(EclFlowOilWaterPolymerProblem, Indices)
{
private:
    // it is unfortunately not possible to simply use 'TypeTag' here because this leads
    // to cyclic definitions of some properties. if this happens the compiler error
    // messages unfortunately are *really* confusing and not really helpful.
    typedef TTAG(EclFlowProblem) BaseTypeTag;
    typedef typename GET_PROP_TYPE(BaseTypeTag, FluidSystem) FluidSystem;

public:
    typedef Opm::BlackOilTwoPhaseIndices<GET_PROP_VALUE(TypeTag, EnableSolvent),
                                         GET_PROP_VALUE(TypeTag, EnablePolymer),
                                         GET_PROP_VALUE(TypeTag, EnableEnergy),
                                         GET_PROP_VALUE(TypeTag, EnableFoam),
                                         GET_PROP_VALUE(TypeTag, EnableSaltWater),
                                         /*PVOffset=*/0,
                                         /*disabledCompIdx=*/FluidSystem::gasCompIdx> type;
};
}}

namespace Opm {
void flowEbosOilWaterPolymerSetDeck(double setupTime, Deck& deck, EclipseState& eclState, Schedule& schedule, SummaryConfig& summaryConfig)
{
    typedef TTAG(EclFlowOilWaterPolymerProblem) TypeTag;
    typedef GET_PROP_TYPE(TypeTag, Vanguard) Vanguard;

    Vanguard::setExternalSetupTime(setupTime);
    Vanguard::setExternalDeck(&deck);
    Vanguard::setExternalEclState(&eclState);
    Vanguard::setExternalSchedule(&schedule);
    Vanguard::setExternalSummaryConfig(&summaryConfig);
}

// ----------------- Main program -----------------
int flowEbosOilWaterPolymerMain(int argc, char** argv, bool outputCout, bool outputFiles)
{
    // we always want to use the default locale, and thus spare us the trouble
    // with incorrect locale settings.
    Opm::resetLocale();

#if HAVE_DUNE_FEM
    Dune::Fem::MPIManager::initialize(argc, argv);
#else
    Dune::MPIHelper::instance(argc, argv);
#endif

    Opm::FlowMainEbos<TTAG(EclFlowOilWaterPolymerProblem)> mainfunc;
    return mainfunc.execute(argc, argv, outputCout, outputFiles);
}

}
