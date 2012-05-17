#include <u4c.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

static void test_timeout(void)
{
    fprintf(stderr, "Sleeping for 40 sec\n");
    sleep(40);
    fprintf(stderr, "Awoke!\n");
}

int
main(int argc, char **argv)
{
    int ec = 0;
    u4c_plan_t *plan = 0;
    if (argc > 1)
    {
	plan = u4c_plan_new();
	u4c_plan_add_specs(plan, argc-1, (const char **)argv+1);
    }
    u4c_runner_t *runner = u4c_init();
    ec = u4c_run_tests(runner, plan);
    if (plan) u4c_plan_delete(plan);
    u4c_done(runner);
    exit(ec);
}