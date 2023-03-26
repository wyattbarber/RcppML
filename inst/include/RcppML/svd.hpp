// This file is part of RcppML, a Rcpp Machine Learning library
//
// Copyright (C) 2021 Zach DeBruine <zacharydebruine@gmail.com>
//
// This source code is subject to the terms of the GNU
// Public License v. 2.0.

#ifndef RcppML_svd
#define RcppML_svd

#define DIV_OFFSET 1e-15

namespace RcppML {
template <class T>
class svd {
   private:
    T& A;
    T t_A;
    Rcpp::SparseMatrix mask_matrix = Rcpp::SparseMatrix(), t_mask_matrix;
    Rcpp::SparseMatrix link_matrix_u = Rcpp::SparseMatrix(), link_matrix_v = Rcpp::SparseMatrix();
    Eigen::MatrixXd u;
    Eigen::MatrixXd v;
    Eigen::VectorXd d;
    double tol_ = -1, mse_ = 0;
    unsigned int iter_ = 0, best_model_ = 0;
    bool mask = false, mask_zeros = false, symmetric = false, transposed = false;
    
    // Specific rank update functions
    void fit_rank_one();
    void fit_rank_two();
    void fit_rank_k(int);

   public:
    bool verbose = true;
    unsigned int maxit = 100, threads = 0;
    std::vector<double> L1 = std::vector<double>(2), L2 = std::vector<double>(2);
    std::vector<bool> link = {false, false};
    double upper_bound = 0;  // set to 0 or negative to not impose upper bound limit

    double tol = 1e-4;

    std::vector<double> debug_errs;

    // CONSTRUCTORS
    // constructor for initialization with a randomly generated "w" matrix
    svd(T& A, const unsigned int k, const unsigned int seed = 0) : A(A) {
        u = randomMatrix(A.rows(), k, seed);
        v = Eigen::MatrixXd(A.cols(), k);
        d = Eigen::VectorXd::Ones(k);
        isSymmetric();
    }

    // constructor for initialization with an initial "u" matrix
    svd(T& A, Eigen::MatrixXd u) : A(A), u(u) {
        if (A.rows() != u.rows()) Rcpp::stop("number of rows in 'A' and 'u' are not equal!");
        v = Eigen::MatrixXd(A.cols(), u.cols());
        d = Eigen::VectorXd::Ones(u.cols());
        isSymmetric();
    }

    // constructor for initialization with a fully-specified model
    svd(T& A, Eigen::MatrixXd u, Eigen::MatrixXd v) : A(A), u(u), v(v) {
        if (A.rows() != u.rows()) Rcpp::stop("dimensions of 'u' and 'A' are not compatible");
        if (A.cols() != v.rows()) Rcpp::stop("dimensions of 'v' and 'A' are not compatible");
        if (u.cols() != v.cols()) Rcpp::stop("rank of 'u' and 'v' are not equal!");
        d = Eigen::VectorXd::Ones(u.cols());
        isSymmetric();
    }

    // SETTERS
    void isSymmetric();
    void maskZeros() {
        if (mask) Rcpp::stop("a masking function has already been specified");
        mask_zeros = true;
    }

    void maskMatrix(Rcpp::SparseMatrix& m) {
        if (mask) Rcpp::stop("a masking function has already been specified");
        if (m.rows() != A.rows() || m.cols() != A.cols()) Rcpp::stop("dimensions of masking matrix and 'A' are not equivalent");
        if (mask_zeros) Rcpp::stop("you already specified to mask zeros. You cannot also supply a masking matrix.");
        mask = true;
        mask_matrix = m;
        if (symmetric) symmetric = mask_matrix.isAppxSymmetric();
    }


    // impose upper maximum limit on NNLS solutions
    void upperBound(double upperbound) {
        upper_bound = upperbound;
    }

    // GETTERS
    Eigen::MatrixXd matrixU() { return u; }
    Eigen::MatrixXd matrixV() { return v; }
    Eigen::VectorXd vectorD() { return d; }
    double fit_tol() { return tol_; }
    unsigned int fit_iter() { return iter_; }
    double fit_mse() { return mse_; }
    unsigned int best_model() { return best_model_; }


    // requires specialized dense and sparse backends
    double mse();
    double mse_masked();

    double norm(Eigen::VectorXd in) {
        return std::sqrt(in.dot(in));
    }


    // fit the model by alternating least squares projections
    void fit() {
        if (verbose) Rprintf("\n%4s | %8s \n---------------\n", "iter", "tol");
               
        for(int k = 0; k < u.cols(); ++k){
            if(k == 0) fit_rank_one();
            else if(k == 1) fit_rank_two();
            else fit_rank_k(k);
        }

        // Calculate diagonal
        for(int i = 0; i < u.cols(); ++i){
            d(i) = u.col(i).norm();
            u.col(i) /= d(i);
        }
    }


    
    // fit the model multiple times and return the best one
    void fit_restarts(Rcpp::List& u_init) {
        Eigen::MatrixXd u_best = u;
        Eigen::MatrixXd v_best = v;
        double tol_best = tol_;
        double mse_best = 0;
        for (unsigned int i = 0; i < u_init.length(); ++i) {
            if (verbose) Rprintf("Fitting model %i/%i:", i + 1, u_init.length());
            u = Rcpp::as<Eigen::MatrixXd>(u_init[i]);
            tol_ = 1;
            iter_ = 0;
            if (u.rows() != v.rows()) Rcpp::stop("rank of 'u' is not equal to rank of 'v'");
            if (u.cols() != A.rows()) Rcpp::stop("dimensions of 'u' and 'A' are not compatible");
            fit();
            mse_ = mse();
            if (verbose) Rprintf("MSE: %8.4e\n\n", mse_);
            if (i == 0 || mse_ < mse_best) {
                best_model_ = i;
                u_best = u;
                v_best = v;
                tol_best = tol_;
                mse_best = mse_;
            }
        }
        if (best_model_ != (u_init.length() - 1)) {
            u = u_best;
            v = v_best;
            tol_ = tol_best;
            mse_ = mse_best;
        }
    }
};

// nmf class methods with specialized dense/sparse backends
template <>
void svd<Rcpp::SparseMatrix>::isSymmetric() {
    symmetric = A.isAppxSymmetric();
}

template <>
void svd<Eigen::MatrixXd>::isSymmetric() {
    symmetric = isAppxSymmetric(A);
}

template <>
double svd<Rcpp::SparseMatrix>::mse() {
    Eigen::MatrixXd u0 = u.transpose();

    // compute losses across all samples in parallel
    Eigen::ArrayXd losses(v.cols());
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(dynamic)
#endif
    for (unsigned int i = 0; i < v.cols(); ++i) {
        Eigen::VectorXd uv_i = u0 * v.col(i);
        if (mask_zeros) {
            for (Rcpp::SparseMatrix::InnerIterator iter(A, i); iter; ++iter)
                losses(i) += std::pow(uv_i(iter.row()) - iter.value(), 2);
        } else {
            for (Rcpp::SparseMatrix::InnerIterator iter(A, i); iter; ++iter)
                uv_i(iter.row()) -= iter.value();
            if (mask) {
                std::vector<unsigned int> m = mask_matrix.InnerIndices(i);
                for (unsigned int it = 0; it < m.size(); ++it)
                    uv_i(m[it]) = 0;
            }
            losses(i) += uv_i.array().square().sum();
        }
    }

    // divide total loss by number of applicable measurements
    if (mask)
        return losses.sum() / ((v.cols() * u.cols()) - mask_matrix.i.size());
    else if (mask_zeros)
        return losses.sum() / A.x.size();
    return losses.sum() / ((v.cols() * u.cols()));
}

template <>
double svd<Eigen::MatrixXd>::mse() {
    if (verbose) Rprintf("mse: Test point 0\n");
    Eigen::MatrixXd u0 = u.transpose();
    if (verbose) Rprintf("mse: Test point 1\n");
    // compute losses across all samples in parallel
    Eigen::ArrayXd losses(v.cols());
    if (verbose) Rprintf("mse: Test point 2\n");
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(dynamic)
#endif
    for (unsigned int i = 0; i < v.cols(); ++i) {
        Eigen::VectorXd uv_i = u0 * v.col(i);
        if (verbose) Rprintf("mse: Test point 3\n");
        if (mask_zeros) {
            if (verbose) Rprintf("mse: Test point 4\n");
            for (unsigned int iter = 0; iter < A.rows(); ++iter)
                if (A(iter, i) != 0)
                    losses(i) += std::pow(uv_i(iter) - A(iter, i), 2);
        } else {
            if (verbose) Rprintf("mse: Test point 5\n");
            for (unsigned int iter = 0; iter < A.rows(); ++iter){
                if (verbose) Rprintf("mse: Test point 5.1\n");
                uv_i(iter) -= A(iter, i);
                if (verbose) Rprintf("mse: Test point 5.2\n");
            }
            if (mask) {
                if (verbose) Rprintf("mse: Test point 5.3\n");
                std::vector<unsigned int> m = mask_matrix.InnerIndices(i);
                if (verbose) Rprintf("mse: Test point 5.4\n");
                for (unsigned int it = 0; it < m.size(); ++it){
                    if (verbose) Rprintf("mse: Test point 5.5\n");
                    uv_i(m[it]) = 0;
                }
            }
            if (verbose) Rprintf("mse: Test point 5.6\n");
            losses(i) += uv_i.array().square().sum();
        }
    }

    // divide total loss by number of applicable measurements
    if (verbose) Rprintf("mse: Test point 6\n");
    if (mask){
        if (verbose) Rprintf("mse: Test point 6.1\n");
        return losses.sum() / ((v.cols() * u.cols()) - mask_matrix.i.size());
    } else if (mask_zeros) {
        if (verbose) Rprintf("mse: Test point 6.2\n");
        return losses.sum() / n_nonzeros(A);
    }
    if (verbose) Rprintf("mse: Test point 6.3\n");
    return losses.sum() / ((v.cols() * u.cols()));
}

template <>
double svd<Rcpp::SparseMatrix>::mse_masked() {
    if (!mask) Rcpp::stop("'mse_masked' can only be run when a masking matrix has been specified");

    Eigen::MatrixXd u0 = u.transpose();

    Eigen::ArrayXd losses(v.cols());
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(dynamic)
#endif
    for (unsigned int i = 0; i < v.cols(); ++i) {
        std::vector<unsigned int> masked_rows = mask_matrix.InnerIndices(i);
        if (masked_rows.size() > 0) {
            for (Rcpp::SparseMatrix::InnerIteratorInRange iter(A, i, masked_rows); iter; ++iter) {
                losses(i) += std::pow((u0.row(iter.row()) * v.col(i)) - iter.value(), 2);
            }
            // get masked rows that are also zero in A.col(i)
            std::vector<unsigned int> zero_rows = A.emptyInnerIndices(i);
            std::vector<unsigned int> masked_zero_rows;
            std::set_intersection(zero_rows.begin(), zero_rows.end(),
                                  masked_rows.begin(), masked_rows.end(),
                                  std::back_inserter(masked_zero_rows));
            for (unsigned int it = 0; it < masked_zero_rows.size(); ++it)
                losses(i) += std::pow(u0.row(masked_zero_rows[it]) * v.col(i), 2);
        }
    }
    return losses.sum() / mask_matrix.i.size();
}

template <>
double svd<Eigen::MatrixXd>::mse_masked() {
    if (!mask) Rcpp::stop("'mse_masked' can only be run when a masking matrix has been specified");

    Eigen::MatrixXd u0 = u.transpose();

    Eigen::ArrayXd losses(v.cols());
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(dynamic)
#endif
    for (unsigned int i = 0; i < v.cols(); ++i) {
        std::vector<unsigned int> masked_rows = mask_matrix.InnerIndices(i);
        for (unsigned int it = 0; it < masked_rows.size(); ++it) {
            const unsigned int row = masked_rows[it];
            losses(i) += std::pow((u0.row(row) * v.col(i)) - A(row, i), 2);
        }
    }
    return losses.sum() / mask_matrix.i.size();
}

template <>
void svd<Eigen::MatrixXd>::fit_rank_one() {
    double d;
    for (; iter_ < maxit; ++iter_) {
        Eigen::MatrixXd u_it = u.col(0);

        // Update V
        double a = u.col(0).dot(u.col(0)) + DIV_OFFSET;
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
        for(int i = 0; i < v.rows(); ++i){
            v.col(0)(i) = u.col(0).dot(A.col(i));
            if(L1[1] > 0) v.col(0)(i) -= L1[1];
            v.col(0)(i) /= a;
        }

        // Scale V
        v.col(0) /= v.col(0).norm() + DIV_OFFSET;

        // Update U
        a = v.col(0).dot(v.col(0)) + DIV_OFFSET;
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
        for(int i = 0; i < u.rows(); ++i){
            u.col(0)(i) = v.col(0).dot(A.row(i));
            if(L1[0] > 0) u.col(0)(i) -= L1[0];
            u.col(0)(i) /= a;
        }

        // Scale U
        d = u.col(0).norm();
        u.col(0) /= (d + DIV_OFFSET);

        // Check exit criteria
        Eigen::MatrixXd u_post_it = u.col(0);
        tol_ = cor(u_post_it, u_it);  // correlation between "u" across consecutive iterations
        if (verbose) Rprintf("%4d | %8.2e\n", iter_ + 1, tol_);

        if (tol_ < tol) break;
        Rcpp::checkUserInterrupt();
    }

    // "unscale" U
    u.col(0) *= d;

    if (tol_ > tol && iter_ == maxit && verbose)
        Rprintf(" convergence not reached in %d iterations\n  (actual tol = %4.2e, target tol = %4.2e)\n", iter_, tol_, tol);
}

template <>
void svd<Eigen::MatrixXd>::fit_rank_two() {
    double d;
    for (; iter_ < maxit; ++iter_) {
        Eigen::MatrixXd u_it = u.col(1);

        double a0 =  u.col(1).dot(u.col(0)) + DIV_OFFSET;
        double a1 =  u.col(1).dot(u.col(1)) + DIV_OFFSET;
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
        for(int i = 0; i < v.rows(); ++i){
            v.col(1)(i) = (u.col(1).dot(A.col(i))) - (u.col(0)(i) * a0);
            if(L1[1] > 0) v.col(1)(i) -= L1[1];
            v.col(1)(i) /= a1;
        }

        // Scale V
        v.col(1) /= v.col(1).norm() + DIV_OFFSET;

        a0 = v.col(1).dot(v.col(0)) + DIV_OFFSET;
        a1 = v.col(1).dot(v.col(1)) + DIV_OFFSET;
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
        for(int i = 0; i < u.rows(); ++i){
            u.col(1)(i) = (v.col(1).dot(A.row(i))) - (v.col(0)(i) * a0);
            if(L1[0] > 0) u.col(1)(i) -= L1[0];
            u.col(1)(i) /= a1;
        }

        // Scale U
        d = u.col(1).norm();
        u.col(1) /= (d + DIV_OFFSET);

        // Check exit criteria
        Eigen::MatrixXd u_post_it = u.col(1);
        tol_ = cor(u_post_it, u_it);  // correlation between "u" across consecutive iterations
        if (verbose) Rprintf("%4d | %8.2e\n", iter_ + 1, tol_);

        if (tol_ < tol) break;
        Rcpp::checkUserInterrupt();
    }

    // "unscale" U
    u.col(1) *= d;

    if (tol_ > tol && iter_ == maxit && verbose)
        Rprintf(" convergence not reached in %d iterations\n  (actual tol = %4.2e, target tol = %4.2e)\n", iter_, tol_, tol);
}

template <>
void svd<Eigen::MatrixXd>::fit_rank_k(int k) {
    // alternating least squares updates
    double d_k;
    for (; iter_ < maxit; ++iter_) {
        Eigen::MatrixXd u_it = u.col(k);

        double a =  u.col(k).dot(u.col(k)) + DIV_OFFSET;
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
        for(int i = 0; i < v.rows(); ++i){
            v.col(k)(i) = (u.col(k).dot(A.col(i)));
            if(L1[1] > 0) v.col(k)(i) -= L1[1];

            for(int _k = k-1; _k >= 0; --_k){
                // TODO: Precalculate a, so that dot product is not recalculated each loop
                v.col(k)(i) -= u.col(k).dot(u.col(_k)) * v.col(_k)(i);
            } 

            v.col(k)(i) /= a;
        }

        v.col(k) /= (u.col(k).dot(u.col(k)) + DIV_OFFSET);

        // Scale V
        v.col(k) /= v.col(k).norm() + DIV_OFFSET;

        // Update U
        a = v.col(k).dot(v.col(k)) + DIV_OFFSET;
#ifdef _OPENMP
#pragma omp parallel for num_threads(threads) schedule(static)
#endif
        for(int i = 0; i < u.rows(); ++i){
            u.col(k)(i) = v.col(k).dot(A.row(i));
            if(L1[0] > 0) u.col(k)(i) -= L1[0];

            for(int _k = k-1; _k >= 0; --_k){
                // TODO: Precalculate a, so that dot product is not recalculated each loop
                u.col(k)(i) -= v.col(k).dot(v.col(_k)) * u.col(_k)(i);
            }

            u.col(k)(i) /= a;
        }

        // Scale U
        d_k = u.col(k).norm();
        u.col(k) /= (d_k + DIV_OFFSET);

        // Check exit criteria
        Eigen::MatrixXd u_post_it = u.col(k);
        tol_ = cor(u_post_it, u_it);  // correlation between "u" across consecutive iterations
        if (verbose) Rprintf("%4d | %8.2e\n", iter_ + 1, tol_);

        if (tol_ < tol) break;
        Rcpp::checkUserInterrupt();
    }

    // "unscale" U
    u.col(k) *= d_k;

    if (tol_ > tol && iter_ == maxit && verbose)
        Rprintf(" convergence not reached in %d iterations\n  (actual tol = %4.2e, target tol = %4.2e)\n", iter_, tol_, tol);
}
}  // namespace RcppML





#endif