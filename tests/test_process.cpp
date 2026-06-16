#include "doctest.h"
#include "Process.h"
#include "Scheduler.h"
#include <cstring>

using namespace kernel;

TEST_CASE("ProcTable: alloc gives distinct pids and tracks parent") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	Process* b = ProcTable::alloc(a->pid);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	CHECK(a->pid != b->pid);
	CHECK(b->parent == a->pid);
	CHECK(a->used);
	CHECK(b->used);
}

TEST_CASE("ProcTable: current() reflects setCurrent; byPid/byTask look up") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	CHECK(ProcTable::current() == nullptr);
	ProcTable::setCurrent(a);
	CHECK(ProcTable::current() == a);
	CHECK(ProcTable::byPid(a->pid) == a);
	CHECK(ProcTable::byPid(9999) == nullptr);

	Task* fake = (Task*) 0x1234;
	a->task = fake;
	CHECK(ProcTable::byTask(fake) == a);
	CHECK(ProcTable::byTask((Task*) 0x5678) == nullptr);
}

TEST_CASE("reapChild: no children -> -ECHILD") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* out = (Process*) 0x1;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == -10);
}

TEST_CASE("reapChild: child alive -> 0; exited -> pid + frees slot") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* child = ProcTable::alloc(parent->pid);

	// A live (not-yet-exited) child: nothing to reap, but it exists.
	Process* out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == 0);
	CHECK(out == nullptr);

	// Now it exits: reapChild returns its pid and hands it back for teardown.
	child->exited = true;
	child->exitCode = 42;
	out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == child->pid);
	CHECK(out == child);
	CHECK(out->exitCode == 42);

	// Caller frees the slot; a second wait finds no children.
	ProcTable::freeSlot(out);
	CHECK(!child->used);
	Process* out2 = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, -1, &out2) == -10);
}

TEST_CASE("process starts as a single-thread group whose leader tid == pid") {
	ProcTable::init();
	kernel::Process* p = kernel::ProcTable::alloc(/*parent*/0);
	REQUIRE(p != nullptr);
	CHECK(p->threadCount == 1);
	CHECK(p->tgid == p->pid);
	CHECK(p->leaderThread()->tid == p->pid);
}

TEST_CASE("currentThread tracks the running thread, not just the process") {
	kernel::ProcTable::init();
	kernel::Process* p = kernel::ProcTable::alloc(0);
	REQUIRE(p != nullptr);
	kernel::ProcTable::setCurrent(p);
	kernel::ProcTable::setCurrentThread(p->leaderThread());
	CHECK(kernel::ProcTable::currentThread() == p->leaderThread());
	CHECK(kernel::ProcTable::currentThread()->proc == p);
}

TEST_CASE("allocThread adds a non-leader thread with a fresh tid; free paths release slots") {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	REQUIRE(p != nullptr);
	Thread* leader = p->leaderThread();
	CHECK(leader->tid == p->pid);

	// A clone-style thread: fresh tid (distinct from the pid), group grows, leader stays at head.
	Thread* t2 = ProcTable::allocThread(p);
	REQUIRE(t2 != nullptr);
	CHECK(t2->tid != p->pid);
	CHECK(t2->proc == p);
	CHECK(p->threadCount == 2);
	CHECK(p->leaderThread() == leader);                  // leader not displaced by the splice
	CHECK(ProcTable::threadByTid(t2->tid) == t2);        // scans the whole thread pool
	CHECK(ProcTable::threadByTid(p->pid) == leader);

	// freeThread releases just that thread; the leader and the group survive.
	int t2tid = t2->tid;
	ProcTable::freeThread(t2);
	CHECK(p->threadCount == 1);
	CHECK(ProcTable::threadByTid(t2tid) == nullptr);
	CHECK(p->leaderThread() == leader);

	// freeSlot releases the process AND its remaining (leader) thread slot — no leak.
	int pid = p->pid;
	ProcTable::freeSlot(p);
	CHECK(ProcTable::threadByTid(pid) == nullptr);
}

static int g_visited;
static int g_visitedKthreads;
static void countVisit(int /*pid*/, bool kthread, void* /*ctx*/) {
	g_visited++;
	if (kthread) g_visitedKthreads++;
}

TEST_CASE("forEachLive visits every live process once, skipping freed slots") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	Process* b = ProcTable::alloc(0);
	Process* c = ProcTable::alloc(0);
	b->kthread = true;
	ProcTable::freeSlot(a);          // a is gone -> must not be visited

	g_visited = 0; g_visitedKthreads = 0;
	ProcTable::forEachLive(countVisit, nullptr);
	CHECK(g_visited == 2);           // only b and c remain
	CHECK(g_visitedKthreads == 1);   // b was flagged a kernel thread
	(void) c;
}

TEST_CASE("reparentChildren: orphans move to init and become reapable there") {
	ProcTable::init();
	Process* init   = ProcTable::alloc(0);          // stands in for pid 1
	Process* parent = ProcTable::alloc(init->pid);
	Process* c1 = ProcTable::alloc(parent->pid);
	Process* c2 = ProcTable::alloc(parent->pid);
	Process* other = ProcTable::alloc(init->pid);   // not a child of `parent`

	// Re-home parent's children onto init; only its own two children move.
	CHECK(ProcTable::reparentChildren(parent->pid, init->pid) == 2);
	CHECK(c1->parent == init->pid);
	CHECK(c2->parent == init->pid);
	CHECK(other->parent == init->pid);              // unchanged (already init's)

	// init can now reap them once they exit; the dead original parent never could.
	c1->exited = true;
	Process* out = nullptr;
	CHECK(ProcTable::reapChild(init->pid, c1->pid, &out) == c1->pid);
	CHECK(out == c1);
	CHECK(ProcTable::reapChild(parent->pid, -1, &out) == -10);   // parent has no children left
}

TEST_CASE("reapChild: wantPid narrows to a specific child") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* c1 = ProcTable::alloc(parent->pid);
	Process* c2 = ProcTable::alloc(parent->pid);
	c1->exited = true; c1->exitCode = 1;
	c2->exited = true; c2->exitCode = 2;

	Process* out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, c2->pid, &out) == c2->pid);
	CHECK(out == c2);
	// A pid that is not a child of parent -> -ECHILD.
	out = nullptr;
	CHECK(ProcTable::reapChild(parent->pid, 9999, &out) == -10);
}

TEST_CASE("setCommand: comm = basename(argv0), cmdline = argv joined") {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	const char* argv[] = { "/disks/main/nanos/bin/ls.nxe", "-l", "/proc", 0 };
	ProcTable::setCommand(p, argv, 3);
	CHECK(strcmp(p->comm, "ls.nxe") == 0);
	CHECK(strcmp(p->cmdline, "/disks/main/nanos/bin/ls.nxe -l /proc") == 0);

	const char* one[] = { "init", 0 };           // no slash -> comm is the whole arg
	ProcTable::setCommand(p, one, 1);
	CHECK(strcmp(p->comm, "init") == 0);
	CHECK(strcmp(p->cmdline, "init") == 0);
}

TEST_CASE("reapStopped: reports a stopped child once, then nothing") {
	ProcTable::init();
	Process* parent = ProcTable::alloc(0);
	Process* child = ProcTable::alloc(parent->pid);

	Process* out = (Process*) 0x1;
	CHECK(ProcTable::reapStopped(parent->pid, -1, &out) == 0);   // not stopped yet

	child->stopped = true;
	child->stopSignal = 20;   // SIGTSTP
	out = nullptr;
	CHECK(ProcTable::reapStopped(parent->pid, -1, &out) == child->pid);
	CHECK(out == child);
	CHECK(child->stopReported);

	out = nullptr;
	CHECK(ProcTable::reapStopped(parent->pid, -1, &out) == 0);   // already reported
	// wantPid that is not this child -> nothing.
	child->stopReported = false;
	CHECK(ProcTable::reapStopped(parent->pid, 9999, &out) == 0);
}

TEST_CASE("infoByPid: a job-control stopped process reads state 'T'") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	const char* av[] = { "spin", 0 };
	ProcTable::setCommand(a, av, 1);
	a->stopped = true;

	ProcInfo pi;
	REQUIRE(ProcTable::infoByPid(a->pid, &pi));
	CHECK(pi.state == 'T');
}

TEST_CASE("snapshot/infoByPid: fields + state char from the task / exit flag") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	const char* av[] = { "init", 0 };
	ProcTable::setCommand(a, av, 1);

	// No task yet -> 'R'.
	ProcInfo pi;
	REQUIRE(ProcTable::infoByPid(a->pid, &pi));
	CHECK(pi.state == 'R');
	CHECK(pi.ppid == 0);
	CHECK(strcmp(pi.comm, "init") == 0);

	// Blocked task -> 'S'.
	Task t; t.state = TASK_BLOCKED; a->task = &t;
	REQUIRE(ProcTable::infoByPid(a->pid, &pi));
	CHECK(pi.state == 'S');

	// Exited overrides the task state -> 'Z'.
	a->exited = true;
	REQUIRE(ProcTable::infoByPid(a->pid, &pi));
	CHECK(pi.state == 'Z');

	// A kthread child shows up in the snapshot.
	Process* k = ProcTable::alloc(a->pid);
	k->kthread = true;
	const char* kv[] = { "clock", 0 };
	ProcTable::setCommand(k, kv, 1);

	ProcInfo arr[8];
	int n = ProcTable::snapshot(arr, 8);
	CHECK(n == 2);
	bool sawKthread = false;
	for (int i = 0; i < n; i++)
		if (arr[i].pid == k->pid) { sawKthread = true; CHECK(arr[i].kthread); }
	CHECK(sawKthread);

	CHECK(!ProcTable::infoByPid(9999, &pi));      // absent pid
}

TEST_CASE("snapshot into a ProcTable::MAX buffer captures every process — no truncation") {
	ProcTable::init();
	// Fill the table to its hard ceiling.
	int made = 0;
	for (int i = 0; i < ProcTable::MAX; i++)
		if (ProcTable::alloc(0)) made++;
	CHECK(made == ProcTable::MAX);
	CHECK(ProcTable::alloc(0) == nullptr);          // full: alloc fails honestly

	// A buffer sized to MAX must hold them all; the count can never exceed MAX.
	ProcInfo arr[ProcTable::MAX];
	int n = ProcTable::snapshot(arr, ProcTable::MAX);
	CHECK(n == ProcTable::MAX);
}

// ---- Sessions + process groups (Stage 4) -------------------------------------------

TEST_CASE("alloc: a fresh process leads its own group and session") {
	ProcTable::init();
	Process* p = ProcTable::alloc(0);
	CHECK(p->pgid == p->pid);
	CHECK(p->sid == p->pid);
}

TEST_CASE("getpgid/getsid: 0 means the current process; bad pid -> -ESRCH") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	ProcTable::setCurrent(a);
	CHECK(ProcTable::getpgid(0) == a->pgid);
	CHECK(ProcTable::getsid(0) == a->sid);
	CHECK(ProcTable::getpgid(a->pid) == a->pgid);
	CHECK(ProcTable::getpgid(9999) == -3);
	CHECK(ProcTable::getsid(9999) == -3);
}

TEST_CASE("setpgid: a child joins a new group of its own, then an existing one") {
	ProcTable::init();
	Process* sh = ProcTable::alloc(0);
	ProcTable::setCurrent(sh);
	Process* c1 = ProcTable::alloc(sh->pid);   // inherits via fork in the kernel; here set it
	c1->pgid = sh->pgid; c1->sid = sh->sid;
	Process* c2 = ProcTable::alloc(sh->pid);
	c2->pgid = sh->pgid; c2->sid = sh->sid;

	// c1 becomes its own group leader (pgid 0 -> its pid).
	CHECK(ProcTable::setpgid(c1->pid, 0) == 0);
	CHECK(c1->pgid == c1->pid);
	// c2 joins c1's group (an existing in-session group).
	CHECK(ProcTable::setpgid(c2->pid, c1->pid) == 0);
	CHECK(c2->pgid == c1->pid);

	int pids[8];
	int n = ProcTable::groupMembers(c1->pid, pids, 8);
	CHECK(n == 2);                              // c1 + c2 share the group
}

TEST_CASE("setpgid: rejects a non-child and a cross-session move") {
	ProcTable::init();
	Process* sh = ProcTable::alloc(0);
	ProcTable::setCurrent(sh);
	Process* other = ProcTable::alloc(0);      // not our child, own session
	CHECK(ProcTable::setpgid(other->pid, 0) == -3);   // -ESRCH: not us / not our child
	CHECK(ProcTable::setpgid(9999, 0) == -3);
	CHECK(ProcTable::setpgid(0, -1) == -22);          // -EINVAL
}

TEST_CASE("setsid: a forked child (not a group leader) starts a new session") {
	ProcTable::init();
	Process* sh = ProcTable::alloc(0);
	ProcTable::setCurrent(sh);
	Process* c = ProcTable::alloc(sh->pid);
	c->pgid = sh->pgid; c->sid = sh->sid;      // fork inheritance: NOT a group leader
	ProcTable::setCurrent(c);
	int sid = ProcTable::setsid();
	CHECK(sid == c->pid);
	CHECK(c->pgid == c->pid);
	CHECK(c->sid == c->pid);
	// A group leader cannot setsid again.
	CHECK(ProcTable::setsid() == -1);          // -EPERM
}

TEST_CASE("groupMembers: only live processes of the given group are listed") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);          // pgid = a->pid
	Process* b = ProcTable::alloc(0);
	b->pgid = a->pid;                          // b joins a's group
	int pids[8];
	CHECK(ProcTable::groupMembers(a->pid, pids, 8) == 2);
	CHECK(ProcTable::groupMembers(b->pid, pids, 8) == 0);   // nobody has b's own pgid
	ProcTable::freeSlot(b);
	CHECK(ProcTable::groupMembers(a->pid, pids, 8) == 1);   // only a remains
}

// ---- CPU accounting (Stage 5 hardening) ----------------------------------------------

TEST_CASE("accountTick splits user/system/idle and bumps the current process") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	ProcTable::setCurrent(a);
	ProcTable::accountTick(true, false);    // user tick for `a`
	ProcTable::accountTick(true, false);
	ProcTable::accountTick(false, false);   // system tick for `a`
	ProcTable::accountTick(false, true);    // idle tick (not attributed to a process)
	CHECK(a->utime == 2);
	CHECK(a->stime == 1);
	unsigned u, s, i;
	ProcTable::cpuTimes(&u, &s, &i);
	CHECK(u == 2);
	CHECK(s == 1);
	CHECK(i == 1);
}

TEST_CASE("forksTotal counts every alloc; lastPid tracks the newest") {
	ProcTable::init();
	CHECK(ProcTable::forksTotal() == 0);
	Process* a = ProcTable::alloc(0);
	Process* b = ProcTable::alloc(a->pid);
	CHECK(ProcTable::forksTotal() == 2);
	CHECK(ProcTable::lastPid() == b->pid);
	ProcTable::freeSlot(b);
	ProcTable::alloc(a->pid);               // a third process: forks keeps climbing
	CHECK(ProcTable::forksTotal() == 3);
}

// ---- Job-control POSIX hardening -----------------------------------------------------

TEST_CASE("setpgid: a child that has exec'd can no longer be moved (-EACCES)") {
	ProcTable::init();
	Process* sh = ProcTable::alloc(0);
	ProcTable::setCurrent(sh);
	Process* c = ProcTable::alloc(sh->pid);
	c->pgid = sh->pgid; c->sid = sh->sid;
	c->execed = true;                          // child called execve
	CHECK(ProcTable::setpgid(c->pid, 0) == -13);   // -EACCES
}

TEST_CASE("isOrphanedGroup: a live in-session out-of-group parent keeps a group attached") {
	ProcTable::init();
	Process* sh = ProcTable::alloc(0);         // group sh, session sh
	Process* c = ProcTable::alloc(sh->pid);    // child leads its own group, same session
	c->pgid = c->pid; c->sid = sh->sid;
	CHECK(!ProcTable::isOrphanedGroup(c->pid));    // sh is a live outside-but-in-session parent
	sh->exited = true;                         // parent leaves
	CHECK(ProcTable::isOrphanedGroup(c->pid));     // now orphaned
	CHECK(!ProcTable::isOrphanedGroup(999));       // empty group is not "orphaned"
}

TEST_CASE("groupHasStopped: true once a member is job-control stopped") {
	ProcTable::init();
	Process* a = ProcTable::alloc(0);
	CHECK(!ProcTable::groupHasStopped(a->pid));
	a->stopped = true;
	CHECK(ProcTable::groupHasStopped(a->pid));
}

// ---- mmap free-list helpers (VA reclaim for munmap/mmap) ------------------------------

TEST_CASE("mmapFreeFind: first-fit picks the first entry large enough") {
	MmapFree list[4] = {{0x1000, 0x1000}, {0x4000, 0x4000}, {0x9000, 0x2000}};
	int count = 3;
	CHECK(mmapFreeFind(list, count, 0x1000) == 0);   // exact first
	CHECK(mmapFreeFind(list, count, 0x2000) == 1);   // entry 0 too small -> entry 1
	CHECK(mmapFreeFind(list, count, 0x4000) == 1);   // entry 1 exact
	CHECK(mmapFreeFind(list, count, 0x5000) == -1);  // nothing fits
	CHECK(mmapFreeFind(list, 0, 0x1000) == -1);      // empty list
}

TEST_CASE("mmapFreeCarve: exact fit removes the entry and compacts") {
	MmapFree list[4] = {{0x1000, 0x1000}, {0x4000, 0x4000}, {0x9000, 0x2000}};
	int count = 3;
	unsigned va = mmapFreeCarve(list, &count, 1, 0x4000);   // exact match on entry 1
	CHECK(va == 0x4000);
	CHECK(count == 2);
	CHECK(list[1].va == 0x9000);   // entry 2 shifted down into slot 1
	CHECK(list[1].len == 0x2000);
}

TEST_CASE("mmapFreeCarve: larger entry shrinks from the front") {
	MmapFree list[4] = {{0x4000, 0x4000}};
	int count = 1;
	unsigned va = mmapFreeCarve(list, &count, 0, 0x1000);
	CHECK(va == 0x4000);           // hands out the original front VA
	CHECK(count == 1);             // entry stays
	CHECK(list[0].va == 0x5000);   // remainder starts one page up
	CHECK(list[0].len == 0x3000);
}

TEST_CASE("mmapFreeAdd: appends until full, then reports failure") {
	MmapFree list[2];
	int count = 0;
	CHECK(mmapFreeAdd(list, &count, 2, 0x1000, 0x1000));   // distinct, non-adjacent
	CHECK(mmapFreeAdd(list, &count, 2, 0x9000, 0x1000));
	CHECK(count == 2);
	CHECK(!mmapFreeAdd(list, &count, 2, 0x20000, 0x1000)); // list full -> false
	CHECK(count == 2);
}

TEST_CASE("mmapFreeAdd: coalesces with an adjacent range on either side") {
	MmapFree list[4] = {{0x4000, 0x1000}};
	int count = 1;
	CHECK(mmapFreeAdd(list, &count, 4, 0x5000, 0x1000));   // touches the top of entry 0
	CHECK(count == 1);
	CHECK(list[0].va == 0x4000);
	CHECK(list[0].len == 0x2000);
	CHECK(mmapFreeAdd(list, &count, 4, 0x3000, 0x1000));   // touches the bottom of entry 0
	CHECK(count == 1);
	CHECK(list[0].va == 0x3000);
	CHECK(list[0].len == 0x3000);
}

TEST_CASE("mmap free-list: a freed range is reused by a later same-size request") {
	// End-to-end of the churn pattern: free a region, then a same-size mmap reuses its VA
	// instead of bumping (what keeps the finite window alive across create/join).
	MmapFree list[8];
	int count = 0;
	CHECK(mmapFreeAdd(list, &count, 8, 0x50000000, 0x23000));   // ~140 KiB freed on join
	int fi = mmapFreeFind(list, count, 0x23000);
	REQUIRE(fi == 0);
	unsigned va = mmapFreeCarve(list, &count, fi, 0x23000);
	CHECK(va == 0x50000000);   // next create gets the recycled VA
	CHECK(count == 0);         // list drained
}
