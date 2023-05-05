// Copyright (c) 2018-2023 Charlie Vanaret
// Licensed under the MIT license. See LICENSE file in the project directory for details.

#include "LPSubproblem.hpp"
#include "solvers/LP/LPSolverFactory.hpp"

LPSubproblem::LPSubproblem(size_t max_number_variables, size_t max_number_constraints, const Options& options) :
      ActiveSetSubproblem(max_number_variables, max_number_constraints),
      solver(LPSolverFactory::create(max_number_variables, max_number_constraints, options.get_string("LP_solver"), options)) {
}

void LPSubproblem::generate_initial_iterate(const NonlinearProblem& /*problem*/, Iterate& /*initial_iterate*/) {
}

void LPSubproblem::evaluate_functions(const NonlinearProblem& problem, Iterate& current_iterate, const WarmstartInformation& warmstart_information) {
   // objective gradient
   if (warmstart_information.objective_changed) {
      problem.evaluate_objective_gradient(current_iterate, this->evaluations.objective_gradient);
   }
   // constraints and constraint Jacobian
   if (warmstart_information.constraints_changed) {
      problem.evaluate_constraints(current_iterate, this->evaluations.constraints);
      problem.evaluate_constraint_jacobian(current_iterate, this->evaluations.constraint_jacobian);
   }
}

Direction LPSubproblem::solve(Statistics& /*statistics*/, const NonlinearProblem& problem, Iterate& current_iterate,
      const WarmstartInformation& warmstart_information) {
   //warmstart_information.display();
   // evaluate the functions at the current iterate
   this->evaluate_functions(problem, current_iterate, warmstart_information);

   // bounds of the variable displacements
   if (warmstart_information.variable_bounds_changed) {
      this->set_variable_bounds(problem, current_iterate);
      this->set_variable_displacement_bounds(problem, current_iterate);
   }

   // bounds of the linearized constraints
   if (warmstart_information.constraint_bounds_changed) {
      this->set_linearized_constraint_bounds(problem, this->evaluations.constraints);
   }
   return this->solve_LP(problem, current_iterate, warmstart_information);
}

Direction LPSubproblem::solve_LP(const NonlinearProblem& problem, Iterate& iterate, const WarmstartInformation& warmstart_information) {
   Direction direction = this->solver->solve_LP(problem.number_variables, problem.number_constraints, this->variable_displacement_bounds,
         this->linearized_constraint_bounds, this->evaluations.objective_gradient, this->evaluations.constraint_jacobian,
         this->initial_point, warmstart_information);
   Subproblem::check_unboundedness(direction);
   ActiveSetSubproblem::compute_dual_displacements(problem, iterate, direction);
   this->number_subproblems_solved++;
   return direction;
}

size_t LPSubproblem::get_hessian_evaluation_count() const {
   // no second order evaluation is used
   return 0;
}