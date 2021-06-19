#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_pipe.h"

typedef enum {
    UNBOUND,
    LISTENER,
    PEER
} SocketType;

typedef struct socket_control_block SocketCB;

typedef struct Request_For_Listener
{
  SocketCB* Socket;
  CondVar is_connected;
  unsigned int admit_flag;
  rlnode req_node;
}Request;

typedef struct listener_socket
{
  rlnode connect_Requests;
  CondVar is_empty;

} ListenerSocket;

typedef struct peers_socket
{
  PipeCB* send;
  PipeCB* receive;
  SocketCB* peerSocket;

} PeerSocket;

typedef struct socket_control_block
{

  unsigned int ref_counter;
  port_t Port;
  SocketType Type;
  FCB* SocketFCB;

  union{
    ListenerSocket LS;
    PeerSocket PS;
  };

} SocketCB;

SocketCB* spawn_Socket();

Request* spawn_Request();

int spawnPipes(SocketCB* client, SocketCB* server);

void ref_counter_incr(SocketCB* socket);

void ref_counter_decr(SocketCB* socket);

int CloseSocket(void* streamobject);

int WriteSocket(void* streamobject, const char* buf, unsigned int size);

int ReadSocket(void* streamobject, char* buf, unsigned int size);

int NullWriteSocket(void* streamobject, const char* buf, unsigned int size);

int NullReadSocket(void* streamobject, char* buf, unsigned int size);

void* NullOpenSocket(uint minor);
