/*
 * node_server_actions_1.cpp
 *
 *  Created on: Sep 23, 2016
 *      Author: dmarce1
 */

#include "diagnostics.hpp"
#include "future.hpp"
#include "node_client.hpp"
#include "node_server.hpp"
#include "options.hpp"
#include "profiler.hpp"
#include "taylor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>

#include <hpx/include/lcos.hpp>
#include <hpx/include/run_as.hpp>
#include <hpx/lcos/broadcast.hpp>

extern options opts;

typedef node_server::load_action load_action_type;
HPX_REGISTER_ACTION(load_action_type);

hpx::mutex rec_size_mutex;
integer rec_size = -1;

void set_locality_data(real omega, space_vector pivot, integer record_size) {
    grid::set_omega(omega,false);
    grid::set_pivot(pivot);
    rec_size = record_size;
}

HPX_PLAIN_ACTION(set_locality_data, set_locality_data_action);
HPX_REGISTER_BROADCAST_ACTION_DECLARATION(set_locality_data_action)
HPX_REGISTER_BROADCAST_ACTION(set_locality_data_action)

hpx::future<grid::output_list_type> node_client::load(
    integer i, const hpx::id_type& _me, bool do_o, std::string s) const {
    return hpx::async<typename node_server::load_action>(get_unmanaged_gid(), i, _me, do_o, s);
}

grid::output_list_type node_server::load(
    integer cnt, const hpx::id_type& _me, bool do_output, std::string filename) {
    if (rec_size == -1 && my_location.level() == 0) {
#ifdef RADIATION
	if (opts.eos == WD) {
		set_cgs(false);
	}
#endif
        real omega = 0;
        space_vector pivot;

        // run output on separate thread
        hpx::threads::run_as_os_thread([&]() {
            FILE* fp = fopen(filename.c_str(), "rb");
            if (fp == NULL) {
                printf("Failed to open file\n");
                abort();
            }
            fseek(fp, -sizeof(integer), SEEK_END);
            std::size_t read_cnt = fread(&rec_size, sizeof(integer), 1, fp);
            fseek(fp, -4 * sizeof(real) - sizeof(integer), SEEK_END);
            read_cnt += fread(&omega, sizeof(real), 1, fp);
            for (auto const& d : geo::dimension::full_set()) {
                real temp_pivot;
                read_cnt += fread(&temp_pivot, sizeof(real), 1, fp);
                pivot[d] = temp_pivot;
            }
            fclose(fp);
        }).get();

        hpx::lcos::broadcast<set_locality_data_action>(options::all_localities, omega, pivot, rec_size).get();
    }

    me = _me;

    char flag = '0';
    integer total_nodes = 0;
    std::vector<integer> counts(NCHILD);

    // run output on separate thread
    FILE* fp = fopen(filename.c_str(), "rb");
    fseek(fp, cnt * rec_size, SEEK_SET);
    std::size_t read_cnt = fread(&flag, sizeof(char), 1, fp);
    hpx::threads::run_as_os_thread([&]() {
        for (auto& this_cnt : counts) {
            read_cnt += fread(&this_cnt, sizeof(integer), 1, fp);
        }
        load_me(fp);
        fclose(fp);

        // work around limitation of ftell returning 32bit offset
        std::ifstream in(filename.c_str(), std::ifstream::ate | std::ifstream::binary);
        std::size_t end_pos = in.tellg();

        total_nodes = end_pos / rec_size;
    }).get();
#ifdef RADIATION
    rad_grid_ptr = grid_ptr->get_rad_grid();
	rad_grid_ptr->sanity_check();
#endif

    std::array<hpx::future<grid::output_list_type>, NCHILD> futs;
    // printf( "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!1\n" );
    if (flag == '1') {
        is_refined = true;

        integer index = 0;
        for (auto const& ci : geo::octant::full_set()) {
            integer loc_id = ((cnt * options::all_localities.size()) / (total_nodes + 1));
			children[ci] = hpx::new_ < node_server
					> (options::all_localities[loc_id], my_location.get_child(ci), me.get_gid(), ZERO, ZERO, step_num, hcycle, gcycle);
#ifdef OCTOTIGER_RESTART_LOAD_SEQ
            children[ci].load(counts[ci], children[ci].get_gid(), do_output, filename).get();
#else
            futs[index++] =
                children[ci].load(counts[ci], children[ci].get_gid(), do_output, filename);
#endif
        }
    } else if (flag == '0') {
        is_refined = false;
        std::fill_n(children.begin(), NCHILD, node_client());
    } else {
        printf("Corrupt checkpoint file\n");
        //		sleep(10);
        hpx::this_thread::sleep_for(std::chrono::seconds(10));
        abort();
    }

    grid::output_list_type my_list;
    for (auto&& fut : futs) {
		if (fut.valid()) {
			if (do_output) {
				grid::merge_output_lists(my_list, fut.get());
			} else {
				fut.get();
			}
		}
    }
    // printf( "***************************************\n" );
    if (!is_refined && do_output) {
        my_list = grid_ptr->get_output_list(false);
        //	grid_ptr = nullptr;
    }
    //	hpx::async<inc_grids_loaded_action>(localities[0]).get();
    if (my_location.level() == 0) {
        if (do_output) {
            if (hydro_on && opts.problem == DWD) {
                diagnostics();
            }
            grid::output(
                my_list, "data.silo", current_time, get_rotation_count() / opts.output_dt, false);
        }
        printf("Loaded checkpoint file\n");
        my_list = decltype(my_list)();
    }

    return my_list;
}

typedef node_server::output_action output_action_type;
HPX_REGISTER_ACTION(output_action_type);

hpx::future<grid::output_list_type> node_client::output(
    std::string fname, int cycle, bool flag) const {
    return hpx::async<typename node_server::output_action>(get_unmanaged_gid(), fname, cycle, flag);
}

grid::output_list_type node_server::output(std::string fname, int cycle, bool analytic) const {
    if (is_refined) {
        std::array<hpx::future<grid::output_list_type>, NCHILD> futs;
        integer index = 0;
        for (auto i = children.begin(); i != children.end(); ++i) {
            futs[index++] = i->output(fname, cycle, analytic);
        }

        auto i = futs.begin();
        grid::output_list_type my_list = i->get();
        for (++i; i != futs.end(); ++i) {
            grid::merge_output_lists(my_list, i->get());
        }

        if (my_location.level() == 0) {
            //			hpx::apply([](const grid::output_list_type&
            // olists, const char* filename) {
            printf("Outputing...\n");
            grid::output(my_list, fname, get_time(), cycle, analytic);
            printf("Done...\n");
            //		}, std::move(my_list), fname.c_str());
        }
        return my_list;

    } else {
        return grid_ptr->get_output_list(analytic);
    }
}

typedef node_server::regrid_gather_action regrid_gather_action_type;
HPX_REGISTER_ACTION(regrid_gather_action_type);

hpx::future<integer> node_client::regrid_gather(bool rb) const {
    return hpx::async<typename node_server::regrid_gather_action>(get_unmanaged_gid(), rb);
}

integer node_server::regrid_gather(bool rebalance_only) {
    integer count = integer(1);

    if (is_refined) {
        if (!rebalance_only) {
            /* Turning refinement off */
            if (refinement_flag == 0) {
                std::fill_n(children.begin(), NCHILD, node_client());
                is_refined = false;
                grid_ptr->set_leaf(true);
            }
        }

        if (is_refined) {
            std::array<hpx::future<integer>, NCHILD> futs;
            integer index = 0;
            for (auto& child : children) {
                futs[index++] = child.regrid_gather(rebalance_only);
            }
            auto futi = futs.begin();
            for (auto const& ci : geo::octant::full_set()) {
                auto child_cnt = futi->get();
                ++futi;
                child_descendant_count[ci] = child_cnt;
                count += child_cnt;
            }
        } else {
            for (auto const& ci : geo::octant::full_set()) {
                child_descendant_count[ci] = 0;
            }
        }
    } else if (!rebalance_only) {
        //		if (grid_ptr->refine_me(my_location.level())) {
        if (refinement_flag != 0) {
            refinement_flag = 0;
            count += NCHILD;

            /* Turning refinement on*/
            is_refined = true;
            grid_ptr->set_leaf(false);

            for (auto& ci : geo::octant::full_set()) {
                child_descendant_count[ci] = 1;
				children[ci] = hpx::new_ < node_server
						> (hpx::find_here(), my_location.get_child(ci), me, current_time, rotational_time, step_num, hcycle, gcycle);
				{
					std::array<integer, NDIM> lb = { 2 * H_BW, 2 * H_BW, 2 * H_BW };
					std::array<integer, NDIM> ub;
					lb[XDIM] += (1 & (ci >> 0)) * (INX);
					lb[YDIM] += (1 & (ci >> 1)) * (INX);
					lb[ZDIM] += (1 & (ci >> 2)) * (INX);
					for (integer d = 0; d != NDIM; ++d) {
						ub[d] = lb[d] + (INX);
					}
					std::vector<real> outflows(NF, ZERO);
					if (ci == 0) {
						outflows = grid_ptr->get_outflows();
					}
					if (current_time > ZERO) {
						children[ci].set_grid(grid_ptr->get_prolong(lb, ub), std::move(outflows)).get();
					}
				}
#ifdef RADIATION
				{
					std::array<integer, NDIM> lb = { 2 * R_BW, 2 * R_BW, 2 * R_BW };
					std::array<integer, NDIM> ub;
					lb[XDIM] += (1 & (ci >> 0)) * (INX);
					lb[YDIM] += (1 & (ci >> 1)) * (INX);
					lb[ZDIM] += (1 & (ci >> 2)) * (INX);
					for (integer d = 0; d != NDIM; ++d) {
						ub[d] = lb[d] + (INX);
					}
				/*	std::vector<real> outflows(NF, ZERO);
					if (ci == 0) {
						outflows = grid_ptr->get_outflows();
					}*/
					if (current_time > ZERO) {
						children[ci].set_rad_grid(rad_grid_ptr->get_prolong(lb, ub)/*, std::move(outflows)*/).get();
					}
				}
#endif


            }
        }
    }

    return count;
}

typedef node_server::regrid_scatter_action regrid_scatter_action_type;
HPX_REGISTER_ACTION(regrid_scatter_action_type);

hpx::future<void> node_client::regrid_scatter(integer a, integer b) const {
    return hpx::async<typename node_server::regrid_scatter_action>(get_unmanaged_gid(), a, b);
}

hpx::future<void> node_server::regrid_scatter(integer a_, integer total) {
    refinement_flag = 0;
    std::array<hpx::future<void>, geo::octant::count()> futs;
    if (is_refined) {
        integer a = a_;
        ++a;
        for (auto& ci : geo::octant::full_set()) {
            const integer loc_index = a * options::all_localities.size() / total;
            const auto child_loc = options::all_localities[loc_index];
            a += child_descendant_count[ci];
            const hpx::id_type id = children[ci].get_gid();
            integer current_child_id = hpx::naming::get_locality_id_from_gid(id.get_gid());
            auto current_child_loc = options::all_localities[current_child_id];
            if (child_loc != current_child_loc) {
                children[ci] = children[ci].copy_to_locality(child_loc);
            }
        }
        a = a_ + 1;
        integer index = 0;
        for (auto const& ci : geo::octant::full_set()) {
            futs[index++] = children[ci].regrid_scatter(a, total);
            a += child_descendant_count[ci];
        }
    }
    clear_family();
    if( is_refined ) {
    	return hpx::when_all(futs);
    } else {
    	return hpx::make_ready_future();
    }
}

typedef node_server::regrid_action regrid_action_type;
HPX_REGISTER_ACTION(regrid_action_type);

hpx::future<void> node_client::regrid(const hpx::id_type& g, real omega, bool rb) const {
    return hpx::async<typename node_server::regrid_action>(get_unmanaged_gid(), g, omega, rb);
}

int node_server::regrid(const hpx::id_type& root_gid, real omega, bool rb) {
    timings::scope ts(timings_, timings::time_regrid);
    assert(grid_ptr != nullptr);
    printf("-----------------------------------------------\n");
    if (!rb) {
        printf("checking for refinement\n");
        check_for_refinement(omega).get();
    }
    printf("regridding\n");
    integer a = regrid_gather(rb);
    printf("rebalancing %i nodes\n", int(a));
    regrid_scatter(0, a).get();
    assert(grid_ptr != nullptr);
    std::vector<hpx::id_type> null_neighbors(geo::direction::count());
    printf("forming tree connections\n");
    form_tree(root_gid, hpx::invalid_id, null_neighbors).get();
    if (current_time > ZERO) {
        printf("solving gravity\n");
        solve_gravity(true);
    }
    printf("regrid done\n-----------------------------------------------\n");
    return a;
}

typedef node_server::save_action save_action_type;
HPX_REGISTER_ACTION(save_action_type);

integer node_client::save(integer i, std::string s) const {
    return hpx::async<typename node_server::save_action>(get_unmanaged_gid(), i, s).get();
}

integer node_server::save(integer cnt, std::string filename) const {
    char flag = is_refined ? '1' : '0';
    integer record_size = 0;

    // run output on separate thread
    hpx::threads::run_as_os_thread([&]() {
        FILE* fp = fopen(filename.c_str(), (cnt == 0) ? "wb" : "ab");
        fwrite(&flag, sizeof(flag), 1, fp);
        ++cnt;
        //	printf("                                   \rSaved %li sub-grids\r",
        //(long int) cnt);
        integer value = cnt;
        std::array<integer, NCHILD> values;
        for (auto& ci : geo::octant::full_set()) {
            if (ci != 0 && is_refined) {
                value += child_descendant_count[ci - 1];
            }
            values[ci] = value;
            fwrite(&value, sizeof(value), 1, fp);
        }
        record_size = save_me(fp) + sizeof(flag) + NCHILD * sizeof(integer);
        fclose(fp);
    }).get();

    if (is_refined) {
        for (auto& ci : geo::octant::full_set()) {
            cnt = children[ci].save(cnt, filename);
        }
    }

    if (my_location.level() == 0) {
        // run output on separate thread
        hpx::threads::run_as_os_thread([&]() {
            FILE* fp = fopen(filename.c_str(), "ab");
            real omega = grid::get_omega();
            space_vector pivot = grid::get_pivot();
            fwrite(&omega, sizeof(real), 1, fp);
            for (auto& d : geo::dimension::full_set()) {
                real temp_pivot;
                fwrite(&temp_pivot, sizeof(real), 1, fp);
                pivot[d] = temp_pivot;
            }
            fwrite(&record_size, sizeof(integer), 1, fp);
            fclose(fp);
        }).get();

        printf("Saved %li grids to checkpoint file\n", (long int) cnt);
    }

    return cnt;
}

typedef node_server::set_aunt_action set_aunt_action_type;
HPX_REGISTER_ACTION(set_aunt_action_type);

hpx::future<void> node_client::set_aunt(const hpx::id_type& aunt, const geo::face& f) const {
    return hpx::async<typename node_server::set_aunt_action>(get_unmanaged_gid(), aunt, f);
}

void node_server::set_aunt(const hpx::id_type& aunt, const geo::face& face) {
    aunts[face] = aunt;
}

typedef node_server::set_grid_action set_grid_action_type;
HPX_REGISTER_ACTION(set_grid_action_type);

hpx::future<void> node_client::set_grid(std::vector<real>&& g, std::vector<real>&& o) const {
    return hpx::async<typename node_server::set_grid_action>(get_unmanaged_gid(), g, o);
}

void node_server::set_grid(const std::vector<real>& data, std::vector<real>&& outflows) {
    grid_ptr->set_prolong(data, std::move(outflows));
}

typedef node_server::solve_gravity_action solve_gravity_action_type;
HPX_REGISTER_ACTION(solve_gravity_action_type);

hpx::future<void> node_client::solve_gravity(bool ene) const {
    return hpx::async<typename node_server::solve_gravity_action>(get_unmanaged_gid(), ene);
}

void node_server::solve_gravity(bool ene) {
    if (!gravity_on) {
        return;
    }
    std::array<hpx::future<void>, NCHILD> child_futs;
    if (is_refined)
    {
        integer index = 0;;
        for (auto& child : children) {
            child_futs[index++] = child.solve_gravity(ene);
        }
    }
    compute_fmm(RHO, ene);
    if( is_refined ) {
    	wait_all_and_propagate_exceptions(child_futs);
    }
}
