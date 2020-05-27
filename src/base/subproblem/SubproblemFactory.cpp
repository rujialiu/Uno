#include "SubproblemFactory.hpp"
#include "SQP.hpp"
#include "SLP.hpp"
#include "Sl1QP.hpp"
//#include "SLPEQP.hpp"
#include "InteriorPoint.hpp"
#include "QPSolverFactory.hpp"

std::shared_ptr<Subproblem> SubproblemFactory::create(Problem& problem, const std::string& type, std::map<std::string, std::string> options, bool use_trust_region, bool scale_residuals) {
    /* active-set methods */
    if (type == "SQP") {
        return std::make_shared<SQP>(problem, options["QP_solver"], options["hessian"], use_trust_region, scale_residuals);
    }
    else if (type == "SLP") {
        return std::make_shared<SLP>(problem, options["QP_solver"], use_trust_region, scale_residuals);
    }
    else if (type == "Sl1QP") {
        double initial_parameter = std::stod(options["Sl1QP_initial_parameter"]);
        return std::make_shared<Sl1QP>(problem, options["QP_solver"], options["hessian"], use_trust_region, scale_residuals, initial_parameter);
    }
//    else if (type == "SLPEQP") {
//          if (use_trust_region) {
//             return std::make_shared<SLPEQP_TR>(problem, default_values["LP_solver"], default_values["hessian"], use_trust_region, scale_residuals);
//          }
//          else {
//             return std::make_shared<SLPEQP_l2>(problem, default_values["hessian"], use_trust_region, scale_residuals);
//          }
//    }
    /* interior point method */
    else if (type == "IPM") {
        return std::make_shared<InteriorPoint>(problem, options["linear_solver"], options["hessian"], use_trust_region, scale_residuals);
    }
    else {
        throw std::invalid_argument("Subproblem method " + type + " does not exist");
    }
}
