#include "octotiger/unitiger/hydro_impl/flux_kernel_interface.hpp"

timestep_t flux_kernel_interface(const hydro::recon_type<NDIM> &Q, hydro::flux_type &F, hydro::x_type &X,
		safe_real omega, const size_t nf_) {
    // input Q, X
    // output F

    timestep_t ts;
    ts.a = 0.0;
    // bunch of tmp containers

    // bunch of small helpers
    static const cell_geometry<3, 8> geo;
    static constexpr auto faces = geo.face_pts();
    static constexpr auto weights = geo.face_weight();
    static constexpr auto xloc = geo.xloc();
    static constexpr auto levi_civita = geo.levi_civita();
    double p, v, v0, c;
    const auto A_ = physics<NDIM>::A_;
    const auto B_ = physics<NDIM>::B_;
    double current_amax = 0.0;
    size_t current_max_index = 0;
    size_t current_d = 0;
    size_t current_dim = 0;

    const auto dx = X[0][geo.H_DNX] - X[0][0];

    static thread_local std::vector<double> UR(nf_), UL(nf_), this_flux(nf_);
    static thread_local std::vector<double> FR(nf_), FL(nf_);
    std::array<double, NDIM> x;
    std::array<double, NDIM> vg;
    for (int dim = 0; dim < NDIM; dim++) {
        const auto indices = geo.get_indexes(3, geo.face_pts()[dim][0]);

        std::array<int, NDIM> lbs = {3, 3, 3};
        std::array<int, NDIM> ubs = {geo.H_NX - 3, geo.H_NX - 3, geo.H_NX - 3};
        for (int dimension = 0; dimension < NDIM; dimension++) {
            ubs[dimension] = geo.xloc()[geo.face_pts()[dim][0]][dimension] == -1 ?
                (geo.H_NX - 3 + 1) :
                (geo.H_NX - 3);
            lbs[dimension] = geo.xloc()[geo.face_pts()[dim][0]][dimension] == +1 ? (3 - 1) : 3;
        }

        // zero-initialize F
        for (int f = 0; f < nf_; f++) {
#pragma ivdep
            for (const auto& i : indices) {
                F[dim][f][i] = 0.0;
            }
        }

        for (int fi = 0; fi < geo.NFACEDIR; fi++) {    // 9
            safe_real ap = 0.0, am = 0.0;              // final am ap for this i
            safe_real this_ap, this_am;                // tmps
            safe_real this_amax;
            const auto d = faces[dim][fi];

            const auto flipped_dim = geo.flip_dim(d, dim);
            for (size_t ix = lbs[0]; ix < ubs[0]; ix++) {
                for (size_t iy = lbs[1]; iy < geo.H_NX; iy++) {
                    for (size_t iz = lbs[2]; iz < geo.H_NX; iz++) {
                        if (iy >= ubs[1] || iz >= ubs[2])
                            continue;
                        const size_t i = ix * geo.H_NX * geo.H_NX + iy * geo.H_NX + iz;
                        // why store this?
                        for (int f = 0; f < nf_; f++) {
                            UR[f] = Q[f][d][i];    // not cache efficient at all - cacheline is
                                                   // going to be dismissed
                            UL[f] = Q[f][flipped_dim][i - geo.H_DN[dim]];
                        }
                        this_amax = inner_flux_loop<double>(X, omega, nf_, A_, B_, UR, UL, FR, FL,
                            this_flux, x, vg, ap, am, dim, d, i, geo, dx);
                        if (this_amax > current_amax) {
                            current_amax = this_amax;
                            current_max_index = i;
                            current_d = d;
                            current_dim = dim;
                        }
#pragma ivdep
                        for (int f = 0; f < nf_; f++) {
                            // field update from flux
                            F[dim][f][i] += weights[fi] * this_flux[f];
                        }
                    }    // end z
                }        // end y
            }            // end x
        }                // end dirs
    }                    // end dim
    ts.a = current_amax;
    ts.x = X[0][current_max_index];
    ts.y = X[1][current_max_index];
    ts.z = X[2][current_max_index];
    const auto flipped_dim = geo.flip_dim(current_d, current_dim);
    for (int f = 0; f < nf_; f++) {
        UR[f] = Q[f][current_d][current_max_index];
        UL[f] = Q[f][flipped_dim][current_max_index - geo.H_DN[current_dim]];
    }
    ts.ul = UL;
    ts.ur = UR;
    ts.dim = current_dim;
    return ts;
}