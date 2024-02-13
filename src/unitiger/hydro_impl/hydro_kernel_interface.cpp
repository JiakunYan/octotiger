//  Copyright (c) 2021-2022 Gregor Daiß
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

/* #undef NDEBUG */
#ifdef OCTOTIGER_HAVE_KOKKOS
#include <hpx/kokkos/executors.hpp>
#include <hpx/kokkos.hpp>
#endif
#include "octotiger/unitiger/hydro_impl/hydro_kernel_interface.hpp"
#include "octotiger/unitiger/hydro_impl/flux_kernel_interface.hpp"
#include "octotiger/unitiger/hydro_impl/hydro_performance_counters.hpp"
#ifdef OCTOTIGER_HAVE_KOKKOS
#include "octotiger/unitiger/hydro_impl/hydro_kokkos_kernel.hpp"
#endif
#if defined(OCTOTIGER_HAVE_KOKKOS) && defined(KOKKOS_ENABLE_SYCL)
#include <CL/sycl.hpp>
// We encounter segfaults on Intel GPUs when running the normal kernels for the first time after
// the program starts. This seems to be some initialization issue as we can simply fix it by
// (non-concurrently) run simple dummy kernel first right after starting octotiger
// (presumably initializes something within the intel gpu runtime).
// Curiousely we have to do this not once per program, but once per lib (octolib and hydrolib).
//
// Somewhat of an ugly workaround but it does the trick and allows us to target Intel GPUs as
// Octo-Tiger runs as expected after applying this workaround.

// TODO(daissgr) Check again in the future to see if the runtime has matured and we don't need this anymore. 
// (last check was 02/2024)

/// Utility function working around segfault on Intel GPU. Initializes something within the runtime by runnning
///a dummy kernel
int touch_sycl_device_by_running_a_dummy_kernel(void) {
    try {
        cl::sycl::queue q(cl::sycl::default_selector_v, cl::sycl::property::queue::in_order{});
        cl::sycl::event my_kernel_event = q.submit(
            [&](cl::sycl::handler& h) {
                h.parallel_for(512, [=](auto i) {});
            },
            cl::sycl::detail::code_location{});
        my_kernel_event.wait();
    } catch (sycl::exception const& e) {
        std::cerr << "(NON-FATAL) ERROR: Caught sycl::exception during SYCL dummy kernel!\n";
        std::cerr << " {what}: " << e.what() << "\n ";
        std::cerr << "Continuing for now as error only occured in the dummy kernel...\n";
        return 2;

    }
    return 1;
}
/// Dummy variable to ensure the touch_sycl_device_by_running_a_dummy_kernel is being run
const int init_sycl_device = touch_sycl_device_by_running_a_dummy_kernel();
#endif
#if defined(OCTOTIGER_HAVE_KOKKOS)
hpx::once_flag init_hydro_kokkos_pool_flag;
#if defined(KOKKOS_ENABLE_CUDA)
using device_executor = hpx::kokkos::cuda_executor;
using device_pool_strategy = round_robin_pool<device_executor>;
using executor_interface_t = stream_interface<device_executor, device_pool_strategy>;
#elif defined(KOKKOS_ENABLE_HIP)
using device_executor = hpx::kokkos::hip_executor;
using device_pool_strategy = round_robin_pool<device_executor>;
using executor_interface_t = stream_interface<device_executor, device_pool_strategy>;
#elif defined(KOKKOS_ENABLE_SYCL)
using device_executor = hpx::kokkos::sycl_executor;
using device_pool_strategy = round_robin_pool<device_executor>;
using executor_interface_t = stream_interface<device_executor, device_pool_strategy>;
#endif
#ifdef OCTOTIGER_HYDRO_HOST_HPX_EXECUTOR
using host_executor = hpx::kokkos::hpx_executor;
#else
using host_executor = hpx::kokkos::serial_executor;
#endif
void init_hydro_kokkos_aggregation_pool(void) {
    const size_t max_slices = opts().max_kernels_fused;
    constexpr size_t number_aggregation_executors = 128;
    Aggregated_Executor_Modes executor_mode = Aggregated_Executor_Modes::EAGER;
    if (max_slices == 1) {
      executor_mode = Aggregated_Executor_Modes::STRICT;
    }
    if (opts().executors_per_gpu > 0) {
#if defined(KOKKOS_ENABLE_CUDA)
    hydro_kokkos_agg_executor_pool<hpx::kokkos::cuda_executor>::init(number_aggregation_executors, max_slices, executor_mode, opts().number_gpus);
#elif defined(KOKKOS_ENABLE_HIP)
    hydro_kokkos_agg_executor_pool<hpx::kokkos::hip_executor>::init(number_aggregation_executors, max_slices, executor_mode, opts().number_gpus);
#elif defined(KOKKOS_ENABLE_SYCL)
    hydro_kokkos_agg_executor_pool<hpx::kokkos::sycl_executor>::init(number_aggregation_executors, max_slices, executor_mode, opts().number_gpus);
#endif
    }
    hydro_kokkos_agg_executor_pool<host_executor>::init(number_aggregation_executors, 1, Aggregated_Executor_Modes::STRICT, 1);
}
#endif


#if defined(OCTOTIGER_HAVE_CUDA) || defined(OCTOTIGER_HAVE_HIP)
#include <hpx/async_cuda/cuda_executor.hpp>
using device_executor_cuda = hpx::cuda::experimental::cuda_executor;
using device_pool_strategy_cuda = round_robin_pool<device_executor_cuda>;
using executor_interface_cuda_t = stream_interface<device_executor_cuda, device_pool_strategy_cuda>;
#endif

timestep_t launch_hydro_kernels(hydro_computer<NDIM, INX, physics<NDIM>>& hydro,
    const std::vector<std::vector<safe_real>>& U, std::vector<std::vector<safe_real>>& X,
    const double omega, std::vector<hydro_state_t<std::vector<safe_real>>>& F,
    const interaction_host_kernel_type host_type, const interaction_device_kernel_type device_type,
    const size_t max_gpu_executor_queue_length) {
    static const cell_geometry<NDIM, INX> geo;

    // interaction_host_kernel_type host_type = opts().hydro_host_kernel_type;
    // interaction_device_kernel_type device_type = opts().hydro_device_kernel_type;

    // Timestep default value
    auto max_lambda = timestep_t{};

    // Try accelerator implementation
    if (device_type != interaction_device_kernel_type::OFF) {
        if (device_type == interaction_device_kernel_type::KOKKOS_CUDA ||
            device_type == interaction_device_kernel_type::KOKKOS_HIP ||
            device_type == interaction_device_kernel_type::KOKKOS_SYCL) {
#if defined(OCTOTIGER_HAVE_KOKKOS) && (defined(KOKKOS_ENABLE_CUDA) || defined(KOKKOS_ENABLE_HIP)|| defined(KOKKOS_ENABLE_SYCL))
            // Init local kernel pool if not done already
            hpx::call_once(init_hydro_kokkos_pool_flag, init_hydro_kokkos_aggregation_pool);
            bool avail = true; 
            // Host execution is possible: Check if there is a launch slot for device - if not 
            // we will execute the kernel on the CPU instead
            if (host_type != interaction_host_kernel_type::DEVICE_ONLY) {
                size_t device_id =
                    stream_pool::get_next_device_id<device_executor, device_pool_strategy>(opts().number_gpus);
                avail = stream_pool::interface_available<device_executor, device_pool_strategy>(
                    max_gpu_executor_queue_length, device_id);
            }
            if (avail) {
                // executor_interface_t executor;
                max_lambda = launch_hydro_kokkos_kernels<device_executor>(
                    hydro, U, X, omega, opts().n_species, F);
                return max_lambda;
            }
        }
#else
            std::cerr << "Trying to call hydro Kokkos kernel with no or the wrong kokkos device backend active! "
                         "Aborting..."
                      << std::endl;
            abort();
        }
#endif
        if (device_type == interaction_device_kernel_type::CUDA) {
#ifdef OCTOTIGER_HAVE_CUDA
            bool avail = true;
            // Host execution is possible: Check if there is a launch slot for device - if not 
            // we will execute the kernel on the CPU instead
            if (host_type != interaction_host_kernel_type::DEVICE_ONLY) {
                size_t device_id =
                    stream_pool::get_next_device_id<device_executor_cuda, device_pool_strategy_cuda>(opts().number_gpus);
                avail = stream_pool::interface_available<hpx::cuda::experimental::cuda_executor,
                    device_pool_strategy_cuda>(max_gpu_executor_queue_length, device_id);
            }
            if (avail) {
                size_t device_id = 0;
                max_lambda = launch_hydro_cuda_kernels(hydro, U, X, omega, device_id, F);
                return max_lambda;
            }
        }
#else
            std::cerr << "Trying to call Hydro CUDA device kernels in a non-CUDA build! "
                      << "Aborting..." << std::endl;
            abort();
        }
#endif
        if (device_type == interaction_device_kernel_type::HIP) {
#ifdef OCTOTIGER_HAVE_HIP
            bool avail = true;
            if (host_type != interaction_host_kernel_type::DEVICE_ONLY) {
                size_t device_id =
                    stream_pool::get_next_device_id<device_executor_cuda, device_pool_strategy_cuda>(opts().number_gpus);
              avail = stream_pool::interface_available<hpx::cuda::experimental::cuda_executor,
                  device_pool_strategy_cuda>(max_gpu_executor_queue_length, device_id);
            }
            if (avail) {
                size_t device_id = 0;
                max_lambda = launch_hydro_cuda_kernels(hydro, U, X, omega, device_id, F);
                return max_lambda;
            }
        }
#else
            std::cerr << "Trying to call Hydro HIP device kernels in a non-HIP build! "
                      << "Aborting..." << std::endl;
            abort();
        }
#endif
    }

    // Nothing is available or device execution is disabled - fallback to host execution
    if (host_type == interaction_host_kernel_type::KOKKOS) {
#ifdef OCTOTIGER_HAVE_KOKKOS
        hpx::call_once(init_hydro_kokkos_pool_flag, init_hydro_kokkos_aggregation_pool);
        max_lambda = launch_hydro_kokkos_kernels<host_executor>(
            hydro, U, X, omega, opts().n_species, F);
        return max_lambda;
#else
        std::cerr << "Trying to call Hydro Kokkos kernels in a non-kokkos build! Aborting..."
                  << std::endl;
        abort();
#endif
    } else if (host_type == interaction_host_kernel_type::LEGACY) {
        // Legacy implementation
        static thread_local auto f = std::vector<std::vector<std::vector<safe_real>>>(NDIM,
            std::vector<std::vector<safe_real>>(opts().n_fields, std::vector<safe_real>(H_N3)));
#ifdef HPX_HAVE_APEX
        auto reconstruct_timer = apex::start("kernel hydro_reconstruct legacy");
#endif
        const auto& q = hydro.reconstruct(U, X, omega);
#ifdef HPX_HAVE_APEX
        apex::stop(reconstruct_timer);
        auto flux_timer = apex::start("kernel hydro_flux legacy");
#endif
        max_lambda = hydro.flux(U, q, f, X, omega);
        octotiger::hydro::hydro_legacy_subgrids_processed++;
#ifdef HPX_HAVE_APEX
        apex::stop(flux_timer);
#endif
        // Use legacy conversion
        for (int dim = 0; dim < NDIM; dim++) {
            for (integer field = 0; field != opts().n_fields; ++field) {
                for (integer i = 0; i <= INX; ++i) {
                    for (integer j = 0; j <= INX; ++j) {
                        for (integer k = 0; k <= INX; ++k) {
                            const auto i0 = findex(i, j, k);
                            F[dim][field][i0] = f[dim][field][hindex(i + H_BW, j + H_BW, k + H_BW)];
                            real rho_tot = 0.0;
                            for (integer field = spc_i; field != spc_i + opts().n_species;
                                 ++field) {
                                rho_tot += F[dim][field][i0];
                            }
                            F[dim][rho_i][i0] = rho_tot;
                        }
                    }
                }
            }
        }
        return max_lambda;
    } else {
        std::cerr << "No valid hydro kernel type given! " << std::endl;
        std::cerr << "Aborting..." << std::endl;
        abort();
    }
    std::cerr << "Invalid state: Could not call any hydro kernel configuration!" << std::endl;
    std::cerr << "Aborting..." << std::endl;
    abort();
    return max_lambda;
}

void convert_x_structure(const hydro::x_type& X, double* const combined_x) {
    constexpr int length_orig = INX + 6;
    constexpr int length_desired = INX + 2;
    auto it_x = combined_x;
    for (size_t dim = 0; dim < NDIM; dim++) {
        auto start_offset = 2 * length_orig * length_orig + 2 * length_orig + 2;
        for (auto ix = 2; ix < 2 + INX + 2; ix++) {
            for (auto iy = 2; iy < 2 + INX + 2; iy++) {
                std::copy(X[dim].begin() + start_offset,
                    X[dim].begin() + start_offset + length_desired, it_x);
                it_x += length_desired;
                start_offset += length_orig;
            }
            start_offset += (2 + 2) * length_orig;
        }
    }
}
