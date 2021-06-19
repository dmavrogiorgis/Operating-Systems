
#include "tinyos.h"
#include "kernel_socket.h"
#include "kernel_streams.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

SocketCB* PORT_MAP[MAX_PORT+1];

void initialize_port_map(){

  for(int i=0;i<=MAX_PORT;i++){
    PORT_MAP[i] = NULL;
  }
}

Fid_t sys_Socket(port_t port)
{

	if(port < 0 || port > MAX_PORT){
		return NOFILE;
	}

	int check = 0;
	Fid_t fids[1];
	FCB* socketFCB[1];

	check = FCB_reserve(1, fids, socketFCB);

	if(check == 0 ){
		return -1;
	}

	SocketCB* socketCB = spawn_Socket();

	if(socketCB == NULL){
		return NOFILE;
	}

	socketCB->Port = port;
	socketCB->SocketFCB = get_fcb(fids[0]);
  ref_counter_incr(socketCB);

	socketFCB[0]->streamobj = socketCB;

	static file_ops socketfile_ops = {
		.Open = NullOpenSocket,
		.Read = NullReadSocket,
		.Write = NullWriteSocket,
		.Close = CloseSocket
	};

	socketFCB[0]->streamfunc = &socketfile_ops;

	return fids[0];
}

int sys_Listen(Fid_t sock)
{

  if(sock < 0 || sock > MAX_FILEID){
    return -1;
  }

	FCB* fcb = get_fcb(sock);

	if(fcb == NULL ){
		return -1;
	}

	SocketCB* socketCB = (SocketCB*)fcb->streamobj;

	if(socketCB == NULL || socketCB->Port <= 0 || socketCB->Port > MAX_PORT || socketCB->Type != UNBOUND){

		return -1;
	}

	socketCB->Type = LISTENER;
  if( PORT_MAP[socketCB->Port] != NULL){
    return -1;
  }else{
    PORT_MAP[socketCB->Port] = socketCB;
    ref_counter_incr(socketCB);
  }

  rlnode_init( &socketCB->LS.connect_Requests, NULL);
	socketCB->LS.is_empty = COND_INIT;

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
  if(lsock == NOFILE){
    return -1;
  }

	FCB* fcb = get_fcb(lsock);
	if(fcb == NULL ){
		return -1;
	}

	SocketCB* lsocketCB = (SocketCB*)fcb->streamobj;
	if( lsocketCB == NULL || lsocketCB->Port == 0 || lsocketCB->Type != LISTENER){
		return -1;
	}
  while(is_rlist_empty(&lsocketCB->LS.connect_Requests)){
    kernel_wait(&lsocketCB->LS.is_empty, SCHED_PIPE);
    if(lsocketCB->Type == UNBOUND){
      return NOFILE;
    }
  }
  rlnode* sel;
  sel=rlist_pop_front(&lsocketCB->LS.connect_Requests);
  Request* request = sel->req;
  SocketCB* client = request->Socket;
  client->Type = PEER;

  Fid_t socketFid = sys_Socket(NOPORT);
  if (socketFid == NOFILE) {
    return NOFILE;
  }
  SocketCB* server = get_fcb(socketFid)->streamobj;
  server->Type = PEER;

  int check = spawnPipes(client,server);

  if(check == 0){
    static file_ops peerfile_ops = {
      .Open = NullOpenSocket,
      .Read = ReadSocket,
      .Write = WriteSocket,
      .Close = CloseSocket
    };
    server->SocketFCB->streamfunc = &peerfile_ops;
    client->SocketFCB->streamfunc = &peerfile_ops;

    kernel_broadcast(&request->is_connected);
  }else{
    return -1;
  }

	return socketFid;
}
int spawnPipes(SocketCB* client, SocketCB* server)
{
  if(client == NULL && server == NULL){
    return -1;
  }
  PipeCB* Pipe1 = spawn_Pipe();
  PipeCB* Pipe2 = spawn_Pipe();
  if(Pipe1==NULL || Pipe2==NULL){
    return -1;
  }
  Pipe1->ReadFCB = server->SocketFCB;
  Pipe1->WriteFCB = client->SocketFCB;
  Pipe2->ReadFCB = client->SocketFCB;
  Pipe2->WriteFCB = server->SocketFCB;

  client->PS.send = Pipe1;
  client->PS.receive = Pipe2;
  server->PS.send = Pipe2;
  server->PS.receive = Pipe1;

  client->PS.peerSocket=server;
  server->PS.peerSocket=client;

  return 0;
}

int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
  if(port <= 0 || port > MAX_PORT || PORT_MAP[port]==NULL || timeout==0 ){
    return -1;
  }

  FCB* fcb = get_fcb(sock);
  if(fcb == NULL ){
    return -1;
  }

  SocketCB* socketCB = (SocketCB*)fcb->streamobj;
  if(socketCB == NULL || socketCB == PORT_MAP[port] || PORT_MAP[port]->Type != LISTENER){
    return -1;
  }

  Request* request = spawn_Request();
  request->Socket = socketCB;

  rlist_push_back(&(PORT_MAP[port]->LS.connect_Requests), &request->req_node);
  kernel_broadcast(&(PORT_MAP[port]->LS.is_empty));

  if(timeout > 0){
    int k = kernel_timedwait(&request->is_connected,SCHED_PIPE,timeout);
    if(k==1){
      request->admit_flag = 1;
    }
  }else{
    kernel_wait(&request->is_connected, SCHED_PIPE);
  }

  return request->admit_flag - 1;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
  FCB* fcb = get_fcb(sock);
  if(fcb == NULL ){
    return -1;
  }

  SocketCB* socket = (SocketCB*)fcb->streamobj;
  if(socket == NULL || socket->Type != PEER){
    return -1;
  }

  switch(how){
    case SHUTDOWN_READ:
      CloseReaderPipe(socket->PS.receive);
      socket->PS.receive = NULL;
      break;
    case SHUTDOWN_WRITE:
      CloseWriterPipe(socket->PS.send);
      socket->PS.send = NULL;
      break;
    case SHUTDOWN_BOTH:
      CloseReaderPipe(socket->PS.receive);
      CloseWriterPipe(socket->PS.send);
      socket->PS.receive = NULL;
      socket->PS.send = NULL;
      break;
  }
	return 0;
}

void ref_counter_incr(SocketCB* socket){
    socket->ref_counter++;
}

void ref_counter_decr(SocketCB* socket){
  socket->ref_counter--;
}

SocketCB* spawn_Socket(){

	SocketCB* socket = (SocketCB*)xmalloc(sizeof(SocketCB));
  if(socket == NULL){
    return NULL;
  }
	socket->ref_counter = 0;
	socket->Type = UNBOUND;
	return socket;
}
Request* spawn_Request(){

  Request* req = (Request*)xmalloc(sizeof(Request));
  if(req == NULL){
    return NULL;
  }
  req->is_connected = COND_INIT;
  req->admit_flag = 0;

  rlnode_init(& req->req_node, req);	/* Intrusive list node */
  return req;
}

int CloseSocket(void* streamobject){
  SocketCB* socket = (SocketCB*)streamobject;

  if(socket == NULL){
    return -1;
  }
  if(socket->Type == UNBOUND){
    free(socket);
  }else if(socket->Type == LISTENER){
    kernel_broadcast(&socket->LS.is_empty);
    if(PORT_MAP[socket->Port]!=NULL){
      PORT_MAP[socket->Port]=NULL;
    }
    free(socket);
  }else{
    CloseReaderPipe(socket->PS.receive);
    CloseWriterPipe(socket->PS.send);
    socket->PS.send = NULL;
    socket->PS.receive = NULL;
    socket->PS.peerSocket = NULL;
    free(socket);
  }

  return 0;
}

int WriteSocket(void* streamobject, const char* buf, unsigned int size){

  SocketCB* from = (SocketCB*)streamobject;
  if(from == NULL || from->Type != PEER){
    return -1;
  }
  SocketCB* to = from->PS.peerSocket;
  if(from->PS.send == NULL || to->PS.receive == NULL){
    return -1;
  }
  PipeCB* pipefrom = from->PS.send;
  PipeCB* pipeto = to->PS.receive;
  if(to == NULL || pipefrom == NULL || pipeto == NULL){
    return -1;
  }

  int bytesize;
  bytesize = WritePipe(pipefrom, buf, size);

  return bytesize;
}

int ReadSocket(void* streamobject, char* buf, unsigned int size){
  SocketCB* from = (SocketCB*)streamobject;
  if(from == NULL || from->Type != PEER){
    return -1;
  }
  SocketCB* to = from->PS.peerSocket;
  if(from->PS.receive == NULL){
    return -1;
  }
  PipeCB* pipefrom = from->PS.receive;
  if(to == NULL || pipefrom == NULL ){
    return -1;
  }

  int bytesize;
  bytesize = ReadPipe(pipefrom, buf, size);

  return bytesize;
}

int NullWriteSocket(void* streamobject, const char* buf, unsigned int size){

  return size;
}
int NullReadSocket(void* streamobject, char* buf, unsigned int size){

  memset(buf, 0, size);
  return size;
}
void* NullOpenSocket(uint minor)
{
  return NULL;
}
