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

#include <flow/flow_4.hpp>

#include <opm/material/common/ResetLocale.hpp>
#include <opm/models/blackoil/blackoildynindices.hh>

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
namespace TTag {
struct EclFlow4Problem {
    using InheritsFrom = std::tuple<EclFlowProblem>;
};
}

//! The indices required by the model
template<class TypeTag>
struct Indices<TypeTag, TTag::EclFlow4Problem>
{
private:
    // it is unfortunately not possible to simply use 'TypeTag' here because this leads
    // to cyclic definitions of some properties. if this happens the compiler error
    // messages unfortunately are *really* confusing and not really helpful.
    using BaseTypeTag = TTag::EclFlowProblem;
    using FluidSystem = GetPropType<BaseTypeTag, Properties::FluidSystem>;

public:
#warning need to make a 4-3 variant for polymer++
    typedef Opm::BlackOilDynIndices<4,4> type;
};
template<class TypeTag>
struct EnableSolvent<TypeTag, TTag::EclFlow4Problem> {
    static constexpr bool value = true;
};
}}

namespace Opm {
void flow4SetDeck(double setupTime, std::unique_ptr<Deck> deck,
                             std::unique_ptr<EclipseState> eclState,
                             std::unique_ptr<Schedule> schedule,
                             std::unique_ptr<SummaryConfig> summaryConfig)
{
    using TypeTag = Properties::TTag::EclFlow4Problem;
    using Vanguard = GetPropType<TypeTag, Properties::Vanguard>;

    Vanguard::setExternalSetupTime(setupTime);
    Vanguard::setExternalDeck(std::move(deck));
    Vanguard::setExternalEclState(std::move(eclState));
    Vanguard::setExternalSchedule(std::move(schedule));
    Vanguard::setExternalSummaryConfig(std::move(summaryConfig));
}

// ----------------- Main program -----------------
int flow4Main(int argc, char** argv, bool outputCout, bool outputFiles)
{
    // we always want to use the default locale, and thus spare us the trouble
    // with incorrect locale settings.
    Opm::resetLocale();

#if HAVE_DUNE_FEM
    Dune::Fem::MPIManager::initialize(argc, argv);
#else
    Dune::MPIHelper::instance(argc, argv);
#endif

    Opm::FlowMainEbos<Properties::TTag::EclFlow4Problem>
        mainfunc {argc, argv, outputCout, outputFiles};
    return mainfunc.execute();
}

}
