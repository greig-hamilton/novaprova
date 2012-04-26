#include "u4c/runner.hxx"
#include "u4c/testnode.hxx"
#include "u4c/plan.hxx"
#include "u4c/text_listener.hxx"
#include "u4c/proxy_listener.hxx"
#include "u4c/child.hxx"
#include "spiegel/spiegel.hxx"
#include "u4c_priv.h"
#include "except.h"
#include <valgrind/memcheck.h>

__u4c_exceptstate_t __u4c_exceptstate;

namespace u4c {
using namespace std;

#define dispatch_listeners(func, ...) \
    do { \
	vector<listener_t*>::iterator _i; \
	for (_i = listeners_.begin() ; _i != listeners_.end() ; ++_i) \
	    (*_i)->func(__VA_ARGS__); \
    } while(0)

runner_t *runner_t::running_;

runner_t::runner_t()
{
    maxchildren_ = 1;
}

runner_t::~runner_t()
{
}

void
runner_t::set_concurrency(int n)
{
    if (n == 0)
    {
	/* shorthand for "best possible" */
	n = sysconf(_SC_NPROCESSORS_ONLN);
    }
    if (n < 1)
	n = 1;
    maxchildren_ = n;
}

void
runner_t::list_tests(plan_t *plan) const
{
    testnode_t *tn;

    bool ourplan = false;
    if (!plan)
    {
	/* build a default plan with all the tests */
	plan_t *plan = new plan_t();
	plan->add_node(testmanager_t::instance()->get_root());
	ourplan = true;
    }

    /* iterate over all tests */
    while ((tn = plan->next()))
	printf("%s\n", tn->get_fullname().c_str());

    if (ourplan)
	delete plan;
}

int
runner_t::run_tests(plan_t *plan)
{
    testnode_t *tn;

    bool ourplan = false;
    if (!plan)
    {
	/* build a default plan with all the tests */
	plan =  new plan_t();
	plan->add_node(testmanager_t::instance()->get_root());
	ourplan = true;
    }

    if (!listeners_.size())
	add_listener(new text_listener_t);

    begin();
    for (;;)
    {
	while (children_.size() < maxchildren_ &&
	       (tn = plan->next()))
	    begin_test(tn);
	if (!children_.size())
	    break;
	wait();
    }
    end();

    if (ourplan)
	delete plan;

    return !!nfailed_;
}

void
runner_t::add_listener(listener_t *l)
{
    /* append to the list.  The order of adding is preserved for
     * dispatching */
    listeners_.push_back(l);
}

void
runner_t::set_listener(listener_t *l)
{
    /* just throw away the old ones */
    listeners_.clear();
    listeners_.push_back(l);
}

static volatile int caught_sigchld = 0;
static void
handle_sigchld(int sig __attribute__((unused)))
{
    caught_sigchld = 1;
}

void
runner_t::begin()
{
    static bool init = false;

    if (!init)
    {
	signal(SIGCHLD, handle_sigchld);
	init = true;
    }

    running_ = this;
    dispatch_listeners(begin);
}

void
runner_t::end()
{
    dispatch_listeners(end);
    running_ = 0;
}


result_t
runner_t::raise_event(const event_t *ev)
{
    ev = ev->normalise();
    dispatch_listeners(add_event, ev);
    return ev->get_result();
}

child_t *
runner_t::fork_child(testnode_t *tn)
{
    pid_t pid;
#define PIPE_READ 0
#define PIPE_WRITE 1
    int pipefd[2];
    child_t *child;
    int delay_ms = 10;
    int max_sleeps = 20;
    int r;

    r = pipe(pipefd);
    if (r < 0)
    {
	perror("u4c: pipe");
	exit(1);
    }

    for (;;)
    {
	pid = fork();
	if (pid < 0)
	{
	    if (errno == EAGAIN && max_sleeps-- > 0)
	    {
		/* rats, we fork-bombed, try again after a delay */
		fprintf(stderr, "u4c: fork bomb! sleeping %u ms.\n",
			delay_ms);
		poll(0, 0, delay_ms);
		delay_ms += (delay_ms>>1);	/* exponential backoff */
		continue;
	    }
	    perror("u4c: fork");
	    exit(1);
	}
	break;
    }

    if (!pid)
    {
	/* child process: return, will run the test */
	close(pipefd[PIPE_READ]);
	event_pipe_ = pipefd[PIPE_WRITE];
	return NULL;
    }

    /* parent process */

    fprintf(stderr, "u4c: spawned child process %d for %s\n",
	    (int)pid, tn->get_fullname().c_str());
    close(pipefd[PIPE_WRITE]);
    child = new child_t(pid, pipefd[PIPE_READ], tn);
    children_.push_back(child);

    return child;
#undef PIPE_READ
#undef PIPE_WRITE
}

void
runner_t::handle_events()
{
    int r;

    if (!children_.size())
	return;

    while (!caught_sigchld)
    {
	pfd_.resize(children_.size());
	vector<child_t*>::iterator citr;
	vector<struct pollfd>::iterator pitr;
	for (pitr = pfd_.begin(), citr = children_.begin() ;
	     citr != children_.end() ; ++pitr, ++citr)
	    (*citr)->poll_setup(*pitr);

	r = poll(pfd_.data(), pfd_.size(), -1);
	if (r < 0)
	{
	    if (errno == EINTR)
		continue;
	    perror("u4c: poll");
	    return;
	}
	/* TODO: implement test timeout handling here */

	for (pitr = pfd_.begin(), citr = children_.begin() ;
	     citr != children_.end() ; ++pitr, ++citr)
	    (*citr)->poll_handle(*pitr);
    }
}

void
runner_t::reap_children()
{
    pid_t pid;
    int status;
    char msg[1024];

    for (;;)
    {
	pid = waitpid(-1, &status, WNOHANG);
	if (pid == 0)
	    break;
	if (pid < 0)
	{
	    if (errno == ESRCH || errno == ECHILD)
		break;
	    perror("u4c: waitpid");
	    return;
	}
	if (WIFSTOPPED(status))
	{
	    fprintf(stderr, "u4c: process %d stopped on signal %d, ignoring\n",
		    (int)pid, WSTOPSIG(status));
	    continue;
	}
	vector<child_t*>::iterator itr;
	for (itr = children_.begin() ;
	     itr != children_.end() && (*itr)->get_pid() != pid ;
	     ++itr)
	    ;
	if (itr == children_.end())
	{
	    /* some other process */
	    fprintf(stderr, "u4c: reaped stray process %d\n", (int)pid);
	    /* TODO: this is probably eventworthy */
	    continue;	    /* whatever */
	}
	child_t *child = *itr;

	if (WIFEXITED(status))
	{
	    if (WEXITSTATUS(status))
	    {
		snprintf(msg, sizeof(msg),
			 "child process %d exited with %d",
			 (int)pid, WEXITSTATUS(status));
		event_t ev(EV_EXIT, msg);
		child->merge_result(raise_event(&ev));
	    }
	}
	else if (WIFSIGNALED(status))
	{
	    snprintf(msg, sizeof(msg),
		    "child process %d died on signal %d",
		    (int)pid, WTERMSIG(status));
	    event_t ev(EV_SIGNAL, msg);
	    child->merge_result(raise_event(&ev));
	}

	/* test is finished; if nothing went wrong then PASS */
	child->merge_result(u4c::R_PASS);

	/* notify listeners */
	nfailed_ += (child->get_result() == R_FAIL);
	nrun_++;
	dispatch_listeners(finished, child->get_result());
	dispatch_listeners(end_node, child->get_node());

	/* detach and clean up */
	children_.erase(itr);
	delete child;
    }

    caught_sigchld = 0;
    /* nothing to reap here, move along */
}

void
runner_t::run_function(functype_t ft, spiegel::function_t *f)
{
    vector<spiegel::value_t> args;
    spiegel::value_t ret = f->invoke(args);

    if (ft == FT_TEST)
    {
	assert(ret.which == spiegel::type_t::TC_VOID);
    }
    else
    {
	assert(ret.which == spiegel::type_t::TC_SIGNED_INT);
	int r = ret.val.vsint;

	if (r)
	{
	    static char cond[64];
	    snprintf(cond, sizeof(cond), "fixture returned %d", r);
	    u4c_throw(event_t(EV_FIXTURE, cond).in_function(f));
	}
    }
}

void
runner_t::run_fixtures(testnode_t *tn, functype_t type)
{
    list<spiegel::function_t*> fixtures = tn->get_fixtures(type);
    list<spiegel::function_t*>::iterator itr;
    for (itr = fixtures.begin() ; itr != fixtures.end() ; ++itr)
	run_function(type, *itr);
}

result_t
runner_t::valgrind_errors()
{
    unsigned long leaked = 0, dubious = 0, reachable = 0, suppressed = 0;
    unsigned long nerrors;
    result_t res = R_UNKNOWN;
    char msg[1024];

    VALGRIND_DO_LEAK_CHECK;
    VALGRIND_COUNT_LEAKS(leaked, dubious, reachable, suppressed);
    if (leaked)
    {
	snprintf(msg, sizeof(msg),
		 "%lu bytes of memory leaked", leaked);
	event_t ev(EV_VALGRIND, msg);
	res = merge(res, raise_event(&ev));
    }

    nerrors = VALGRIND_COUNT_ERRORS;
    if (nerrors)
    {
	snprintf(msg, sizeof(msg),
		 "%lu unsuppressed errors found by valgrind", nerrors);
	event_t ev(EV_VALGRIND, msg);
	res = merge(res, raise_event(&ev));
    }

    return res;
}

result_t
runner_t::run_test_code(testnode_t *tn)
{
    result_t res = R_UNKNOWN;
    event_t *ev;

    tn->pre_fixture();

    u4c_try
    {
	run_fixtures(tn, FT_BEFORE);
    }
    u4c_catch(ev)
    {
	ev->in_functype(FT_BEFORE);
	res = merge(res, raise_event(ev));
    }

    if (res == R_UNKNOWN)
    {
	u4c_try
	{
	    run_function(FT_TEST, tn->get_function(FT_TEST));
	}
	u4c_catch(ev)
	{
	    ev->in_functype(FT_TEST);
	    res = merge(res, raise_event(ev));
	}

	u4c_try
	{
	    run_fixtures(tn, FT_AFTER);
	}
	u4c_catch(ev)
	{
	    ev->in_functype(FT_AFTER);
	    res = merge(res, raise_event(ev));
	}

	/* If we got this far and nothing bad
	 * happened, we might have passed */
	res = merge(res, R_PASS);
    }

    tn->post_fixture();
    res = merge(res, valgrind_errors());
    return res;
}


void
runner_t::begin_test(testnode_t *tn)
{
    child_t *child;
    result_t res;

    {
	static int n = 0;
	if (++n > 60)
	    return;
    }

    fprintf(stderr, "%s: begin test %s\n",
	    u4c_reltimestamp(), tn->get_fullname().c_str());

    dispatch_listeners(begin_node, tn);

    child = fork_child(tn);
    if (child)
	return; /* parent process */

    /* child process */
    set_listener(new proxy_listener_t(event_pipe_));
    res = run_test_code(tn);
    dispatch_listeners(finished, res);
    fprintf(stderr, "u4c: child process %d (%s) finishing\n",
	    (int)getpid(), tn->get_fullname().c_str());
    exit(0);
}

void
runner_t::wait()
{
    handle_events();
    reap_children();
}

/*-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-*/

extern "C" void
u4c_set_concurrency(u4c_runner_t *runner, int n)
{
    runner->set_concurrency(n);
}

extern "C" void
u4c_list_tests(u4c_runner_t *runner, u4c_plan_t *plan)
{
    runner->list_tests(plan);
}

extern "C" int
u4c_run_tests(u4c_runner_t *runner, u4c_plan_t *plan)
{
    return runner->run_tests(plan);
}

// close the namespace
};
