/*
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

#ifndef OPM_ISTLSOLVER_EBOS_HEADER_INCLUDED
#define OPM_ISTLSOLVER_EBOS_HEADER_INCLUDED

#include <opm/autodiff/BlackoilAmg.hpp>
#include <opm/autodiff/CPRPreconditioner.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterleaved.hpp>
#include <opm/autodiff/NewtonIterationUtilities.hpp>
#include <opm/autodiff/ParallelRestrictedAdditiveSchwarz.hpp>
#include <opm/autodiff/ParallelOverlappingILU0.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>

#include <opm/common/Exceptions.hpp>
#include <opm/core/linalg/ParallelIstlInformation.hpp>
#include <opm/common/utility/platform_dependent/disable_warnings.h>

#include <ewoms/common/parametersystem.hh>
#include <ewoms/common/propertysystem.hh>

#include <dune/istl/scalarproducts.hh>
#include <dune/istl/operators.hh>
#include <dune/istl/preconditioners.hh>
#include <dune/istl/solvers.hh>
#include <dune/istl/owneroverlapcopy.hh>
#include <dune/istl/paamg/amg.hh>

#include <opm/common/utility/platform_dependent/reenable_warnings.h>

BEGIN_PROPERTIES

NEW_TYPE_TAG(FlowIstlSolver, INHERITS_FROM(FlowIstlSolverParams));

NEW_PROP_TAG(Scalar);
NEW_PROP_TAG(GlobalEqVector);
NEW_PROP_TAG(JacobianMatrix);
NEW_PROP_TAG(Indices);
NEW_PROP_TAG(Simulator);
NEW_PROP_TAG(EclWellModel);
END_PROPERTIES

namespace Dune
{
namespace FMatrixHelp {
//! invert 4x4 Matrix without changing the original matrix
template <typename K>
static inline K invertMatrix (const FieldMatrix<K,4,4> &matrix, FieldMatrix<K,4,4> &inverse)
{
    inverse[0][0] = matrix[1][1] * matrix[2][2] * matrix[3][3] -
            matrix[1][1] * matrix[2][3] * matrix[3][2] -
            matrix[2][1] * matrix[1][2] * matrix[3][3] +
            matrix[2][1] * matrix[1][3] * matrix[3][2] +
            matrix[3][1] * matrix[1][2] * matrix[2][3] -
            matrix[3][1] * matrix[1][3] * matrix[2][2];

    inverse[1][0] = -matrix[1][0] * matrix[2][2] * matrix[3][3] +
            matrix[1][0] * matrix[2][3] * matrix[3][2] +
            matrix[2][0] * matrix[1][2] * matrix[3][3] -
            matrix[2][0] * matrix[1][3] * matrix[3][2] -
            matrix[3][0] * matrix[1][2] * matrix[2][3] +
            matrix[3][0] * matrix[1][3] * matrix[2][2];

    inverse[2][0] = matrix[1][0] * matrix[2][1] * matrix[3][3] -
            matrix[1][0] * matrix[2][3] * matrix[3][1] -
            matrix[2][0] * matrix[1][1] * matrix[3][3] +
            matrix[2][0] * matrix[1][3] * matrix[3][1] +
            matrix[3][0] * matrix[1][1] * matrix[2][3] -
            matrix[3][0] * matrix[1][3] * matrix[2][1];

    inverse[3][0] = -matrix[1][0] * matrix[2][1] * matrix[3][2] +
            matrix[1][0] * matrix[2][2] * matrix[3][1] +
            matrix[2][0] * matrix[1][1] * matrix[3][2] -
            matrix[2][0] * matrix[1][2] * matrix[3][1] -
            matrix[3][0] * matrix[1][1] * matrix[2][2] +
            matrix[3][0] * matrix[1][2] * matrix[2][1];

    inverse[0][1]= -matrix[0][1]  * matrix[2][2] * matrix[3][3] +
            matrix[0][1] * matrix[2][3] * matrix[3][2] +
            matrix[2][1] * matrix[0][2] * matrix[3][3] -
            matrix[2][1] * matrix[0][3] * matrix[3][2] -
            matrix[3][1] * matrix[0][2] * matrix[2][3] +
            matrix[3][1] * matrix[0][3] * matrix[2][2];

    inverse[1][1] = matrix[0][0] * matrix[2][2] * matrix[3][3] -
            matrix[0][0] * matrix[2][3] * matrix[3][2] -
            matrix[2][0] * matrix[0][2] * matrix[3][3] +
            matrix[2][0] * matrix[0][3] * matrix[3][2] +
            matrix[3][0] * matrix[0][2] * matrix[2][3] -
            matrix[3][0] * matrix[0][3] * matrix[2][2];

    inverse[2][1] = -matrix[0][0] * matrix[2][1] * matrix[3][3] +
            matrix[0][0] * matrix[2][3] * matrix[3][1] +
            matrix[2][0] * matrix[0][1] * matrix[3][3] -
            matrix[2][0] * matrix[0][3] * matrix[3][1] -
            matrix[3][0] * matrix[0][1] * matrix[2][3] +
            matrix[3][0] * matrix[0][3] * matrix[2][1];

    inverse[3][1] = matrix[0][0] * matrix[2][1] * matrix[3][2] -
            matrix[0][0] * matrix[2][2] * matrix[3][1] -
            matrix[2][0] * matrix[0][1] * matrix[3][2] +
            matrix[2][0] * matrix[0][2] * matrix[3][1] +
            matrix[3][0] * matrix[0][1] * matrix[2][2] -
            matrix[3][0] * matrix[0][2] * matrix[2][1];

    inverse[0][2] = matrix[0][1] * matrix[1][2] * matrix[3][3] -
            matrix[0][1] * matrix[1][3] * matrix[3][2] -
            matrix[1][1] * matrix[0][2] * matrix[3][3] +
            matrix[1][1] * matrix[0][3] * matrix[3][2] +
            matrix[3][1] * matrix[0][2] * matrix[1][3] -
            matrix[3][1] * matrix[0][3] * matrix[1][2];

    inverse[1][2] = -matrix[0][0]  * matrix[1][2] * matrix[3][3] +
            matrix[0][0] * matrix[1][3] * matrix[3][2] +
            matrix[1][0] * matrix[0][2] * matrix[3][3] -
            matrix[1][0] * matrix[0][3] * matrix[3][2] -
            matrix[3][0] * matrix[0][2] * matrix[1][3] +
            matrix[3][0] * matrix[0][3] * matrix[1][2];

    inverse[2][2] = matrix[0][0] * matrix[1][1] * matrix[3][3] -
            matrix[0][0] * matrix[1][3] * matrix[3][1] -
            matrix[1][0] * matrix[0][1] * matrix[3][3] +
            matrix[1][0] * matrix[0][3] * matrix[3][1] +
            matrix[3][0] * matrix[0][1] * matrix[1][3] -
            matrix[3][0] * matrix[0][3] * matrix[1][1];

    inverse[3][2] = -matrix[0][0] * matrix[1][1] * matrix[3][2] +
            matrix[0][0] * matrix[1][2] * matrix[3][1] +
            matrix[1][0] * matrix[0][1] * matrix[3][2] -
            matrix[1][0] * matrix[0][2] * matrix[3][1] -
            matrix[3][0] * matrix[0][1] * matrix[1][2] +
            matrix[3][0] * matrix[0][2] * matrix[1][1];

    inverse[0][3] = -matrix[0][1] * matrix[1][2] * matrix[2][3] +
            matrix[0][1] * matrix[1][3] * matrix[2][2] +
            matrix[1][1] * matrix[0][2] * matrix[2][3] -
            matrix[1][1] * matrix[0][3] * matrix[2][2] -
            matrix[2][1] * matrix[0][2] * matrix[1][3] +
            matrix[2][1] * matrix[0][3] * matrix[1][2];

    inverse[1][3] = matrix[0][0] * matrix[1][2] * matrix[2][3] -
            matrix[0][0] * matrix[1][3] * matrix[2][2] -
            matrix[1][0] * matrix[0][2] * matrix[2][3] +
            matrix[1][0] * matrix[0][3] * matrix[2][2] +
            matrix[2][0] * matrix[0][2] * matrix[1][3] -
            matrix[2][0] * matrix[0][3] * matrix[1][2];

    inverse[2][3] = -matrix[0][0] * matrix[1][1] * matrix[2][3] +
            matrix[0][0] * matrix[1][3] * matrix[2][1] +
            matrix[1][0] * matrix[0][1] * matrix[2][3] -
            matrix[1][0] * matrix[0][3] * matrix[2][1] -
            matrix[2][0] * matrix[0][1] * matrix[1][3] +
            matrix[2][0] * matrix[0][3] * matrix[1][1];

    inverse[3][3] = matrix[0][0] * matrix[1][1] * matrix[2][2] -
            matrix[0][0] * matrix[1][2] * matrix[2][1] -
            matrix[1][0] * matrix[0][1] * matrix[2][2] +
            matrix[1][0] * matrix[0][2] * matrix[2][1] +
            matrix[2][0] * matrix[0][1] * matrix[1][2] -
            matrix[2][0] * matrix[0][2] * matrix[1][1];

    K det = matrix[0][0] * inverse[0][0] + matrix[0][1] * inverse[1][0] + matrix[0][2] * inverse[2][0] + matrix[0][3] * inverse[3][0];

    // return identity for singular or nearly singular matrices.
    if (std::abs(det) < 1e-40) {
        for (int i = 0; i < 4; ++i){
            inverse[i][i] = 1.0;
        }
        return 1.0;
    }
    K inv_det = 1.0 / det;
    inverse *= inv_det;

    return det;
}
} // end FMatrixHelp

namespace ISTLUtility {

//! invert matrix by calling FMatrixHelp::invert
template <typename K>
static inline void invertMatrix (FieldMatrix<K,1,1> &matrix)
{
    FieldMatrix<K,1,1> A ( matrix );
    FMatrixHelp::invertMatrix(A, matrix );
}

//! invert matrix by calling FMatrixHelp::invert
template <typename K>
static inline void invertMatrix (FieldMatrix<K,2,2> &matrix)
{
    FieldMatrix<K,2,2> A ( matrix );
    FMatrixHelp::invertMatrix(A, matrix );
}

//! invert matrix by calling FMatrixHelp::invert
template <typename K>
static inline void invertMatrix (FieldMatrix<K,3,3> &matrix)
{
    FieldMatrix<K,3,3> A ( matrix );
    FMatrixHelp::invertMatrix(A, matrix );
}

//! invert matrix by calling FMatrixHelp::invert
template <typename K>
static inline void invertMatrix (FieldMatrix<K,4,4> &matrix)
{
    FieldMatrix<K,4,4> A ( matrix );
    FMatrixHelp::invertMatrix(A, matrix );
}

//! invert matrix by calling matrix.invert
template <typename K, int n>
static inline void invertMatrix (FieldMatrix<K,n,n> &matrix)
{
#if ! DUNE_VERSION_NEWER( DUNE_COMMON, 2, 7 )
    Dune::FMatrixPrecision<K>::set_singular_limit(1.e-20);
#endif
    matrix.invert();
}

} // end ISTLUtility

template <class Scalar, int n, int m>
class MatrixBlock : public Dune::FieldMatrix<Scalar, n, m>
{
public:
    typedef Dune::FieldMatrix<Scalar, n, m>  BaseType;

    using BaseType :: operator= ;
    using BaseType :: rows;
    using BaseType :: cols;
    explicit MatrixBlock( const Scalar scalar = 0 ) : BaseType( scalar ) {}
    void invert()
    {
        ISTLUtility::invertMatrix( *this );
    }
    const BaseType& asBase() const { return static_cast< const BaseType& > (*this); }
    BaseType& asBase() { return static_cast< BaseType& > (*this); }
};

template<class K, int n, int m>
void
print_row (std::ostream& s, const MatrixBlock<K,n,m>& A,
           typename FieldMatrix<K,n,m>::size_type I,
           typename FieldMatrix<K,n,m>::size_type J,
           typename FieldMatrix<K,n,m>::size_type therow, int width,
           int precision)
{
    print_row(s, A.asBase(), I, J, therow, width, precision);
}

template<class K, int n, int m>
K& firstmatrixelement (MatrixBlock<K,n,m>& A)
{
   return firstmatrixelement( A.asBase() );
}



template<typename Scalar, int n, int m>
struct MatrixDimension< MatrixBlock< Scalar, n, m > >
: public MatrixDimension< typename MatrixBlock< Scalar, n, m >::BaseType >
{
};


#if HAVE_UMFPACK

/// \brief UMFPack specialization for MatrixBlock to make AMG happy
///
/// Without this the empty default implementation would be used.
template<typename T, typename A, int n, int m>
class UMFPack<BCRSMatrix<MatrixBlock<T,n,m>, A> >
    : public UMFPack<BCRSMatrix<FieldMatrix<T,n,m>, A> >
{
    typedef UMFPack<BCRSMatrix<FieldMatrix<T,n,m>, A> > Base;
    typedef BCRSMatrix<FieldMatrix<T,n,m>, A> Matrix;

public:
    typedef BCRSMatrix<MatrixBlock<T,n,m>, A> RealMatrix;

    UMFPack(const RealMatrix& matrix, int verbose, bool)
        : Base(reinterpret_cast<const Matrix&>(matrix), verbose)
    {}
};
#endif

#if HAVE_SUPERLU

/// \brief SuperLU specialization for MatrixBlock to make AMG happy
///
/// Without this the empty default implementation would be used.
template<typename T, typename A, int n, int m>
class SuperLU<BCRSMatrix<MatrixBlock<T,n,m>, A> >
    : public SuperLU<BCRSMatrix<FieldMatrix<T,n,m>, A> >
{
    typedef SuperLU<BCRSMatrix<FieldMatrix<T,n,m>, A> > Base;
    typedef BCRSMatrix<FieldMatrix<T,n,m>, A> Matrix;

public:
    typedef BCRSMatrix<MatrixBlock<T,n,m>, A> RealMatrix;

    SuperLU(const RealMatrix& matrix, int verbose, bool reuse=true)
        : Base(reinterpret_cast<const Matrix&>(matrix), verbose, reuse)
    {}
};
#endif


} // end namespace Dune

namespace Opm
{
//=====================================================================
// Implementation for ISTL-matrix based operator
//=====================================================================

/*!
   \brief Adapter to turn a matrix into a linear operator.

   Adapts a matrix to the assembled linear operator interface
 */
template<class M, class X, class Y, class WellModel, bool overlapping >
class WellModelMatrixAdapter : public Dune::AssembledLinearOperator<M,X,Y>
{
  typedef Dune::AssembledLinearOperator<M,X,Y> BaseType;

public:
  typedef M matrix_type;
  typedef X domain_type;
  typedef Y range_type;
  typedef typename X::field_type field_type;

#if HAVE_MPI
  typedef Dune::OwnerOverlapCopyCommunication<int,int> communication_type;
#else
  typedef Dune::CollectiveCommunication< Grid > communication_type;
#endif

#if DUNE_VERSION_NEWER(DUNE_ISTL, 2, 6)
  Dune::SolverCategory::Category category() const override
  {
    return overlapping ?
           Dune::SolverCategory::overlapping : Dune::SolverCategory::sequential;
  }
#else
  enum {
    //! \brief The solver category.
    category = overlapping ?
        Dune::SolverCategory::overlapping :
        Dune::SolverCategory::sequential
  };
#endif

  //! constructor: just store a reference to a matrix
  WellModelMatrixAdapter (const M& A,
                          const M& A_for_precond,
                          const WellModel& wellMod,
                          const boost::any& parallelInformation = boost::any() )
      : A_( A ), A_for_precond_(A_for_precond), wellMod_( wellMod ), comm_()
  {
#if HAVE_MPI
    if( parallelInformation.type() == typeid(ParallelISTLInformation) )
    {
      const ParallelISTLInformation& info =
          boost::any_cast<const ParallelISTLInformation&>( parallelInformation);
      comm_.reset( new communication_type( info.communicator() ) );
    }
#endif
  }

  virtual void apply( const X& x, Y& y ) const
  {
    A_.mv( x, y );

    // add well model modification to y
    wellMod_.apply(x, y );

#if HAVE_MPI
    if( comm_ )
      comm_->project( y );
#endif
  }

  // y += \alpha * A * x
  virtual void applyscaleadd (field_type alpha, const X& x, Y& y) const
  {
    A_.usmv(alpha,x,y);

    // add scaled well model modification to y
    wellMod_.applyScaleAdd( alpha, x, y );

#if HAVE_MPI
    if( comm_ )
      comm_->project( y );
#endif
  }

  virtual const matrix_type& getmat() const { return A_for_precond_; }

  communication_type* comm()
  {
      return comm_.operator->();
  }

protected:
  const matrix_type& A_ ;
  const matrix_type& A_for_precond_ ;
  const WellModel& wellMod_;
  std::unique_ptr< communication_type > comm_;
};
}

namespace Opm
{
namespace Detail
{
    //! calculates ret = A^T * B
    template< class K, int m, int n, int p >
    static inline void multMatrixTransposed ( const Dune::FieldMatrix< K, n, m > &A,
                                              const Dune::FieldMatrix< K, n, p > &B,
                                              Dune::FieldMatrix< K, m, p > &ret )
    {
        typedef typename Dune::FieldMatrix< K, m, p > :: size_type size_type;

        for( size_type i = 0; i < m; ++i )
        {
            for( size_type j = 0; j < p; ++j )
            {
                ret[ i ][ j ] = K( 0 );
                for( size_type k = 0; k < n; ++k )
                    ret[ i ][ j ] += A[ k ][ i ] * B[ k ][ j ];
            }
        }
    }
}
    /// This class solves the fully implicit black-oil system by
    /// solving the reduced system (after eliminating well variables)
    /// as a block-structured matrix (one block for all cell variables) for a fixed
    /// number of cell variables np .
    /// \tparam MatrixBlockType The type of the matrix block used.
    /// \tparam VectorBlockType The type of the vector block used.
    /// \tparam pressureIndex The index of the pressure component in the vector
    ///                       vector block. It is used to guide the AMG coarsening.
    ///                       Default is zero.
    template <class TypeTag>
    class ISTLSolverEbos
    {
        typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
        typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
        typedef typename GET_PROP_TYPE(TypeTag, JacobianMatrix) Matrix;
        typedef typename GET_PROP_TYPE(TypeTag, GlobalEqVector) Vector;
        typedef typename GET_PROP_TYPE(TypeTag, Indices) Indices;

        enum { pressureIndex = Indices::pressureSwitchIdx };

    public:
        typedef Dune::AssembledLinearOperator< Matrix, Vector, Vector > AssembledLinearOperatorType;


        /// Construct a system solver.
        /// \param[in] parallelInformation In the case of a parallel run
        ///                                with dune-istl the information about the parallelization.
        ISTLSolverEbos(const Simulator& simulator)
            : simulator_(simulator)
            , iterations_( 0 )
        {
            parameters_.template init<TypeTag>();
            //extractParallelGridInformationToISTL(simulator_.vanguard().grid(), parallelInformation_);
        }


        static void registerParameters()
        {
            NewtonIterationBlackoilInterleavedParameters::registerParameters<TypeTag>();
        }

        void eraseMatrix() {}

        void prepareMatrix(const Matrix& M) {
        }

        void prepareRhs(const Matrix& M, Vector& b) {
            matrix_ = &M;
            rhs_ = &b;
        }

        bool solve(Vector& x) {

            Dune::InverseOperatorResult result;
            typedef typename GET_PROP_TYPE(TypeTag, EclWellModel) WellModel;

            // Parallel version is deactivated until we figure out how to do it properly.
//#if HAVE_MPI
            if (parallelInformation_.type() == typeid(ParallelISTLInformation))
            {
                const ParallelISTLInformation& info =
                    boost::any_cast<const ParallelISTLInformation&>( parallelInformation_);

                const size_t size = matrix_->N();
                typedef WellModelMatrixAdapter< Matrix, Vector, Vector, WellModel, true > Operator;
                Operator opA(*matrix_, *matrix_, simulator_.problem().wellModel(),
                             info );

                // As we use a dune-istl with block size np the number of components
                // per parallel is only one.
                Comm istlComm(info.communicator());
                info.copyValuesTo(istlComm.indexSet(), istlComm.remoteIndices(),size, 1);
                // Construct operator, scalar product and vectors needed.
                //constructPreconditionerAndSolve<Dune::SolverCategory::overlapping>(opA, x, *rhs_, *opA.comm(), result);
            }
            else
//#endif
            {
                Dune::Amg::SequentialInformation info;
                typedef WellModelMatrixAdapter< Matrix, Vector, Vector, WellModel, false > Operator;
                Operator opA(*matrix_, *matrix_, simulator_.problem().wellModel());
                constructPreconditionerAndSolve(opA, x, *rhs_, info, result);
            }


            checkConvergence( result );
            return result.converged;

        }





        /// Construct a system solver.
        /// \param[in] parallelInformation In the case of a parallel run
        ///                                with dune-istl the information about the parallelization.
        ISTLSolverEbos(const boost::any& parallelInformation_arg=boost::any())
            : iterations_( 0 )
            , parallelInformation_(parallelInformation_arg)
            , isIORank_(isIORank(parallelInformation_arg))
        {
            parameters_.template init<TypeTag>();
        }

        const NewtonIterationBlackoilInterleavedParameters& parameters() const
        { return parameters_; }


        /// Solve the system of linear equations Ax = b, with A being the
        /// combined derivative matrix of the residual and b
        /// being the residual itself.
        /// \param[in] residual   residual object containing A and b.
        /// \return               the solution x

        /// \copydoc NewtonIterationBlackoilInterface::iterations
        int iterations () const { return iterations_; }

        /// \copydoc NewtonIterationBlackoilInterface::parallelInformation
        const boost::any& parallelInformation() const { return parallelInformation_; }

    public:
        /// \brief construct the CPR preconditioner and the solver.
        /// \tparam P The type of the parallel information.
        /// \param parallelInformation the information about the parallelization.
#if DUNE_VERSION_NEWER(DUNE_ISTL, 2, 6)
        template<Dune::SolverCategory::Category category=Dune::SolverCategory::sequential,
                 class LinearOperator, class POrComm>
#else
        template<int category=Dune::SolverCategory::sequential, class LinearOperator, class POrComm>
#endif
        void constructPreconditionerAndSolve(LinearOperator& linearOperator,
                                             Vector& x, Vector& istlb,
                                             const POrComm& parallelInformation_arg,
                                             Dune::InverseOperatorResult& result) const
        {
            // Construct scalar product.
#if DUNE_VERSION_NEWER(DUNE_ISTL, 2, 6)
            auto sp = Dune::createScalarProduct<Vector,POrComm>(parallelInformation_arg, category);
#else
            typedef Dune::ScalarProductChooser<Vector, POrComm, category> ScalarProductChooser;
            typedef std::unique_ptr<typename ScalarProductChooser::ScalarProduct> SPPointer;
            SPPointer sp(ScalarProductChooser::construct(parallelInformation_arg));
#endif

            // Communicate if parallel.
            parallelInformation_arg.copyOwnerToAll(istlb, istlb);

#if 0 //FLOW_SUPPORT_AMG // activate AMG if either flow_ebos is used or UMFPack is not available
            if( parameters_.linear_solver_use_amg_ || parameters_.use_cpr_)
            {
                typedef ISTLUtility::CPRSelector< Matrix, Vector, Vector, POrComm>  CPRSelectorType;
                typedef typename CPRSelectorType::Operator MatrixOperator;

                std::unique_ptr< MatrixOperator > opA;

                if( ! std::is_same< LinearOperator, MatrixOperator > :: value )
                {
                    // create new operator in case linear operator and matrix operator differ
                    opA.reset( CPRSelectorType::makeOperator( linearOperator.getmat(), parallelInformation_arg ) );
                }

                const double relax = parameters_.ilu_relaxation_;
                const MILU_VARIANT ilu_milu  = parameters_.ilu_milu_;
                if (  parameters_.use_cpr_ )
                {
                    using Matrix         = typename MatrixOperator::matrix_type;
                    using CouplingMetric = Dune::Amg::Diagonal<pressureIndex>;
                    using CritBase       = Dune::Amg::SymmetricCriterion<Matrix, CouplingMetric>;
                    using Criterion      = Dune::Amg::CoarsenCriterion<CritBase>;
                    using AMG = typename ISTLUtility
                        ::BlackoilAmgSelector< Matrix, Vector, Vector,POrComm, Criterion, pressureIndex >::AMG;

                    std::unique_ptr< AMG > amg;
                    // Construct preconditioner.
                    Criterion crit(15, 2000);
                    constructAMGPrecond<Criterion>( linearOperator, parallelInformation_arg, amg, opA, relax, ilu_milu );

                    // Solve.
                    solve(linearOperator, x, istlb, *sp, *amg, result);
                }
                else
                {
                    typedef typename CPRSelectorType::AMG AMG;
                    std::unique_ptr< AMG > amg;

                    // Construct preconditioner.
                    constructAMGPrecond( linearOperator, parallelInformation_arg, amg, opA, relax, ilu_milu );

                    // Solve.
                    solve(linearOperator, x, istlb, *sp, *amg, result);
                }
            }
            else
#endif
            {
                // Construct preconditioner.
                auto precond = constructPrecond(linearOperator);

                // Solve.
                //static_assert(decltype(*precond)::category == decltype(*sp)::category, "Bla");
                solve(linearOperator, x, istlb, *sp, *precond, result);
            }
        }


	// 3x3 matrix block inversion was unstable at least 2.3 until and including
	// 2.5.0. There may still be some issue with the 4x4 matrix block inversion
	// we therefore still use the block inversion in OPM
        typedef ParallelOverlappingILU0<Dune::BCRSMatrix<Dune::MatrixBlock<typename Matrix::field_type,
                                                                           Matrix::block_type::rows,
                                                                           Matrix::block_type::cols> >,
                                        				   Vector, Vector> SeqPreconditioner;


        template <class Operator>
        std::unique_ptr<SeqPreconditioner> constructPrecond(Operator& opA) const
        {
            const double relax   = parameters_.ilu_relaxation_;
            const int ilu_fillin = parameters_.ilu_fillin_level_;
            const MILU_VARIANT ilu_milu  = parameters_.ilu_milu_;
            const bool ilu_redblack = parameters_.ilu_redblack_;
            const bool ilu_reorder_spheres = parameters_.ilu_reorder_sphere_;
            std::unique_ptr<SeqPreconditioner> precond(new SeqPreconditioner(opA.getmat(), ilu_fillin, relax, ilu_milu, ilu_redblack, ilu_reorder_spheres));
            return precond;
        }

#if HAVE_MPI
        typedef Dune::OwnerOverlapCopyCommunication<int, int> Comm;
#if DUNE_VERSION_NEWER_REV(DUNE_ISTL, 2 , 5, 1)
        // 3x3 matrix block inversion was unstable from at least 2.3 until and
        // including 2.5.0
        typedef ParallelOverlappingILU0<Matrix,Vector,Vector,Comm> ParPreconditioner;
#else
        typedef ParallelOverlappingILU0<Dune::BCRSMatrix<Dune::MatrixBlock<typename Matrix::field_type,
                                                                           Matrix::block_type::rows,
                                                                           Matrix::block_type::cols> >,
                                        Vector, Vector, Comm> ParPreconditioner;
#endif
        template <class Operator>
        std::unique_ptr<ParPreconditioner>
        constructPrecond(Operator& opA, const Comm& comm) const
        {
            typedef std::unique_ptr<ParPreconditioner> Pointer;
            const double relax  = parameters_.ilu_relaxation_;
            const MILU_VARIANT ilu_milu  = parameters_.ilu_milu_;
            const bool ilu_redblack = parameters_.ilu_redblack_;
            const bool ilu_reorder_spheres = parameters_.ilu_reorder_sphere_;
            return Pointer(new ParPreconditioner(opA.getmat(), comm, relax, ilu_milu, ilu_redblack, ilu_reorder_spheres));
        }
#endif

        template <class LinearOperator, class MatrixOperator, class POrComm, class AMG >
        void
        constructAMGPrecond(LinearOperator& /* linearOperator */, const POrComm& comm, std::unique_ptr< AMG >& amg, std::unique_ptr< MatrixOperator >& opA, const double relax, const MILU_VARIANT milu) const
        {
            ISTLUtility::template createAMGPreconditionerPointer<pressureIndex>( *opA, relax, milu, comm, amg );
        }


        template <class C, class LinearOperator, class MatrixOperator, class POrComm, class AMG >
        void
        constructAMGPrecond(LinearOperator& /* linearOperator */, const POrComm& comm, std::unique_ptr< AMG >& amg, std::unique_ptr< MatrixOperator >& opA, const double relax,
                            const MILU_VARIANT milu ) const
        {
            ISTLUtility::template createAMGPreconditionerPointer<C>( *opA, relax,
                                                                     comm, amg, parameters_ );
        }


        /// \brief Solve the system using the given preconditioner and scalar product.
        template <class Operator, class ScalarProd, class Precond>
        void solve(Operator& opA, Vector& x, Vector& istlb, ScalarProd& sp, Precond& precond, Dune::InverseOperatorResult& result) const
        {
            // TODO: Revise when linear solvers interface opm-core is done
            // Construct linear solver.
            // GMRes solver
            int verbosity = ( isIORank_ ) ? parameters_.linear_solver_verbosity_ : 0;

            if ( parameters_.newton_use_gmres_ ) {
                Dune::RestartedGMResSolver<Vector> linsolve(opA, sp, precond,
                          parameters_.linear_solver_reduction_,
                          parameters_.linear_solver_restart_,
                          parameters_.linear_solver_maxiter_,
                          verbosity);
                // Solve system.
                linsolve.apply(x, istlb, result);
            }
            else { // BiCGstab solver
                Dune::BiCGSTABSolver<Vector> linsolve(opA, sp, precond,
                          parameters_.linear_solver_reduction_,
                          parameters_.linear_solver_maxiter_,
                          verbosity);
                // Solve system.
                linsolve.apply(x, istlb, result);
            }
        }


        /// Solve the linear system Ax = b, with A being the
        /// combined derivative matrix of the residual and b
        /// being the residual itself.
        /// \param[in] A   matrix A
        /// \param[inout] x  solution to be computed x
        /// \param[in] b   right hand side b
        void solve(Matrix& A, Vector& x, Vector& b ) const
        {
            // Parallel version is deactivated until we figure out how to do it properly.
#if HAVE_MPI
            if (parallelInformation_.type() == typeid(ParallelISTLInformation))
            {
                typedef Dune::OwnerOverlapCopyCommunication<int,int> Comm;
                const ParallelISTLInformation& info =
                    boost::any_cast<const ParallelISTLInformation&>( parallelInformation_);
                Comm istlComm(info.communicator());

                // Construct operator, scalar product and vectors needed.
                typedef Dune::OverlappingSchwarzOperator<Matrix, Vector, Vector,Comm> Operator;
                Operator opA(A, istlComm);
                solve( opA, x, b, istlComm  );
            }
            else
#endif
            {
                // Construct operator, scalar product and vectors needed.
                Dune::MatrixAdapter< Matrix, Vector, Vector> opA( A );
                solve( opA, x, b );
            }
        }

        /// Solve the linear system Ax = b, with A being the
        /// combined derivative matrix of the residual and b
        /// being the residual itself.
        /// \param[in] A   matrix A
        /// \param[inout] x  solution to be computed x
        /// \param[in] b   right hand side b
        template <class Operator, class Comm >
        void solve(Operator& opA, Vector& x, Vector& b, Comm& comm) const
        {
            Dune::InverseOperatorResult result;
            // Parallel version is deactivated until we figure out how to do it properly.
#if HAVE_MPI
            if (parallelInformation_.type() == typeid(ParallelISTLInformation))
            {
                const size_t size = opA.getmat().N();
                const ParallelISTLInformation& info =
                    boost::any_cast<const ParallelISTLInformation&>( parallelInformation_);

                // As we use a dune-istl with block size np the number of components
                // per parallel is only one.
                info.copyValuesTo(comm.indexSet(), comm.remoteIndices(),
                                  size, 1);
                // Construct operator, scalar product and vectors needed.
                constructPreconditionerAndSolve<Dune::SolverCategory::overlapping>(opA, x, b, comm, result);
            }
            else
#endif
            {
                OPM_THROW(std::logic_error,"this method if for parallel solve only");
            }

            checkConvergence( result );
        }

        /// Solve the linear system Ax = b, with A being the
        /// combined derivative matrix of the residual and b
        /// being the residual itself.
        /// \param[in] A   matrix A
        /// \param[inout] x  solution to be computed x
        /// \param[in] b   right hand side b
        template <class Operator>
        void solve(Operator& opA, Vector& x, Vector& b ) const
        {
            Dune::InverseOperatorResult result;
            // Construct operator, scalar product and vectors needed.
            Dune::Amg::SequentialInformation info;
            constructPreconditionerAndSolve(opA, x, b, info, result);
            checkConvergence( result );
        }

        void checkConvergence( const Dune::InverseOperatorResult& result ) const
        {
            // store number of iterations
            iterations_ = result.iterations;

            // Check for failure of linear solver.
            if (!parameters_.ignoreConvergenceFailure_ && !result.converged) {
                const std::string msg("Convergence failure for linear solver.");
                OPM_THROW_NOLOG(LinearSolverProblem, msg);
            }
        }
    protected:
        const Simulator& simulator_;
        mutable int iterations_;
        boost::any parallelInformation_;
        bool isIORank_;
        const Matrix *matrix_;
        Vector *rhs_;

        NewtonIterationBlackoilInterleavedParameters parameters_;
    }; // end ISTLSolver

} // namespace Opm
#endif
