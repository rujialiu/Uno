// Copyright (c) 2018-2024 Charlie Vanaret
// Licensed under the MIT license. See LICENSE file in the project directory for details.

#include "InequalityConstrainedMethod.hpp"
#include "ingredients/subproblem/Direction.hpp"
#include "linear_algebra/Vector.hpp"
#include "reformulation/l1RelaxedProblem.hpp"
#include "tools/Options.hpp"

InequalityConstrainedMethod::InequalityConstrainedMethod(size_t max_number_variables, size_t max_number_constraints):
      Subproblem(max_number_variables, max_number_constraints),
      initial_point(max_number_variables),
      direction_lower_bounds(max_number_variables),
      direction_upper_bounds(max_number_variables),
      linearized_constraints_lower_bounds(max_number_constraints),
      linearized_constraints_upper_bounds(max_number_constraints) {
}

void InequalityConstrainedMethod::initialize_statistics(Statistics& /*statistics*/, const Options& /*options*/) {
}

void InequalityConstrainedMethod::set_initial_point(const std::vector<double>& point) {
   copy_from(this->initial_point, point);
}

void InequalityConstrainedMethod::initialize_feasibility_problem(const l1RelaxedProblem& /*problem*/, Iterate& /*current_iterate*/) {
   // do nothing
}

void InequalityConstrainedMethod::set_elastic_variable_values(const l1RelaxedProblem& problem, Iterate& current_iterate) {
   problem.set_elastic_variable_values(current_iterate, [&](Iterate& iterate, size_t /*j*/, size_t elastic_index, double /*jacobian_coefficient*/) {
      iterate.primals[elastic_index] = 0.;
      iterate.multipliers.lower_bounds[elastic_index] = 1.;
   });
}

void InequalityConstrainedMethod::exit_feasibility_problem(const OptimizationProblem& /*problem*/, Iterate& /*trial_iterate*/) {
   // do nothing
}

void InequalityConstrainedMethod::set_direction_bounds(const OptimizationProblem& problem, const Iterate& current_iterate) {
   // bounds of original variables intersected with trust region
   for (size_t variable_index: Range(problem.get_number_original_variables())) {
      this->direction_lower_bounds[variable_index] = std::max(-this->trust_region_radius,
            problem.variable_lower_bound(variable_index) - current_iterate.primals[variable_index]);
      this->direction_upper_bounds[variable_index] = std::min(this->trust_region_radius,
            problem.variable_upper_bound(variable_index) - current_iterate.primals[variable_index]);
   }
   // bounds of additional variables (no trust region!)
   for (size_t variable_index: Range(problem.get_number_original_variables(), problem.number_variables)) {
      this->direction_lower_bounds[variable_index] = problem.variable_lower_bound(variable_index) - current_iterate.primals[variable_index];
      this->direction_upper_bounds[variable_index] = problem.variable_upper_bound(variable_index) - current_iterate.primals[variable_index];
   }
}

void InequalityConstrainedMethod::set_linearized_constraint_bounds(const OptimizationProblem& problem, const std::vector<double>& current_constraints) {
   for (size_t constraint_index: Range(problem.number_constraints)) {
      this->linearized_constraints_lower_bounds[constraint_index] = problem.constraint_lower_bound(constraint_index) -
            current_constraints[constraint_index];
      this->linearized_constraints_upper_bounds[constraint_index] = problem.constraint_upper_bound(constraint_index) -
            current_constraints[constraint_index];
   }
}

void InequalityConstrainedMethod::compute_dual_displacements(const OptimizationProblem& problem, const Iterate& current_iterate, Direction& direction) {
   // compute dual *displacements* (note: active-set methods usually compute the new duals, not the displacements)
   for (size_t constraint_index: Range(problem.number_constraints)) {
      direction.multipliers.constraints[constraint_index] -= current_iterate.multipliers.constraints[constraint_index];
   }
   for (size_t variable_index: Range(problem.number_variables)) {
      direction.multipliers.lower_bounds[variable_index] -= current_iterate.multipliers.lower_bounds[variable_index];
      direction.multipliers.upper_bounds[variable_index] -= current_iterate.multipliers.upper_bounds[variable_index];
   }
}

// auxiliary measure is 0 in inequality-constrained methods
void InequalityConstrainedMethod::set_auxiliary_measure(const Model& /*model*/, Iterate& iterate) {
   iterate.progress.auxiliary = 0.;
}

double InequalityConstrainedMethod::compute_predicted_auxiliary_reduction_model(const Model& /*model*/, const Iterate& /*current_iterate*/,
      const Direction& /*direction*/, double /*step_length*/) const {
   return 0.;
}

void InequalityConstrainedMethod::postprocess_iterate(const OptimizationProblem& /*problem*/, Iterate& /*iterate*/) {
}
