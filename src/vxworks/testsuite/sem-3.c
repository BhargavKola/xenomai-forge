#include <stdio.h>
#include <stdlib.h>
#include <copperplate/traceobj.h>
#include <vxworks/errnoLib.h>
#include <vxworks/taskLib.h>
#include <vxworks/semLib.h>
#include <vxworks/kernelLib.h>

static struct traceobj trobj;

static int tseq[] = {
	1, 2, 3, 5, 4, 6
};

SEM_ID sem_id;

void rootTask(long a0, long a1, long a2, long a3, long a4,
	      long a5, long a6, long a7, long a8, long a9)
{
	int ret;

	traceobj_enter(&trobj);

	traceobj_mark(&trobj, 1);

	sem_id = semBCreate(SEM_Q_FIFO, SEM_FULL);
	traceobj_assert(&trobj, sem_id != 0);

	ret = semGive(sem_id);
	traceobj_assert(&trobj, ret == OK);

	traceobj_mark(&trobj, 2);

	ret = semTake(sem_id, NO_WAIT);
	traceobj_assert(&trobj, ret == OK);

	traceobj_mark(&trobj, 3);

	ret = semTake(sem_id, WAIT_FOREVER);
	traceobj_assert(&trobj, ret == ERROR && errno == S_objLib_OBJ_DELETED);

	traceobj_mark(&trobj, 4);

	traceobj_exit(&trobj);
}

int main(int argc, char *argv[])
{
	int ret;

	traceobj_init(&trobj, argv[0], sizeof(tseq) / sizeof(int));

	ret = kernelInit(rootTask, argc, argv);
	traceobj_assert(&trobj, ret == OK);

	traceobj_mark(&trobj, 5);

	ret = semDelete(sem_id);
	traceobj_assert(&trobj, ret == OK);

	traceobj_mark(&trobj, 6);

	traceobj_join(&trobj);

	traceobj_verify(&trobj, tseq, sizeof(tseq) / sizeof(int));

	exit(0);
}
