/**
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the LGPL License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 *
 * (C) Copyright 2016 Intel Corporation.
 */
/**
 * This file is part of dsm
 *
 * dsm/tests/container.c
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <daos_mgmt.h>
#include <daos_m.h>
#include <daos_event.h>
#include <daos/common.h>

typedef struct {
	daos_rank_t		ranks[8];
	daos_rank_list_t	svc;
	uuid_t			uuid;
	daos_handle_t		eq;
	daos_handle_t		poh;
	bool			async;
} test_arg_t;

/** create/destroy container */
static void
co_create(void **state)
{
	test_arg_t	*arg = *state;
	uuid_t		 uuid;
	daos_handle_t	 coh;
	daos_co_info_t	 info;
	daos_event_t	 ev;
	daos_event_t	*evp;
	int		 rc;

	if (arg->async) {
		rc = daos_event_init(&ev, arg->eq, NULL);
		assert_int_equal(rc, 0);
	}

	/** container uuid */
	uuid_generate(uuid);

	/** create container */
	print_message("creating container %ssynchronously ...\n",
		      arg->async ? "a" : "");
	rc = dsm_co_create(arg->poh, uuid, arg->async ? &ev : NULL);
	assert_int_equal(rc, 0);

	if (arg->async) {
		/** wait for container creation */
		rc = daos_eq_poll(arg->eq, 1, DAOS_EQ_WAIT, 1, &evp);
		assert_int_equal(rc, 1);
		assert_ptr_equal(evp, &ev);
		assert_int_equal(ev.ev_error, 0);
	}
	print_message("container created\n");

	print_message("opening container %ssynchronously\n",
		      arg->async ? "a" : "");
	rc = dsm_co_open(arg->poh, uuid, DAOS_COO_RW, NULL /* failed */, &coh,
			 &info, arg->async ? &ev : NULL);
	assert_int_equal(rc, 0);

	if (arg->async) {
		/** wait for container open */
		rc = daos_eq_poll(arg->eq, 1, DAOS_EQ_WAIT, 1, &evp);
		assert_int_equal(rc, 1);
		assert_ptr_equal(evp, &ev);
		assert_int_equal(ev.ev_error, 0);
	}
	print_message("contained opened\n");

	print_message("container info:\n");
	print_message("  hce: "DF_U64"\n", info.ci_epoch_state.es_hce);
	print_message("  lre: "DF_U64"\n", info.ci_epoch_state.es_lre);
	print_message("  lhe: "DF_U64"\n", info.ci_epoch_state.es_lhe);
	print_message("  ghce: "DF_U64"\n", info.ci_epoch_state.es_glb_hce);
	print_message("  glre: "DF_U64"\n", info.ci_epoch_state.es_glb_lre);
	print_message("  ghpce: "DF_U64"\n", info.ci_epoch_state.es_glb_hpce);

	print_message("closing container %ssynchronously ...\n",
		      arg->async ? "a" : "");
	rc = dsm_co_close(coh, arg->async ? &ev : NULL);
	assert_int_equal(rc, 0);

	if (arg->async) {
		/** wait for container close */
		rc = daos_eq_poll(arg->eq, 1, DAOS_EQ_WAIT, 1, &evp);
		assert_int_equal(rc, 1);
		assert_ptr_equal(evp, &ev);
		assert_int_equal(ev.ev_error, 0);
	}
	print_message("container closed\n");

	/** destroy container */
	print_message("destroying container %ssynchronously ...\n",
		      arg->async ? "a" : "");
	rc = dsm_co_destroy(arg->poh, uuid, 1 /* force */,
			    arg->async ? &ev : NULL);
	assert_int_equal(rc, 0);

	if (arg->async) {
		/** wait for container destroy */
		rc = daos_eq_poll(arg->eq, 1, DAOS_EQ_WAIT, 1, &evp);
		assert_int_equal(rc, 1);
		assert_ptr_equal(evp, &ev);
		assert_int_equal(ev.ev_error, 0);

		rc = daos_event_fini(&ev);
		assert_int_equal(rc, 0);
	}
	print_message("container destroyed\n");
}

static int
async_enable(void **state)
{
	test_arg_t	*arg = *state;

	arg->async = true;
	return 0;
}

static int
async_disable(void **state)
{
	test_arg_t	*arg = *state;

	arg->async = false;
	return 0;
}

static const struct CMUnitTest co_tests[] = {
	{ "DSM100: create/open/close/destroy container",
	  co_create, async_disable, NULL},
	{ "DSM101: create/open/close/destroy container (async)",
	  co_create, async_enable, NULL},
};

static int
setup(void **state)
{
	test_arg_t	*arg;
	int		 rc;

	arg = malloc(sizeof(test_arg_t));
	if (arg == NULL)
		return -1;

	rc = daos_eq_create(&arg->eq);
	if (rc)
		return rc;

	arg->svc.rl_nr.num = 8;
	arg->svc.rl_nr.num_out = 0;
	arg->svc.rl_ranks = arg->ranks;

	/** create pool with minimal size */
	rc = dmg_pool_create(0, geteuid(), getegid(), "srv_grp", NULL, "pmem",
			     0, &arg->svc, arg->uuid, NULL);
	if (rc)
		return rc;

	/** connect to pool */
	rc = dsm_pool_connect(arg->uuid, NULL /* grp */, &arg->svc,
			      DAOS_PC_RW, NULL /* failed */, &arg->poh,
			      NULL /* ev */);
	if (rc)
		return rc;

	*state = arg;
	return 0;
}

static int
teardown(void **state) {
	test_arg_t	*arg = *state;
	int		 rc;

	rc = dsm_pool_disconnect(arg->poh, NULL /* ev */);
	if (rc)
		return rc;

	rc = dmg_pool_destroy(arg->uuid, "srv_grp", 1, NULL);
	if (rc)
		return rc;

	rc = daos_eq_destroy(arg->eq, 0);
	if (rc)
		return rc;

	free(arg);
	return 0;
}

int
run_co_test(void)
{
	return cmocka_run_group_tests_name("DSM container tests", co_tests,
					   setup, teardown);
}
