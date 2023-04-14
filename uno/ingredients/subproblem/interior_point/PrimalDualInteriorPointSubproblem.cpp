// Copyright (c) 2018-2023 Charlie Vanaret
// Licensed under the MIT license. See LICENSE file in the project directory for details.

#include <cmath>
#include "PrimalDualInteriorPointSubproblem.hpp"
#include "solvers/linear/SymmetricIndefiniteLinearSolverFactory.hpp"
#include "linear_algebra/SymmetricMatrixFactory.hpp"
#include "preprocessing/Preprocessing.hpp"
#include "tools/Infinity.hpp"

PrimalDualInteriorPointSubproblem::PrimalDualInteriorPointSubproblem(size_t max_number_variables, size_t max_number_constraints,
         size_t max_number_hessian_nonzeros, const Options& options):
      Subproblem(max_number_variables, max_number_constraints),
      augmented_system(options.get_string("sparse_format"), max_number_variables + max_number_constraints,
            max_number_hessian_nonzeros
            + max_number_variables /* diagonal barrier terms for bound constraints */
            + max_number_variables * max_number_constraints /* Jacobian (TODO: find out the number of nonzeros) */,
            true, /* use regularization */
            options),
      // the Hessian is not convexified. Instead, the augmented system will be.
      hessian_model(HessianModelFactory::create(options.get_string("hessian_model"), max_number_variables, max_number_hessian_nonzeros, false, options)),
      linear_solver(SymmetricIndefiniteLinearSolverFactory::create(options.get_string("linear_solver"), max_number_variables + max_number_constraints,
            max_number_hessian_nonzeros
            + max_number_variables + max_number_constraints /* regularization */
            + 2 * max_number_variables /* diagonal barrier terms */
            + max_number_variables * max_number_constraints /* Jacobian */)),
      barrier_parameter_update_strategy(options),
      previous_barrier_parameter(options.get_double("barrier_initial_parameter")),
      default_multiplier(options.get_double("barrier_default_multiplier")),
      parameters({
            options.get_double("barrier_tau_min"),
            options.get_double("barrier_k_sigma"),
            options.get_double("barrier_regularization_exponent"),
            options.get_double("barrier_small_direction_factor"),
            options.get_double("barrier_push_variable_to_interior_k1"),
            options.get_double("barrier_push_variable_to_interior_k2")
      }),
      least_square_multiplier_max_norm(options.get_double("least_square_multiplier_max_norm")),
      damping_factor(options.get_double("barrier_damping_factor")),
      lower_delta_z(max_number_variables), upper_delta_z(max_number_variables),
      statistics_regularization_column_order(options.get_int("statistics_regularization_column_order")),
      statistics_barrier_parameter_column_order(options.get_int("statistics_barrier_parameter_column_order")) {
}

inline void PrimalDualInteriorPointSubproblem::initialize(Statistics& statistics, const NonlinearProblem& problem, Iterate& first_iterate) {
   assert(problem.inequality_constraints.empty() && "The problem has inequality constraints. Create an instance of EqualityConstrainedModel");
   statistics.add_column("regularization", Statistics::double_width, this->statistics_regularization_column_order);
   statistics.add_column("barrier param.", Statistics::double_width, this->statistics_barrier_parameter_column_order);

   // evaluate the constraints at the original point
   first_iterate.evaluate_constraints(problem.model);

   // make the initial point strictly feasible wrt the bounds
   for (size_t i: Range(problem.number_variables)) {
      const Interval bounds = {problem.get_variable_lower_bound(i), problem.get_variable_upper_bound(i)};
      first_iterate.primals[i] = PrimalDualInteriorPointSubproblem::push_variable_to_interior(first_iterate.primals[i], bounds);
   }

   // set the slack variables (if any)
   if (not problem.model.slacks.empty()) {
      // set the slacks to the constraint values
      problem.model.slacks.for_each([&](size_t j, size_t slack_index) {
         const Interval bounds = {problem.get_variable_lower_bound(slack_index), problem.get_variable_upper_bound(slack_index)};
         first_iterate.primals[slack_index] = PrimalDualInteriorPointSubproblem::push_variable_to_interior(first_iterate.evaluations.constraints[j], bounds);
      });
   }
   first_iterate.is_objective_gradient_computed = false;
   first_iterate.are_constraints_computed = false;
   first_iterate.is_constraint_jacobian_computed = false;

   // set the bound multipliers
   for (size_t i: problem.lower_bounded_variables) {
      first_iterate.multipliers.lower_bounds[i] = this->default_multiplier;
   }
   for (size_t i: problem.upper_bounded_variables) {
      first_iterate.multipliers.upper_bounds[i] = -this->default_multiplier;
   }

   // compute least-square multipliers
   if (problem.is_constrained()) {
      this->compute_least_square_multipliers(problem, first_iterate);
   }
}

double PrimalDualInteriorPointSubproblem::barrier_parameter() const {
   return this->barrier_parameter_update_strategy.get_barrier_parameter();
}

/*
void InfeasibleInteriorPointSubproblem::check_interior_primals(const NonlinearProblem& problem, const Iterate& iterate) {
   static double machine_epsilon = std::numeric_limits<double>::epsilon();
   const double factor = std::pow(machine_epsilon, 0.75);
   // check that the current iterate is interior
   for (size_t i: problem.lower_bounded_variables) {
      if (iterate.primals[i] - this->variable_bounds[i].lb < machine_epsilon*this->barrier_parameter()) {
         this->variable_bounds[i].lb -= factor * std::max(1., this->variable_bounds[i].lb);
      }
      //assert(this->variable_bounds[i].lb < iterate.primals[i] && "Barrier subproblem: a variable is at its lower bound");
   }
   for (size_t i: problem.upper_bounded_variables) {
      if (this->variable_bounds[i].ub - iterate.primals[i] < machine_epsilon*this->barrier_parameter()) {
         this->variable_bounds[i].ub += factor * std::max(1., this->variable_bounds[i].ub);
      }
      //assert(iterate.primals[i] < this->variable_bounds[i].ub && "Barrier subproblem: a variable is at its upper bound");
   }
}
*/

double PrimalDualInteriorPointSubproblem::push_variable_to_interior(double variable_value, const Interval& variable_bounds) const {
   const double range = variable_bounds.ub - variable_bounds.lb;
   const double perturbation_lb = std::min(this->parameters.push_variable_to_interior_k1 * std::max(1., std::abs(variable_bounds.lb)),
         this->parameters.push_variable_to_interior_k2 * range);
   const double perturbation_ub = std::min(this->parameters.push_variable_to_interior_k1 * std::max(1., std::abs(variable_bounds.ub)),
         this->parameters.push_variable_to_interior_k2 * range);
   variable_value = std::max(variable_value, variable_bounds.lb + perturbation_lb);
   variable_value = std::min(variable_value, variable_bounds.ub - perturbation_ub);
   return variable_value;
}

void PrimalDualInteriorPointSubproblem::evaluate_functions(Statistics& statistics, const NonlinearProblem& problem, Iterate& current_iterate) {
   // original Hessian and barrier objective gradient
   this->hessian_model->evaluate(statistics, problem, current_iterate.primals, current_iterate.multipliers.constraints);
   problem.evaluate_objective_gradient(current_iterate, this->evaluations.objective_gradient);

   for (size_t i: Range(problem.number_variables)) {
      // Hessian: diagonal barrier terms (grouped by variable)
      double hessian_diagonal_barrier_term = 0.;
      // objective gradient
      double objective_barrier_term = 0.;
      // TODO urgent: use the correct bounds (if TR, all the original variables are bounded)
      if (is_finite(problem.get_variable_lower_bound(i))) { // lower bounded
         const double inverse_distance = 1. / (current_iterate.primals[i] - problem.get_variable_lower_bound(i));
         hessian_diagonal_barrier_term += current_iterate.multipliers.lower_bounds[i] * inverse_distance;
         objective_barrier_term += -this->barrier_parameter() * inverse_distance;
         // damping
         if (not is_finite(problem.get_variable_upper_bound(i))) {
            objective_barrier_term += this->damping_factor*this->barrier_parameter();
         }
      }
      if (is_finite(problem.get_variable_upper_bound(i))) { // upper bounded
         const double inverse_distance = 1. / (current_iterate.primals[i] - problem.get_variable_upper_bound(i));
         hessian_diagonal_barrier_term += current_iterate.multipliers.upper_bounds[i] * inverse_distance;
         objective_barrier_term += -this->barrier_parameter() * inverse_distance;
         // damping
         if (not is_finite(problem.get_variable_lower_bound(i))) {
            objective_barrier_term -= this->damping_factor*this->barrier_parameter();
         }
      }
      this->hessian_model->hessian->insert(hessian_diagonal_barrier_term, i, i);
      this->evaluations.objective_gradient.insert(i, objective_barrier_term);
   }
   // TODO: the allocated size for objective_gradient is probably too small

   // constraints
   problem.evaluate_constraints(current_iterate, this->evaluations.constraints);

   // constraint Jacobian
   problem.evaluate_constraint_jacobian(current_iterate, this->evaluations.constraint_jacobian);
}

Direction PrimalDualInteriorPointSubproblem::solve(Statistics& statistics, const NonlinearProblem& problem, Iterate& current_iterate) {
   assert(problem.inequality_constraints.empty() && "The problem has inequality constraints. Create an instance of EqualityConstrainedModel");

   // update the barrier parameter if the current iterate solves the subproblem
   this->update_barrier_parameter(problem, current_iterate);

   this->relax_variable_bounds(problem, current_iterate);

   //this->check_interior_primals(problem, current_iterate);

   // evaluate the functions at the current iterate
   this->evaluate_functions(statistics, problem, current_iterate);

   // set up the augmented system (with the correct inertia)
   this->assemble_augmented_system(statistics, problem, current_iterate);

   // compute the solution (Δx, -Δλ)
   this->augmented_system.solve(*this->linear_solver);
   Subproblem::check_unboundedness(this->direction);
   assert(this->direction.status == SubproblemStatus::OPTIMAL && "The barrier subproblem was not solved to optimality");
   this->number_subproblems_solved++;
   this->generate_primal_dual_direction(problem, current_iterate);
   statistics.add_statistic("barrier param.", this->barrier_parameter());

   // determine if the direction is a "small direction" (Section 3.9 of the Ipopt paper) TODO
   const bool is_small_step = PrimalDualInteriorPointSubproblem::is_small_step(problem, current_iterate, this->direction);
   if (is_small_step) {
      DEBUG << "This is a small step\n";
   }
   return this->direction;
}

void PrimalDualInteriorPointSubproblem::relax_variable_bounds(const NonlinearProblem& problem, const Iterate& current_iterate) {
   // slightly relax the bounds whenever the current point is too close to the bounds (Section 3.5 in IPOPT paper)
   static double machine_epsilon = std::numeric_limits<double>::epsilon();
   // const double factor = std::pow(machine_epsilon, 0.75);
   for (size_t i: problem.lower_bounded_variables) {
      if (current_iterate.primals[i] - problem.get_variable_lower_bound(i) < machine_epsilon*this->barrier_parameter()) {
      }
   }
   for (size_t i: problem.upper_bounded_variables) {
      if (problem.get_variable_upper_bound(i) - current_iterate.primals[i] < machine_epsilon*this->barrier_parameter()) {
      }
   }
}

void PrimalDualInteriorPointSubproblem::assemble_augmented_system(Statistics& statistics, const NonlinearProblem& problem,
      const Iterate& current_iterate) {
   // assemble, factorize and regularize the augmented matrix
   this->augmented_system.assemble_matrix(*this->hessian_model->hessian, this->evaluations.constraint_jacobian,
         problem.number_variables, problem.number_constraints);
   this->augmented_system.factorize_matrix(problem.model, *this->linear_solver);
   const double dual_regularization_parameter = std::pow(this->barrier_parameter(), this->parameters.regularization_exponent);
   this->augmented_system.regularize_matrix(statistics, problem.model, *this->linear_solver, problem.number_variables, problem.number_constraints,
         dual_regularization_parameter);
   [[maybe_unused]] auto[number_pos_eigenvalues, number_neg_eigenvalues, number_zero_eigenvalues] = this->linear_solver->get_inertia();
   assert(number_pos_eigenvalues == problem.number_variables && number_neg_eigenvalues == problem.number_constraints && number_zero_eigenvalues == 0);

   // assemble the right-hand side
   this->generate_augmented_rhs(problem, current_iterate);
}

Direction PrimalDualInteriorPointSubproblem::compute_second_order_correction(const NonlinearProblem& problem, Iterate& trial_iterate,
      double primal_step_length) {
   DEBUG << "\nEntered SOC computation\n";

   // scale the current constraint values with the primal step length
   for (size_t j: Range(problem.number_constraints)) {
      this->augmented_system.rhs[problem.number_variables + j] *= primal_step_length;
   }

   // shift the RHS with the values of the constraints at the trial iterate
   problem.evaluate_constraints(trial_iterate, this->evaluations.constraints);
   for (size_t j: Range(problem.number_constraints)) {
      this->augmented_system.rhs[problem.number_variables + j] -= this->evaluations.constraints[j];
   }
   DEBUG << "SOC RHS: "; print_vector(DEBUG, this->augmented_system.rhs, 0, problem.number_variables + problem.number_constraints);

   // compute the solution (Δx, -Δλ)
   this->augmented_system.solve(*this->linear_solver);
   Subproblem::check_unboundedness(this->direction);
   this->number_subproblems_solved++;
   this->generate_primal_dual_direction(problem, trial_iterate);
   return this->direction;
}

void PrimalDualInteriorPointSubproblem::initialize_feasibility_problem() {
   // if we're building the feasibility subproblem, temporarily update the objective multiplier
   this->solving_feasibility_problem = true;
   this->previous_barrier_parameter = this->barrier_parameter();
   const double new_barrier_parameter = std::max(this->barrier_parameter(), norm_inf(this->evaluations.constraints));
   this->barrier_parameter_update_strategy.set_barrier_parameter(new_barrier_parameter);
   DEBUG << "Barrier parameter mu temporarily updated to " << this->barrier_parameter() << '\n';
   this->subproblem_definition_changed = true;
}

// set the elastic variables of the current iterate
void PrimalDualInteriorPointSubproblem::set_elastic_variable_values(const l1RelaxedProblem& problem, Iterate& current_iterate) {
   DEBUG << "Setting the elastic variables\n";
   // c(x) - p + n = 0
   // analytical expression for p and n:
   // (mu_over_rho - jacobian_coefficient*this->barrier_constraints[j] + std::sqrt(radical))/2.
   // where jacobian_coefficient = -1 for p, +1 for n
   // Note: IPOPT uses a '+' sign because they define the Lagrangian as f(x) + \lambda^T c(x)
   const double barrier_parameter = this->barrier_parameter();
   const auto elastic_setting_function = [&](Iterate& iterate, size_t j, size_t elastic_index, double jacobian_coefficient) {
      // precomputations
      const double constraint_j = this->evaluations.constraints[j];
      const double mu_over_rho = barrier_parameter; // here, rho = 1
      const double radical = std::pow(constraint_j, 2) + std::pow(mu_over_rho, 2);
      const double sqrt_radical = std::sqrt(radical);

      iterate.primals[elastic_index] = (mu_over_rho - jacobian_coefficient * constraint_j + sqrt_radical) / 2.;
      iterate.multipliers.lower_bounds[elastic_index] = barrier_parameter / iterate.primals[elastic_index];
      assert(0. < iterate.primals[elastic_index] && "The elastic variable is not strictly positive.");
      assert(0. < iterate.multipliers.lower_bounds[elastic_index] && "The elastic dual is not strictly positive.");
   };
   problem.set_elastic_variable_values(current_iterate, elastic_setting_function);
}

void PrimalDualInteriorPointSubproblem::exit_feasibility_problem(const NonlinearProblem& problem, Iterate& trial_iterate) {
   assert(this->solving_feasibility_problem && "The barrier subproblem did not know it was solving the feasibility problem.");
   this->barrier_parameter_update_strategy.set_barrier_parameter(this->previous_barrier_parameter);
   this->solving_feasibility_problem = false;
   this->compute_least_square_multipliers(problem, trial_iterate);
}

void PrimalDualInteriorPointSubproblem::set_auxiliary_measure(const NonlinearProblem& problem, Iterate& iterate) {
   //this->check_interior_primals(problem, iterate);
   // auxiliary measure: barrier terms
   double barrier_terms = 0.;
   for (size_t i: problem.lower_bounded_variables) {
      barrier_terms -= std::log(iterate.primals[i] - problem.get_variable_lower_bound(i));
   }
   for (size_t i: problem.upper_bounded_variables) {
      barrier_terms -= std::log(problem.get_variable_upper_bound(i) - iterate.primals[i]);
   }
   // damping
   for (size_t i: problem.single_lower_bounded_variables) {
      barrier_terms += this->damping_factor*(iterate.primals[i] - problem.get_variable_lower_bound(i));
   }
   for (size_t i: problem.single_upper_bounded_variables) {
      barrier_terms += this->damping_factor*(problem.get_variable_upper_bound(i) - iterate.primals[i]);
   }
   barrier_terms *= this->barrier_parameter();
   assert(not std::isnan(barrier_terms) && "The optimality measure is not an number.");
   iterate.progress.auxiliary_terms = barrier_terms;
}

double PrimalDualInteriorPointSubproblem::generate_predicted_auxiliary_reduction_model(const NonlinearProblem& problem,
      const Iterate& current_iterate, const Direction& direction, double step_length) const {
   const double directional_derivative = this->compute_barrier_term_directional_derivative(problem, current_iterate, direction);
   return step_length * (-directional_derivative);
   // }, "α*(μ*X^{-1} e^T d)"};
}

double PrimalDualInteriorPointSubproblem::compute_barrier_term_directional_derivative(const NonlinearProblem& problem, const Iterate& current_iterate,
      const Direction& direction) const {
   double directional_derivative = 0.;
   for (size_t i: problem.lower_bounded_variables) {
      directional_derivative += -this->barrier_parameter() / (current_iterate.primals[i] - problem.get_variable_lower_bound(i))*direction.primals[i];
   }
   for (size_t i: problem.upper_bounded_variables) {
      directional_derivative += -this->barrier_parameter() / (current_iterate.primals[i] - problem.get_variable_upper_bound(i))*direction.primals[i];
   }
   // damping
   for (size_t i: problem.single_lower_bounded_variables) {
      directional_derivative += this->damping_factor*this->barrier_parameter()*direction.primals[i];
   }
   for (size_t i: problem.single_upper_bounded_variables) {
      directional_derivative -= this->damping_factor*this->barrier_parameter()*direction.primals[i];
   }
   return directional_derivative;
}

void PrimalDualInteriorPointSubproblem::update_barrier_parameter(const NonlinearProblem& problem, const Iterate& current_iterate) {
    const bool barrier_parameter_updated = this->barrier_parameter_update_strategy.update_barrier_parameter(problem, current_iterate);
    // the barrier parameter may have been changed earlier when entering restoration
    this->subproblem_definition_changed = this->subproblem_definition_changed || barrier_parameter_updated;
}

// Section 3.9 in IPOPT paper
bool PrimalDualInteriorPointSubproblem::is_small_step(const NonlinearProblem& problem, const Iterate& current_iterate, const Direction& direction) const {
   const auto relative_measure_function = [&](size_t i) {
      return direction.primals[i] / (1 + std::abs(current_iterate.primals[i]));
   };
   static double machine_epsilon = std::numeric_limits<double>::epsilon();
   return (norm_inf<double>(relative_measure_function, Range(problem.number_variables)) < this->parameters.small_direction_factor * machine_epsilon);
}

double PrimalDualInteriorPointSubproblem::evaluate_subproblem_objective() const {
   const double linear_term = dot(this->direction.primals, this->evaluations.objective_gradient);
   const double quadratic_term = this->hessian_model->hessian->quadratic_product(this->direction.primals, this->direction.primals) / 2.;
   return linear_term + quadratic_term;
}

double PrimalDualInteriorPointSubproblem::primal_fraction_to_boundary(const NonlinearProblem& problem, const Iterate& current_iterate, double tau) {
   double primal_length = 1.;
   for (size_t i: problem.lower_bounded_variables) {
      if (this->augmented_system.solution[i] < 0.) {
         double trial_alpha_xi = -tau * (current_iterate.primals[i] - problem.get_variable_lower_bound(i)) / this->augmented_system.solution[i];
         if (0. < trial_alpha_xi) {
            primal_length = std::min(primal_length, trial_alpha_xi);
         }
      }
   }
   for (size_t i: problem.upper_bounded_variables) {
      if (0. < this->augmented_system.solution[i]) {
         double trial_alpha_xi = -tau * (current_iterate.primals[i] - problem.get_variable_upper_bound(i)) / this->augmented_system.solution[i];
         if (0. < trial_alpha_xi) {
            primal_length = std::min(primal_length, trial_alpha_xi);
         }
      }
   }
   assert(0. < primal_length && primal_length <= 1. && "The primal fraction-to-boundary factor is not in (0, 1]");
   return primal_length;
}

double PrimalDualInteriorPointSubproblem::dual_fraction_to_boundary(const NonlinearProblem& problem, const Iterate& current_iterate, double tau) {
   double dual_length = 1.;
   for (size_t i: problem.lower_bounded_variables) {
      if (this->lower_delta_z[i] < 0.) {
         double trial_alpha_zj = -tau * current_iterate.multipliers.lower_bounds[i] / this->lower_delta_z[i];
         if (0. < trial_alpha_zj) {
            dual_length = std::min(dual_length, trial_alpha_zj);
         }
      }
   }
   for (size_t i: problem.upper_bounded_variables) {
      if (0. < this->upper_delta_z[i]) {
         double trial_alpha_zj = -tau * current_iterate.multipliers.upper_bounds[i] / this->upper_delta_z[i];
         if (0. < trial_alpha_zj) {
            dual_length = std::min(dual_length, trial_alpha_zj);
         }
      }
   }
   assert(0. < dual_length && dual_length <= 1. && "The dual fraction-to-boundary factor is not in (0, 1]");
   return dual_length;
}

// generate the right-hand side
void PrimalDualInteriorPointSubproblem::generate_augmented_rhs(const NonlinearProblem& problem, const Iterate& current_iterate) {
   initialize_vector(this->augmented_system.rhs, 0.);

   // objective gradient
   this->evaluations.objective_gradient.for_each([&](size_t i, double derivative) {
      this->augmented_system.rhs[i] -= derivative;
   });

   // constraint: evaluations and gradients
   for (size_t j: Range(problem.number_constraints)) {
      // Lagrangian
      if (current_iterate.multipliers.constraints[j] != 0.) {
         this->evaluations.constraint_jacobian[j].for_each([&](size_t i, double derivative) {
            this->augmented_system.rhs[i] += current_iterate.multipliers.constraints[j] * derivative;
         });
      }
      // constraints
      this->augmented_system.rhs[problem.number_variables + j] = -this->evaluations.constraints[j];
   }
   DEBUG << "RHS: "; print_vector(DEBUG, this->augmented_system.rhs, 0, problem.number_variables + problem.number_constraints); DEBUG << '\n';
}

void PrimalDualInteriorPointSubproblem::generate_primal_dual_direction(const NonlinearProblem& problem, const Iterate& current_iterate) {
   this->direction.set_dimensions(problem.number_variables, problem.number_constraints);

   // retrieve +Δλ (Nocedal p590)
   for (size_t j: Range(problem.number_variables, this->augmented_system.solution.size())) {
      this->augmented_system.solution[j] = -this->augmented_system.solution[j];
   }

   // "fraction-to-boundary" rule for primal variables and constraints multipliers
   const double tau = std::max(this->parameters.tau_min, 1. - this->barrier_parameter());
   const double primal_dual_step_length = this->primal_fraction_to_boundary(problem, current_iterate, tau);
   for (size_t i: Range(problem.number_variables)) {
      this->direction.primals[i] = this->augmented_system.solution[i];
   }
   for (size_t j: Range(problem.number_constraints)) {
      this->direction.multipliers.constraints[j] = this->augmented_system.solution[problem.number_variables + j];
   }

   // compute bound multiplier direction Δz
   this->compute_bound_dual_direction(problem, current_iterate);

   // "fraction-to-boundary" rule for bound multipliers
   const double bound_dual_step_length = this->dual_fraction_to_boundary(problem, current_iterate, tau);
   for (size_t i: Range(problem.number_variables)) {
      this->direction.multipliers.lower_bounds[i] = this->lower_delta_z[i];
      this->direction.multipliers.upper_bounds[i] = this->upper_delta_z[i];
   }
   DEBUG << "primal-dual length = " << primal_dual_step_length << '\n';
   DEBUG << "bound dual length = " << bound_dual_step_length << '\n';

   this->direction.primal_dual_step_length = primal_dual_step_length;
   this->direction.bound_dual_step_length = bound_dual_step_length;
   this->direction.subproblem_objective = this->evaluate_subproblem_objective();
}

void PrimalDualInteriorPointSubproblem::compute_bound_dual_direction(const NonlinearProblem& problem, const Iterate& current_iterate) {
   initialize_vector(this->lower_delta_z, 0.);
   initialize_vector(this->upper_delta_z, 0.);
   for (size_t i: problem.lower_bounded_variables) {
      const double distance_to_bound = current_iterate.primals[i] - problem.get_variable_lower_bound(i);
      this->lower_delta_z[i] = (this->barrier_parameter() - this->augmented_system.solution[i] * current_iterate.multipliers.lower_bounds[i]) /
                               distance_to_bound - current_iterate.multipliers.lower_bounds[i];
      assert(is_finite(this->lower_delta_z[i]) && "The displacement lower_delta_z is infinite");
   }
   for (size_t i: problem.upper_bounded_variables) {
      const double distance_to_bound = current_iterate.primals[i] - problem.get_variable_upper_bound(i);
      this->upper_delta_z[i] = (this->barrier_parameter() - this->augmented_system.solution[i] * current_iterate.multipliers.upper_bounds[i]) /
                               distance_to_bound - current_iterate.multipliers.upper_bounds[i];
      assert(is_finite(this->upper_delta_z[i]) && "The displacement upper_delta_z is infinite");
   }
}

void PrimalDualInteriorPointSubproblem::compute_least_square_multipliers(const NonlinearProblem& problem, Iterate& iterate) {
   this->augmented_system.matrix->dimension = problem.number_variables + problem.number_constraints;
   this->augmented_system.matrix->reset();
   Preprocessing::compute_least_square_multipliers(problem.model, *this->augmented_system.matrix, this->augmented_system.rhs, *this->linear_solver,
         iterate, iterate.multipliers.constraints, this->least_square_multiplier_max_norm);
}

void PrimalDualInteriorPointSubproblem::postprocess_iterate(const NonlinearProblem& problem, Iterate& iterate) {
   // rescale the bound multipliers (Eq. 16 in Ipopt paper)
   for (size_t i: problem.lower_bounded_variables) {
      const double coefficient = this->barrier_parameter() / (iterate.primals[i] - problem.get_variable_lower_bound(i));
      const double lb = coefficient / this->parameters.k_sigma;
      const double ub = coefficient * this->parameters.k_sigma;
      if (lb <= ub) {
         const double current_value = iterate.multipliers.lower_bounds[i];
         iterate.multipliers.lower_bounds[i] = std::max(std::min(iterate.multipliers.lower_bounds[i], ub), lb);
         if (iterate.multipliers.lower_bounds[i] != current_value) {
            DEBUG << "Multiplier for lower bound " << i << " rescaled from " << current_value << " to " << iterate.multipliers.lower_bounds[i] << '\n';
         }
      }
      else {
         WARNING << YELLOW << "Barrier subproblem: the bounds are in the wrong order in the lower bound multiplier reset" << RESET << '\n';
      }
   }
   for (size_t i: problem.upper_bounded_variables) {
      const double coefficient = this->barrier_parameter() / (iterate.primals[i] - problem.get_variable_upper_bound(i));
      const double lb = coefficient * this->parameters.k_sigma;
      const double ub = coefficient / this->parameters.k_sigma;
      if (lb <= ub) {
         const double current_value = iterate.multipliers.upper_bounds[i];
         iterate.multipliers.upper_bounds[i] = std::max(std::min(iterate.multipliers.upper_bounds[i], ub), lb);
         if (iterate.multipliers.upper_bounds[i] != current_value) {
            DEBUG << "Multiplier for upper bound " << i << " rescaled from " << current_value << " to " << iterate.multipliers.upper_bounds[i] << '\n';
         }
      }
      else {
         WARNING << YELLOW << "Barrier subproblem: the bounds are in the wrong order in the upper bound multiplier reset" << RESET << '\n';
      }
   }
}

size_t PrimalDualInteriorPointSubproblem::get_hessian_evaluation_count() const {
   return this->hessian_model->evaluation_count;
}

void PrimalDualInteriorPointSubproblem::set_initial_point(const std::vector<double>& /*initial_point*/) {
   // do nothing
}
