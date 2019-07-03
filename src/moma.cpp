// -*- mode: C++; c-indent-level: 4; c-basic-offset: 4; indent-tabs-mode: nil;
// -*-
#include "moma.h"

MoMA::MoMA(const arma::mat &i_X,  // Pass X_ as a reference to avoid copy
           /*
            * sparsity - enforced through penalties
            */
           double i_lambda_u,  // regularization level
           double i_lambda_v,
           Rcpp::List i_prox_arg_list_u,
           Rcpp::List i_prox_arg_list_v,

           /*
            * smoothness - enforced through constraints
            */
           double i_alpha_u,  // Smoothing levels
           double i_alpha_v,
           const arma::mat &i_Omega_u,  // Smoothing matrices
           const arma::mat &i_Omega_v,

           /*
            * Algorithm parameters:
            */
           double i_EPS,
           long i_MAX_ITER,
           double i_EPS_inner,
           long i_MAX_ITER_inner,
           std::string i_solver)
    : n(i_X.n_rows),
      p(i_X.n_cols),
      alpha_u(i_alpha_u),
      alpha_v(i_alpha_v),
      lambda_u(i_lambda_u),
      lambda_v(i_lambda_v),
      X(i_X),  // no copy of the data
      Omega_u(i_Omega_u),
      Omega_v(i_Omega_v),
      MAX_ITER(i_MAX_ITER),
      EPS(i_EPS),
      solver_u(i_solver,
               alpha_u,
               i_Omega_u,
               lambda_u,
               i_prox_arg_list_u,
               i_EPS_inner,
               i_MAX_ITER_inner,
               i_X.n_rows),
      solver_v(i_solver,
               alpha_v,
               i_Omega_v,
               lambda_v,
               i_prox_arg_list_v,
               i_EPS_inner,
               i_MAX_ITER_inner,
               i_X.n_cols)
// const reference must be passed to initializer list
{
    if (i_EPS >= 1 || i_EPS_inner >= 1)
    {
        MoMALogger::error("EPS or EPS_inner too large.");
    }

    bicsr_u.bind(&solver_u, &PR_solver::bic);
    bicsr_v.bind(&solver_v, &PR_solver::bic);

    MoMALogger::info("Initializing MoMA object:")
        << " lambda_u " << lambda_u << " lambda_v " << lambda_v << " alpha_u " << alpha_u
        << " alpha_v " << alpha_v << " P_u " << Rcpp::as<std::string>(i_prox_arg_list_u["P"])
        << " P_v " << Rcpp::as<std::string>(i_prox_arg_list_v["P"]) << " EPS " << i_EPS
        << " MAX_ITER " << i_MAX_ITER << " EPS_inner " << i_EPS_inner << " MAX_ITER_inner "
        << i_MAX_ITER_inner << " solver " << i_solver;
    // Step 2: Initialize to leading singular vectors
    //
    //         MoMA is a regularized SVD, which is a non-convex (bi-convex)
    //         problem, so we need to be cautious about initialization to
    //         avoid local-minima. Initialization at the SVD (global solution
    //         to the non-regularized problem) seems to be a good trade-off:
    //         for problems with little regularization, the MoMA solution will
    //         lie near the SVD solution; for problems with significant
    //         regularization the problem becomes more well-behaved and less
    //         sensitive to initialization
    initialize_uv();
    is_initialzied = true;
    is_solved      = false;  // TODO: check if alphauv == 0
};

int MoMA::deflate(double d)
{
    MoMALogger::debug("Deflating:\n")
        << "\nX = \n"
        << X << "u^T = " << u.t() << "v^T = " << v.t() << "d = u^TXv = " << d;

    if (d <= 0.0)
    {
        MoMALogger::error("Cannot deflate by non-positive factor.");
    }
    X = X - d * u * v.t();
    // Re-initialize u and v after deflation
    initialize_uv();
    return d;
}

// Dependence on MoMA's internal states: MoMA::X, MoMA::u, MoMA::v, MoMA::alpha_u/v,
// MoMA::lambda_u/v
// After calling MoMA::solve(), MoMA::u and MoMA::v become the solution to the penalized regression.
void MoMA::solve()
{
    double tol = 1;
    int iter   = 0;
    arma::vec oldu;
    arma::vec oldv;
    while (tol > EPS && iter < MAX_ITER)
    {
        iter++;
        oldu = u;
        oldv = v;

        u = solver_u.solve(X * v, u);
        v = solver_v.solve(X.t() * u, v);

        double scale_u = norm(oldu) == 0.0 ? 1 : norm(oldu);
        double scale_v = norm(oldv) == 0.0 ? 1 : norm(oldv);

        tol = norm(oldu - u) / scale_u + norm(oldv - v) / scale_v;
        MoMALogger::debug("Real-time PG loop info:  (iter, tol) = (") << iter << ", " << tol << ")";
    }

    MoMALogger::info("Finish PG loop. Total iter = ") << iter;
    check_convergence(iter, tol);
    is_solved = true;
}

double MoMA::evaluate_loss()
{
    if (!is_solved)
    {
        MoMALogger::error("Please call MoMA::solve first before MoMA::evaluate_loss.");
    }
    double u_ellipsi_constaint = arma::as_scalar(u.t() * u + alpha_u * u.t() * Omega_u * u);
    double v_ellipsi_constaint = arma::as_scalar(v.t() * v + alpha_v * v.t() * Omega_v * v);

    if ((u_ellipsi_constaint != 0 && u_ellipsi_constaint != 1.0) ||
        (v_ellipsi_constaint != 0 && v_ellipsi_constaint != 1.0))
    {
        MoMALogger::error("Ellipse constraint is not met.");
    }

    return 1;  // TODO
}

int MoMA::initialize_uv()
{
    // TODO: we can set MoMA::u and MoMA::v to
    // the solution of pSVD with only smoothness constraints.

    // Set MoMA::v, MoMA::u as leading SVs of X
    arma::mat U;
    arma::vec s;
    arma::mat V;
    arma::svd(U, s, V, X);
    v              = V.col(0);
    u              = U.col(0);
    is_initialzied = true;
    return 0;
}

int MoMA::check_convergence(int iter, double tol)
{
    if (iter >= MAX_ITER || tol > EPS)
    {
        MoMALogger::warning("No convergence in MoMA!")
            << " lambda_u " << lambda_u << " lambda_v " << lambda_v << " alpha_u " << alpha_u
            << " alpha_v " << alpha_v;
    }
    return 0;
}

// Note it does not change MoMA::u and MoMA::v
int MoMA::reset(double newlambda_u, double newlambda_v, double newalpha_u, double newalpha_v)
{
    solver_u.reset(newlambda_u, newalpha_u);
    solver_v.reset(newlambda_v, newalpha_v);

    // NOTE: We must keep the alpha's and lambda's up-to-date
    // in both MoMA and solve_u/v
    if (std::abs(alpha_u - newalpha_u) > EPS || std::abs(alpha_v - newalpha_v) > EPS ||
        std::abs(lambda_v - newlambda_v) > EPS || std::abs(lambda_u - newlambda_u) > EPS)
    {
        // update internal states of MoMA
        is_solved = false;
        alpha_u   = newalpha_u;
        alpha_v   = newalpha_v;
        lambda_u  = newlambda_u;
        lambda_v  = newlambda_v;
    }

    return 0;
}

int MoMA::set_X(arma::mat new_X)
{
    X = new_X;
    initialize_uv();
    return 0;
}
