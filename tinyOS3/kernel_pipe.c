
#include "tinyos.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_pipe.h"

int ReadCounter = 0;
int WriteCounter = 0;

int sys_Pipe(pipe_t* pipe)
{
	int check = 0;
	Fid_t fid[2];
	FCB* pipeFCBs[2];

	check = FCB_reserve(2, fid, pipeFCBs);	/* Reserve two FCBs for the two end points of the pipe */

	if(check == 0 ){
		return -1;			/* If there are no avaliable FCBs for the pipe, we return -1 (fail the creation of pipe) */
	}

	PipeCB* pipeCB = spawn_Pipe();		/* Initialization of the pipe */

	if(pipeCB == NULL){
		return -1;
	}

	pipe->read = fid[0];
	pipe->write = fid[1];

	pipeCB->ReadFCB = get_fcb(fid[0]);
  pipeCB->WriteFCB = get_fcb(fid[1]);

	for(int i=0;i<2;i++){
		pipeFCBs[i]->streamobj = pipeCB;		/* Initialization of the stream object in the two FCBs */
		refcounter_incr(pipeCB);
	}

	static file_ops readfile_ops = {
		.Open = NullOpenPipe,
		.Read = ReadPipe,
		.Write = NullWritePipe,
		.Close = CloseReaderPipe
	};		/* Initialization of the stream functions in the read FCB */

	static file_ops writefile_ops = {
	  .Open = NullOpenPipe,
		.Read = NullReadPipe,
		.Write = WritePipe,
		.Close = CloseWriterPipe
	};		/* Initialization of the stream functions in the write FCB */

	pipeFCBs[0]->streamfunc = &readfile_ops;		/* Initialization of the stream functions in the read FCB */
	pipeFCBs[1]->streamfunc = &writefile_ops;		/* Initialization of the stream functions in the write FCB */

	return 0;		/* Return 0, as success for the creation of the pipe*/
}
PipeCB* spawn_Pipe(){

  PipeCB* pipeCB = (PipeCB*)xmalloc(sizeof(PipeCB));

  if(pipeCB == NULL){
    return NULL;
  }

  memset(pipeCB->buffer, 0, BUFFER_SIZE);

  pipeCB->Head = 0;
  pipeCB->NElements = 0;

  pipeCB->Producer = COND_INIT;
  pipeCB->Consumer = COND_INIT;

	pipeCB->ref_counter = 0;

  return pipeCB;
}

void refcounter_incr(PipeCB* pipe){

	pipe->ref_counter += 1;
}

void refcounter_decr(PipeCB* pipe){

	pipe->ref_counter -= 1;
}

int WritePipe(void* streamobject, const char* buf, unsigned int size){

   PipeCB* pipe = (PipeCB*)streamobject;

   if(pipe == NULL || pipe->ReadFCB == NULL ){
     return -1;
   }

	 while(pipe->NElements == BUFFER_SIZE && pipe->ReadFCB!=NULL){
 		kernel_wait(&pipe->Producer,SCHED_PIPE);
 	 }

	 int count = 0;
   while(count < size){
		if(pipe->NElements == BUFFER_SIZE){
			 kernel_broadcast(&pipe->Consumer);
			 return count;
		}else{
	  	pipe->buffer[(pipe->Head + pipe->NElements)%BUFFER_SIZE] = buf[count];
		  pipe->NElements++;
	 	}
		count++;
  }
	kernel_broadcast(&pipe->Consumer);
  return count;
}


int ReadPipe(void* streamobject, char* buf, unsigned int size){

  PipeCB* pipe = (PipeCB*)streamobject;

  if(pipe == NULL){
    return -1;
  }
 int count = 0;
	if(pipe->WriteFCB == NULL && pipe->NElements == 0){
		return count;
	}

	while(pipe->NElements == 0 && pipe->WriteFCB!=NULL){
		kernel_wait(&pipe->Consumer, SCHED_PIPE);
	}

  while(count < size ){
		if(pipe->NElements == 0 ){
			if(pipe->WriteFCB != NULL){
				kernel_broadcast(&pipe->Producer);
			}
			return count;
		}else{
			buf[count] = pipe->buffer[pipe->Head];
			pipe->Head = (pipe->Head + 1) % BUFFER_SIZE;
			pipe->NElements--;
		}

		count++;
	}
	kernel_broadcast(&pipe->Producer);
  return size;
}

int CloseReaderPipe(void* streamobject){

  PipeCB* pipe = (PipeCB*)streamobject;

  if(pipe == NULL){
    return -1;
  }
	if(pipe->WriteFCB != NULL){
		kernel_broadcast(&pipe->Producer);
	}
	pipe->ReadFCB = NULL;

	if(pipe->WriteFCB == NULL && pipe->ReadFCB == NULL){
		free(pipe);
	}

  return 0;
}

int CloseWriterPipe(void* streamobject){

  PipeCB* pipe = (PipeCB*)streamobject;
  if(pipe == NULL){
    return -1;
  }

	if(pipe->ReadFCB != NULL){
		kernel_broadcast(&pipe->Consumer);
	}
	pipe->WriteFCB = NULL;

	if(pipe->WriteFCB == NULL && pipe->ReadFCB == NULL){
		free(pipe);
	}

  return 0;
}

int NullWritePipe(void* streamobject, const char* buf, unsigned int size){


  return size;
}


int NullReadPipe(void* streamobject, char* buf, unsigned int size){

  memset(buf, 0, size);
  return size;
}

void* NullOpenPipe(uint minor)
{
  return NULL;
}
