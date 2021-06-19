
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"

/*
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;
unsigned int process_counter = 0;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->PTCB_list, NULL);


  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;

  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process)
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /*
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {

    newproc->main_thread = spawn_thread(newproc, start_main_thread); /* Spawn the main thread and initialize the variable of the current process  */
    newproc->num_of_threads += 1;
    PTCB* new_ptcb = spawn_process_thread(newproc); /* Spawn the ptcb of the main thread */
    newproc->main_thread->owner_ptcb = new_ptcb;

    rlist_push_back(&newproc->PTCB_list, &new_ptcb->PTCB_node); /* Push the ptcb to the list of ptcb's in current process */

    wakeup(newproc->main_thread); /* We wake up the thread so it can be "served" by the scheduler */
  }


finish:
  return get_pid(newproc);
}

PTCB* spawn_process_thread(PCB* pcb)
{
  /* The allocated process thread size */
  PTCB* ptcb = (PTCB*)xmalloc(sizeof(PTCB));

  /* Initialize the other attributes */
  ptcb->exit_val = pcb->exitval;
  ptcb->thread = pcb->main_thread;
  ptcb->main_task = pcb->main_task;
  ptcb->argl = pcb->argl;
  ptcb->args = pcb->args;
  ptcb->exited = 0;
  ptcb->detached = 0;
  ptcb->joined = COND_INIT;
  ptcb->ref_counter = 0;
  rlnode_init(& ptcb->PTCB_node, ptcb);

  return ptcb;
}

/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);

  cleanup_zombie(child, status);

finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  if(is_rlist_empty(& parent->children_list)) {
    cpid = NOPROC;
    goto finish;
  }

  while(is_rlist_empty(& parent->exited_list)) {
    kernel_wait(& parent->child_exit, SCHED_USER);
  }

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

finish:
  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{
  /* Right here, we must check that we are not the boot task. If we are,
     we must wait until all processes exit. */
  if(sys_GetPid()==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* Do all the other cleanup we want here, close files etc. */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  /* Reparent any children of the exiting process to the
     initial task */
  PCB* initpcb = get_pcb(1);
  while(!is_rlist_empty(& curproc->children_list)) {
    rlnode* child = rlist_pop_front(& curproc->children_list);
    child->pcb->parent = initpcb;
    rlist_push_front(& initpcb->children_list, child);
  }

  /* Add exited children to the initial task's exited list
     and signal the initial task */
  if(!is_rlist_empty(& curproc->exited_list)) {
    rlist_append(& initpcb->exited_list, &curproc->exited_list);
    kernel_broadcast(& initpcb->child_exit);
  }

  /* Put me into my parent's exited list */
  if(curproc->parent != NULL) {   /* Maybe this is init */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);
  }

  PTCB* curPTCB = curproc->main_thread->owner_ptcb;

	assert(curPTCB != NULL);	/* We assert the there is the ptcb */

  curPTCB->thread->state = EXITED;	/* We mark the state of the current thread as exited */
  curPTCB->thread = NULL;
  curPTCB->exited = 1;		/* We change the exited variable of the ptcb to 1 to keep the information that the current thread just exited */
  curproc->num_of_threads -= 1;

	if(curPTCB->ref_counter > 0){
		kernel_broadcast(&curPTCB->joined);		/* We check if the there are reference counters(joiners) and we broadcast all joined threads */
	}
	rlist_remove(&curPTCB->PTCB_node);		/* We remove the current ptcb from the list */
	free(curPTCB);		/* And then free the memory we alLocated during its creation and set the curPTCB pointer to NULL*/

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;
  curproc->exitval = exitval;

  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);
}



Fid_t sys_OpenInfo()
{
  int check = 0;
  Fid_t fid;
  FCB* procinfoFCB;

  check = FCB_reserve(1, &fid, &procinfoFCB);

  if(check == 0){
    return NOFILE;
  }

  procinfo* procinfoCB = spawn_ProcInfo();

  if(procinfoCB == NULL){
		return NOFILE;
	}

	procinfoFCB->streamobj = procinfoCB;

	static file_ops procinfofile_ops = {
		.Open = NullOpenInfo,
		.Read = ReadInfo,
		.Write = NullWriteInfo,
		.Close = CloseInfo
	};

	procinfoFCB->streamfunc = &procinfofile_ops;

	return fid;
}

procinfo* spawn_ProcInfo(){

  procinfo* procinfoCB = (procinfo*)xmalloc(sizeof(procinfo));

  if(procinfoCB == NULL){
    return NULL;
  }
  procinfoCB->pid = 0;
  procinfoCB->ppid = 0;
  procinfoCB->alive = 0;
  procinfoCB->thread_count = 0;
  procinfoCB->main_task = NULL;
  procinfoCB->argl = 0;
  memset(procinfoCB->args,0,PROCINFO_MAX_ARGS_SIZE);

  return procinfoCB;
}

void* NullOpenInfo(uint minor)
{
  return NULL;
}

int ReadInfo(void* streamobject, char* buf, unsigned int size){

  procinfo* procinfoCB = (procinfo*)streamobject;

  if(procinfoCB == NULL){
    return -1;
  }

  while(process_counter < MAX_PROC && process_counter < process_count){
    if(PT[process_counter].pstate != FREE){
      procinfoCB->pid = get_pid(&PT[process_counter]);
      procinfoCB->ppid = get_pid(PT[process_counter].parent);
      if(PT[process_counter].pstate ==ALIVE) {
        procinfoCB->alive = 1;
      }else{
        procinfoCB->alive = 0;
      }
      procinfoCB->thread_count = PT[process_counter].num_of_threads;
      procinfoCB->main_task = PT[process_counter].main_task;
      procinfoCB->argl = PT[process_counter].argl;

      memcpy(procinfoCB->args, PT[process_counter].args, procinfoCB->argl);

      memcpy(buf, (char*)procinfoCB, size);
      process_counter += 1;
      return size;
    }
    process_counter += 1;
  }
    process_counter = 0;
  return 0;
}

int NullWriteInfo(void* streamobject, const char* buf, unsigned int size){

  return size;
}

int CloseInfo(void* streamobject){

  procinfo* procinfoCB = (procinfo*)streamobject;

  if(procinfoCB == NULL){
    return -1;
  }
	free(procinfoCB);
  process_counter = 0;
  return 0;
}
