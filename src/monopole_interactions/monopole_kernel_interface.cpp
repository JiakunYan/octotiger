
#include "octotiger/monopole_interactions/monopole_kernel_interface.hpp"
#include "octotiger/monopole_interactions/legacy/cuda_monopole_interaction_interface.hpp"
#include "octotiger/options.hpp"

#include "octotiger/common_kernel/interactions_iterators.hpp"
#include "octotiger/monopole_interactions/legacy/monopole_interaction_interface.hpp"
#include "octotiger/monopole_interactions/util/calculate_stencil.hpp"
#include "octotiger/options.hpp"

#include <algorithm>
#include <array>
#include <vector>

#include <aligned_buffer_util.hpp>
#include <buffer_manager.hpp>
#include <stream_manager.hpp>

#ifdef OCTOTIGER_HAVE_KOKKOS
#include <hpx/kokkos.hpp>
#endif

#ifdef OCTOTIGER_HAVE_KOKKOS
        using device_executor = hpx::kokkos::cuda_executor;
        using host_executor = hpx::kokkos::serial_executor;
        // using host_executor = hpx::kokkos::hpx_executor;
        using device_pool_strategy = round_robin_pool<device_executor>;
        using executor_interface_t = stream_interface<device_executor, device_pool_strategy>;
#endif

#include "octotiger/monopole_interactions/kernel/kokkos_kernel.hpp"

namespace octotiger {
namespace fmm {
    namespace monopole_interactions {

        void p2p_kernel_interface(std::vector<real>& monopoles,
            std::vector<std::shared_ptr<std::vector<space_vector>>>& com_ptr,
            std::vector<neighbor_gravity_type>& neighbors, gsolve_type type, real dx,
            std::array<bool, geo::direction::count()>& is_direction_empty,
            std::shared_ptr<grid>& grid_ptr, const bool contains_multipole_neighbor) {
            // accelerator_kernel_type device_type = DEVICE_CUDA;
            // host_kernel_type host_type = HOST_VC;
            accelerator_kernel_type device_type = DEVICE_KOKKOS;
            host_kernel_type host_type = HOST_KOKKOS;

            // Try accelerator implementation
            if (device_type != OFF) {
                if (device_type == DEVICE_KOKKOS) {
#ifdef OCTOTIGER_HAVE_KOKKOS
                    bool avail =
                        stream_pool::interface_available<device_executor, device_pool_strategy>(
                            opts().cuda_buffer_capacity);
                    if (avail) {
                        executor_interface_t executor;
                        p2p_kernel<device_executor>(executor, monopoles, com_ptr, neighbors, type,
                            dx, opts().theta, is_direction_empty, grid_ptr,
                            contains_multipole_neighbor);
                        return;
                    }
#else
                    std::cerr
                        << "Trying to call P2P Kokkos kernel in a non-kokkos build! Aborting..."
                        << std::endl;
                    abort();
#endif
                }
                if (device_type == DEVICE_CUDA) {
                    cuda_p2p_interaction_interface
                        p2p_interactor{};
                    p2p_interactor.compute_p2p_interactions(monopoles, com_ptr, neighbors, type, dx,
                        is_direction_empty, grid_ptr, contains_multipole_neighbor);
                    return;
                }
            }    // Nothing is available or device execution is disabled - fallback to host
                 // execution

            if (host_type == HOST_KOKKOS) {
#ifdef OCTOTIGER_HAVE_KOKKOS
                host_executor executor{};
                p2p_kernel<host_executor>(executor, monopoles, com_ptr, neighbors, type, dx,
                    opts().theta, is_direction_empty, grid_ptr, contains_multipole_neighbor);
                return;
#else
                std::cerr << "Trying to call P2P Kokkos kernel in a non-kokkos build! Aborting..."
                          << std::endl;
                abort();
#endif
            } else {
                p2p_interaction_interface p2p_interactor{};
                p2p_interactor.compute_p2p_interactions(monopoles, com_ptr, neighbors, type, dx,
                    is_direction_empty, grid_ptr, contains_multipole_neighbor);
                return;
            }

            return;
        }

    }    // namespace monopole_interactions
}    // namespace fmm
}    // namespace octotiger
