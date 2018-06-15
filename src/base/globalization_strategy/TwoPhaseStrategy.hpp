#ifndef TWOPHASESTRATEGY_H
#define TWOPHASESTRATEGY_H

#include "GlobalizationStrategy.hpp"

/*! \class TwoPhaseConstants
* \brief Constants for filter and tube strategies
*
*  Set of constants to control the filter and tube strategies
*/
struct TwoPhaseConstants {
	double Sigma; /*!< Sufficient reduction constant */
	double Delta; /*!< Switching constant */
};

/*! \class GlobalizationStrategy
* \brief Step acceptance strategy
*
*  Strategy that accepts or declines a trial step (virtual class)
*/
class TwoPhaseStrategy: public GlobalizationStrategy {
	public:
		/*!
         *  Constructor that takes an optimization problem and a set of constants
         * 
         * \param problem: optimization problem
         * \param constants: set of constants
         */
		TwoPhaseStrategy(LocalApproximation& local_approximation, TwoPhaseConstants& constants, double tolerance);
		
		/*!
         *  TwoPhaseStrategy implements the compute_step method
         */
		LocalSolution compute_step(Problem& problem, Iterate& current_iterate, double radius);
		
		virtual bool check_step(Problem& problem, Iterate& current_iterate, LocalSolution& solution, double step_length = 1.) = 0;
		
		virtual void initialize(Problem& problem, Iterate& current_iterate) = 0;
		
		double compute_KKT_error(Problem& problem, Iterate& current_iterate);
		
		Phase phase; /*!< Current phase (optimality or feasibility restoration) */
		TwoPhaseConstants constants; /*!< Set of constants */
	
	protected:
		void update_restoration_multipliers(Iterate& trial_iterate, ConstraintPartition& constraint_partition);
};

#endif // TWOPHASESTRATEGY_H
