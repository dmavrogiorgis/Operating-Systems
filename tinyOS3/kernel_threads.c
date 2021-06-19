
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
/**
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
	/* Allocate memory for the initialization f the ptcb */
	PTCB* newPTCB = (PTCB*)xmalloc(sizeof(PTCB));

	newPTCB->exit_val = 0;
  newPTCB->main_task = task;
  newPTCB->argl = argl;
  newPTCB->args = args;
	newPTCB->thread = spawn_thread(CURPROC, start_thread);
	CURPROC->num_of_threads += 1;
	newPTCB->exited = 0;
	newPTCB->detached = 0;
	newPTCB->joined = COND_INIT;
  newPTCB->ref_counter = 0;
	newPTCB->thread->owner_ptcb = newPTCB;

  rlnode_init(& newPTCB->PTCB_node, newPTCB);	/* Intrusive list node */

	rlist_push_back(&CURPROC->PTCB_list, &newPTCB->PTCB_node); /* Push th ptcb to the list of ptcbs in stack of the current process*/

	wakeup(newPTCB->thread);	/* We wake up the thread so it can be "served" by the scheduler */

	return (Tid_t)newPTCB;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf(){

	return (Tid_t)CURTHREAD->owner_ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{

	PTCB* joined = (PTCB*)tid;		/* We cast the tid of the thread to a ptcb object */

	if(joined == NULL){
		return -1;	/*If there isn't joined thread we return -1*/
	}

	if(joined->detached == 1 ){
		return -1;		/* If there is already a joined thread we return -1 */
	}

	refcounter_increment(joined);	/* We increase the ref_counter cause there is joined thread */

	while(joined->exited != 1 && joined->detached != 1){
		kernel_wait( &joined->joined, SCHED_USER);	/* We sleep the thread which joins the ptcb until the joined exits	*/
	}

	if(exitval != NULL){
		*exitval = joined->exited;	/* We return the exited value of the joined thread */
	}
	refcounter_decrement(joined);	/* We decrease the ref_counter, cause joined thread ended*/

	kernel_broadcast( &joined->joined);	/* We broadcast all the joined threads */

	return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
	PTCB* ptcb = (PTCB*)tid;	/* We cast the tid of the thread to a ptcb object */

	if(ptcb == NULL){
		return -1;	/*If there isn't joined thread, we return -1*/
	}

	if(ptcb->exited == 1 ){
		return -1;	/* If thread is exited, we return -1 */
	}
  TCB* tcb = ptcb->thread;
	if(tcb ==NULL || tcb->state == EXITED){
		return -1;
	}
	if(tcb->owner_pcb == CURPROC){
		ptcb->detached = 1;					/* We change the detached to 1 because we want to detach all the joiners */
		if(ptcb->ref_counter > 1){
				kernel_broadcast(& ptcb->joined);		/* If there are joiners we broadcast them all */
		}
		return 0; /* Return 0 when the process succeeds */
	}
	return -1;	/* Return -1 when the process fails	*/
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
	TCB* curTCB = CURTHREAD;
	PTCB* curPTCB = curTCB->owner_ptcb;

	assert(curPTCB != NULL);	/* We assert the there is the ptcb */

	curPTCB->thread->state = EXITED;	/* We mark the state of the current thread as exited */
	curPTCB->thread = NULL;
	curPTCB->exited = 1;		/* We change the exited variable of the ptcb to 1 to keep the information that the current thread just exited */
	CURPROC->num_of_threads -= 1;
	if(curPTCB->ref_counter > 0){
		kernel_broadcast(&curPTCB->joined);		/* We check if the there are reference counters(joiners) and we broadcast all joined threads */
	}
	rlist_remove(&curPTCB->PTCB_node);		/* We remove the current ptcb from the list */
	free(curPTCB);		/* And then free the memory we alLocated during its creation and set the curPTCB pointer to NULL*/
	curPTCB = NULL;

	kernel_sleep(EXITED, SCHED_USER);
}

void start_thread(){

	int exitval;

	TCB* curTCB = CURTHREAD;
	PTCB* curPTCB = curTCB->owner_ptcb;

	Task call =  curPTCB->main_task;
	int argl = curPTCB->argl;
	void* args = curPTCB->args;

	exitval = call(argl,args);
	ThreadExit(exitval);
}

void refcounter_increment(PTCB* ptcb){

	ptcb->ref_counter += 1;
}

void refcounter_decrement(PTCB* ptcb){

	ptcb->ref_counter -= 1;
}
