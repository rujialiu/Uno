// Copyright (c) 2018-2023 Charlie Vanaret
// Licensed under the MIT license. See LICENSE file in the project directory for details.

#ifndef UNO_NONLINEARPROBLEM_H
#define UNO_NONLINEARPROBLEM_H

#include <vector>
#include "optimization/Iterate.hpp"
#include "optimization/Model.hpp"
#include "linear_algebra/SparseVector.hpp"
#include "linear_algebra/Vector.hpp"
#include "linear_algebra/RectangularMatrix.hpp"
#include "ingredients/subproblem/Direction.hpp"
#include "tools/Range.hpp"

class NonlinearProblem {
public:
   NonlinearProblem(const Model& model, size_t number_variables, size_t number_constraints);
   virtual ~NonlinearProblem() = default;

   const Model& model;
   const size_t number_variables; /*!< Number of variables */
   const size_t number_constraints; /*!< Number of constraints */

   [[nodiscard]] bool is_constrained() const;

   // SparseVector<size_t> equality_constraints{}; /*!< inequality constraints */
   std::vector<size_t> equality_constraints{}; /*!< inequality constraints */
   std::vector<size_t> inequality_constraints{}; /*!< inequality constraints */
   // lists of bounded variables
   std::vector<size_t> lower_bounded_variables{}; // indices of the lower-bounded variables
   std::vector<size_t> upper_bounded_variables{}; // indices of the upper-bounded variables
   std::vector<size_t> single_lower_bounded_variables{}; // indices of the single lower-bounded variables
   std::vector<size_t> single_upper_bounded_variables{}; // indices of the single upper-bounded variables

   // function evaluations
   [[nodiscard]] virtual double get_objective_multiplier() const = 0;
   [[nodiscard]] virtual double evaluate_objective(Iterate& iterate) const = 0;
   virtual void evaluate_objective_gradient(Iterate& iterate, SparseVector<double>& objective_gradient) const = 0;
   virtual void evaluate_constraints(Iterate& iterate, std::vector<double>& constraints) const = 0;
   virtual void evaluate_constraint_jacobian(Iterate& iterate, RectangularMatrix<double>& constraint_jacobian) const = 0;
   virtual void evaluate_lagrangian_hessian(const std::vector<double>& x, const std::vector<double>& multipliers, SymmetricMatrix<double>& hessian) const = 0;

   virtual void set_infeasibility_measure(Iterate& iterate, Norm progress_norm) const = 0;
   virtual void set_optimality_measure(Iterate& iterate) const = 0;
   [[nodiscard]] virtual double compute_predicted_infeasibility_reduction_model(const Iterate& current_iterate, const Direction& direction,
         double step_length, Norm progress_norm) const = 0;
   [[nodiscard]] virtual std::function<double(double)> compute_predicted_optimality_reduction_model(const Iterate& current_iterate,
         const Direction& direction, double step_length) const = 0;

   [[nodiscard]] static double compute_linearized_constraint_violation(const Model& model, const Iterate& current_iterate,
         const Direction& direction, double step_length);

   [[nodiscard]] size_t get_number_original_variables() const;
   [[nodiscard]] virtual double get_variable_lower_bound(size_t i) const = 0;
   [[nodiscard]] virtual double get_variable_upper_bound(size_t i) const = 0;
   [[nodiscard]] virtual double get_constraint_lower_bound(size_t j) const = 0;
   [[nodiscard]] virtual double get_constraint_upper_bound(size_t j) const = 0;

   [[nodiscard]] virtual size_t get_number_objective_gradient_nonzeros() const = 0;
   [[nodiscard]] virtual size_t get_number_jacobian_nonzeros() const = 0;
   [[nodiscard]] virtual size_t get_number_hessian_nonzeros() const = 0;
};

inline NonlinearProblem::NonlinearProblem(const Model& model, size_t number_variables, size_t number_constraints):
      model(model), number_variables(number_variables), number_constraints(number_constraints) {
   this->equality_constraints.reserve(this->number_constraints);
   this->inequality_constraints.reserve(this->number_constraints);
   this->lower_bounded_variables.reserve(this->number_variables);
   this->upper_bounded_variables.reserve(this->number_variables);
   this->single_lower_bounded_variables.reserve(this->number_variables);
   this->single_upper_bounded_variables.reserve(this->number_variables);
}

inline bool NonlinearProblem::is_constrained() const {
   return (0 < this->number_constraints);
}

inline size_t NonlinearProblem::get_number_original_variables() const {
   return this->model.number_variables;
}

inline double NonlinearProblem::compute_linearized_constraint_violation(const Model& model, const Iterate& current_iterate,
      const Direction& direction, double step_length) {
   // determine the linearized constraint violation term: c(x_k) + alpha*\nabla c(x_k)^T d
   const auto jth_component = [&](size_t j) {
      const double component_j = current_iterate.evaluations.constraints[j] + step_length * dot(direction.primals,
            current_iterate.evaluations.constraint_jacobian[j]);
      return model.compute_constraint_violation(component_j, j);
   };

   return norm_1<double>(jth_component, Range(model.number_constraints));
}

#endif // UNO_NONLINEARPROBLEM_H
