#include "tinyos.h"
#include "kernel_streams.h"

#define BUFFER_SIZE 8192

typedef struct pipe_control_block
{
	char buffer[BUFFER_SIZE];

  unsigned int Head;
  unsigned int NElements;

  FCB* ReadFCB;
	FCB* WriteFCB;

	CondVar Producer;
	CondVar Consumer;

  unsigned int ref_counter;

} PipeCB;


PipeCB* spawn_Pipe();

void refcounter_incr(PipeCB* pipe);

void refcounter_decr(PipeCB* pipe);

int WritePipe(void* streamobject, const char* buf, unsigned int size);

int ReadPipe(void* streamobject, char* buf, unsigned int size);

int CloseReaderPipe(void* streamobject);

int CloseWriterPipe(void* streamobject);

int NullWritePipe(void* streamobject, const char* buf, unsigned int size);

int NullReadPipe(void* streamobject, char* buf, unsigned int size);

void* NullOpenPipe(uint minor);
