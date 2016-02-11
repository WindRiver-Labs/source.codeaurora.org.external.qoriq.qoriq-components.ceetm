/**************************************************************************
 * Copyright 2013, Freescale Semiconductor, Inc. All rights reserved.
 ***************************************************************************/
/*
 * File:        q_ceetm.c
 *
 * Description: Userspace ceetm command handler.
 *
 * Authors:     Ganga <B46167@freescale.com>
 *
 *
 */
/******************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdbool.h>

#include "utils.h"
#include "tc_util.h"


static void explain(void)
{
	fprintf(stderr, "Usage:\n"
		"... qdisc add ... ceetm type root [rate R [ceil C] overhead O"
		" mpu M]\n"
		"... class add ... ceetm (weight CW | rate R [ceil C])\n"
		"... qdisc add ... ceetm type prio [cr_map CR1 ... CR8] "
		"[er_map ER1 ... ER8]\n"
		"... qdisc add ... ceetm type wbfs queues Q W1 ... Wn [cr_map "
		"CR1] [er_map ER1]\n"
		"\n"
		"Qdisc types:\n"
		"root - link a CEETM LNI to a FMan port\n"
		"prio - create an eigth-class Priority Scheduler\n"
		"wbfs - create a four/eight-class Weighted Bandwidth Fair "
		"Scheduler\n"
		"\n"
		"Class types:\n"
		"weighted - create an unshaped channel\n"
		"rated - create a shaped channel\n"
		"\n"
		"Options:\n"
		"R - the CR of the LNI's or channel's dual-rate shaper "
		"(required for shaping scenarios)\n"
		"C - the ER of the LNI's or channel's dual-rate shaper "
		"(optional for shaping scenarios, defaults to 0)\n"
		"O - per-packet size overhead used in rate computations "
		"(required for shaping scenarios, recommended value is 24 i.e."
		" 12 bytes IFG + 8 bytes Preamble + 4 bytes FCS)\n"
		"M - minimum packet size used in rate computations (required"
		" for shaping scenarios)\n"
		"CW - the weight of an unshaped channel measured in MB "
		"(required for unshaped channels)\n"
		"CRx - boolean marking if the class group or corresponding "
		"class queue contributes to CR shaping (1) or not (0) "
		"(optional, defaults to 1 for shaping scenarios)\n"
		"ERx - boolean marking if the class group or corresponding "
		"class queue contributes to ER shaping (1) or not (0) "
		"(optional, defaults to 1 for shaping scenarios)\n"
		"Q - the number of class queues in the class group "
		"(either 4 or 8)\n"
		"Wx - the weights of each class in the class group measured "
		"in a log scale with values from 1 to 248 (either four or "
		"eight, depending on the size of the class group)\n"
	);
}

static void explain1(int type_mode)
{
	if (type_mode == 1)
		fprintf(stderr, "Usage:\n"
		" ... qdisc add ... ceetm type root"
		" rate 1000mbit ceil 1000mbit mpu 64 overhead 24\n"
		);
	else if (type_mode == 0)
		fprintf(stderr, "Usage:\n"
		"a) ... class add ... ceetm rate"
		" 1000mbit ceil 1000mbit\nb) ... class add ..."
		" ceetm weight 1\n");
	else if (type_mode == 2)
		fprintf(stderr, "Usage:\n"
				" ... qdisc add ... ceetm type prio"
				" cr_map CR1 ... CR8 er_map ER1 ... ER8\n");
	else if (type_mode == 3) {
		fprintf(stderr, "Usage:\n"
				"a) ... qdisc add ... ceetm type wbfs"
				" queues 8 W1 ... W8 cr_map CR1 er_map ER1\n"
				"b) ... qdisc add ... ceetm type wbfs queues 4"
				" W1 ... W4 cr_map CR1 er_map ER1\n"
			);
	} else
		fprintf(stderr, "\nINCORRECT COMMAND LINE\n");
}

static int ceetm_parse_qopt(struct qdisc_util *qu, int argc, char **argv,
		struct nlmsghdr *n)
{
	struct tc_ceetm_qopt opt;
	bool overhead_set = false;
	bool rate_set = false;
	bool ceil_set = false;
	bool cr_set = false;
	bool er_set = false;
	struct rtattr *tail;
	memset(&opt, 0, sizeof(opt));

	while (argc > 0) {
		if (strcmp(*argv, "type") == 0) {
			if (opt.type) {
				fprintf(stderr, "type already specified\n");
				return -1;
			}

			NEXT_ARG();

			if (matches(*argv, "root") == 0)
				opt.type = CEETM_ROOT;

			else if (matches(*argv, "prio") == 0)
				opt.type = CEETM_PRIO;

			else if (matches(*argv, "wbfs") == 0)
				opt.type = CEETM_WBFS;

			else {
				fprintf(stderr, "Illegal type argument\n");
				return -1;
			}

		} else if (strcmp(*argv, "qcount") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the qdisc "
						"type before the qcount.\n");
				return -1;

			} else if (opt.type == CEETM_ROOT) {
				fprintf(stderr, "qcount belongs to prio and wbfs qdiscs only.\n");
				return -1;
			}

			if (opt.qcount) {
				fprintf(stderr, "qcount already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_u16(&opt.qcount, *argv, 10) || opt.qcount == 0) {
				fprintf(stderr, "Illegal qcount argument\n");
				return -1;
			}

			if (opt.type == CEETM_PRIO && opt.qcount > CEETM_MAX_PRIO_QCOUNT) {
				fprintf(stderr, "qcount must be between 1 and "
					"%d for prio qdiscs\n", CEETM_MAX_PRIO_QCOUNT);
				return -1;
			}

			if (opt.type == CEETM_WBFS &&
					opt.qcount != CEETM_MIN_WBFS_QCOUNT &&
					opt.qcount != CEETM_MAX_WBFS_QCOUNT) {
				fprintf(stderr, "qcount must be either %d or "
					"%d for wbfs qdiscs\n", CEETM_MIN_WBFS_QCOUNT,
								CEETM_MAX_WBFS_QCOUNT);
				return -1;
			}

		} else if (strcmp(*argv, "rate") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the qdisc type "
							"before the rate.\n");
				return -1;

			} else if (opt.type != CEETM_ROOT) {
				fprintf(stderr, "rate belongs to root qdiscs only.\n");
				return -1;
			}

			if (rate_set) {
				fprintf(stderr, "rate already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_rate(&opt.rate, *argv)) {
				fprintf(stderr, "Illegal rate argument\n");
				return -1;
			}

			rate_set = true;

		} else if (strcmp(*argv, "ceil") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the qdisc type "
							"before the ceil.\n");
				return -1;

			} else if (opt.type != CEETM_ROOT) {
				fprintf(stderr, "ceil belongs to root qdiscs only.\n");
				return -1;
			}

			if (ceil_set) {
				fprintf(stderr, "ceil already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_rate(&opt.ceil, *argv)) {
				fprintf(stderr, "Illegal ceil argument\n");
				return -1;
			}

			ceil_set = true;

		} else if (strcmp(*argv, "overhead") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the qdisc type "
							"before the overhead.\n");
				return -1;

			} else if (opt.type != CEETM_ROOT) {
				fprintf(stderr, "overhead belongs to root qdiscs only.\n");
				return -1;
			}

			if (overhead_set) {
				fprintf(stderr, "overhead already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_u16(&opt.overhead, *argv, 10)) {
				fprintf(stderr, "Illegal overhead argument\n");
				return -1;
			}

			overhead_set = 1;

		} else if (strcmp(*argv, "cr") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the qdisc type "
							"before the cr.\n");
				return -1;

			} else if (opt.type != CEETM_WBFS) {
				fprintf(stderr, "cr belongs to wbfs qdiscs only.\n");
				return -1;
			}

			if (cr_set) {
				fprintf(stderr, "cr already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_u16(&opt.cr, *argv, 10) || opt.cr > 1) {
				fprintf(stderr, "Illegal cr argument\n");
				return -1;
			}

			cr_set = 1;

		} else if (strcmp(*argv, "er") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the qdisc type "
							"before the er.\n");
				return -1;

			} else if (opt.type != CEETM_WBFS) {
				fprintf(stderr, "er belongs to wbfs qdiscs only.\n");
				return -1;
			}

			if (er_set) {
				fprintf(stderr, "er already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_u16(&opt.er, *argv, 10) || opt.er > 1) {
				fprintf(stderr, "Illegal er argument\n");
				return -1;
			}

			er_set = 1;

		} else {
			// TODO: qweight argument
			fprintf(stderr, "Illegal argument\n");
		}

		argc--; argv++;
	}

	if (!opt.type) {
		fprintf(stderr, "please specify the qdisc type\n");
		return -1;
	}

	if (opt.type == CEETM_ROOT && (ceil_set || overhead_set) && !rate_set) {
		fprintf(stderr, "rate is mandatory for a shaped root qdisc\n");
		return -1;
	}

	if (opt.type == CEETM_PRIO && !opt.qcount) {
		fprintf(stderr, "qcount is mandatory for a prio qdisc\n");
		return -1;
	}

	if (opt.type == CEETM_WBFS && !opt.qcount) {
		fprintf(stderr, "qcount is mandatory for a wbfs qdisc\n");
		return -1;
	}

	if (opt.type == CEETM_ROOT && rate_set)
		opt.shaped = 1;
	else
		opt.shaped = 0;

	tail = NLMSG_TAIL(n);
	addattr_l(n, 1024, TCA_OPTIONS, NULL, 0);
	addattr_l(n, 1024, TCA_CEETM_QOPS, &opt, sizeof(opt));
	tail->rta_len = (void *) NLMSG_TAIL(n) - (void *) tail;

	return 0;
}

static int ceetm_parse_copt(struct qdisc_util *qu, int argc, char **argv,
		struct nlmsghdr *n)
{
	struct tc_ceetm_copt opt;
	struct rtattr *tail;
	memset(&opt, 0, sizeof(opt));
	bool tbl_set = false;
	bool rate_set = false;
	bool ceil_set = false;

	while (argc > 0) {
		if (strcmp(*argv, "type") == 0) {
			if (opt.type) {
				fprintf(stderr, "type already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (matches(*argv, "root") == 0) {
				opt.type = CEETM_ROOT;

			} else if (matches(*argv, "prio") == 0) {
				opt.type = CEETM_PRIO;

			} else {
				fprintf(stderr, "Illegal type argument\n");
				return -1;
			}

		} else if (strcmp(*argv, "rate") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the class type "
							"before the rate.\n");
				return -1;

			} else if (opt.type != CEETM_ROOT) {
				fprintf(stderr, "rate belongs to root classes only.\n");
				return -1;
			}

			if (rate_set) {
				fprintf(stderr, "rate already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_rate(&opt.rate, *argv)) {
				fprintf(stderr, "Illegal rate argument\n");
				return -1;
			}

			rate_set = true;

		} else if (strcmp(*argv, "ceil") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the class type "
							"before the ceil.\n");
				return -1;

			} else if (opt.type != CEETM_ROOT) {
				fprintf(stderr, "ceil belongs to root classes only.\n");
				return -1;
			}

			if (ceil_set) {
				fprintf(stderr, "ceil already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_rate(&opt.ceil, *argv)) {
				fprintf(stderr, "Illegal ceil argument\n");
				return -1;
			}

			ceil_set = true;

		} else if (strcmp(*argv, "tbl") == 0) {
			if (!opt.type) {
				fprintf(stderr, "Please specify the class type "
							"before the tbl.\n");
				return -1;

			} else if (opt.type != CEETM_ROOT) {
				fprintf(stderr, "tbl belongs to root classes only.\n");
				return -1;
			}

			if (tbl_set) {
				fprintf(stderr, "tbl already specified\n");
				return -1;
			}

			NEXT_ARG();
			if (get_u16(&opt.tbl, *argv, 10)) {
				fprintf(stderr, "Illegal tbl argument\n");
				return -1;
			}

			tbl_set = true;
			//TODO: is the tbl mandatory?
			//TODO: what tbl values are accepted?

		} else {
			fprintf(stderr, "Illegal argument\n");
			return -1;
		}

		argc--; argv++;
	}

	if (!opt.type) {
		fprintf(stderr, "please specify the class type\n");
		return -1;
	}

	if (opt.type == CEETM_ROOT && !tbl_set && !rate_set) {
		fprintf(stderr, "either tbl or rate are mandatory for root classes\n");
		return -1;
	}

	if (opt.type == CEETM_ROOT && tbl_set && rate_set) {
		fprintf(stderr, "both tbl and rate can not be used for root classes\n");
		return -1;
	}

	if (opt.type == CEETM_ROOT && ceil_set && !rate_set) {
		fprintf(stderr, "rate is mandatory for shaped root classes\n");
		return -1;
	}

	if (rate_set)
		opt.shaped = 1;
	else
		opt.shaped = 0;

	tail = NLMSG_TAIL(n);
	addattr_l(n, 1024, TCA_OPTIONS, NULL, 0);
	addattr_l(n, 2024, TCA_CEETM_COPT, &opt, sizeof(opt));
	tail->rta_len = (void *) NLMSG_TAIL(n) - (void *) tail;

	return 0;
}

int ceetm_print_opt(struct qdisc_util *qu, FILE *f, struct rtattr *opt)
{
	struct rtattr *tb[TCA_CEETM_MAX+1];
	struct tc_ceetm_qopt *qopt = NULL;
	struct tc_ceetm_copt *copt = NULL;
	char buf[64];

	if (opt == NULL)
		return 0;

	parse_rtattr_nested(tb, TCA_CEETM_MAX, opt);

	if (tb[TCA_CEETM_QOPS]) {
		if (RTA_PAYLOAD(tb[TCA_CEETM_QOPS]) < sizeof(*qopt))
			fprintf(stderr, "CEETM: too short opt\n");
		else
			qopt = RTA_DATA(tb[TCA_CEETM_QOPS]);
	}

	if (tb[TCA_CEETM_COPT]) {
		if (RTA_PAYLOAD(tb[TCA_CEETM_COPT]) < sizeof(*copt))
			fprintf(stderr, "CEETM: too short opt\n");
		else
			copt = RTA_DATA(tb[TCA_CEETM_COPT]);
	}

	if (qopt) {
		if (qopt->type == CEETM_ROOT) {
			fprintf(f, "type root");

			if (qopt->shaped) {
				print_rate(buf, sizeof(buf), qopt->rate);
				fprintf(f, " shaped rate %s ", buf);

				print_rate(buf, sizeof(buf), qopt->ceil);
				fprintf(f, "ceil %s ", buf);

				fprintf(f, "overhead %u ", qopt->overhead);

			} else {
				fprintf(f, " unshaped");
			}

		} else if (qopt->type == CEETM_PRIO) {
			fprintf(f, "type prio %s qcount %u ",
					qopt->shaped ? "shaped" : "unshaped",
					qopt->qcount);

		} else if (qopt->type == CEETM_WBFS) {
			fprintf(f, "type wbfs ");

			if (qopt->shaped) {
				fprintf(f, "shaped cr %d er %d ", qopt->cr, qopt->er);
			} else {
				fprintf(f, "unshaped ");
			}

			fprintf(f, "qcount %u", qopt->qcount);
		}
	}

	if (copt) {
		if (copt->type == CEETM_ROOT) {
			fprintf(f, "type root ");

			if (copt->shaped) {
				print_rate(buf, sizeof(buf), copt->rate);
				fprintf(f, "shaped rate %s ", buf);

				print_rate(buf, sizeof(buf), copt->ceil);
				fprintf(f, "ceil %s ", buf);

			} else {
				fprintf(f, "unshaped tbl %d", copt->tbl);
			}

		} else if (copt->type == CEETM_PRIO) {
			fprintf(f, "type prio ");

			if (copt->shaped) {
				fprintf(f, "shaped CR %d ER %d", copt->cr, copt->er);
			} else {
				fprintf(f, "unshaped");
			}

		} else if (copt->type == CEETM_WBFS) {
			fprintf(f, "type wbfs weight %d", copt->weight);
		}
	}

	return 0;
}


static int ceetm_print_xstats(struct qdisc_util *qu, FILE *f, struct rtattr *xstats)
{
	struct tc_ceetm_xstats *st;

	if (xstats == NULL)
		return 0;

	if (RTA_PAYLOAD(xstats) < sizeof(*st))
		return -1;

	st = RTA_DATA(xstats);
	fprintf(f, "enqueue %llu drop %llu dequeue %llu dequeue_bytes %llu\n",
			st->enqueue, st->drop, st->dequeue, st->deq_bytes);
	return 0;
}

struct qdisc_util ceetm_qdisc_util = {
	.id	 	= "ceetm",
	.parse_qopt	= ceetm_parse_qopt,
	.print_qopt	= ceetm_print_opt,
	.parse_copt	= ceetm_parse_copt,
	.print_copt	= ceetm_print_opt,
	.print_xstats	= ceetm_print_xstats,
};
